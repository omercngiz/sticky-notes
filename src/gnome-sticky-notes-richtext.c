/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include "gnome-sticky-notes-richtext.h"

#include <gdk/gdkkeysyms.h>
#include <string.h>

/* Prefix marking a stored note as rich text. Anything without a known marker
 * is treated as legacy plain text, so old notes keep opening correctly.
 * V1 = pre-ToDo format; V2 adds the todo-run field. The marker bytes are the
 * same length so stripping/offsetting is uniform. */
#define RICH_TEXT_MARKER "GSNRT2:"
#define RICH_TEXT_MARKER_V1 "GSNRT1:"

/* GVariant layout of a serialized note:
 *   s            full text (anchors included as U+FFFC, via get_slice)
 *   a(uua{sv})   character runs: (start_offset, end_offset, attributes)
 *   a(uus)       paragraph runs:  (start_offset, end_offset, alignment)
 *   a(ub)        todo runs:       (anchor_offset, checked) -- V2 only
 * Offsets are in characters, matching GtkTextIter offsets. */
#define RICH_TEXT_VARIANT_TYPE "(sa(uua{sv})a(uus)a(ub))"
#define RICH_TEXT_VARIANT_TYPE_V1 "(sa(uua{sv})a(uus))"

/* Tag-name prefixes for value attributes. Toggle tags use the bare names
 * "b"/"i"/"u"/"s" and are matched by pointer instead. */
#define FAMILY_PREFIX "fam:"
#define SIZE_PREFIX "sz:"
#define COLOR_PREFIX "fg:"
#define ALIGN_PREFIX "al:"

/* A resolved set of character attributes read from the buffer. */
typedef struct
{
  gboolean bold;
  gboolean italic;
  gboolean underline;
  gboolean strike;
  char *family; /* owned, NULL = default */
  gboolean has_size;
  double size;
  gboolean has_color;
  GdkRGBA color;
} AttrSet;

struct _GnomeStickyNotesRichText
{
  GObject parent_instance;

  GtkTextView *view;     /* owned ref */
  GtkTextBuffer *buffer; /* unowned, lives on the view */

  /* The four toggle tags, kept by pointer for fast identity checks. */
  GtkTextTag *tag_bold;
  GtkTextTag *tag_italic;
  GtkTextTag *tag_underline;
  GtkTextTag *tag_strike;

  /* Applied over a checked ToDo's text: strikethrough + faded. Derived from
   * the checkbox state, never stored as a character run. */
  GtkTextTag *tag_done;

  /* Active formatting: what the next typed character gets, and what the
   * toolbar should display. Recomputed as the cursor moves. */
  AttrSet active;
  GtkJustification active_align;

  /* "Armed" formatting set with a collapsed cursor (no selection), applied
   * to the next typed character. Like Word/LibreOffice, it stays live only
   * while the cursor remains where it was set; moving the cursor cancels it.
   * This is what lets a font/colour chosen from the toolbar survive clicking
   * back into the text without losing focus to a recompute. */
  guint pending : 1;
  int pending_offset;

  guint inserting : 1; /* set across an interactive insertion */
  guint applying : 1;  /* set while we mutate tags ourselves */
  guint syncing : 1;   /* set while recomputing/active update */
};

enum
{
  STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GnomeStickyNotesRichText, gnome_sticky_notes_rich_text, G_TYPE_OBJECT)

/* --- small helpers ------------------------------------------------------- */

static void
attr_set_clear (AttrSet *a)
{
  g_clear_pointer (&a->family, g_free);
  a->bold = a->italic = a->underline = a->strike = FALSE;
  a->has_size = FALSE;
  a->size = 0;
  a->has_color = FALSE;
}

static GtkTextTag *
toggle_tag (GnomeStickyNotesRichText *self,
            GnomeStickyNotesRichTextToggle which)
{
  switch (which)
    {
    case GNOME_STICKY_NOTES_RICH_TEXT_BOLD:
      return self->tag_bold;
    case GNOME_STICKY_NOTES_RICH_TEXT_ITALIC:
      return self->tag_italic;
    case GNOME_STICKY_NOTES_RICH_TEXT_UNDERLINE:
      return self->tag_underline;
    case GNOME_STICKY_NOTES_RICH_TEXT_STRIKETHROUGH:
      return self->tag_strike;
    default:
      return NULL;
    }
}

/* Returns an existing tag by name or creates it with the given property. */
static GtkTextTag *
family_tag (GnomeStickyNotesRichText *self,
            const char *family)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table (self->buffer);
  g_autofree char *name = g_strconcat (FAMILY_PREFIX, family, NULL);
  GtkTextTag *tag = gtk_text_tag_table_lookup (table, name);

  if (tag == NULL)
    tag = gtk_text_buffer_create_tag (self->buffer, name, "family", family, NULL);

  return tag;
}

static GtkTextTag *
size_tag (GnomeStickyNotesRichText *self,
          double points)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table (self->buffer);
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_autofree char *name = NULL;
  GtkTextTag *tag;

  g_ascii_formatd (num, sizeof num, "%g", points);
  name = g_strconcat (SIZE_PREFIX, num, NULL);
  tag = gtk_text_tag_table_lookup (table, name);

  if (tag == NULL)
    tag = gtk_text_buffer_create_tag (self->buffer, name, "size-points", points, NULL);

  return tag;
}

static GtkTextTag *
color_tag (GnomeStickyNotesRichText *self,
           const GdkRGBA *rgba)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table (self->buffer);
  g_autofree char *str = gdk_rgba_to_string (rgba);
  g_autofree char *name = g_strconcat (COLOR_PREFIX, str, NULL);
  GtkTextTag *tag = gtk_text_tag_table_lookup (table, name);

  if (tag == NULL)
    tag = gtk_text_buffer_create_tag (self->buffer, name, "foreground-rgba", rgba, NULL);

  return tag;
}

