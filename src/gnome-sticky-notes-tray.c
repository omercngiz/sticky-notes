/* MIT License
 *
 * Copyright (c) 2026 omer
 *
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include <glib/gi18n.h>
#include <string.h>

#include "gnome-sticky-notes-tray.h"

/* Object paths we export on the application's bus connection. */
#define SNI_OBJECT_PATH  "/StatusNotifierItem"
#define MENU_OBJECT_PATH "/StatusNotifierItem/Menu"

#define WATCHER_NAME       "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT     "/StatusNotifierWatcher"
#define WATCHER_INTERFACE  "org.kde.StatusNotifierWatcher"

/* One row in the tray menu. A NULL action with a "separator" type draws a
 * divider. Action names are resolved against the application action group. */
typedef struct
{
  gint        id;
  const char *label;   /* untranslated; run through gettext at build time */
  const char *action;  /* "app.<name>" or NULL */
  const char *type;    /* "separator" or NULL for a standard item */
} MenuItem;

static const MenuItem menu_items[] = {
  { 1, N_ ("New Note"),       "app.new-note",    NULL },
  { 2, N_ ("Show All Notes"), "app.show-all",    NULL },
  { 3, NULL,                  NULL,              "separator" },
  { 4, N_ ("Settings"),       "app.preferences", NULL },
  { 5, N_ ("About"),          "app.about",       NULL },
  { 6, NULL,                  NULL,              "separator" },
  { 7, N_ ("Quit"),           "app.quit",        NULL },
};

struct _GnomeStickyNotesTray
{
  GObject          parent_instance;

  GApplication    *app;          /* unowned; the app owns us */
  GDBusConnection *connection;   /* unowned; owned by the app */

  guint            sni_reg_id;
  guint            menu_reg_id;
  guint            watcher_watch_id;
  guint            menu_revision;
};

