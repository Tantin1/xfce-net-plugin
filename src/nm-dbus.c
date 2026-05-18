#include "nm-dbus.h"
#include <string.h>

#define NM_BUS_NAME     "org.freedesktop.NetworkManager"
#define NM_OBJECT_PATH  "/org/freedesktop/NetworkManager"
#define NM_IFACE        "org.freedesktop.NetworkManager"
#define NM_DEVICE_IFACE "org.freedesktop.NetworkManager.Device"
#define NM_WIFI_IFACE   "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_IFACE     "org.freedesktop.NetworkManager.AccessPoint"
#define NM_SETTINGS_PATH  "/org/freedesktop/NetworkManager/Settings"
#define NM_SETTINGS_IFACE "org.freedesktop.NetworkManager.Settings"
#define NM_CONN_IFACE     "org.freedesktop.NetworkManager.Settings.Connection"

#define NM_DEVICE_TYPE_WIFI 2

GDBusConnection *
nm_dbus_connect (void)
{
    GError          *err  = NULL;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!conn) {
        g_warning ("nm-dbus: no se pudo conectar al bus: %s", err->message);
        g_error_free (err);
    }
    return conn;
}

static GVariant *
get_property (GDBusConnection *conn,
              const gchar     *object_path,
              const gchar     *iface,
              const gchar     *prop)
{
    GVariant *result, *value = NULL;
    GError   *err = NULL;

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", iface, prop),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);

    if (result) {
        g_variant_get (result, "(v)", &value);
        g_variant_unref (result);
    } else {
        g_warning ("nm-dbus: Get %s.%s en %s: %s",
                   iface, prop, object_path, err->message);
        g_error_free (err);
    }
    return value;
}

/* Callback genérico para llamadas async: solo loggea el error si hubo.
 * No necesita user_data porque la confirmación del cambio real llega
 * por señal DBus, no por este callback. */
static void
on_async_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    const gchar *op_name = user_data;
    GError      *err = NULL;
    GVariant    *result = g_dbus_connection_call_finish (
                              G_DBUS_CONNECTION (src), res, &err);
    if (result) {
        g_variant_unref (result);
    } else if (err) {
        g_warning ("nm-dbus async: %s falló: %s",
                   op_name ? op_name : "(?)", err->message);
        g_error_free (err);
    }
}

GSList *
nm_get_wifi_devices (GDBusConnection *conn)
{
    GVariant    *result, *paths_v;
    GError      *err = NULL;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "GetDevices", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: GetDevices: %s", err->message);
        g_error_free (err);
        return NULL;
    }

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        GVariant *type_v, *iface_v;
        guint32   dev_type;

        type_v = get_property (conn, paths[i], NM_DEVICE_IFACE, "DeviceType");
        if (!type_v) continue;
        dev_type = g_variant_get_uint32 (type_v);
        g_variant_unref (type_v);
        if (dev_type != NM_DEVICE_TYPE_WIFI) continue;

        iface_v = get_property (conn, paths[i], NM_DEVICE_IFACE, "Interface");
        if (!iface_v) continue;

        NmDevice *dev    = g_new0 (NmDevice, 1);
        dev->iface       = g_strdup (g_variant_get_string (iface_v, NULL));
        dev->object_path = g_strdup (paths[i]);
        dev->enabled     = TRUE;
        g_variant_unref (iface_v);

        list = g_slist_append (list, dev);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    return list;
}

void
nm_device_list_free (GSList *list)
{
    GSList *l;
    for (l = list; l; l = l->next) {
        NmDevice *dev = l->data;
        g_free (dev->iface);
        g_free (dev->object_path);
        g_free (dev);
    }
    g_slist_free (list);
}

static gchar *
get_active_ap_path (GDBusConnection *conn, const gchar *device_path)
{
    GVariant    *v;
    const gchar *path;
    gchar       *result = NULL;

    v = get_property (conn, device_path, NM_WIFI_IFACE, "ActiveAccessPoint");
    if (!v)
        return NULL;

    path = g_variant_get_string (v, NULL);

    if (path && strcmp (path, "/") != 0)
        result = g_strdup (path);

    g_variant_unref (v);
    return result;
}