static const char *
align_to_string (GtkJustification just)
{
  switch (just)
    {
    case GTK_JUSTIFY_CENTER:
      return "center";
    case GTK_JUSTIFY_RIGHT:
      return "right";
    case GTK_JUSTIFY_FILL:
      return "fill";
    case GTK_JUSTIFY_LEFT:
    default:
      return "left";
    }
}

static GtkJustification
align_from_string (const char *s)
{
  if (g_strcmp0 (s, "center") == 0)
    return GTK_JUSTIFY_CENTER;
  if (g_strcmp0 (s, "right") == 0)
    return GTK_JUSTIFY_RIGHT;
  if (g_strcmp0 (s, "fill") == 0)
    return GTK_JUSTIFY_FILL;
  return GTK_JUSTIFY_LEFT;
}

static GtkTextTag *
align_tag (GnomeStickyNotesRichText *self,
           GtkJustification just)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table (self->buffer);
  g_autofree char *name = g_strconcat (ALIGN_PREFIX, align_to_string (just), NULL);
  GtkTextTag *tag = gtk_text_tag_table_lookup (table, name);

  if (tag == NULL)
    tag = gtk_text_buffer_create_tag (self->buffer, name, "justification", just, NULL);

  return tag;
}

typedef struct
{
  GtkTextBuffer *buffer;
  const GtkTextIter *start;
  const GtkTextIter *end;
  const char *prefix;
} RemoveCtx;

static void
remove_prefixed_cb (GtkTextTag *tag,
                    gpointer data)
{
  RemoveCtx *ctx = data;
  g_autofree char *name = NULL;

  g_object_get (tag, "name", &name, NULL);
  if (name != NULL && g_str_has_prefix (name, ctx->prefix))
    gtk_text_buffer_remove_tag (ctx->buffer, tag, ctx->start, ctx->end);
}

/* Removes every tag whose name starts with prefix from [start, end). */
static void
remove_tags_with_prefix (GnomeStickyNotesRichText *self,
                         const GtkTextIter *start,
                         const GtkTextIter *end,
                         const char *prefix)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table (self->buffer);
  RemoveCtx ctx = { self->buffer, start, end, prefix };

  gtk_text_tag_table_foreach (table, remove_prefixed_cb, &ctx);
}

/* Reads the character attributes that apply to the character at iter. */
static void
read_attrs (GnomeStickyNotesRichText *self,
            const GtkTextIter *iter,
            AttrSet *out)
{
  GSList *tags = gtk_text_iter_get_tags (iter);

  attr_set_clear (out);

  for (GSList *l = tags; l != NULL; l = l->next)
    {
      GtkTextTag *tag = l->data;
      g_autofree char *name = NULL;

      if (tag == self->tag_bold)
        {
          out->bold = TRUE;
          continue;
        }
      if (tag == self->tag_italic)
        {
          out->italic = TRUE;
          continue;
        }
      if (tag == self->tag_underline)
        {
          out->underline = TRUE;
          continue;
        }
      if (tag == self->tag_strike)
        {
          out->strike = TRUE;
          continue;
        }

      g_object_get (tag, "name", &name, NULL);
      if (name == NULL)
        continue;

      if (g_str_has_prefix (name, FAMILY_PREFIX))
        {
          g_free (out->family);
          out->family = g_strdup (name + strlen (FAMILY_PREFIX));
        }
      else if (g_str_has_prefix (name, SIZE_PREFIX))
        {
          out->size = g_ascii_strtod (name + strlen (SIZE_PREFIX), NULL);
          out->has_size = TRUE;
        }
      else if (g_str_has_prefix (name, COLOR_PREFIX))
        {
          if (gdk_rgba_parse (&out->color, name + strlen (COLOR_PREFIX)))
            out->has_color = TRUE;
        }
    }

  g_slist_free (tags);
}

/* Reads the paragraph alignment at the start of the line containing iter. */
static GtkJustification
read_align (GnomeStickyNotesRichText *self,
            const GtkTextIter *iter)
{
  GtkTextIter ls = *iter;
  GtkJustification just = GTK_JUSTIFY_LEFT;
  GSList *tags;

  gtk_text_iter_set_line_offset (&ls, 0);
  tags = gtk_text_iter_get_tags (&ls);

  for (GSList *l = tags; l != NULL; l = l->next)
    {
      g_autofree char *name = NULL;

      g_object_get (l->data, "name", &name, NULL);
      if (name != NULL && g_str_has_prefix (name, ALIGN_PREFIX))
        just = align_from_string (name + strlen (ALIGN_PREFIX));
    }

  g_slist_free (tags);
  return just;
}

/* --- active-state tracking ----------------------------------------------- */

/* Character offset of the insertion point. */
static int
insert_offset (GnomeStickyNotesRichText *self)
{
  GtkTextIter it;

  gtk_text_buffer_get_iter_at_mark (self->buffer, &it,
                                    gtk_text_buffer_get_insert (self->buffer));
  return gtk_text_iter_get_offset (&it);
}

/* Arms the current active formatting at the collapsed cursor so it survives a
 * trip through the toolbar (and a click back into the text at the same spot)
 * until the next typed character consumes it or the cursor moves elsewhere. */
static void
arm_pending (GnomeStickyNotesRichText *self)
{
  self->pending = TRUE;
  self->pending_offset = insert_offset (self);
}

/* Recomputes the active formatting from the cursor or selection and notifies
 * listeners. Skipped while we are mutating the buffer ourselves. */