G_DEFINE_FINAL_TYPE (GnomeStickyNotesTray, gnome_sticky_notes_tray, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ *
 *  Interface definitions                                             *
 * ------------------------------------------------------------------ */

static const char sni_introspection_xml[] =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='IconThemePath' type='s' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <method name='ContextMenu'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='Activate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='SecondaryActivate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='Scroll'>"
  "      <arg type='i' name='delta' direction='in'/>"
  "      <arg type='s' name='orientation' direction='in'/>"
  "    </method>"
  "    <signal name='NewTitle'/>"
  "    <signal name='NewIcon'/>"
  "    <signal name='NewStatus'>"
  "      <arg type='s' name='status'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

static const char menu_introspection_xml[] =
  "<node>"
  "  <interface name='com.canonical.dbusmenu'>"
  "    <property name='Version' type='u' access='read'/>"
  "    <property name='TextDirection' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconThemePath' type='as' access='read'/>"
  "    <method name='GetLayout'>"
  "      <arg type='i' name='parentId' direction='in'/>"
  "      <arg type='i' name='recursionDepth' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='u' name='revision' direction='out'/>"
  "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg type='ai' name='ids' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
  "    </method>"
  "    <method name='GetProperty'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='s' name='name' direction='in'/>"
  "      <arg type='v' name='value' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='s' name='eventId' direction='in'/>"
  "      <arg type='v' name='data' direction='in'/>"
  "      <arg type='u' name='timestamp' direction='in'/>"
  "    </method>"
  "    <method name='EventGroup'>"
  "      <arg type='a(isvu)' name='events' direction='in'/>"
  "      <arg type='ai' name='idErrors' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShow'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='b' name='needUpdate' direction='out'/>"
  "    </method>"
  "    <signal name='ItemsPropertiesUpdated'>"
  "      <arg type='a(ia{sv})' name='updatedProps'/>"
  "      <arg type='a(ias)' name='removedProps'/>"
  "    </signal>"
  "    <signal name='LayoutUpdated'>"
  "      <arg type='u' name='revision'/>"
  "      <arg type='i' name='parent'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *sni_node_info;
static GDBusNodeInfo *menu_node_info;

/* ------------------------------------------------------------------ *
 *  Menu helpers                                                      *
 * ------------------------------------------------------------------ */

static const MenuItem *
find_menu_item (gint id)
{
  for (gsize i = 0; i < G_N_ELEMENTS (menu_items); i++)
    if (menu_items[i].id == id)
      return &menu_items[i];

  return NULL;
}

/* Builds the property dict for a single menu item. */
static void
build_item_props (GVariantBuilder *props,
                  const MenuItem  *item)
{
  g_variant_builder_init (props, G_VARIANT_TYPE ("a{sv}"));

  if (g_strcmp0 (item->type, "separator") == 0)
    {
      g_variant_builder_add (props, "{sv}", "type", g_variant_new_string ("separator"));
      return;
    }

  if (item->label != NULL)
    g_variant_builder_add (props, "{sv}", "label", g_variant_new_string (_ (item->label)));

  g_variant_builder_add (props, "{sv}", "enabled", g_variant_new_boolean (TRUE));
  g_variant_builder_add (props, "{sv}", "visible", g_variant_new_boolean (TRUE));
}

/* Returns a floating (ia{sv}av) layout node for a leaf item. */
static GVariant *
build_item_node (const MenuItem *item)
{
  GVariantBuilder props;
  GVariantBuilder children;

  build_item_props (&props, item);
  g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

  return g_variant_new ("(ia{sv}av)", item->id, &props, &children);
}

/* Dispatches a menu activation as an application action. */
static void
activate_menu_item (GnomeStickyNotesTray *self,
                    gint                  id)
{
  const MenuItem *item = find_menu_item (id);
  const char *dot;

  if (item == NULL || item->action == NULL)
    return;

  dot = strchr (item->action, '.');
  if (dot == NULL)
    return;

  g_action_group_activate_action (G_ACTION_GROUP (self->app), dot + 1, NULL);
}

/* ------------------------------------------------------------------ *
 *  dbusmenu interface                                                *
 * ------------------------------------------------------------------ */

static void
menu_method_call (GDBusConnection       *connection,
                  const char            *sender,
                  const char            *object_path,
                  const char            *interface_name,
                  const char            *method_name,
                  GVariant              *parameters,
                  GDBusMethodInvocation *invocation,
                  gpointer               user_data)
{
  GnomeStickyNotesTray *self = user_data;

  if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
      GVariantBuilder root_props;
      GVariantBuilder root_children;

      g_variant_builder_init (&root_props, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&root_props, "{sv}", "children-display",
                             g_variant_new_string ("submenu"));

      g_variant_builder_init (&root_children, G_VARIANT_TYPE ("av"));
      for (gsize i = 0; i < G_N_ELEMENTS (menu_items); i++)
        g_variant_builder_add (&root_children, "v", build_item_node (&menu_items[i]));

      g_dbus_method_invocation_return_value (
        invocation,
        g_variant_new ("(u(ia{sv}av))", self->menu_revision, 0, &root_props, &root_children));
    }
  else if (g_strcmp0 (method_name, "GetGroupProperties") == 0)
    {
      GVariantBuilder result;
      g_autoptr (GVariantIter) ids = NULL;
      gint id;

      g_variant_get (parameters, "(aias)", &ids, NULL);
      g_variant_builder_init (&result, G_VARIANT_TYPE ("a(ia{sv})"));

      while (g_variant_iter_next (ids, "i", &id))
        {
          const MenuItem *item = find_menu_item (id);
          GVariantBuilder props;

          if (item == NULL)
            continue;

          build_item_props (&props, item);
          g_variant_builder_add (&result, "(ia{sv})", id, &props);
        }

      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(a(ia{sv}))", &result));
    }
  else if (g_strcmp0 (method_name, "GetProperty") == 0)
    {
      const MenuItem *item;
      const char *name;
      GVariantBuilder props;
      gint id;

      g_variant_get (parameters, "(i&s)", &id, &name);
      item = find_menu_item (id);

      if (item == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "No such menu item %d", id);
          return;
        }

      /* Build the dict, then hand back the single requested property. */
      build_item_props (&props, item);
      {
        g_autoptr (GVariant) dict = g_variant_builder_end (&props);
        g_autoptr (GVariant) value = g_variant_lookup_value (dict, name, NULL);

        if (value == NULL)
          value = g_variant_ref_sink (g_variant_new_string (""));

        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(v)", value));
      }
    }
  else if (g_strcmp0 (method_name, "Event") == 0)
    {
      const char *event_id;
      gint id;

      g_variant_get (parameters, "(i&svu)", &id, &event_id, NULL, NULL);

      if (g_strcmp0 (event_id, "clicked") == 0)
        activate_menu_item (self, id);

      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_strcmp0 (method_name, "EventGroup") == 0)
    {
      g_autoptr (GVariantIter) events = NULL;
      gint id;

      g_variant_get (parameters, "(a(isvu))", &events);
      while (g_variant_iter_next (events, "(i&svu)", &id, NULL, NULL, NULL))
        ; /* best effort: groups are only used for batched non-click events */

      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(ai)", NULL));
    }
  else if (g_strcmp0 (method_name, "AboutToShow") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(b)", FALSE));
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method %s", method_name);
    }
}