GSList *
nm_get_access_points (GDBusConnection *conn, const gchar *device_path)
{
    GVariant    *result, *paths_v;
    GError      *err = NULL;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;
    gchar       *active_path;

    active_path = get_active_ap_path (conn, device_path);

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, device_path, NM_WIFI_IFACE,
        "GetAllAccessPoints", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: GetAllAccessPoints en %s: %s",
                   device_path, err->message);
        g_error_free (err);
        g_free (active_path);
        return NULL;
    }

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        GVariant     *ssid_v, *strength_v, *flags_v;
        GVariantIter *iter;
        guchar        c;
        GString      *ssid_str;
        guint32       flags;

        ssid_v = get_property (conn, paths[i], NM_AP_IFACE, "Ssid");
        if (!ssid_v) continue;

        ssid_str = g_string_new (NULL);
        iter = g_variant_iter_new (ssid_v);
        while (g_variant_iter_next (iter, "y", &c))
            g_string_append_c (ssid_str, (gchar) c);
        g_variant_iter_free (iter);
        g_variant_unref (ssid_v);

        if (ssid_str->len == 0) {
            g_string_free (ssid_str, TRUE);
            continue;
        }

        strength_v = get_property (conn, paths[i], NM_AP_IFACE, "Strength");
        flags_v    = get_property (conn, paths[i], NM_AP_IFACE, "WpaFlags");
        GVariant *rsn_v  = get_property (conn, paths[i], NM_AP_IFACE, "RsnFlags");
        GVariant *freq_v = get_property (conn, paths[i], NM_AP_IFACE, "Frequency");

        NmAccessPoint *ap = g_new0 (NmAccessPoint, 1);
        ap->ssid        = g_string_free (ssid_str, FALSE);
        ap->object_path = g_strdup (paths[i]);
        ap->strength    = strength_v
                          ? (gint) g_variant_get_byte (strength_v) : 0;
        ap->frequency   = freq_v ? g_variant_get_uint32 (freq_v) : 0;
        flags      = (flags_v ? g_variant_get_uint32 (flags_v) : 0)
                   | (rsn_v  ? g_variant_get_uint32 (rsn_v)  : 0);
        ap->secure = (flags != 0);
        ap->active = (active_path && strcmp (paths[i], active_path) == 0);

        if (strength_v) g_variant_unref (strength_v);
        if (flags_v)    g_variant_unref (flags_v);
        if (rsn_v)      g_variant_unref (rsn_v);
        if (freq_v)     g_variant_unref (freq_v);

        list = g_slist_append (list, ap);
    }

    g_free (active_path);
    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    return list;
}

void
nm_ap_list_free (GSList *list)
{
    GSList *l;
    for (l = list; l; l = l->next) {
        NmAccessPoint *ap = l->data;
        g_free (ap->ssid);
        g_free (ap->object_path);
        g_free (ap);
    }
    g_slist_free (list);
}

/* ---------- ACCIÓN ASYNC: desconectar dispositivo ---------- */

void
nm_disconnect_device_async (GDBusConnection *conn, const gchar *device_path)
{
    g_dbus_connection_call (
        conn, NM_BUS_NAME, device_path, NM_DEVICE_IFACE,
        "Disconnect", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_async_done, "Disconnect");
}

/* ---------- conexiones guardadas ---------- */

static gchar *
find_connection_path_by_ssid (GDBusConnection *conn, const gchar *ssid)
{
    GVariant    *result, *paths_v;
    GError      *err = NULL;
    gsize        n, i;
    const gchar **paths;
    gchar       *found = NULL;

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, NM_SETTINGS_PATH, NM_SETTINGS_IFACE,
        "ListConnections", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: ListConnections: %s", err->message);
        g_error_free (err);
        return NULL;
    }

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n && !found; i++) {
        GVariant *settings = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, paths[i], NM_CONN_IFACE,
            "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

        if (!settings) continue;

        GVariant *outer     = g_variant_get_child_value (settings, 0);
        GVariant *wifi_dict = NULL;
        g_variant_lookup (outer, "802-11-wireless", "@a{sv}", &wifi_dict);

        if (wifi_dict) {
            GVariant *ssid_v = NULL;
            g_variant_lookup (wifi_dict, "ssid", "@ay", &ssid_v);
            if (ssid_v) {
                gsize         len;
                const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
                gchar        *conn_ssid = g_strndup ((const gchar *) bytes, len);
                if (g_strcmp0 (conn_ssid, ssid) == 0)
                    found = g_strdup (paths[i]);
                g_free (conn_ssid);
                g_variant_unref (ssid_v);
            }
            g_variant_unref (wifi_dict);
        }

        g_variant_unref (outer);
        g_variant_unref (settings);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    return found;
}

gboolean
nm_has_saved_connection (GDBusConnection *conn, const gchar *ssid)
{
    gchar    *path  = find_connection_path_by_ssid (conn, ssid);
    gboolean  found = (path != NULL);
    g_free (path);
    return found;
}

