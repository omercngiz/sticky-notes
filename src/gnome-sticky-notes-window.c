/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gnome-sticky-notes-window.h"
#include "gnome-sticky-notes-richtext.h"

/* Debounce delay (ms) for autosaving note text while typing. */
#define AUTOSAVE_DELAY_MS 800

struct _GnomeStickyNotesWindow
{
	AdwApplicationWindow      parent_instance;

	GnomeStickyNotesDatabase *database;   /* owned */
	GnomeStickyNotesRichText *rich_text;  /* owned, formatting engine */
	gint64                    note_id;
	char                     *color;      /* pass-through until theming lands */
	char                     *monitor;    /* pass-through until positioning lands */
	guint                     save_timeout_id;
	int                       cur_width;   /* latest real allocation */
	int                       cur_height;

	/* Defaults used when the cursor sits on unformatted text. */
	char                     *default_family;
	double                    default_size;
	GdkRGBA                   default_color;

	guint                     ui_sync : 1; /* set while pushing engine state into the toolbar */

	/* Template widgets */
	GtkTextView              *text_view;
	GtkMenuButton            *format_button;
	GtkPopover               *format_popover;
	GtkToggleButton          *bold_button;
	GtkToggleButton          *italic_button;
	GtkToggleButton          *underline_button;
	GtkToggleButton          *strike_button;
	GtkFontDialogButton      *font_button;
	GtkSpinButton            *size_spin;
	GtkToggleButton          *align_left_button;
	GtkToggleButton          *align_center_button;
	GtkToggleButton          *align_right_button;
	GtkToggleButton          *align_fill_button;
	GtkColorDialogButton     *color_button;
};

G_DEFINE_FINAL_TYPE (GnomeStickyNotesWindow, gnome_sticky_notes_window, ADW_TYPE_APPLICATION_WINDOW)

/* Reads the live note state into a stack record (no allocation of the record
 * itself; caller frees the returned content string). Position is left unset
 * (-1) because GTK4 exposes no client window position on Wayland. */
static void
gnome_sticky_notes_window_collect (GnomeStickyNotesWindow   *self,
                                   GnomeStickyNotesNoteData *out,
                                   char                    **out_content)
{
	int width = self->cur_width;
	int height = self->cur_height;

	/* Persist the full rich-text document, not just the plain characters. */
	*out_content = gnome_sticky_notes_rich_text_serialize (self->rich_text);

	/* Fall back to the live allocation if size_allocate hasn't run yet. */
	if (width <= 0)
		width = gtk_widget_get_width (GTK_WIDGET (self));
	if (height <= 0)
		height = gtk_widget_get_height (GTK_WIDGET (self));

	out->id      = self->note_id;
	out->content = *out_content;
	out->color   = self->color;
	out->pos_x   = -1;
	out->pos_y   = -1;
	out->width   = width;
	out->height  = height;
	out->monitor = self->monitor;
}

static void
gnome_sticky_notes_window_save_now (GnomeStickyNotesWindow *self)
{
	GnomeStickyNotesNoteData data = { 0 };
	g_autofree char *content = NULL;
	g_autoptr(GError) error = NULL;

	if (self->database == NULL || self->note_id <= 0)
		return;

	gnome_sticky_notes_window_collect (self, &data, &content);

	if (!gnome_sticky_notes_database_save_note (self->database, &data, &error))
		g_warning ("Failed to save note %" G_GINT64_FORMAT ": %s",
		           self->note_id, error->message);
}

static gboolean
autosave_timeout_cb (gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;

	self->save_timeout_id = 0;
	gnome_sticky_notes_window_save_now (self);

	return G_SOURCE_REMOVE;
}

/* Coalesce rapid edits/resizes into a single delayed save. */
static void
gnome_sticky_notes_window_schedule_save (GnomeStickyNotesWindow *self)
{
	g_clear_handle_id (&self->save_timeout_id, g_source_remove);
	self->save_timeout_id = g_timeout_add (AUTOSAVE_DELAY_MS,
	                                        autosave_timeout_cb,
	                                        self);
}

static void
on_buffer_changed (GtkTextBuffer          *buffer,
                   GnomeStickyNotesWindow *self)
{
	gnome_sticky_notes_window_schedule_save (self);
}

/* Formatting changes the tags but not the text, so "changed" never fires for
 * them; persist on tag edits too. */