static GVariant *
menu_get_property (GDBusConnection  *connection,
                   const char       *sender,
                   const char       *object_path,
                   const char       *interface_name,
                   const char       *property_name,
                   GError          **error,
                   gpointer          user_data)
{
  if (g_strcmp0 (property_name, "Version") == 0)
    return g_variant_new_uint32 (3);
  if (g_strcmp0 (property_name, "TextDirection") == 0)
    return g_variant_new_string ("ltr");
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("normal");
  if (g_strcmp0 (property_name, "IconThemePath") == 0)
    return g_variant_new_strv (NULL, 0);

  return NULL;
}

static const GDBusInterfaceVTable menu_vtable = {
  menu_method_call,
  menu_get_property,
  NULL,
};

/* ------------------------------------------------------------------ *
 *  StatusNotifierItem interface                                      *
 * ------------------------------------------------------------------ */

static void
sni_method_call (GDBusConnection       *connection,
                 const char            *sender,
                 const char            *object_path,
                 const char            *interface_name,
                 const char            *method_name,
                 GVariant              *parameters,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
  GnomeStickyNotesTray *self = user_data;

  /* Left/middle click: surface the notes. The host renders the menu itself
   * for ContextMenu since ItemIsMenu is TRUE. */
  if (g_strcmp0 (method_name, "Activate") == 0
      || g_strcmp0 (method_name, "SecondaryActivate") == 0)
    g_action_group_activate_action (G_ACTION_GROUP (self->app), "show-all", NULL);

  g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
sni_get_property (GDBusConnection  *connection,
                  const char       *sender,
                  const char       *object_path,
                  const char       *interface_name,
                  const char       *property_name,
                  GError          **error,
                  gpointer          user_data)
{
  if (g_strcmp0 (property_name, "Category") == 0)
    return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (property_name, "Id") == 0)
    return g_variant_new_string ("io.omercngiz.GnomeStickyNotes");
  if (g_strcmp0 (property_name, "Title") == 0)
    return g_variant_new_string ("Gnome Sticky Notes");
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("Active");
  if (g_strcmp0 (property_name, "IconName") == 0)
    return g_variant_new_string ("io.omercngiz.GnomeStickyNotes");
  if (g_strcmp0 (property_name, "IconThemePath") == 0)
    return g_variant_new_string ("");
  if (g_strcmp0 (property_name, "ItemIsMenu") == 0)
    return g_variant_new_boolean (TRUE);
  if (g_strcmp0 (property_name, "Menu") == 0)
    return g_variant_new_object_path (MENU_OBJECT_PATH);

  return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
  sni_method_call,
  sni_get_property,
  NULL,
};

