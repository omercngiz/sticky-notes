/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include <glib/gi18n.h>

#include "gnome-sticky-notes-application.h"
#include "gnome-sticky-notes-autostart.h"
#include "gnome-sticky-notes-backup.h"
#include "gnome-sticky-notes-database.h"
#include "gnome-sticky-notes-preferences.h"
#include "gnome-sticky-notes-tray.h"
#include "gnome-sticky-notes-window.h"

struct _GnomeStickyNotesApplication
{
  AdwApplication parent_instance;

  GnomeStickyNotesDatabase *database;
  GnomeStickyNotesTray     *tray;
  GSettings                *settings;
};

G_DEFINE_FINAL_TYPE (GnomeStickyNotesApplication, gnome_sticky_notes_application, ADW_TYPE_APPLICATION)

GnomeStickyNotesApplication *
gnome_sticky_notes_application_new (const char *application_id,
                                    GApplicationFlags flags)
{
  g_return_val_if_fail (application_id != NULL, NULL);

  return g_object_new (GNOME_STICKY_NOTES_TYPE_APPLICATION,
                       "application-id", application_id,
                       "flags", flags,
                       "resource-base-path", "/io/omercngiz/GnomeStickyNotes",
                       NULL);
}

/* Creates and presents a note window for the given record. */
static void
open_note (GnomeStickyNotesApplication *self,
           const GnomeStickyNotesNoteData *data)
{
  GnomeStickyNotesWindow *window;

  window = gnome_sticky_notes_window_new (GTK_APPLICATION (self), self->database, data);
  gtk_window_present (GTK_WINDOW (window));
}

/* Loads every saved note and shows it. If there are none yet, creates a
 * first empty note so the user always has something on screen. */
static void
open_all_notes (GnomeStickyNotesApplication *self)
{
  g_autoptr (GPtrArray) notes = NULL;
  g_autoptr (GError) error = NULL;

  notes = gnome_sticky_notes_database_load_all (self->database, &error);
  if (notes == NULL)
    {
      g_warning ("Failed to load notes: %s", error->message);
      return;
    }

  if (notes->len == 0)
    {
      gint64 id = gnome_sticky_notes_database_create_note (self->database, &error);
      GnomeStickyNotesNoteData data;

      if (id < 0)
        {
          g_warning ("Failed to create initial note: %s", error->message);
          return;
        }

      data = (GnomeStickyNotesNoteData) {
        .id = id,
        .content = (char *) "",
        .pos_x = -1,
        .pos_y = -1,
        .width = 280,
        .height = 320,
      };
      open_note (self, &data);
      return;
    }

  for (guint i = 0; i < notes->len; i++)
    open_note (self, g_ptr_array_index (notes, i));
}

/* Reconciles the on-disk XDG autostart entry with the "autostart" GSetting,
 * which is the source of truth. Called at startup and whenever the key
 * changes (e.g. from the Settings window). */
static void
gnome_sticky_notes_application_sync_autostart (GnomeStickyNotesApplication *self)
{
  g_autoptr (GError) error = NULL;
  gboolean enabled = g_settings_get_boolean (self->settings, "autostart");

  if (!gnome_sticky_notes_autostart_set_enabled (enabled, &error))
    g_warning ("Failed to update autostart entry: %s", error->message);
}

static void
gnome_sticky_notes_application_autostart_changed (GSettings  *settings,
                                                  const char *key,
                                                  gpointer    user_data)
{
  gnome_sticky_notes_application_sync_autostart (user_data);
}

static void
gnome_sticky_notes_application_startup (GApplication *app)
{
  GnomeStickyNotesApplication *self = GNOME_STICKY_NOTES_APPLICATION (app);
  g_autoptr (GError) error = NULL;

  G_APPLICATION_CLASS (gnome_sticky_notes_application_parent_class)->startup (app);

  self->database = gnome_sticky_notes_database_new (&error);
  if (self->database == NULL)
    {
      g_critical ("Failed to open database: %s", error->message);
      g_application_quit (app);
      return;
    }

  /* Keep running as a background service even when every note window is
   * closed; the tray and autostart drive the lifecycle. */
  g_application_hold (app);

  self->tray = gnome_sticky_notes_tray_new (app);

  /* Keep the XDG autostart entry in sync with the user's preference. */
  self->settings = g_settings_new ("io.omercngiz.GnomeStickyNotes");
  g_signal_connect (self->settings, "changed::autostart",
                    G_CALLBACK (gnome_sticky_notes_application_autostart_changed),
                    self);
  gnome_sticky_notes_application_sync_autostart (self);
}

