/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOME_STICKY_NOTES_TYPE_TRAY (gnome_sticky_notes_tray_get_type())

G_DECLARE_FINAL_TYPE (GnomeStickyNotesTray, gnome_sticky_notes_tray, GNOME_STICKY_NOTES, TRAY, GObject)

/* Creates a StatusNotifierItem tray icon backed by the given application.
 * Menu clicks are dispatched as actions on @application's action group
 * (e.g. "new-note", "quit"). The tray registers itself with the
 * StatusNotifierWatcher when one becomes available (the GNOME "AppIndicator
 * and KStatusNotifier" extension provides it). */
GnomeStickyNotesTray *gnome_sticky_notes_tray_new (GApplication *application);

G_END_DECLS