static void
sync_state (GnomeStickyNotesRichText *self)
{
  GtkTextIter start, end, ref;
  AttrSet a = { 0 };

  if (self->syncing)
    return;
  self->syncing = TRUE;

  if (gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end))
    {
      /* Display the attributes of the first selected character. */
      self->pending = FALSE;
      read_attrs (self, &start, &a);
      ref = start;
    }
  else
    {
      gtk_text_buffer_get_iter_at_mark (self->buffer, &start,
                                        gtk_text_buffer_get_insert (self->buffer));
      ref = start;

      /* A format armed with a collapsed cursor stays active as long as
       * the cursor has not moved away from where it was set. Keep it
       * untouched so picking a font/colour in the toolbar and clicking
       * back at the same spot does not wipe the choice. */
      if (self->pending &&
          gtk_text_iter_get_offset (&ref) == self->pending_offset)
        {
          self->syncing = FALSE;
          g_signal_emit (self, signals[STATE_CHANGED], 0);
          return;
        }

      self->pending = FALSE;

      /* Prefer the character to the left (what you just typed), except
       * at the very start of a line where we look right instead. */
      if (!gtk_text_iter_is_start (&ref) && !gtk_text_iter_starts_line (&ref))
        gtk_text_iter_backward_char (&ref);

      read_attrs (self, &ref, &a);
    }

  attr_set_clear (&self->active);
  self->active.bold = a.bold;
  self->active.italic = a.italic;
  self->active.underline = a.underline;
  self->active.strike = a.strike;
  self->active.family = g_steal_pointer (&a.family);
  self->active.has_size = a.has_size;
  self->active.size = a.size;
  self->active.has_color = a.has_color;
  self->active.color = a.color;
  self->active_align = read_align (self, &ref);

  attr_set_clear (&a);

  self->syncing = FALSE;
  g_signal_emit (self, signals[STATE_CHANGED], 0);
}

/* Normalises [start, end) to exactly the active character formatting. Used
 * after an insertion so newly typed text matches the active state regardless
 * of what it inherited from its neighbours. */
static void
apply_active_to_range (GnomeStickyNotesRichText *self,
                       const GtkTextIter *start,
                       const GtkTextIter *end)
{
  self->applying = TRUE;

  gtk_text_buffer_remove_tag (self->buffer, self->tag_bold, start, end);
  gtk_text_buffer_remove_tag (self->buffer, self->tag_italic, start, end);
  gtk_text_buffer_remove_tag (self->buffer, self->tag_underline, start, end);
  gtk_text_buffer_remove_tag (self->buffer, self->tag_strike, start, end);
  remove_tags_with_prefix (self, start, end, FAMILY_PREFIX);
  remove_tags_with_prefix (self, start, end, SIZE_PREFIX);
  remove_tags_with_prefix (self, start, end, COLOR_PREFIX);

  if (self->active.bold)
    gtk_text_buffer_apply_tag (self->buffer, self->tag_bold, start, end);
  if (self->active.italic)
    gtk_text_buffer_apply_tag (self->buffer, self->tag_italic, start, end);
  if (self->active.underline)
    gtk_text_buffer_apply_tag (self->buffer, self->tag_underline, start, end);
  if (self->active.strike)
    gtk_text_buffer_apply_tag (self->buffer, self->tag_strike, start, end);
  if (self->active.family != NULL)
    gtk_text_buffer_apply_tag (self->buffer, family_tag (self, self->active.family), start, end);
  if (self->active.has_size)
    gtk_text_buffer_apply_tag (self->buffer, size_tag (self, self->active.size), start, end);
  if (self->active.has_color)
    gtk_text_buffer_apply_tag (self->buffer, color_tag (self, &self->active.color), start, end);

  self->applying = FALSE;
}

/* --- buffer signal handlers ---------------------------------------------- */

static void
on_insert_before (GtkTextBuffer *buffer,
                  GtkTextIter *location,
                  char *text,
                  int len,
                  gpointer user_data)
{
  GnomeStickyNotesRichText *self = user_data;

  /* Mark the insertion so the mark-set fired by the default handler does
   * not overwrite the active state before we have applied it. */
  self->inserting = TRUE;
}

static void
on_insert_after (GtkTextBuffer *buffer,
                 GtkTextIter *location,
                 char *text,
                 int len,
                 gpointer user_data)
{
  GnomeStickyNotesRichText *self = user_data;
  GtkTextIter start = *location;
  long nchars = g_utf8_strlen (text, len);

  gtk_text_iter_backward_chars (&start, nchars);
  apply_active_to_range (self, &start, location);

  self->inserting = FALSE;
  sync_state (self);
}

static void
on_mark_set (GtkTextBuffer *buffer,
             GtkTextIter *location,
             GtkTextMark *mark,
             gpointer user_data)
{
  GnomeStickyNotesRichText *self = user_data;

  if (self->inserting || self->applying || self->syncing)
    return;

  if (mark != gtk_text_buffer_get_insert (buffer) &&
      mark != gtk_text_buffer_get_selection_bound (buffer))
    return;

  sync_state (self);
}

/* --- ToDo items ---------------------------------------------------------- */

/* Mutes our own buffer handlers around a structural edit (anchor insert/delete,
 * a programmatic newline) so it is not treated as interactive typing. Safe to
 * nest: deserialize already blocks, and these just bump the block count. */
static void
block_buffer_signals (GnomeStickyNotesRichText *self)
{
  g_signal_handlers_block_by_func (self->buffer, on_insert_before, self);
  g_signal_handlers_block_by_func (self->buffer, on_insert_after, self);
  g_signal_handlers_block_by_func (self->buffer, on_mark_set, self);
}

static void
unblock_buffer_signals (GnomeStickyNotesRichText *self)
{
  g_signal_handlers_unblock_by_func (self->buffer, on_insert_before, self);
  g_signal_handlers_unblock_by_func (self->buffer, on_insert_after, self);
  g_signal_handlers_unblock_by_func (self->buffer, on_mark_set, self);
}

