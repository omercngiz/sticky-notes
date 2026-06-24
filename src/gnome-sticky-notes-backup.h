/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <gio/gio.h>

#include "gnome-sticky-notes-database.h"

G_BEGIN_DECLS

/* Writes every note in the database to a portable backup file (a versioned
 * GVariant text payload). */
gboolean   gnome_sticky_notes_backup_export (GnomeStickyNotesDatabase *db,
                                             GFile                    *file,
                                             GError                  **error);

/* Parses a backup file into a GPtrArray of GnomeStickyNotesNoteData (id == -1,
 * caller assigns real ids on insert). Free with g_ptr_array_unref. Does NOT
 * touch the database. NULL on error. */
GPtrArray *gnome_sticky_notes_backup_read   (GFile                    *file,
                                             GError                  **error);

G_END_DECLS
