/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include "gnome-sticky-notes-preferences.h"

struct _GnomeStickyNotesPreferences
{
  AdwPreferencesDialog  parent_instance;

  GSettings            *settings;

  /* Template widgets */
  AdwSwitchRow         *autostart_row;
};

G_DEFINE_FINAL_TYPE (GnomeStickyNotesPreferences, gnome_sticky_notes_preferences, ADW_TYPE_PREFERENCES_DIALOG)

GnomeStickyNotesPreferences *
gnome_sticky_notes_preferences_new (void)
{
  return g_object_new (GNOME_STICKY_NOTES_TYPE_PREFERENCES, NULL);
}

static void
gnome_sticky_notes_preferences_dispose (GObject *object)
{
  GnomeStickyNotesPreferences *self = GNOME_STICKY_NOTES_PREFERENCES (object);

  g_clear_object (&self->settings);
  gtk_widget_dispose_template (GTK_WIDGET (object), GNOME_STICKY_NOTES_TYPE_PREFERENCES);

  G_OBJECT_CLASS (gnome_sticky_notes_preferences_parent_class)->dispose (object);
}

static void
gnome_sticky_notes_preferences_class_init (GnomeStickyNotesPreferencesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnome_sticky_notes_preferences_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/io/omercngiz/StickyNotes/gnome-sticky-notes-preferences.ui");
  gtk_widget_class_bind_template_child (widget_class, GnomeStickyNotesPreferences, autostart_row);
}

static void
gnome_sticky_notes_preferences_init (GnomeStickyNotesPreferences *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Bind the toggle straight to the GSetting. The application watches
   * "changed::autostart" and reconciles the XDG autostart entry. */
  self->settings = g_settings_new ("io.omercngiz.StickyNotes");
  g_settings_bind (self->settings, "autostart",
                   self->autostart_row, "active",
                   G_SETTINGS_BIND_DEFAULT);
}