/* Applies (or clears) the faded/struck "done" look over a todo's text, i.e.
 * the line following its checkbox anchor. */
static void
apply_done (GnomeStickyNotesRichText *self,
            GtkTextChildAnchor *anchor,
            gboolean done)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_iter_at_child_anchor (self->buffer, &start, anchor);
  gtk_text_iter_forward_char (&start); /* step over the checkbox itself */
  end = start;
  if (!gtk_text_iter_ends_line (&end))
    gtk_text_iter_forward_to_line_end (&end);

  if (done)
    {
      GtkTextTagTable *table = gtk_text_buffer_get_tag_table (self->buffer);

      /* Keep it above any user colour/strike so completed text always
       * reads as faded. */
      gtk_text_tag_set_priority (self->tag_done,
                                 gtk_text_tag_table_get_size (table) - 1);
      gtk_text_buffer_apply_tag (self->buffer, self->tag_done, &start, &end);
    }
  else
    gtk_text_buffer_remove_tag (self->buffer, self->tag_done, &start, &end);
}

static void
on_checkbox_toggled (GtkCheckButton *button,
                     GnomeStickyNotesRichText *self)
{
  GtkTextChildAnchor *anchor = g_object_get_data (G_OBJECT (button), "gsn-anchor");

  if (anchor == NULL || gtk_text_child_anchor_get_deleted (anchor))
    return;

  apply_done (self, anchor, gtk_check_button_get_active (button));
}

/* Strips the theme's chunky min-size/padding off the embedded checkbox so it
 * lines up with the text instead of inflating the line height. Installed once
 * for the whole display. */