static void
on_tag_changed (GtkTextBuffer          *buffer,
                GtkTextTag             *tag,
                GtkTextIter            *start,
                GtkTextIter            *end,
                GnomeStickyNotesWindow *self)
{
	gnome_sticky_notes_window_schedule_save (self);
}

/* --- toolbar <-> engine sync --------------------------------------------- */

/* Pushes the engine's current formatting into the toolbar controls. Guarded
 * so the resulting widget signals do not loop back into the engine. */
static void
on_state_changed (GnomeStickyNotesRichText *rt,
                  GnomeStickyNotesWindow   *self)
{
	GtkJustification just;
	PangoFontDescription *desc;
	const char *family;
	double size;
	GdkRGBA color;

	self->ui_sync = TRUE;

	gtk_toggle_button_set_active (self->bold_button,
		gnome_sticky_notes_rich_text_get_toggle (rt, GNOME_STICKY_NOTES_RICH_TEXT_BOLD));
	gtk_toggle_button_set_active (self->italic_button,
		gnome_sticky_notes_rich_text_get_toggle (rt, GNOME_STICKY_NOTES_RICH_TEXT_ITALIC));
	gtk_toggle_button_set_active (self->underline_button,
		gnome_sticky_notes_rich_text_get_toggle (rt, GNOME_STICKY_NOTES_RICH_TEXT_UNDERLINE));
	gtk_toggle_button_set_active (self->strike_button,
		gnome_sticky_notes_rich_text_get_toggle (rt, GNOME_STICKY_NOTES_RICH_TEXT_STRIKETHROUGH));

	family = gnome_sticky_notes_rich_text_get_family (rt);
	desc = pango_font_description_new ();
	pango_font_description_set_family (desc, family ? family : self->default_family);
	gtk_font_dialog_button_set_font_desc (self->font_button, desc);
	pango_font_description_free (desc);

	size = gnome_sticky_notes_rich_text_get_size (rt);
	gtk_spin_button_set_value (self->size_spin, size > 0 ? size : self->default_size);

	just = gnome_sticky_notes_rich_text_get_alignment (rt);
	switch (just)
		{
		case GTK_JUSTIFY_CENTER: gtk_toggle_button_set_active (self->align_center_button, TRUE); break;
		case GTK_JUSTIFY_RIGHT:  gtk_toggle_button_set_active (self->align_right_button, TRUE);  break;
		case GTK_JUSTIFY_FILL:   gtk_toggle_button_set_active (self->align_fill_button, TRUE);   break;
		case GTK_JUSTIFY_LEFT:
		default:                 gtk_toggle_button_set_active (self->align_left_button, TRUE);   break;
		}

	if (!gnome_sticky_notes_rich_text_get_color (rt, &color))
		color = self->default_color;
	gtk_color_dialog_button_set_rgba (self->color_button, &color);

	self->ui_sync = FALSE;
}

static void
on_bold_toggled (GtkToggleButton *button, GnomeStickyNotesWindow *self)
{
	if (self->ui_sync) return;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_BOLD);
}

static void
on_italic_toggled (GtkToggleButton *button, GnomeStickyNotesWindow *self)
{
	if (self->ui_sync) return;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_ITALIC);
}

static void
on_underline_toggled (GtkToggleButton *button, GnomeStickyNotesWindow *self)
{
	if (self->ui_sync) return;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_UNDERLINE);
}

static void
on_strike_toggled (GtkToggleButton *button, GnomeStickyNotesWindow *self)
{
	if (self->ui_sync) return;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_STRIKETHROUGH);
}

static void
on_font_changed (GtkFontDialogButton *button, GParamSpec *pspec, GnomeStickyNotesWindow *self)
{
	const PangoFontDescription *desc;
	const char *family;

	if (self->ui_sync) return;

	desc = gtk_font_dialog_button_get_font_desc (button);
	if (desc == NULL)
		return;

	family = pango_font_description_get_family (desc);
	if (family != NULL)
		gnome_sticky_notes_rich_text_set_family (self->rich_text, family);
}

static void
on_size_changed (GtkSpinButton *spin, GnomeStickyNotesWindow *self)
{
	if (self->ui_sync) return;
	gnome_sticky_notes_rich_text_set_size (self->rich_text, gtk_spin_button_get_value (spin));
}

static void
on_color_changed (GtkColorDialogButton *button, GParamSpec *pspec, GnomeStickyNotesWindow *self)
{
	if (self->ui_sync) return;
	gnome_sticky_notes_rich_text_set_color (self->rich_text,
	                                        gtk_color_dialog_button_get_rgba (button));
}