/* ------------------------------------------------------------------ *
 *  Watcher registration                                              *
 * ------------------------------------------------------------------ */

static void
on_register_done (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (reply == NULL)
    g_warning ("Failed to register with StatusNotifierWatcher: %s", error->message);
}

static void
on_watcher_appeared (GDBusConnection *connection,
                     const char      *name,
                     const char      *name_owner,
                     gpointer         user_data)
{
  GnomeStickyNotesTray *self = user_data;

  /* Hand the watcher our well-known bus name; it looks up the item object
   * at the conventional /StatusNotifierItem path. */
  g_dbus_connection_call (connection,
                          WATCHER_NAME,
                          WATCHER_OBJECT,
                          WATCHER_INTERFACE,
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", g_application_get_application_id (self->app)),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          on_register_done,
                          self);
}

/* ------------------------------------------------------------------ *
 *  Lifecycle                                                         *
 * ------------------------------------------------------------------ */

static void
gnome_sticky_notes_tray_dispose (GObject *object)
{
  GnomeStickyNotesTray *self = GNOME_STICKY_NOTES_TRAY (object);

  if (self->watcher_watch_id != 0)
    {
      g_bus_unwatch_name (self->watcher_watch_id);
      self->watcher_watch_id = 0;
    }

  if (self->connection != NULL)
    {
      if (self->sni_reg_id != 0)
        g_dbus_connection_unregister_object (self->connection, self->sni_reg_id);
      if (self->menu_reg_id != 0)
        g_dbus_connection_unregister_object (self->connection, self->menu_reg_id);
    }

  self->sni_reg_id = 0;
  self->menu_reg_id = 0;

  G_OBJECT_CLASS (gnome_sticky_notes_tray_parent_class)->dispose (object);
}

static void
gnome_sticky_notes_tray_class_init (GnomeStickyNotesTrayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnome_sticky_notes_tray_dispose;

  sni_node_info = g_dbus_node_info_new_for_xml (sni_introspection_xml, NULL);
  menu_node_info = g_dbus_node_info_new_for_xml (menu_introspection_xml, NULL);
}

static void
gnome_sticky_notes_tray_init (GnomeStickyNotesTray *self)
{
  self->menu_revision = 1;
}

GnomeStickyNotesTray *
gnome_sticky_notes_tray_new (GApplication *application)
{
  GnomeStickyNotesTray *self;
  GDBusConnection *connection;
  g_autoptr (GError) error = NULL;

  g_return_val_if_fail (G_IS_APPLICATION (application), NULL);

  connection = g_application_get_dbus_connection (application);
  if (connection == NULL)
    {
      g_warning ("No D-Bus connection; tray icon unavailable.");
      return NULL;
    }

  self = g_object_new (GNOME_STICKY_NOTES_TYPE_TRAY, NULL);
  self->app = application;
  self->connection = connection;

  self->sni_reg_id = g_dbus_connection_register_object (connection,
                                                        SNI_OBJECT_PATH,
                                                        sni_node_info->interfaces[0],
                                                        &sni_vtable,
                                                        self, NULL, &error);
  if (self->sni_reg_id == 0)
    {
      g_warning ("Failed to export StatusNotifierItem: %s", error->message);
      g_clear_error (&error);
    }

  self->menu_reg_id = g_dbus_connection_register_object (connection,
                                                         MENU_OBJECT_PATH,
                                                         menu_node_info->interfaces[0],
                                                         &menu_vtable,
                                                         self, NULL, &error);
  if (self->menu_reg_id == 0)
    {
      g_warning ("Failed to export dbusmenu: %s", error->message);
      g_clear_error (&error);
    }

  /* Register now (if the watcher is already up) and whenever it reappears. */
  self->watcher_watch_id = g_bus_watch_name_on_connection (connection,
                                                           WATCHER_NAME,
                                                           G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                           on_watcher_appeared,
                                                           NULL,
                                                           self, NULL);

  return self;
}