static void
gnome_sticky_notes_application_activate (GApplication *app)
{
  GnomeStickyNotesApplication *self = GNOME_STICKY_NOTES_APPLICATION (app);
  GtkApplication *gtk_app = GTK_APPLICATION (app);

  g_assert (GNOME_STICKY_NOTES_IS_APPLICATION (self));

  /* First launch: materialize saved notes. On a re-activation (e.g. the
   * user launches the app again) just bring existing notes forward. */
  if (gtk_application_get_windows (gtk_app) == NULL)
    {
      open_all_notes (self);
    }
  else
    {
      for (GList *l = gtk_application_get_windows (gtk_app); l != NULL; l = l->next)
        gtk_window_present (GTK_WINDOW (l->data));
    }
}

static void
gnome_sticky_notes_application_dispose (GObject *object)
{
  GnomeStickyNotesApplication *self = GNOME_STICKY_NOTES_APPLICATION (object);

  g_clear_object (&self->settings);
  g_clear_object (&self->tray);
  g_clear_object (&self->database);

  G_OBJECT_CLASS (gnome_sticky_notes_application_parent_class)->dispose (object);
}

static void
gnome_sticky_notes_application_class_init (GnomeStickyNotesApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = gnome_sticky_notes_application_dispose;
  app_class->startup = gnome_sticky_notes_application_startup;
  app_class->activate = gnome_sticky_notes_application_activate;
}

static void
gnome_sticky_notes_application_new_note_action (GSimpleAction *action,
                                                GVariant *parameter,
                                                gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  g_autoptr (GError) error = NULL;
  GnomeStickyNotesNoteData data;
  gint64 id;

  g_assert (GNOME_STICKY_NOTES_IS_APPLICATION (self));

  id = gnome_sticky_notes_database_create_note (self->database, &error);
  if (id < 0)
    {
      g_warning ("Failed to create note: %s", error->message);
      return;
    }

  data = (GnomeStickyNotesNoteData) {
    .id = id,
    .content = (char *) "",
    .pos_x = -1,
    .pos_y = -1,
    .width = 280,
    .height = 320,
  };
  open_note (self, &data);
}

static void
gnome_sticky_notes_application_show_all_action (GSimpleAction *action,
                                                GVariant *parameter,
                                                gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  GtkApplication *gtk_app = GTK_APPLICATION (self);

  g_assert (GNOME_STICKY_NOTES_IS_APPLICATION (self));

  if (gtk_application_get_windows (gtk_app) == NULL)
    {
      open_all_notes (self);
    }
  else
    {
      for (GList *l = gtk_application_get_windows (gtk_app); l != NULL; l = l->next)
        gtk_window_present (GTK_WINDOW (l->data));
    }
}

static void
gnome_sticky_notes_application_preferences_action (GSimpleAction *action,
                                                   GVariant *parameter,
                                                   gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  GnomeStickyNotesPreferences *prefs;
  GtkWindow *window;

  g_assert (GNOME_STICKY_NOTES_IS_APPLICATION (self));

  /* The dialog needs a parent to anchor to; when no note window is open
   * (tray-only) it still presents fine with a NULL parent. */
  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  prefs = gnome_sticky_notes_preferences_new ();
  adw_dialog_present (ADW_DIALOG (prefs), window ? GTK_WIDGET (window) : NULL);
}

static void
gnome_sticky_notes_application_about_action (GSimpleAction *action,
                                             GVariant *parameter,
                                             gpointer user_data)
{
  static const char *developers[] = { "omer", NULL };
  GnomeStickyNotesApplication *self = user_data;
  GtkWindow *window = NULL;

  g_assert (GNOME_STICKY_NOTES_IS_APPLICATION (self));

  window = gtk_application_get_active_window (GTK_APPLICATION (self));

  adw_show_about_dialog (GTK_WIDGET (window),
                         "application-name", "Gnome Sticky Notes",
                         "application-icon", "io.omercngiz.GnomeStickyNotes",
                         "developer-name", "omer",
                         "translator-credits", _ ("translator-credits"),
                         "version", "0.1.0",
                         "developers", developers,
                         "copyright", "© 2026 omer",
                         NULL);
}

/* --- backup / restore ----------------------------------------------------- */

/* Inserts each backed-up note as a fresh row (new id) and opens a window for
 * it, so a restore works the same on an empty or a populated database. */