static void
ensure_todo_css (GtkWidget *widget)
{
  static gsize once = 0;

  if (g_once_init_enter (&once))
    {
      GtkCssProvider *provider = gtk_css_provider_new ();

      /* Zero the theme's chunky button chrome (padding/border) so the
       * indicator is no taller than the surrounding text line. If it
       * exceeds the line height the GtkTextView grows the line and the
       * widget's valign:center floats it above where the text sits; kept
       * at ~1em it centres cleanly on the text instead. */
      gtk_css_provider_load_from_string (provider,
                                         "checkbutton.todo-check { min-height: 0; padding: 0; margin: 0; }"
                                         "checkbutton.todo-check > check { min-height: 0.7em; min-width: 0.7em;"
                                         " padding: 0; margin: 0; border-radius: 0.2em; cursor: pointer; }");
      gtk_style_context_add_provider_for_display (gtk_widget_get_display (widget),
                                                  GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref (provider);

      g_once_init_leave (&once, 1);
    }
}

/* Embeds a fresh checkbox at the given anchor and wires its toggle. */
static GtkWidget *
make_todo_checkbox (GnomeStickyNotesRichText *self,
                    GtkTextChildAnchor *anchor)
{
  GtkWidget *check = gtk_check_button_new ();

  ensure_todo_css (GTK_WIDGET (self->view));

  gtk_widget_set_focusable (check, FALSE); /* clickable, but never a tab stop */
  gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (check, 4);
  gtk_widget_add_css_class (check, "todo-check");
  g_object_set_data (G_OBJECT (check), "gsn-anchor", anchor);
  g_signal_connect (check, "toggled", G_CALLBACK (on_checkbox_toggled), self);

  gtk_text_view_add_child_at_anchor (self->view, check, anchor);
  return check;
}

/* If the line containing iter begins with one of our todo checkboxes, returns
 * that anchor; otherwise NULL. */
static GtkTextChildAnchor *
line_todo_anchor (const GtkTextIter *iter)
{
  GtkTextIter ls = *iter;
  GtkTextChildAnchor *anchor;
  GtkWidget **widgets;
  guint n = 0;
  gboolean is_todo;

  gtk_text_iter_set_line_offset (&ls, 0);
  anchor = gtk_text_iter_get_child_anchor (&ls);
  if (anchor == NULL)
    return NULL;

  widgets = gtk_text_child_anchor_get_widgets (anchor, &n);
  is_todo = (n > 0 && GTK_IS_CHECK_BUTTON (widgets[0]));
  g_free (widgets);

  return is_todo ? anchor : NULL;
}

/* Inserts a checkbox at *iter (advanced past it on return). */
static void
insert_todo_anchor_at (GnomeStickyNotesRichText *self,
                       GtkTextIter *iter,
                       gboolean checked)
{
  GtkTextChildAnchor *anchor;
  GtkWidget *check;

  block_buffer_signals (self);
  anchor = gtk_text_buffer_create_child_anchor (self->buffer, iter);
  check = make_todo_checkbox (self, anchor);
  if (checked)
    gtk_check_button_set_active (GTK_CHECK_BUTTON (check), TRUE);
  unblock_buffer_signals (self);
}

/* Converts a todo line back to plain text by removing its checkbox and any
 * leftover "done" styling. */
static void
remove_todo (GnomeStickyNotesRichText *self,
             GtkTextChildAnchor *anchor)
{
  GtkTextIter start, end, le;

  gtk_text_buffer_get_iter_at_child_anchor (self->buffer, &start, anchor);
  end = start;
  gtk_text_iter_forward_char (&end);

  le = start;
  if (!gtk_text_iter_ends_line (&le))
    gtk_text_iter_forward_to_line_end (&le);
  gtk_text_buffer_remove_tag (self->buffer, self->tag_done, &start, &le);

  block_buffer_signals (self);
  gtk_text_buffer_delete (self->buffer, &start, &end);
  unblock_buffer_signals (self);

  sync_state (self);
}

/* Enter/Backspace handling that gives todos their Notion-like flow. */
static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint keyval,
                guint keycode,
                GdkModifierType state,
                GnomeStickyNotesRichText *self)
{
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
  GtkTextIter cursor;
  GtkTextChildAnchor *anchor;

  if (mods != 0)
    return GDK_EVENT_PROPAGATE;

  gtk_text_buffer_get_iter_at_mark (self->buffer, &cursor,
                                    gtk_text_buffer_get_insert (self->buffer));
  anchor = line_todo_anchor (&cursor);
  if (anchor == NULL)
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    {
      GtkTextIter ls = cursor, le;

      gtk_text_iter_set_line_offset (&ls, 0);
      le = ls;
      if (!gtk_text_iter_ends_line (&le))
        gtk_text_iter_forward_to_line_end (&le);

      /* Empty todo (just the checkbox): leave the list. */
      if (gtk_text_iter_get_offset (&le) - gtk_text_iter_get_offset (&ls) <= 1)
        {
          remove_todo (self, anchor);
          return GDK_EVENT_STOP;
        }

      /* Otherwise split the line and start a new todo below. */
      gtk_text_buffer_begin_user_action (self->buffer);
      gtk_text_buffer_insert (self->buffer, &cursor, "\n", 1);
      insert_todo_anchor_at (self, &cursor, FALSE);
      gtk_text_buffer_place_cursor (self->buffer, &cursor);
      gtk_text_buffer_end_user_action (self->buffer);
      sync_state (self);
      return GDK_EVENT_STOP;
    }

  if (keyval == GDK_KEY_BackSpace &&
      !gtk_text_buffer_get_has_selection (self->buffer) &&
      gtk_text_iter_get_line_offset (&cursor) == 1)
    {
      remove_todo (self, anchor);
      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

void
gnome_sticky_notes_rich_text_insert_todo (GnomeStickyNotesRichText *self)
{
  GtkTextIter cursor, ls;
  GtkTextChildAnchor *anchor;

  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  gtk_text_buffer_get_iter_at_mark (self->buffer, &cursor,
                                    gtk_text_buffer_get_insert (self->buffer));

  /* Toggle: a second press on a todo line turns it back into plain text. */
  anchor = line_todo_anchor (&cursor);
  if (anchor != NULL)
    {
      remove_todo (self, anchor);
      gtk_widget_grab_focus (GTK_WIDGET (self->view));
      return;
    }

  ls = cursor;
  gtk_text_iter_set_line_offset (&ls, 0);

  gtk_text_buffer_begin_user_action (self->buffer);
  insert_todo_anchor_at (self, &ls, FALSE);
  gtk_text_buffer_place_cursor (self->buffer, &ls); /* just after the checkbox */
  gtk_text_buffer_end_user_action (self->buffer);

  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  sync_state (self);
}

/* --- public formatting commands ------------------------------------------ */

static gboolean
range_has_tag_everywhere (GtkTextIter *start,
                          GtkTextIter *end,
                          GtkTextTag *tag)
{
  GtkTextIter it = *start;

  while (gtk_text_iter_compare (&it, end) < 0)
    {
      if (!gtk_text_iter_has_tag (&it, tag))
        return FALSE;
      if (!gtk_text_iter_forward_char (&it))
        break;
    }

  return TRUE;
}

void
gnome_sticky_notes_rich_text_toggle (GnomeStickyNotesRichText *self,
                                     GnomeStickyNotesRichTextToggle which)
{
  GtkTextTag *tag;
  GtkTextIter start, end;

  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  tag = toggle_tag (self, which);

  if (gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end))
    {
      if (range_has_tag_everywhere (&start, &end, tag))
        gtk_text_buffer_remove_tag (self->buffer, tag, &start, &end);
      else
        gtk_text_buffer_apply_tag (self->buffer, tag, &start, &end);
      sync_state (self);
    }
  else
    {
      /* No selection: just flip the active state for the next text. */
      switch (which)
        {
        case GNOME_STICKY_NOTES_RICH_TEXT_BOLD:
          self->active.bold = !self->active.bold;
          break;
        case GNOME_STICKY_NOTES_RICH_TEXT_ITALIC:
          self->active.italic = !self->active.italic;
          break;
        case GNOME_STICKY_NOTES_RICH_TEXT_UNDERLINE:
          self->active.underline = !self->active.underline;
          break;
        case GNOME_STICKY_NOTES_RICH_TEXT_STRIKETHROUGH:
          self->active.strike = !self->active.strike;
          break;
        default:
          break;
        }
      arm_pending (self);
      g_signal_emit (self, signals[STATE_CHANGED], 0);
    }
}

void
gnome_sticky_notes_rich_text_set_family (GnomeStickyNotesRichText *self,
                                         const char *family)
{
  GtkTextIter start, end;

  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  if (gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end))
    {
      remove_tags_with_prefix (self, &start, &end, FAMILY_PREFIX);
      if (family != NULL)
        gtk_text_buffer_apply_tag (self->buffer, family_tag (self, family), &start, &end);
      sync_state (self);
    }
  else
    {
      g_free (self->active.family);
      self->active.family = g_strdup (family);
      arm_pending (self);
      g_signal_emit (self, signals[STATE_CHANGED], 0);
    }
}

void
gnome_sticky_notes_rich_text_set_size (GnomeStickyNotesRichText *self,
                                       double points)
{
  GtkTextIter start, end;

  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  if (gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end))
    {
      remove_tags_with_prefix (self, &start, &end, SIZE_PREFIX);
      if (points > 0)
        gtk_text_buffer_apply_tag (self->buffer, size_tag (self, points), &start, &end);
      sync_state (self);
    }
  else
    {
      self->active.has_size = (points > 0);
      self->active.size = points;
      arm_pending (self);
      g_signal_emit (self, signals[STATE_CHANGED], 0);
    }
}

