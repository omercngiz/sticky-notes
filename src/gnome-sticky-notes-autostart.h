/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Whether the XDG autostart entry for the app currently exists. */
gboolean gnome_sticky_notes_autostart_get_enabled (void);

/* Writes (or removes) ~/.config/autostart/io.omercngiz.StickyNotes.desktop
 * so the app launches at login. The Exec line points at the currently running
 * executable, so it tracks wherever the app was installed. */
gboolean gnome_sticky_notes_autostart_set_enabled (gboolean   enabled,
                                                   GError   **error);

G_END_DECLS