static void
on_align_toggled (GtkToggleButton *button, GnomeStickyNotesWindow *self)
{
	GtkJustification just;

	if (self->ui_sync || !gtk_toggle_button_get_active (button))
		return;

	if (button == self->align_center_button)     just = GTK_JUSTIFY_CENTER;
	else if (button == self->align_right_button) just = GTK_JUSTIFY_RIGHT;
	else if (button == self->align_fill_button)  just = GTK_JUSTIFY_FILL;
	else                                         just = GTK_JUSTIFY_LEFT;

	gnome_sticky_notes_rich_text_set_alignment (self->rich_text, just);
}

/* The format popover stays open (autohide is off) so its nested font/colour
 * dialogs work in a single click. Dismiss it the moment the user clicks back
 * into the text view, so they don't have to re-press the toolbar icon. */
static void
on_text_view_focus_enter (GtkEventControllerFocus *controller,
                          GnomeStickyNotesWindow  *self)
{
	if (gtk_widget_get_visible (GTK_WIDGET (self->format_popover)))
		gtk_popover_popdown (self->format_popover);
}

/* Track the real toplevel size as the user resizes, and persist it. Reading
 * the allocation here is reliable, unlike gtk_window_get_default_size(). */
static void
gnome_sticky_notes_window_size_allocate (GtkWidget *widget,
                                         int        width,
                                         int        height,
                                         int        baseline)
{
	GnomeStickyNotesWindow *self = GNOME_STICKY_NOTES_WINDOW (widget);

	GTK_WIDGET_CLASS (gnome_sticky_notes_window_parent_class)->size_allocate (widget, width, height, baseline);

	if (width <= 0 || height <= 0)
		return;

	if (width != self->cur_width || height != self->cur_height)
		{
			self->cur_width = width;
			self->cur_height = height;
			gnome_sticky_notes_window_schedule_save (self);
		}
}

static gboolean
on_close_request (GtkWindow *window,
                  gpointer   user_data)
{
	GnomeStickyNotesWindow *self = GNOME_STICKY_NOTES_WINDOW (window);

	/* Flush any pending edit and persist final geometry before the window
	 * goes away. The note is kept in the database so it reopens next launch. */
	g_clear_handle_id (&self->save_timeout_id, g_source_remove);
	gnome_sticky_notes_window_save_now (self);

	return GDK_EVENT_PROPAGATE; /* allow the close to proceed */
}

/* Removes the note from the database and closes the window. */
static void
gnome_sticky_notes_window_do_delete (GnomeStickyNotesWindow *self)
{
	g_autoptr(GError) error = NULL;

	g_clear_handle_id (&self->save_timeout_id, g_source_remove);

	if (self->database != NULL && self->note_id > 0)
		{
			if (!gnome_sticky_notes_database_delete_note (self->database, self->note_id, &error))
				g_warning ("Failed to delete note %" G_GINT64_FORMAT ": %s",
				           self->note_id, error->message);
		}

	/* Avoid the close handler re-saving a row we just deleted. */
	self->note_id = -1;
	gtk_window_destroy (GTK_WINDOW (self));
}

static void
on_delete_confirmed (AdwAlertDialog *dialog,
                     GAsyncResult   *result,
                     gpointer        user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	const char *response = adw_alert_dialog_choose_finish (dialog, result);

	if (g_strcmp0 (response, "delete") == 0)
		gnome_sticky_notes_window_do_delete (self);
}

static void
delete_action (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	AdwAlertDialog *dialog;

	dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Delete Note?"),
	                                                 _("This note will be permanently deleted.")));
	adw_alert_dialog_add_responses (dialog,
	                                "cancel", _("_Cancel"),
	                                "delete", _("_Delete"),
	                                NULL);
	adw_alert_dialog_set_response_appearance (dialog, "delete", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response (dialog, "cancel");
	adw_alert_dialog_set_close_response (dialog, "cancel");

	adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
	                         (GAsyncReadyCallback) on_delete_confirmed, self);
}

static void
fmt_bold_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_BOLD);
}

static void
fmt_italic_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_ITALIC);
}

static void
fmt_underline_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_toggle (self->rich_text, GNOME_STICKY_NOTES_RICH_TEXT_UNDERLINE);
}

static void
align_left_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_set_alignment (self->rich_text, GTK_JUSTIFY_LEFT);
}

static void
align_center_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_set_alignment (self->rich_text, GTK_JUSTIFY_CENTER);
}