gboolean
nm_forget_connection (GDBusConnection *conn, const gchar *ssid)
{
    GVariant     *result, *paths_v;
    GError       *err = NULL;
    gsize         n, i;
    const gchar **paths;
    gboolean      deleted_any = FALSE;

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, NM_SETTINGS_PATH, NM_SETTINGS_IFACE,
        "ListConnections", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: ListConnections: %s", err->message);
        g_error_free (err);
        return FALSE;
    }

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        GVariant *settings = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, paths[i], NM_CONN_IFACE,
            "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

        if (!settings) continue;

        GVariant *outer     = g_variant_get_child_value (settings, 0);
        GVariant *wifi_dict = NULL;
        gboolean  match     = FALSE;
        g_variant_lookup (outer, "802-11-wireless", "@a{sv}", &wifi_dict);

        if (wifi_dict) {
            GVariant *ssid_v = NULL;
            g_variant_lookup (wifi_dict, "ssid", "@ay", &ssid_v);
            if (ssid_v) {
                gsize         len;
                const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
                gchar        *conn_ssid = g_strndup ((const gchar *) bytes, len);
                if (g_strcmp0 (conn_ssid, ssid) == 0)
                    match = TRUE;
                g_free (conn_ssid);
                g_variant_unref (ssid_v);
            }
            g_variant_unref (wifi_dict);
        }
        g_variant_unref (outer);
        g_variant_unref (settings);

        if (match) {
            GError   *derr = NULL;
            GVariant *dres = g_dbus_connection_call_sync (
                conn, NM_BUS_NAME, paths[i], NM_CONN_IFACE,
                "Delete", NULL, NULL,
                G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &derr);
            if (dres) {
                g_variant_unref (dres);
                deleted_any = TRUE;
            } else {
                g_warning ("nm-dbus: Delete %s: %s", paths[i], derr->message);
                g_error_free (derr);
            }
        }
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    return deleted_any;
}

/* ---------- Migración: quitar interface-name de perfiles viejos ---------- */

/* Devuelve un GSList* (lista enlazada) con los object_paths (gchar* duplicados)
 * de todos los perfiles guardados cuya SSID coincide con `ssid`. */
static GSList *
list_connection_paths_by_ssid (GDBusConnection *conn, const gchar *ssid)
{
    GSList       *matches = NULL;
    GVariant     *result, *paths_v;
    const gchar **paths;
    gsize         n, i;

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, NM_SETTINGS_PATH, NM_SETTINGS_IFACE,
        "ListConnections", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (!result) return NULL;

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        GVariant *settings = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, paths[i], NM_CONN_IFACE,
            "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
        if (!settings) continue;

        GVariant *outer     = g_variant_get_child_value (settings, 0);
        GVariant *wifi_dict = NULL;
        g_variant_lookup (outer, "802-11-wireless", "@a{sv}", &wifi_dict);

        if (wifi_dict) {
            GVariant *ssid_v = NULL;
            g_variant_lookup (wifi_dict, "ssid", "@ay", &ssid_v);
            if (ssid_v) {
                gsize         len;
                const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
                gchar        *conn_ssid = g_strndup ((const gchar *) bytes, len);
                if (g_strcmp0 (conn_ssid, ssid) == 0)
                    matches = g_slist_prepend (matches, g_strdup (paths[i]));
                g_free (conn_ssid);
                g_variant_unref (ssid_v);
            }
            g_variant_unref (wifi_dict);
        }
        g_variant_unref (outer);
        g_variant_unref (settings);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    return matches;
}

/* Devuelve TRUE si el perfil en `conn_path` tiene `connection.interface-name`
 * fijado (no vacío). */
static gboolean
connection_has_interface_name (GDBusConnection *conn, const gchar *conn_path)
{
    GVariant *settings = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, conn_path, NM_CONN_IFACE,
        "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!settings) return FALSE;

    gboolean  has_iface = FALSE;
    GVariant *outer     = g_variant_get_child_value (settings, 0);
    GVariant *conn_dict = NULL;
    g_variant_lookup (outer, "connection", "@a{sv}", &conn_dict);
    if (conn_dict) {
        const gchar *iface = NULL;
        if (g_variant_lookup (conn_dict, "interface-name", "&s", &iface)
            && iface && *iface)
            has_iface = TRUE;
        g_variant_unref (conn_dict);
    }
    g_variant_unref (outer);
    g_variant_unref (settings);
    return has_iface;
}