void
gnome_sticky_notes_rich_text_set_color (GnomeStickyNotesRichText *self,
                                        const GdkRGBA *rgba)
{
  GtkTextIter start, end;

  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  if (gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end))
    {
      remove_tags_with_prefix (self, &start, &end, COLOR_PREFIX);
      if (rgba != NULL)
        gtk_text_buffer_apply_tag (self->buffer, color_tag (self, rgba), &start, &end);
      sync_state (self);
    }
  else
    {
      self->active.has_color = (rgba != NULL);
      if (rgba != NULL)
        self->active.color = *rgba;
      arm_pending (self);
      g_signal_emit (self, signals[STATE_CHANGED], 0);
    }
}

void
gnome_sticky_notes_rich_text_set_alignment (GnomeStickyNotesRichText *self,
                                            GtkJustification justification)
{
  GtkTextIter start, end;

  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end);

  /* Extend to whole paragraphs: start of the first line through the start
   * of the line after the last, so the trailing newline is covered and new
   * text on that line keeps the alignment. */
  gtk_text_iter_set_line_offset (&start, 0);
  if (!gtk_text_iter_ends_line (&end))
    gtk_text_iter_forward_to_line_end (&end);
  gtk_text_iter_forward_char (&end); /* include the newline if present */

  remove_tags_with_prefix (self, &start, &end, ALIGN_PREFIX);
  if (justification != GTK_JUSTIFY_LEFT) /* left is the view default */
    gtk_text_buffer_apply_tag (self->buffer, align_tag (self, justification), &start, &end);

  sync_state (self);
}

/* --- state getters ------------------------------------------------------- */

gboolean
gnome_sticky_notes_rich_text_get_toggle (GnomeStickyNotesRichText *self,
                                         GnomeStickyNotesRichTextToggle which)
{
  g_return_val_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self), FALSE);

  switch (which)
    {
    case GNOME_STICKY_NOTES_RICH_TEXT_BOLD:
      return self->active.bold;
    case GNOME_STICKY_NOTES_RICH_TEXT_ITALIC:
      return self->active.italic;
    case GNOME_STICKY_NOTES_RICH_TEXT_UNDERLINE:
      return self->active.underline;
    case GNOME_STICKY_NOTES_RICH_TEXT_STRIKETHROUGH:
      return self->active.strike;
    default:
      return FALSE;
    }
}

const char *
gnome_sticky_notes_rich_text_get_family (GnomeStickyNotesRichText *self)
{
  g_return_val_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self), NULL);

  return self->active.family;
}

double
gnome_sticky_notes_rich_text_get_size (GnomeStickyNotesRichText *self)
{
  g_return_val_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self), 0);

  return self->active.has_size ? self->active.size : 0;
}

gboolean
gnome_sticky_notes_rich_text_get_color (GnomeStickyNotesRichText *self,
                                        GdkRGBA *out)
{
  g_return_val_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self), FALSE);

  if (self->active.has_color && out != NULL)
    *out = self->active.color;

  return self->active.has_color;
}

GtkJustification
gnome_sticky_notes_rich_text_get_alignment (GnomeStickyNotesRichText *self)
{
  g_return_val_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self), GTK_JUSTIFY_LEFT);

  return self->active_align;
}

/* --- serialization ------------------------------------------------------- */

/* Builds the a{sv} attribute dictionary for the character at iter. Returns
 * TRUE if any attribute was set. */
static gboolean
build_attr_dict (GnomeStickyNotesRichText *self,
                 const GtkTextIter *iter,
                 GVariantBuilder *builder)
{
  AttrSet a = { 0 };
  gboolean any = FALSE;

  read_attrs (self, iter, &a);

  g_variant_builder_init (builder, G_VARIANT_TYPE ("a{sv}"));

  if (a.bold)
    {
      g_variant_builder_add (builder, "{sv}", "b", g_variant_new_boolean (TRUE));
      any = TRUE;
    }
  if (a.italic)
    {
      g_variant_builder_add (builder, "{sv}", "i", g_variant_new_boolean (TRUE));
      any = TRUE;
    }
  if (a.underline)
    {
      g_variant_builder_add (builder, "{sv}", "u", g_variant_new_boolean (TRUE));
      any = TRUE;
    }
  if (a.strike)
    {
      g_variant_builder_add (builder, "{sv}", "s", g_variant_new_boolean (TRUE));
      any = TRUE;
    }
  if (a.family)
    {
      g_variant_builder_add (builder, "{sv}", "fam", g_variant_new_string (a.family));
      any = TRUE;
    }
  if (a.has_size)
    {
      g_variant_builder_add (builder, "{sv}", "sz", g_variant_new_double (a.size));
      any = TRUE;
    }
  if (a.has_color)
    {
      g_autofree char *str = gdk_rgba_to_string (&a.color);
      g_variant_builder_add (builder, "{sv}", "fg", g_variant_new_string (str));
      any = TRUE;
    }

  attr_set_clear (&a);
  return any;
}