static void
import_notes (GnomeStickyNotesApplication *self,
              GPtrArray                   *notes)
{
  for (guint i = 0; i < notes->len; i++)
    {
      GnomeStickyNotesNoteData *n = g_ptr_array_index (notes, i);
      g_autoptr (GError) error = NULL;
      gint64 id;

      id = gnome_sticky_notes_database_create_note (self->database, &error);
      if (id < 0)
        {
          g_warning ("Failed to create note on import: %s", error->message);
          continue;
        }

      n->id = id;
      if (!gnome_sticky_notes_database_save_note (self->database, n, &error))
        {
          g_warning ("Failed to save imported note: %s", error->message);
          continue;
        }

      open_note (self, n);
    }
}

static void
on_export_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);
  if (file == NULL)
    return; /* cancelled or failed; dialog already reported user-facing errors */

  if (!gnome_sticky_notes_backup_export (self->database, file, &error))
    g_warning ("Backup export failed: %s", error->message);
}

static void
gnome_sticky_notes_application_export_backup_action (GSimpleAction *action,
                                                     GVariant *parameter,
                                                     gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, _("Export Backup"));
  gtk_file_dialog_set_initial_name (dialog, "gnome-sticky-notes-backup.gsnbak");
  gtk_file_dialog_save (dialog,
                        gtk_application_get_active_window (GTK_APPLICATION (self)),
                        NULL, on_export_ready, self);
  g_object_unref (dialog);
}

static void
on_import_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GPtrArray) notes = NULL;
  g_autoptr (GError) error = NULL;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);
  if (file == NULL)
    return; /* cancelled */

  notes = gnome_sticky_notes_backup_read (file, &error);
  if (notes == NULL)
    {
      g_warning ("Backup import failed: %s", error->message);
      return;
    }

  import_notes (self, notes);
}

static void
gnome_sticky_notes_application_import_backup_action (GSimpleAction *action,
                                                     GVariant *parameter,
                                                     gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, _("Import Backup"));
  gtk_file_dialog_open (dialog,
                        gtk_application_get_active_window (GTK_APPLICATION (self)),
                        NULL, on_import_ready, self);
  g_object_unref (dialog);
}

/* Writes a backup to a cache file and hands it to the user's mail client as an
 * attachment via xdg-email (so it lands in whatever account they send from,
 * e.g. Gmail). */
static void
gnome_sticky_notes_application_email_backup_action (GSimpleAction *action,
                                                    GVariant *parameter,
                                                    gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GSubprocess) proc = NULL;

  /* A .txt name (the payload is plain text) is accepted by more mail clients
   * than an unknown .gsnbak extension; import reads it back regardless. */
  path = g_build_filename (g_get_user_cache_dir (),
                           "gnome-sticky-notes-backup.txt", NULL);
  file = g_file_new_for_path (path);

  if (!gnome_sticky_notes_backup_export (self->database, file, &error))
    {
      g_warning ("Backup export for email failed: %s", error->message);
      return;
    }

  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error,
                           "xdg-email",
                           "--subject", "GNOME Sticky Notes Backup",
                           "--attach", path,
                           NULL);
  if (proc == NULL)
    g_warning ("Failed to launch mail client: %s", error->message);
}

static void
gnome_sticky_notes_application_quit_action (GSimpleAction *action,
                                            GVariant *parameter,
                                            gpointer user_data)
{
  GnomeStickyNotesApplication *self = user_data;

  g_assert (GNOME_STICKY_NOTES_IS_APPLICATION (self));

  g_application_quit (G_APPLICATION (self));
}

static const GActionEntry app_actions[] = {
  { "new-note", gnome_sticky_notes_application_new_note_action },
  { "show-all", gnome_sticky_notes_application_show_all_action },
  { "preferences", gnome_sticky_notes_application_preferences_action },
  { "export-backup", gnome_sticky_notes_application_export_backup_action },
  { "import-backup", gnome_sticky_notes_application_import_backup_action },
  { "email-backup", gnome_sticky_notes_application_email_backup_action },
  { "about", gnome_sticky_notes_application_about_action },
  { "quit", gnome_sticky_notes_application_quit_action },
};

static void
gnome_sticky_notes_application_init (GnomeStickyNotesApplication *self)
{
  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   self);
  gtk_application_set_accels_for_action (GTK_APPLICATION (self),
                                         "app.quit",
                                         (const char *[]) { "<primary>q", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (self),
                                         "app.new-note",
                                         (const char *[]) { "<primary>n", NULL });
}