/* Reemplaza completamente las settings del perfil, quitando interface-name. */
static gboolean
connection_strip_interface_name (GDBusConnection *conn, const gchar *conn_path)
{
    /* Obtener settings actuales con secretos para no perder la psk. */
    GVariant *settings = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, conn_path, NM_CONN_IFACE,
        "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!settings) return FALSE;

    GVariant *outer = g_variant_get_child_value (settings, 0);

    /* Reconstruir el dict de settings, omitiendo interface-name dentro de
     * la sección "connection". */
    GVariantBuilder out_builder;
    g_variant_builder_init (&out_builder, G_VARIANT_TYPE ("a{sa{sv}}"));

    GVariantIter sec_iter;
    g_variant_iter_init (&sec_iter, outer);

    const gchar *sec_name;
    GVariant    *sec_dict;
    while (g_variant_iter_loop (&sec_iter, "{&s@a{sv}}", &sec_name, &sec_dict)) {
        GVariantBuilder inner_builder;
        g_variant_builder_init (&inner_builder, G_VARIANT_TYPE ("a{sv}"));

        GVariantIter prop_iter;
        g_variant_iter_init (&prop_iter, sec_dict);
        const gchar *prop_name;
        GVariant    *prop_val;
        while (g_variant_iter_loop (&prop_iter, "{&sv}", &prop_name, &prop_val)) {
            /* Omitir interface-name dentro de la sección "connection". */
            if (g_strcmp0 (sec_name, "connection") == 0 &&
                g_strcmp0 (prop_name, "interface-name") == 0)
                continue;
            g_variant_builder_add (&inner_builder, "{sv}", prop_name, prop_val);
        }
        g_variant_builder_add (&out_builder, "{sa{sv}}", sec_name, &inner_builder);
    }

    g_variant_unref (outer);
    g_variant_unref (settings);

    /* Llamar Update con el dict nuevo. */
    GError   *err = NULL;
    GVariant *res = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, conn_path, NM_CONN_IFACE,
        "Update",
        g_variant_new ("(a{sa{sv}})", &out_builder),
        NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (!res) {
        g_warning ("nm-dbus: Update %s: %s", conn_path,
                   err ? err->message : "?");
        if (err) g_error_free (err);
        return FALSE;
    }
    g_variant_unref (res);
    return TRUE;
}