char *
gnome_sticky_notes_rich_text_serialize (GnomeStickyNotesRichText *self)
{
  GVariantBuilder chars, paras, todos;
  GtkTextIter start, end, it;
  g_autofree char *text = NULL;
  g_autofree char *printed = NULL;
  GVariant *variant;
  int n_lines, i;

  g_return_val_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self), NULL);

  gtk_text_buffer_get_bounds (self->buffer, &start, &end);
  /* get_slice (not get_text) keeps the U+FFFC placeholder for each checkbox
   * anchor, so character/paragraph offsets stay aligned with the buffer. */
  text = gtk_text_buffer_get_slice (self->buffer, &start, &end, TRUE);

  /* Character runs. */
  g_variant_builder_init (&chars, G_VARIANT_TYPE ("a(uua{sv})"));
  it = start;
  while (!gtk_text_iter_is_end (&it))
    {
      GtkTextIter next = it;
      GVariantBuilder dict;

      if (!gtk_text_iter_forward_to_tag_toggle (&next, NULL))
        next = end;

      if (build_attr_dict (self, &it, &dict))
        g_variant_builder_add (&chars, "(uua{sv})",
                               (guint32) gtk_text_iter_get_offset (&it),
                               (guint32) gtk_text_iter_get_offset (&next),
                               &dict);
      else
        g_variant_builder_clear (&dict);

      it = next;
    }

  /* Paragraph alignment runs. */
  g_variant_builder_init (&paras, G_VARIANT_TYPE ("a(uus)"));
  n_lines = gtk_text_buffer_get_line_count (self->buffer);
  for (i = 0; i < n_lines; i++)
    {
      GtkTextIter ls, le;
      GtkJustification just;

      gtk_text_buffer_get_iter_at_line (self->buffer, &ls, i);
      le = ls;
      if (!gtk_text_iter_forward_line (&le))
        gtk_text_buffer_get_end_iter (self->buffer, &le);

      just = read_align (self, &ls);
      if (just != GTK_JUSTIFY_LEFT)
        g_variant_builder_add (&paras, "(uus)",
                               (guint32) gtk_text_iter_get_offset (&ls),
                               (guint32) gtk_text_iter_get_offset (&le),
                               align_to_string (just));
    }

  /* ToDo runs: one (offset, checked) per line that starts with a checkbox. */
  g_variant_builder_init (&todos, G_VARIANT_TYPE ("a(ub)"));
  for (i = 0; i < n_lines; i++)
    {
      GtkTextIter ls;
      GtkTextChildAnchor *anchor;

      gtk_text_buffer_get_iter_at_line (self->buffer, &ls, i);
      anchor = line_todo_anchor (&ls);
      if (anchor != NULL)
        {
          GtkWidget **widgets;
          guint nw = 0;
          gboolean checked;

          widgets = gtk_text_child_anchor_get_widgets (anchor, &nw);
          checked = (nw > 0 && gtk_check_button_get_active (GTK_CHECK_BUTTON (widgets[0])));
          g_free (widgets);

          g_variant_builder_add (&todos, "(ub)",
                                 (guint32) gtk_text_iter_get_offset (&ls),
                                 checked);
        }
    }

  variant = g_variant_new ("(sa(uua{sv})a(uus)a(ub))", text, &chars, &paras, &todos);
  printed = g_variant_print (variant, TRUE);
  g_variant_unref (g_variant_ref_sink (variant));

  return g_strconcat (RICH_TEXT_MARKER, printed, NULL);
}

static void
deserialize_rich (GnomeStickyNotesRichText *self,
                  const char *payload,
                  gboolean v1)
{
  g_autoptr (GVariant) variant = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) chars = NULL;
  g_autoptr (GVariant) paras = NULL;
  g_autoptr (GVariant) todos = NULL;
  const char *text = NULL;
  GVariantIter iter;
  guint32 cs, ce;
  GVariant *dict;

  variant = g_variant_parse (G_VARIANT_TYPE (v1 ? RICH_TEXT_VARIANT_TYPE_V1
                                                : RICH_TEXT_VARIANT_TYPE),
                             payload, NULL, NULL, &error);
  if (variant == NULL)
    {
      g_warning ("Failed to parse rich text, loading as plain: %s", error->message);
      gtk_text_buffer_set_text (self->buffer, payload, -1);
      return;
    }

  if (v1)
    g_variant_get (variant, "(&s@a(uua{sv})@a(uus))", &text, &chars, &paras);
  else
    g_variant_get (variant, "(&s@a(uua{sv})@a(uus)@a(ub))", &text, &chars, &paras, &todos);

  gtk_text_buffer_set_text (self->buffer, text ? text : "", -1);

  /* Character runs. */
  g_variant_iter_init (&iter, chars);
  while (g_variant_iter_next (&iter, "(uu@a{sv})", &cs, &ce, &dict))
    {
      GtkTextIter s, e;
      GVariantIter di;
      char *key;
      GVariant *val;

      gtk_text_buffer_get_iter_at_offset (self->buffer, &s, cs);
      gtk_text_buffer_get_iter_at_offset (self->buffer, &e, ce);

      g_variant_iter_init (&di, dict);
      while (g_variant_iter_next (&di, "{sv}", &key, &val))
        {
          if (g_strcmp0 (key, "b") == 0)
            gtk_text_buffer_apply_tag (self->buffer, self->tag_bold, &s, &e);
          else if (g_strcmp0 (key, "i") == 0)
            gtk_text_buffer_apply_tag (self->buffer, self->tag_italic, &s, &e);
          else if (g_strcmp0 (key, "u") == 0)
            gtk_text_buffer_apply_tag (self->buffer, self->tag_underline, &s, &e);
          else if (g_strcmp0 (key, "s") == 0)
            gtk_text_buffer_apply_tag (self->buffer, self->tag_strike, &s, &e);
          else if (g_strcmp0 (key, "fam") == 0)
            gtk_text_buffer_apply_tag (self->buffer,
                                       family_tag (self, g_variant_get_string (val, NULL)), &s, &e);
          else if (g_strcmp0 (key, "sz") == 0)
            gtk_text_buffer_apply_tag (self->buffer,
                                       size_tag (self, g_variant_get_double (val)), &s, &e);
          else if (g_strcmp0 (key, "fg") == 0)
            {
              GdkRGBA rgba;
              if (gdk_rgba_parse (&rgba, g_variant_get_string (val, NULL)))
                gtk_text_buffer_apply_tag (self->buffer, color_tag (self, &rgba), &s, &e);
            }

          g_free (key);
          g_variant_unref (val);
        }

      g_variant_unref (dict);
    }

  /* Paragraph alignment runs. */
  g_variant_iter_init (&iter, paras);
  {
    const char *align;
    while (g_variant_iter_next (&iter, "(uu&s)", &cs, &ce, &align))
      {
        GtkTextIter s, e;
        GtkJustification just = align_from_string (align);

        if (just == GTK_JUSTIFY_LEFT)
          continue;

        gtk_text_buffer_get_iter_at_offset (self->buffer, &s, cs);
        gtk_text_buffer_get_iter_at_offset (self->buffer, &e, ce);
        gtk_text_buffer_apply_tag (self->buffer, align_tag (self, just), &s, &e);
      }
  }

  /* ToDo runs: turn each U+FFFC placeholder back into a real checkbox.
   * Deleting one char and inserting an anchor is net-zero, so offsets of
   * later todos (emitted in ascending order) stay valid. */
  if (todos != NULL)
    {
      guint32 off;
      gboolean checked;

      g_variant_iter_init (&iter, todos);
      while (g_variant_iter_next (&iter, "(ub)", &off, &checked))
        {
          GtkTextIter it, it2;

          gtk_text_buffer_get_iter_at_offset (self->buffer, &it, off);
          it2 = it;
          gtk_text_iter_forward_char (&it2);
          gtk_text_buffer_delete (self->buffer, &it, &it2);
          insert_todo_anchor_at (self, &it, checked);
        }
    }
}