static void
align_right_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_set_alignment (self->rich_text, GTK_JUSTIFY_RIGHT);
}

static void
align_fill_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_set_alignment (self->rich_text, GTK_JUSTIFY_FILL);
}

static void
insert_todo_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GnomeStickyNotesWindow *self = user_data;
	gnome_sticky_notes_rich_text_insert_todo (self->rich_text);
}

static const GActionEntry win_actions[] = {
	{ "delete", delete_action },
	{ "fmt-bold", fmt_bold_action },
	{ "fmt-italic", fmt_italic_action },
	{ "fmt-underline", fmt_underline_action },
	{ "align-left", align_left_action },
	{ "align-center", align_center_action },
	{ "align-right", align_right_action },
	{ "align-fill", align_fill_action },
	{ "insert-todo", insert_todo_action },
};

static void
gnome_sticky_notes_window_dispose (GObject *object)
{
	GnomeStickyNotesWindow *self = GNOME_STICKY_NOTES_WINDOW (object);

	g_clear_handle_id (&self->save_timeout_id, g_source_remove);
	g_clear_object (&self->rich_text);
	g_clear_object (&self->database);
	g_clear_pointer (&self->color, g_free);
	g_clear_pointer (&self->monitor, g_free);
	g_clear_pointer (&self->default_family, g_free);

	G_OBJECT_CLASS (gnome_sticky_notes_window_parent_class)->dispose (object);
}

static void
gnome_sticky_notes_window_class_init (GnomeStickyNotesWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gnome_sticky_notes_window_dispose;
	widget_class->size_allocate = gnome_sticky_notes_window_size_allocate;

	gtk_widget_class_set_template_from_resource (widget_class, "/io/omercngiz/StickyNotes/gnome-sticky-notes-window.ui");
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, text_view);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, format_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, format_popover);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, bold_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, italic_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, underline_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, strike_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, font_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, size_spin);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, align_left_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, align_center_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, align_right_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, align_fill_button);
	gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesWindow, color_button);
}

static void
add_shortcut (GtkShortcutController *controller,
              const char           *trigger,
              const char           *action_name)
{
	GtkShortcut *shortcut;

	shortcut = gtk_shortcut_new (gtk_shortcut_trigger_parse_string (trigger),
	                             gtk_named_action_new (action_name));
	gtk_shortcut_controller_add_shortcut (controller, shortcut);
}

/* Reads the view's resolved default font/colour so the toolbar can show a
 * sensible value when the cursor sits on unformatted text. */
static void
gnome_sticky_notes_window_capture_defaults (GnomeStickyNotesWindow *self)
{
	PangoContext *ctx = gtk_widget_get_pango_context (GTK_WIDGET (self->text_view));
	const PangoFontDescription *desc = pango_context_get_font_description (ctx);
	const char *family = desc ? pango_font_description_get_family (desc) : NULL;
	int size = desc ? pango_font_description_get_size (desc) : 0;

	self->default_family = g_strdup (family ? family : "Sans");
	self->default_size = size > 0 ? (double) size / PANGO_SCALE : 11.0;
	gtk_widget_get_color (GTK_WIDGET (self->text_view), &self->default_color);
}