gboolean
nm_strip_interface_name (GDBusConnection *conn, const gchar *ssid)
{
    GSList   *paths = list_connection_paths_by_ssid (conn, ssid);
    gboolean  changed = FALSE;
    guint     count  = g_slist_length (paths);

    if (count == 0) {
        g_slist_free_full (paths, g_free);
        return FALSE;
    }

    /* Si hay duplicados, conservar uno (el primero) y borrar los demás. */
    if (count > 1) {
        for (GSList *l = paths->next; l; l = l->next) {
            GError *err = NULL;
            GVariant *r = g_dbus_connection_call_sync (
                conn, NM_BUS_NAME, (const gchar *) l->data, NM_CONN_IFACE,
                "Delete", NULL, NULL,
                G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
            if (r) {
                g_variant_unref (r);
                changed = TRUE;
            } else if (err) {
                g_warning ("nm-dbus: Delete %s: %s",
                           (const gchar *) l->data, err->message);
                g_error_free (err);
            }
        }
    }

    /* Al perfil que queda (el primero) le sacamos interface-name si lo tiene. */
    const gchar *keep = (const gchar *) paths->data;
    if (connection_has_interface_name (conn, keep)) {
        if (connection_strip_interface_name (conn, keep))
            changed = TRUE;
    }

    g_slist_free_full (paths, g_free);
    return changed;
}

/* ---------- ACCIÓN ASYNC: activar conexión guardada ---------- */

void
nm_activate_connection_async (GDBusConnection *conn,
                              const gchar     *device_path,
                              const gchar     *ap_path,
                              const gchar     *ssid)
{
    gchar *conn_path = find_connection_path_by_ssid (conn, ssid);

    if (!conn_path) {
        g_warning ("nm-dbus: ActivateConnection: no se encontró perfil para %s", ssid);
        return;
    }

    g_dbus_connection_call (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "ActivateConnection",
        g_variant_new ("(ooo)", conn_path, device_path,
                       ap_path ? ap_path : "/"),
        G_VARIANT_TYPE ("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_async_done, "ActivateConnection");

    g_free (conn_path);
}

/* ---------- ACCIÓN ASYNC: añadir y activar conexión nueva ---------- */

void
nm_add_and_activate_connection_async (GDBusConnection *conn,
                                      const gchar     *device_path,
                                      const gchar     *ap_path,
                                      const gchar     *ssid,
                                      const gchar     *password,
                                      gboolean         autoconnect)
{
    GVariantBuilder conn_builder, wifi_builder, ipv4_builder, ipv6_builder,
                    meta_builder;

    GVariantBuilder ssid_builder;
    g_variant_builder_init (&ssid_builder, G_VARIANT_TYPE ("ay"));
    for (const gchar *p = ssid; *p; p++)
        g_variant_builder_add (&ssid_builder, "y", (guchar) *p);

    g_variant_builder_init (&wifi_builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&wifi_builder, "{sv}", "ssid",
                           g_variant_builder_end (&ssid_builder));
    g_variant_builder_add (&wifi_builder, "{sv}", "mode",
                           g_variant_new_string ("infrastructure"));

    g_variant_builder_init (&ipv4_builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&ipv4_builder, "{sv}", "method",
                           g_variant_new_string ("auto"));

    g_variant_builder_init (&ipv6_builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&ipv6_builder, "{sv}", "method",
                           g_variant_new_string ("auto"));

    g_variant_builder_init (&meta_builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&meta_builder, "{sv}", "type",
                           g_variant_new_string ("802-11-wireless"));
    g_variant_builder_add (&meta_builder, "{sv}", "autoconnect",
                           g_variant_new_boolean (autoconnect));
    /* Importante: NO pasamos "interface-name". Así el perfil no queda atado a
     * un adapter específico y cualquier wlanX puede usarlo. */

    g_variant_builder_init (&conn_builder, G_VARIANT_TYPE ("a{sa{sv}}"));
    g_variant_builder_add (&conn_builder, "{sa{sv}}", "connection",
                           &meta_builder);
    g_variant_builder_add (&conn_builder, "{sa{sv}}", "802-11-wireless",
                           &wifi_builder);
    g_variant_builder_add (&conn_builder, "{sa{sv}}", "ipv4", &ipv4_builder);
    g_variant_builder_add (&conn_builder, "{sa{sv}}", "ipv6", &ipv6_builder);

    if (password && *password) {
        GVariantBuilder sec_builder;
        g_variant_builder_init (&sec_builder, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&sec_builder, "{sv}", "key-mgmt",
                               g_variant_new_string ("wpa-psk"));
        g_variant_builder_add (&sec_builder, "{sv}", "psk",
                               g_variant_new_string (password));
        g_variant_builder_add (&conn_builder, "{sa{sv}}",
                               "802-11-wireless-security", &sec_builder);
    }

    g_dbus_connection_call (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "AddAndActivateConnection",
        g_variant_new ("(a{sa{sv}}oo)",
                       &conn_builder, device_path, ap_path),
        G_VARIANT_TYPE ("(oo)"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_async_done, "AddAndActivateConnection");
}

/* ---------- radio Wi-Fi global ---------- */

gboolean
nm_get_wifi_enabled (GDBusConnection *conn)
{
    GVariant *v = get_property (conn, NM_OBJECT_PATH, NM_IFACE, "WirelessEnabled");
    if (!v) return FALSE;
    gboolean enabled = g_variant_get_boolean (v);
    g_variant_unref (v);
    return enabled;
}

void
nm_set_wifi_enabled (GDBusConnection *conn, gboolean enabled)
{
    g_dbus_connection_call (
        conn, NM_BUS_NAME, NM_OBJECT_PATH,
        "org.freedesktop.DBus.Properties", "Set",
        g_variant_new ("(ssv)", NM_IFACE, "WirelessEnabled",
                       g_variant_new_boolean (enabled)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
        on_async_done, "SetWirelessEnabled");
}

/* ---------- radio Wi-Fi por adaptador ---------- */

gboolean
nm_get_device_enabled (GDBusConnection *conn, const gchar *device_path)
{
    GVariant *v = get_property (conn, device_path, NM_DEVICE_IFACE, "State");
    if (!v) return FALSE;
    guint32 state = g_variant_get_uint32 (v);
    g_variant_unref (v);
    /* State 20 = UNAVAILABLE (managed pero sin radio), 10 = UNMANAGED */
    return (state > 20);
}

/* Callback intermedio para encender: tras setear Managed=true, dispara Connect. */
static void
on_managed_true_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    GError   *err = NULL;
    GVariant *result = g_dbus_connection_call_finish (
                          G_DBUS_CONNECTION (src), res, &err);
    if (result) {
        g_variant_unref (result);
    } else if (err) {
        g_warning ("nm-dbus async: device set Managed=true falló: %s", err->message);
        g_error_free (err);
        g_free (user_data);
        return;
    }

    gchar *device_path = user_data;
    g_dbus_connection_call (
        G_DBUS_CONNECTION (src), NM_BUS_NAME, device_path, NM_DEVICE_IFACE,
        "Connect", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_async_done, "device Connect");
    g_free (device_path);
}

void
nm_set_device_enabled_async (GDBusConnection *conn, const gchar *device_path,
                             gboolean enabled)
{
    if (!enabled) {
        /* Apagar: marcar el dispositivo como no gestionado por NM */
        g_dbus_connection_call (
            conn, NM_BUS_NAME, device_path,
            "org.freedesktop.DBus.Properties", "Set",
            g_variant_new ("(ssv)", NM_DEVICE_IFACE, "Managed",
                           g_variant_new_boolean (FALSE)),
            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
            on_async_done, "device set Managed=false");
    } else {
        /* Encender: primero Managed=true, luego Connect (en el callback). */
        g_dbus_connection_call (
            conn, NM_BUS_NAME, device_path,
            "org.freedesktop.DBus.Properties", "Set",
            g_variant_new ("(ssv)", NM_DEVICE_IFACE, "Managed",
                           g_variant_new_boolean (TRUE)),
            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
            on_managed_true_done, g_strdup (device_path));
    }
}

/* ---------- dispositivos Ethernet conectados ---------- */

#define NM_DEVICE_TYPE_ETHERNET   1
#define NM_DEVICE_STATE_ACTIVATED 100

GSList *
nm_get_ethernet_devices (GDBusConnection *conn)
{
    GVariant    *result, *paths_v;
    GError      *err = NULL;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "GetDevices", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: GetDevices (eth): %s", err->message);
        g_error_free (err);
        return NULL;
    }

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        GVariant *type_v, *state_v, *iface_v;
        guint32   dev_type, state;

        type_v = get_property (conn, paths[i], NM_DEVICE_IFACE, "DeviceType");
        if (!type_v) continue;
        dev_type = g_variant_get_uint32 (type_v);
        g_variant_unref (type_v);
        if (dev_type != NM_DEVICE_TYPE_ETHERNET) continue;

        state_v = get_property (conn, paths[i], NM_DEVICE_IFACE, "State");
        if (!state_v) continue;
        state = g_variant_get_uint32 (state_v);
        g_variant_unref (state_v);
        if (state != NM_DEVICE_STATE_ACTIVATED) continue;

        iface_v = get_property (conn, paths[i], NM_DEVICE_IFACE, "Interface");
        if (!iface_v) continue;

        NmDevice *dev    = g_new0 (NmDevice, 1);
        dev->iface       = g_strdup (g_variant_get_string (iface_v, NULL));
        dev->object_path = g_strdup (paths[i]);
        dev->enabled     = TRUE;
        g_variant_unref (iface_v);

        list = g_slist_append (list, dev);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    return list;
}

/* ---------- VPN ---------- */

GSList *
nm_get_vpn_connections (GDBusConnection *conn)
{
    GVariant    *result, *paths_v;
    GError      *err = NULL;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;

    GVariant *active_v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, NM_OBJECT_PATH,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)", NM_IFACE, "ActiveConnections"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

    GHashTable *active_uuids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                       g_free, NULL);
    if (active_v) {
        GVariant *inner, *array;
        g_variant_get (active_v, "(v)", &inner);
        array = inner;
        gsize an = g_variant_n_children (array);
        for (gsize ai = 0; ai < an; ai++) {
            GVariant    *path_v = g_variant_get_child_value (array, ai);
            const gchar *path   = g_variant_get_string (path_v, NULL);
            GVariant    *uuid_v = get_property (conn, path,
                "org.freedesktop.NetworkManager.Connection.Active", "Uuid");
            if (uuid_v) {
                g_hash_table_add (active_uuids,
                                  g_strdup (g_variant_get_string (uuid_v, NULL)));
                g_variant_unref (uuid_v);
            }
            g_variant_unref (path_v);
        }
        g_variant_unref (inner);
        g_variant_unref (active_v);
    }

    result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, NM_SETTINGS_PATH, NM_SETTINGS_IFACE,
        "ListConnections", NULL, G_VARIANT_TYPE ("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: ListConnections: %s", err->message);
        g_error_free (err);
        g_hash_table_destroy (active_uuids);
        return NULL;
    }

    g_variant_get (result, "(@ao)", &paths_v);
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        GVariant *settings = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, paths[i], NM_CONN_IFACE,
            "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
        if (!settings) continue;

        GVariant *outer = g_variant_get_child_value (settings, 0);
        GVariant *conn_dict = NULL;
        g_variant_lookup (outer, "connection", "@a{sv}", &conn_dict);

        if (!conn_dict) {
            g_variant_unref (outer);
            g_variant_unref (settings);
            continue;
        }

        GVariant *type_v = NULL, *name_v = NULL, *uuid_v = NULL;
        g_variant_lookup (conn_dict, "type", "@s", &type_v);
        g_variant_lookup (conn_dict, "id",   "@s", &name_v);
        g_variant_lookup (conn_dict, "uuid", "@s", &uuid_v);

        if (type_v && name_v && uuid_v) {
            const gchar *type = g_variant_get_string (type_v, NULL);
            if (g_strcmp0 (type, "wireguard") == 0 ||
                g_strcmp0 (type, "vpn") == 0) {
                const gchar *uuid = g_variant_get_string (uuid_v, NULL);
                const gchar *name = g_variant_get_string (name_v, NULL);

                NmVpnConnection *vpn = g_new0 (NmVpnConnection, 1);
                vpn->name      = g_strdup (name);
                vpn->uuid      = g_strdup (uuid);
                vpn->conn_path = g_strdup (paths[i]);
                vpn->active    = g_hash_table_contains (active_uuids, uuid);

                list = g_slist_append (list, vpn);
            }
        }

        if (type_v) g_variant_unref (type_v);
        if (name_v) g_variant_unref (name_v);
        if (uuid_v) g_variant_unref (uuid_v);
        g_variant_unref (conn_dict);
        g_variant_unref (outer);
        g_variant_unref (settings);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    g_hash_table_destroy (active_uuids);
    return list;
}