void
gnome_sticky_notes_rich_text_deserialize (GnomeStickyNotesRichText *self,
                                          const char *data)
{
  g_return_if_fail (GNOME_STICKY_NOTES_IS_RICH_TEXT (self));

  /* Mute our own buffer handlers so loading does not get treated as user
   * typing (which would re-apply the active formatting). */
  g_signal_handlers_block_by_func (self->buffer, on_insert_before, self);
  g_signal_handlers_block_by_func (self->buffer, on_insert_after, self);
  g_signal_handlers_block_by_func (self->buffer, on_mark_set, self);

  if (data != NULL && g_str_has_prefix (data, RICH_TEXT_MARKER))
    deserialize_rich (self, data + strlen (RICH_TEXT_MARKER), FALSE);
  else if (data != NULL && g_str_has_prefix (data, RICH_TEXT_MARKER_V1))
    deserialize_rich (self, data + strlen (RICH_TEXT_MARKER_V1), TRUE);
  else
    gtk_text_buffer_set_text (self->buffer, data ? data : "", -1);

  g_signal_handlers_unblock_by_func (self->buffer, on_insert_before, self);
  g_signal_handlers_unblock_by_func (self->buffer, on_insert_after, self);
  g_signal_handlers_unblock_by_func (self->buffer, on_mark_set, self);

  sync_state (self);
}

/* --- object lifecycle ---------------------------------------------------- */

static void
gnome_sticky_notes_rich_text_dispose (GObject *object)
{
  GnomeStickyNotesRichText *self = GNOME_STICKY_NOTES_RICH_TEXT (object);

  if (self->buffer != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->buffer, self);
      self->buffer = NULL;
    }
  g_clear_object (&self->view);
  attr_set_clear (&self->active);

  G_OBJECT_CLASS (gnome_sticky_notes_rich_text_parent_class)->dispose (object);
}

static void
gnome_sticky_notes_rich_text_class_init (GnomeStickyNotesRichTextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnome_sticky_notes_rich_text_dispose;

  signals[STATE_CHANGED] =
      g_signal_new ("state-changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

static void
gnome_sticky_notes_rich_text_init (GnomeStickyNotesRichText *self)
{
  self->active_align = GTK_JUSTIFY_LEFT;
}

GnomeStickyNotesRichText *
gnome_sticky_notes_rich_text_new (GtkTextView *view)
{
  GnomeStickyNotesRichText *self;

  g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), NULL);

  self = g_object_new (GNOME_STICKY_NOTES_TYPE_RICH_TEXT, NULL);
  self->view = g_object_ref (view);
  self->buffer = gtk_text_view_get_buffer (view);

  self->tag_bold = gtk_text_buffer_create_tag (self->buffer, "b", "weight", PANGO_WEIGHT_BOLD, NULL);
  self->tag_italic = gtk_text_buffer_create_tag (self->buffer, "i", "style", PANGO_STYLE_ITALIC, NULL);
  self->tag_underline = gtk_text_buffer_create_tag (self->buffer, "u", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  self->tag_strike = gtk_text_buffer_create_tag (self->buffer, "s", "strikethrough", TRUE, NULL);

  {
    /* Completed-todo styling: struck through and faded to 50% of the
     * view's text colour. Priority is re-asserted on each apply. */
    GdkRGBA c;

    gtk_widget_get_color (GTK_WIDGET (view), &c);
    c.alpha = 0.5;
    self->tag_done = gtk_text_buffer_create_tag (self->buffer, "done",
                                                 "strikethrough", TRUE,
                                                 "foreground-rgba", &c,
                                                 NULL);
  }

  g_signal_connect (self->buffer, "insert-text",
                    G_CALLBACK (on_insert_before), self);
  g_signal_connect_after (self->buffer, "insert-text",
                          G_CALLBACK (on_insert_after), self);
  g_signal_connect (self->buffer, "mark-set",
                    G_CALLBACK (on_mark_set), self);

  {
    /* Capture Enter/Backspace before the view so todos behave like a list. */
    GtkEventController *key = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (key, GTK_PHASE_CAPTURE);
    g_signal_connect (key, "key-pressed", G_CALLBACK (on_key_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (view), key);
  }

  return self;
}