static void
gnome_sticky_notes_window_init (GnomeStickyNotesWindow *self)
{
	GtkTextBuffer *buffer;
	GtkShortcutController *controller;
	GtkEventControllerFocus *focus;

	self->note_id = -1;

	gtk_widget_init_template (GTK_WIDGET (self));

	g_action_map_add_action_entries (G_ACTION_MAP (self),
	                                 win_actions,
	                                 G_N_ELEMENTS (win_actions),
	                                 self);

	gnome_sticky_notes_window_capture_defaults (self);

	g_signal_connect (self, "close-request",
	                  G_CALLBACK (on_close_request), NULL);

	buffer = gtk_text_view_get_buffer (self->text_view);
	g_signal_connect (buffer, "changed", G_CALLBACK (on_buffer_changed), self);
	g_signal_connect (buffer, "apply-tag", G_CALLBACK (on_tag_changed), self);
	g_signal_connect (buffer, "remove-tag", G_CALLBACK (on_tag_changed), self);

	/* Auto-dismiss the formatting toolbar when the text view regains focus. */
	focus = GTK_EVENT_CONTROLLER_FOCUS (gtk_event_controller_focus_new ());
	g_signal_connect (focus, "enter", G_CALLBACK (on_text_view_focus_enter), self);
	gtk_widget_add_controller (GTK_WIDGET (self->text_view), GTK_EVENT_CONTROLLER (focus));

	/* Toolbar controls drive the formatting engine. */
	g_signal_connect (self->bold_button, "toggled", G_CALLBACK (on_bold_toggled), self);
	g_signal_connect (self->italic_button, "toggled", G_CALLBACK (on_italic_toggled), self);
	g_signal_connect (self->underline_button, "toggled", G_CALLBACK (on_underline_toggled), self);
	g_signal_connect (self->strike_button, "toggled", G_CALLBACK (on_strike_toggled), self);
	g_signal_connect (self->font_button, "notify::font-desc", G_CALLBACK (on_font_changed), self);
	g_signal_connect (self->size_spin, "value-changed", G_CALLBACK (on_size_changed), self);
	g_signal_connect (self->color_button, "notify::rgba", G_CALLBACK (on_color_changed), self);
	g_signal_connect (self->align_left_button, "toggled", G_CALLBACK (on_align_toggled), self);
	g_signal_connect (self->align_center_button, "toggled", G_CALLBACK (on_align_toggled), self);
	g_signal_connect (self->align_right_button, "toggled", G_CALLBACK (on_align_toggled), self);
	g_signal_connect (self->align_fill_button, "toggled", G_CALLBACK (on_align_toggled), self);

	/* Keyboard shortcuts, captured before the text view sees the keys. */
	controller = GTK_SHORTCUT_CONTROLLER (gtk_shortcut_controller_new ());
	gtk_shortcut_controller_set_scope (controller, GTK_SHORTCUT_SCOPE_MANAGED);
	gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (controller), GTK_PHASE_CAPTURE);
	add_shortcut (controller, "<Control>b", "win.fmt-bold");
	add_shortcut (controller, "<Control>i", "win.fmt-italic");
	add_shortcut (controller, "<Control>u", "win.fmt-underline");
	add_shortcut (controller, "<Control><Shift>l", "win.align-left");
	add_shortcut (controller, "<Control><Shift>e", "win.align-center");
	add_shortcut (controller, "<Control><Shift>r", "win.align-right");
	add_shortcut (controller, "<Control><Shift>j", "win.align-fill");
	gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (controller));
}

GnomeStickyNotesWindow *
gnome_sticky_notes_window_new (GtkApplication                 *application,
                               GnomeStickyNotesDatabase       *database,
                               const GnomeStickyNotesNoteData *data)
{
	GnomeStickyNotesWindow *self;
	GtkTextBuffer *buffer;

	g_return_val_if_fail (GTK_IS_APPLICATION (application), NULL);
	g_return_val_if_fail (GNOME_STICKY_NOTES_IS_DATABASE (database), NULL);
	g_return_val_if_fail (data != NULL, NULL);

	self = g_object_new (GNOME_STICKY_NOTES_TYPE_WINDOW,
	                     "application", application,
	                     NULL);

	self->database = g_object_ref (database);
	self->note_id  = data->id;
	self->color    = g_strdup (data->color);
	self->monitor  = g_strdup (data->monitor);

	/* Restore size. Position is intentionally not applied: on Wayland a
	 * client cannot place its own toplevel; the value is persisted for a
	 * future X11 backend. */
	gtk_window_set_default_size (GTK_WINDOW (self),
	                             data->width  > 0 ? data->width  : 280,
	                             data->height > 0 ? data->height : 320);

	/* Bring up the formatting engine and keep the toolbar in sync with it. */
	self->rich_text = gnome_sticky_notes_rich_text_new (self->text_view);
	g_signal_connect (self->rich_text, "state-changed",
	                  G_CALLBACK (on_state_changed), self);

	/* Load content without triggering an immediate autosave. The tag edits
	 * the loader makes must not be mistaken for user activity either. */
	buffer = gtk_text_view_get_buffer (self->text_view);
	g_signal_handlers_block_by_func (buffer, on_buffer_changed, self);
	g_signal_handlers_block_by_func (buffer, on_tag_changed, self);
	gnome_sticky_notes_rich_text_deserialize (self->rich_text, data->content);
	g_signal_handlers_unblock_by_func (buffer, on_buffer_changed, self);
	g_signal_handlers_unblock_by_func (buffer, on_tag_changed, self);

	return self;
}

gint64
gnome_sticky_notes_window_get_note_id (GnomeStickyNotesWindow *self)
{
	g_return_val_if_fail (GNOME_STICKY_NOTES_IS_WINDOW (self), -1);

	return self->note_id;
}