void
nm_vpn_list_free (GSList *list)
{
    GSList *l;
    for (l = list; l; l = l->next) {
        NmVpnConnection *vpn = l->data;
        g_free (vpn->name);
        g_free (vpn->uuid);
        g_free (vpn->conn_path);
        g_free (vpn);
    }
    g_slist_free (list);
}

/* ---------- ACCIÓN ASYNC: activar VPN ---------- */

void
nm_activate_vpn_async (GDBusConnection *conn, const gchar *conn_path)
{
    g_dbus_connection_call (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "ActivateConnection",
        g_variant_new ("(ooo)", conn_path, "/", "/"),
        G_VARIANT_TYPE ("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_async_done, "ActivateConnection VPN");
}

/* ---------- ACCIÓN ASYNC: desactivar VPN ---------- */

/* Para desactivar hay que primero buscar el active connection path
 * que corresponde a este conn_path. Eso requiere una llamada sync rápida
 * (la lista de active connections + filtrado), después la deactivate va async. */
void
nm_deactivate_vpn_async (GDBusConnection *conn, const gchar *conn_path)
{
    GVariant    *active_v, *inner, *array;
    gboolean     found = FALSE;
    gsize        n, i;

    active_v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, NM_OBJECT_PATH,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)", NM_IFACE, "ActiveConnections"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!active_v) return;

    g_variant_get (active_v, "(v)", &inner);
    array = inner;
    n = g_variant_n_children (array);

    for (i = 0; i < n && !found; i++) {
        GVariant    *path_v      = g_variant_get_child_value (array, i);
        const gchar *active_path = g_variant_get_string (path_v, NULL);

        GVariant *cp_v = get_property (conn, active_path,
            "org.freedesktop.NetworkManager.Connection.Active",
            "Connection");
        if (cp_v) {
            const gchar *cp = g_variant_get_string (cp_v, NULL);
            if (g_strcmp0 (cp, conn_path) == 0) {
                g_dbus_connection_call (
                    conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
                    "DeactivateConnection",
                    g_variant_new ("(o)", active_path),
                    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                    on_async_done, "DeactivateConnection VPN");
                found = TRUE;
            }
            g_variant_unref (cp_v);
        }
        g_variant_unref (path_v);
    }

    g_variant_unref (inner);
    g_variant_unref (active_v);
}

/* ---------- estado VPN ---------- */

gboolean
nm_get_vpn_active (GDBusConnection *conn)
{
    GVariant    *v, *inner, *array;
    gboolean     found = FALSE;
    gsize        n, i;

    v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, NM_OBJECT_PATH,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)", NM_IFACE, "ActiveConnections"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!v) return FALSE;

    g_variant_get (v, "(v)", &inner);
    array = inner;
    n = g_variant_n_children (array);

    for (i = 0; i < n && !found; i++) {
        GVariant    *path_v = g_variant_get_child_value (array, i);
        const gchar *path   = g_variant_get_string (path_v, NULL);

        GVariant *type_v = get_property (conn, path,
            "org.freedesktop.NetworkManager.Connection.Active", "Type");
        if (type_v) {
            const gchar *type = g_variant_get_string (type_v, NULL);
            if (g_strcmp0 (type, "vpn") == 0 ||
                g_strcmp0 (type, "wireguard") == 0)
                found = TRUE;
            g_variant_unref (type_v);
        }
        g_variant_unref (path_v);
    }

    g_variant_unref (inner);
    g_variant_unref (v);
    return found;
}

