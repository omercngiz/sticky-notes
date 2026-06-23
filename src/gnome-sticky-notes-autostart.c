/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include "gnome-sticky-notes-autostart.h"

#include <errno.h>
#include <glib/gstdio.h>

#define AUTOSTART_BASENAME "io.omercngiz.GnomeStickyNotes.desktop"

static char *
autostart_file_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "autostart",
                           AUTOSTART_BASENAME, NULL);
}

gboolean
gnome_sticky_notes_autostart_get_enabled (void)
{
  g_autofree char *path = autostart_file_path ();

  return g_file_test (path, G_FILE_TEST_EXISTS);
}

gboolean
gnome_sticky_notes_autostart_set_enabled (gboolean   enabled,
                                          GError   **error)
{
  g_autofree char *dir = g_build_filename (g_get_user_config_dir (), "autostart", NULL);
  g_autofree char *path = g_build_filename (dir, AUTOSTART_BASENAME, NULL);
  g_autofree char *exe = NULL;
  g_autofree char *contents = NULL;

  if (!enabled)
    {
      if (g_file_test (path, G_FILE_TEST_EXISTS) && g_remove (path) != 0)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "Failed to remove autostart entry: %s", g_strerror (errno));
          return FALSE;
        }
      return TRUE;
    }

  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Failed to create autostart directory: %s", g_strerror (errno));
      return FALSE;
    }

  /* Resolve the real binary path so the entry survives a PATH that does not
   * include the install prefix at login time. */
  exe = g_file_read_link ("/proc/self/exe", NULL);

  contents = g_strdup_printf ("[Desktop Entry]\n"
                              "Type=Application\n"
                              "Name=Gnome Sticky Notes\n"
                              "Exec=%s\n"
                              "Icon=io.omercngiz.GnomeStickyNotes\n"
                              "Terminal=false\n"
                              "X-GNOME-Autostart-enabled=true\n",
                              exe != NULL ? exe : "gnome-sticky-notes");

  return g_file_set_contents (path, contents, -1, error);
}
