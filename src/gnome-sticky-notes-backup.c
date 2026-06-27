/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include "gnome-sticky-notes-backup.h"

#include <string.h>

/* A backup is this marker followed by a printed GVariant. Keeping our own
 * format (rather than a raw DB dump) means it survives schema changes and the
 * note "content" string already carries the rich-text payload as-is. */
#define BACKUP_MARKER       "GSNBAK1:"
#define BACKUP_VARIANT_TYPE "(saa{sv})"   /* app version, then a dict per note */

gboolean
gnome_sticky_notes_backup_export (GnomeStickyNotesDatabase *db,
                                  GFile                    *file,
                                  GError                  **error)
{
	g_autoptr (GPtrArray) notes = NULL;
	GVariantBuilder builder;
	GVariant *variant;
	g_autofree char *printed = NULL;
	g_autofree char *data = NULL;

	g_return_val_if_fail (GNOME_STICKY_NOTES_IS_DATABASE (db), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	notes = gnome_sticky_notes_database_load_all (db, error);
	if (notes == NULL)
		return FALSE;

	g_variant_builder_init (&builder, G_VARIANT_TYPE (BACKUP_VARIANT_TYPE));
	g_variant_builder_add (&builder, "s", PACKAGE_VERSION);
	g_variant_builder_open (&builder, G_VARIANT_TYPE ("aa{sv}"));

	for (guint i = 0; i < notes->len; i++)
		{
			GnomeStickyNotesNoteData *n = g_ptr_array_index (notes, i);

			g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
			g_variant_builder_add (&builder, "{sv}", "content",
			                       g_variant_new_string (n->content ? n->content : ""));
			if (n->color != NULL)
				g_variant_builder_add (&builder, "{sv}", "color", g_variant_new_string (n->color));
			if (n->monitor != NULL)
				g_variant_builder_add (&builder, "{sv}", "monitor", g_variant_new_string (n->monitor));
			g_variant_builder_add (&builder, "{sv}", "width", g_variant_new_int32 (n->width));
			g_variant_builder_add (&builder, "{sv}", "height", g_variant_new_int32 (n->height));
			g_variant_builder_add (&builder, "{sv}", "pos_x", g_variant_new_int32 (n->pos_x));
			g_variant_builder_add (&builder, "{sv}", "pos_y", g_variant_new_int32 (n->pos_y));
			g_variant_builder_close (&builder);
		}

	g_variant_builder_close (&builder);

	variant = g_variant_builder_end (&builder);
	printed = g_variant_print (variant, TRUE);
	g_variant_unref (g_variant_ref_sink (variant));

	data = g_strconcat (BACKUP_MARKER, printed, NULL);
	return g_file_replace_contents (file, data, strlen (data), NULL, FALSE,
	                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error);
}

GPtrArray *
gnome_sticky_notes_backup_read (GFile   *file,
                                GError **error)
{
	g_autofree char *contents = NULL;
	g_autoptr (GVariant) variant = NULL;
	g_autoptr (GVariant) arr = NULL;
	const char *version = NULL;
	GPtrArray *notes;
	GVariantIter outer;
	GVariant *dict;

	g_return_val_if_fail (G_IS_FILE (file), NULL);

	if (!g_file_load_contents (file, NULL, &contents, NULL, NULL, error))
		return NULL;

	if (!g_str_has_prefix (contents, BACKUP_MARKER))
		{
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			                     "Not a Sticky Notes backup file");
			return NULL;
		}

	variant = g_variant_parse (G_VARIANT_TYPE (BACKUP_VARIANT_TYPE),
	                           contents + strlen (BACKUP_MARKER), NULL, NULL, error);
	if (variant == NULL)
		return NULL;

	g_variant_get (variant, "(&s@aa{sv})", &version, &arr);

	notes = g_ptr_array_new_with_free_func ((GDestroyNotify) gnome_sticky_notes_note_data_free);

	g_variant_iter_init (&outer, arr);
	while (g_variant_iter_next (&outer, "@a{sv}", &dict))
		{
			GnomeStickyNotesNoteData *n = g_new0 (GnomeStickyNotesNoteData, 1);
			const char *s;
			gint32 v;

			n->id = -1;
			n->pos_x = -1;
			n->pos_y = -1;
			n->width = 280;
			n->height = 320;

			n->content = g_variant_lookup (dict, "content", "&s", &s) ? g_strdup (s) : g_strdup ("");
			if (g_variant_lookup (dict, "color", "&s", &s))   n->color = g_strdup (s);
			if (g_variant_lookup (dict, "monitor", "&s", &s)) n->monitor = g_strdup (s);
			if (g_variant_lookup (dict, "width", "i", &v))    n->width = v;
			if (g_variant_lookup (dict, "height", "i", &v))   n->height = v;
			if (g_variant_lookup (dict, "pos_x", "i", &v))    n->pos_x = v;
			if (g_variant_lookup (dict, "pos_y", "i", &v))    n->pos_y = v;

			g_ptr_array_add (notes, n);
			g_variant_unref (dict);
		}

	return notes;
}