/* ---------- contraseña del perfil guardado ---------- */

gchar *
nm_get_saved_password (GDBusConnection *conn, const gchar *ssid)
{
    gchar    *conn_path = find_connection_path_by_ssid (conn, ssid);
    GVariant *secrets, *outer, *sec_dict, *psk_v;
    gchar    *password = NULL;

    if (!conn_path) return NULL;

    secrets = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, conn_path, NM_CONN_IFACE,
        "GetSecrets",
        g_variant_new ("(s)", "802-11-wireless-security"),
        G_VARIANT_TYPE ("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);

    g_free (conn_path);
    if (!secrets) return NULL;

    outer    = g_variant_get_child_value (secrets, 0);
    sec_dict = NULL;
    g_variant_lookup (outer, "802-11-wireless-security", "@a{sv}", &sec_dict);

    if (sec_dict) {
        g_variant_lookup (sec_dict, "psk", "@s", &psk_v);
        if (psk_v) {
            password = g_strdup (g_variant_get_string (psk_v, NULL));
            g_variant_unref (psk_v);
        }
        g_variant_unref (sec_dict);
    }

    g_variant_unref (outer);
    g_variant_unref (secrets);
    return password;
}

/* ---------- suscripción a señales ---------- */

typedef struct {
    NmSignalCallback  callback;
    gpointer          user_data;
} SignalData;

static void
on_nm_signal (GDBusConnection *conn,
              const gchar     *sender,
              const gchar     *object_path,
              const gchar     *iface,
              const gchar     *signal_name,
              GVariant        *params,
              gpointer         user_data)
{
    (void) conn; (void) sender; (void) object_path;
    (void) iface; (void) signal_name; (void) params;

    SignalData *sd = user_data;
    sd->callback (sd->user_data);
}

guint *
nm_subscribe_signals (GDBusConnection *conn,
                      NmSignalCallback callback,
                      gpointer         user_data)
{
    /* Espacio para hasta N suscripciones + terminador 0.
     * Reservamos 64 para soportar varios adaptadores y conexiones activas. */
    guint      *ids = g_new0 (guint, 64);
    gint        n   = 0;

    SignalData *sd  = g_new0 (SignalData, 1);
    sd->callback  = callback;
    sd->user_data = user_data;

    /* 1. PropertiesChanged en el objeto raíz de NM (WirelessEnabled,
     *    PrimaryConnection, ActiveConnections, etc.) */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NM_OBJECT_PATH, NM_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    /* 2. PropertiesChanged en CUALQUIER dispositivo (interfaz Device).
     *    Sin filtrar por object_path, así capturamos altas y bajas de
     *    adaptadores (USB tethering, etc.) sin necesidad de re-suscribir. */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, NM_DEVICE_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    /* 3. PropertiesChanged en CUALQUIER adaptador wireless (ActiveAccessPoint, etc.) */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, NM_WIFI_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    /* 4. AccessPointAdded en cualquier adaptador wireless */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_WIFI_IFACE,
        "AccessPointAdded", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    /* 5. AccessPointRemoved en cualquier adaptador wireless */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_WIFI_IFACE,
        "AccessPointRemoved", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    /* 6. DeviceAdded / DeviceRemoved en el objeto raíz */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_IFACE,
        "DeviceAdded", NM_OBJECT_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_IFACE,
        "DeviceRemoved", NM_OBJECT_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_nm_signal, sd, NULL);

    /* Guardamos sd para poder liberarlo luego.
     * Cuidado: si hay dos llamadores (plugin y popup), el segundo pisa al primero.
     * Por eso indexamos por puntero al ids para que sean independientes. */
    gchar *key = g_strdup_printf ("nm-signal-data-%p", (void *) ids);
    g_object_set_data_full (G_OBJECT (conn), key, sd, g_free);
    g_free (key);

    return ids;
}

void
nm_unsubscribe_signals (GDBusConnection *conn, guint *ids)
{
    if (!ids) return;
    for (gint i = 0; ids[i] != 0; i++)
        g_dbus_connection_signal_unsubscribe (conn, ids[i]);

    gchar *key = g_strdup_printf ("nm-signal-data-%p", (void *) ids);
    g_object_set_data (G_OBJECT (conn), key, NULL);
    g_free (key);

    g_free (ids);
}

gboolean
nm_any_wifi_device_connecting (GDBusConnection *conn)
{
    gboolean found = FALSE;
    GSList  *devs  = nm_get_wifi_devices (conn);
    for (GSList *l = devs; l && !found; l = l->next) {
        NmDevice *dev = l->data;
        GVariant *v   = get_property (conn, dev->object_path,
                                      NM_DEVICE_IFACE, "State");
        if (v) {
            guint32 state = g_variant_get_uint32 (v);
            /* 40=PREPARE 50=CONFIG 60=NEED_AUTH 70=IP_CONFIG 80=IP_CHECK 90=SECONDARIES */
            if (state >= 40 && state <= 90)
                found = TRUE;
            g_variant_unref (v);
        }
    }
    nm_device_list_free (devs);
    return found;
}

void
nm_request_scan (GDBusConnection *conn, const gchar *device_path)
{
    GVariantBuilder options;
    g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));

    g_dbus_connection_call (
        conn, NM_BUS_NAME, device_path, NM_WIFI_IFACE,
        "RequestScan",
        g_variant_new ("(a{sv})", &options),
        NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_async_done, "RequestScan");
}
