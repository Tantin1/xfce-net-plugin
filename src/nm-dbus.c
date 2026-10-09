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

/* Quita sufijos corporativos del vendor ("Corporation", "Inc.", etc.)
 * Modifica el string in-place. */
static void
simplify_vendor (gchar *vendor)
{
    static const gchar *suffixes[] = {
        " Corporation", " Corp.", " Corp", " Incorporated",
        " Inc.", " Inc", " Ltd.", " Ltd", " Co.", " Co",
        " GmbH", " S.A.", " Systems", NULL
    };
    if (!vendor) return;
    gsize vlen = strlen (vendor);
    for (gint i = 0; suffixes[i]; i++) {
        gsize slen = strlen (suffixes[i]);
        if (vlen > slen && g_str_has_suffix (vendor, suffixes[i])) {
            vendor[vlen - slen] = '\0';
            break;
        }
    }
}

/* Replica la lógica de nm_device_get_description() de libnm usando
 * los datos de udev en /run/udev/data/n<ifindex>. Sin dependencias nuevas.
 * Liberar con g_free(). */
gchar *
nm_get_device_description (const gchar *iface)
{
    gchar *ifindex_path = g_strdup_printf ("/sys/class/net/%s/ifindex", iface);
    gchar *ifindex_str  = NULL;
    if (!g_file_get_contents (ifindex_path, &ifindex_str, NULL, NULL)) {
        g_free (ifindex_path);
        return NULL;
    }
    g_free (ifindex_path);
    g_strstrip (ifindex_str);

    gchar *udev_path = g_strdup_printf ("/run/udev/data/n%s", ifindex_str);
    g_free (ifindex_str);
    gchar *udev_data = NULL;
    if (!g_file_get_contents (udev_path, &udev_data, NULL, NULL)) {
        g_free (udev_path);
        return NULL;
    }
    g_free (udev_path);

    gchar *model_from_db  = NULL;
    gchar *model          = NULL;
    gchar *vendor_from_db = NULL;
    gchar *vendor         = NULL;

    gchar **lines = g_strsplit (udev_data, "\n", -1);
    g_free (udev_data);

    for (gint i = 0; lines[i]; i++) {
        const gchar *line = lines[i];
        if (g_str_has_prefix (line, "E:ID_MODEL_FROM_DATABASE=") && !model_from_db)
            model_from_db = g_strdup (line + strlen ("E:ID_MODEL_FROM_DATABASE="));
        else if (g_str_has_prefix (line, "E:ID_MODEL=") && !model)
            model = g_strdup (line + strlen ("E:ID_MODEL="));
        else if (g_str_has_prefix (line, "E:ID_VENDOR_FROM_DATABASE=") && !vendor_from_db)
            vendor_from_db = g_strdup (line + strlen ("E:ID_VENDOR_FROM_DATABASE="));
        else if (g_str_has_prefix (line, "E:ID_VENDOR=") && !vendor)
            vendor = g_strdup (line + strlen ("E:ID_VENDOR="));
    }
    g_strfreev (lines);

    /* Elegir vendor y product más informativos */
    gchar *v = g_strdup (vendor_from_db ? vendor_from_db : vendor);
    gchar *p = g_strdup (model_from_db  ? model_from_db  : model);

    g_free (model_from_db);
    g_free (model);
    g_free (vendor_from_db);
    g_free (vendor);

    if (!v && !p)
        return NULL;

    /* Simplificar vendor: quitar sufijos corporativos */
    if (v)
        simplify_vendor (v);

    gchar *desc = NULL;

    if (p) {
        /* Quitar contenido entre corchetes, ej: " [Stone Peak]" */
        gchar *bracket = strchr (p, '[');
        if (bracket) {
            while (bracket > p && *(bracket - 1) == ' ')
                bracket--;
            *bracket = '\0';
        }
        g_strstrip (p);

        /* Quitar prefijo vendor del product si coincide */
        if (v) {
            for (gint pass = 0; pass < 2; pass++) {
                /* pass 0: vendor simplificado, pass 1: vendor original (ya simplificado) */
                if (g_ascii_strncasecmp (p, v, strlen (v)) == 0) {
                    gchar *tmp = g_strdup (g_strstrip (p + strlen (v)));
                    g_free (p);
                    p = tmp;
                    break;
                }
            }
        }

        /* Quitar prefijos genéricos de ancho de banda */
        static const gchar *band_prefixes[] = {
            "Dual Band ", "Dual-Band ", "Single Band ", NULL
        };
        for (gint i = 0; band_prefixes[i]; i++) {
            if (g_str_has_prefix (p, band_prefixes[i])) {
                gchar *tmp = g_strdup (p + strlen (band_prefixes[i]));
                g_free (p);
                p = tmp;
                break;
            }
        }

        /* Products genéricos → reemplazar con "Wi-Fi" */
        if (g_regex_match_simple ("^802\\.11\\w*( NIC)?$", p, G_REGEX_CASELESS, 0) ||
            g_strcmp0 (p, "NIC") == 0 ||
            g_strcmp0 (p, "Wireless NIC") == 0 ||
            g_strcmp0 (p, "WLAN") == 0) {
            g_free (p);
            p = g_strdup ("Wi-Fi");
        }
    }

    /* Combinar: si vendor ya está en p, usar solo p */
    if (v && p) {
        if (g_strstr_len (p, -1, v))
            desc = g_strdup (p);
        else
            desc = g_strconcat (v, " ", p, NULL);
    } else if (p) {
        desc = g_strdup (p);
    } else {
        desc = g_strdup (v);
    }

    g_free (v);
    g_free (p);
    return desc;
}

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

/* ---------- caché de instantánea (ObjectManager) ----------
 *
 * El gestor de red expone el estándar "ObjectManager": una sola llamada al
 * bus (GetManagedObjects) devuelve TODOS sus objetos (dispositivos, puntos
 * de acceso, conexiones activas) con TODAS sus propiedades.
 *
 * nm_cache_begin() toma esa instantánea; mientras esté activa, get_property
 * y get_all_properties resuelven desde memoria, con cero llamadas al bus.
 * nm_cache_end() la libera. Anidable (contador); si la llamada inicial
 * falla, todo sigue funcionando con llamadas sueltas como antes.
 *
 * OJO: la instantánea es una foto. Solo usarla en bloques cortos de lectura
 * (armar la UI); nunca mantenerla abierta esperando que cambie. */

static GVariant *om_snapshot = NULL;   /* tipo a{oa{sa{sv}}} */
static gint      om_depth    = 0;

void
nm_cache_begin (GDBusConnection *conn)
{
    om_depth++;
    if (om_depth > 1)
        return;

    GError   *err = NULL;
    GVariant *result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, "/org/freedesktop",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (result) {
        om_snapshot = g_variant_get_child_value (result, 0);
        g_variant_unref (result);
    } else {
        g_warning ("nm-dbus: GetManagedObjects: %s "
                   "(sin instantánea; se sigue con llamadas sueltas)",
                   err->message);
        g_error_free (err);
        om_snapshot = NULL;
    }
}

void
nm_cache_end (void)
{
    if (om_depth <= 0) {
        g_warning ("nm-dbus: nm_cache_end sin nm_cache_begin");
        return;
    }
    om_depth--;
    if (om_depth == 0 && om_snapshot) {
        g_variant_unref (om_snapshot);
        om_snapshot = NULL;
    }
}

/* Busca en la instantánea el diccionario de propiedades de una interfaz de
 * un objeto. Devuelve una referencia nueva, o NULL si no hay instantánea o
 * el objeto/interfaz no figura. */
static GVariant *
snapshot_get_props (const gchar *object_path, const gchar *iface)
{
    if (!om_snapshot)
        return NULL;

    GVariant *ifaces = g_variant_lookup_value (om_snapshot, object_path,
                                               G_VARIANT_TYPE ("a{sa{sv}}"));
    if (!ifaces)
        return NULL;

    GVariant *props = g_variant_lookup_value (ifaces, iface,
                                              G_VARIANT_TYPE ("a{sv}"));
    g_variant_unref (ifaces);
    return props;
}

static GVariant *
get_property (GDBusConnection *conn,
              const gchar     *object_path,
              const gchar     *iface,
              const gchar     *prop)
{
    GVariant *result, *value = NULL;
    GError   *err = NULL;

    /* Primero la instantánea, si hay una activa: cero llamadas al bus. */
    GVariant *props = snapshot_get_props (object_path, iface);
    if (props) {
        value = g_variant_lookup_value (props, prop, NULL);
        g_variant_unref (props);
        if (value)
            return value;
        /* Propiedad ausente en la instantánea: caer a la llamada directa. */
    }

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

/* Trae TODAS las propiedades de una interfaz en una sola llamada al bus
 * (método GetAll), en vez de una llamada por propiedad. Devuelve un
 * diccionario {s,v} a liberar con g_variant_unref(), o NULL si falló. */
static GVariant *
get_all_properties (GDBusConnection *conn,
                    const gchar     *object_path,
                    const gchar     *iface)
{
    GError   *err = NULL;

    /* Primero la instantánea, si hay una activa: cero llamadas al bus. */
    GVariant *props = snapshot_get_props (object_path, iface);
    if (props)
        return props;

    GVariant *result = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new ("(s)", iface),
        G_VARIANT_TYPE ("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);

    if (!result) {
        g_warning ("nm-dbus: GetAll %s en %s: %s",
                   iface, object_path, err->message);
        g_error_free (err);
        return NULL;
    }

    GVariant *dict = g_variant_get_child_value (result, 0);
    g_variant_unref (result);
    return dict;
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
    GVariant    *paths_v;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;

    /* Propiedad "Devices" en vez del método GetDevices: mismo contenido,
     * pero al ser propiedad se resuelve gratis desde la instantánea. */
    paths_v = get_property (conn, NM_OBJECT_PATH, NM_IFACE, "Devices");
    if (!paths_v)
        return NULL;
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        /* Una sola llamada GetAll por dispositivo en vez de un Get por propiedad. */
        GVariant *props = get_all_properties (conn, paths[i], NM_DEVICE_IFACE);
        if (!props) continue;

        guint32      dev_type = 0;
        const gchar *iface    = NULL;
        g_variant_lookup (props, "DeviceType", "u",  &dev_type);
        g_variant_lookup (props, "Interface",  "&s", &iface);

        if (dev_type != NM_DEVICE_TYPE_WIFI || !iface) {
            g_variant_unref (props);
            continue;
        }

        NmDevice *dev    = g_new0 (NmDevice, 1);
        dev->iface       = g_strdup (iface);
        dev->object_path = g_strdup (paths[i]);
        dev->description = nm_get_device_description (dev->iface);
        g_variant_unref (props);

        list = g_slist_append (list, dev);
    }

    g_free (paths);
    g_variant_unref (paths_v);
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
        g_free (dev->description);
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

NmActiveApInfo *
nm_get_active_ap_info (GDBusConnection *conn)
{
    NmActiveApInfo *info = g_new0 (NmActiveApInfo, 1);
    GSList *devices = nm_get_wifi_devices (conn);

    for (GSList *l = devices; l && !info->connected; l = l->next) {
        NmDevice *dev     = l->data;
        gchar    *ap_path = get_active_ap_path (conn, dev->object_path);
        if (!ap_path)
            continue;

        /* Una sola llamada GetAll en vez de cinco Get. */
        GVariant *props = get_all_properties (conn, ap_path, NM_AP_IFACE);

        info->connected = TRUE;

        if (props) {
            guchar  strength = 0;
            guint32 wpa = 0, rsn = 0, freq = 0;
            g_variant_lookup (props, "Strength",  "y", &strength);
            g_variant_lookup (props, "WpaFlags",  "u", &wpa);
            g_variant_lookup (props, "RsnFlags",  "u", &rsn);
            g_variant_lookup (props, "Frequency", "u", &freq);

            info->strength = (gint) strength;
            info->secure   = (wpa | rsn) != 0;

            if (freq >= 5925)      info->band = g_strdup ("6G");
            else if (freq >= 5000) info->band = g_strdup ("5G");
            else if (freq > 0)     info->band = g_strdup ("2.4G");

            GVariant *ssid_v = g_variant_lookup_value (props, "Ssid",
                                                       G_VARIANT_TYPE ("ay"));
            if (ssid_v) {
                gsize         len;
                const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
                gchar        *raw   = g_strndup ((const gchar *) bytes, len);
                /* SSIDs son bytes sin codificación garantizada: forzar UTF-8
                 * válido para que el dibujado de etiquetas nunca se rompa. */
                info->ssid = g_utf8_make_valid (raw, -1);
                g_free (raw);
                g_variant_unref (ssid_v);
            }
            g_variant_unref (props);
        }
        g_free (ap_path);
    }

    nm_device_list_free (devices);
    return info;
}

void
nm_active_ap_info_free (NmActiveApInfo *info)
{
    if (!info) return;
    g_free (info->ssid);
    g_free (info->band);
    g_free (info);
}

/* Banda a partir de la frecuencia, para agrupar duplicados. */
static gint
ap_band_of (guint frequency)
{
    if (frequency >= 5925) return 6;
    if (frequency >= 5000) return 5;
    return 2;
}

/* Agrupa puntos de acceso repetidos (mismo SSID y misma banda: repetidores,
 * redes en malla). Conserva el activo si lo hay; si no, el de mejor señal.
 * Libera los descartados y devuelve la lista nueva. */
static GSList *
ap_list_dedupe (GSList *list)
{
    GSList *out = NULL;

    for (GSList *l = list; l; l = l->next) {
        NmAccessPoint *ap   = l->data;
        GSList        *node = NULL;

        for (GSList *o = out; o; o = o->next) {
            NmAccessPoint *cand = o->data;
            if (g_strcmp0 (cand->ssid, ap->ssid) == 0 &&
                ap_band_of (cand->frequency) == ap_band_of (ap->frequency)) {
                node = o;
                break;
            }
        }

        if (!node) {
            out = g_slist_append (out, ap);
            continue;
        }

        NmAccessPoint *kept    = node->data;
        gboolean       replace = (ap->active && !kept->active) ||
                                 (ap->active == kept->active &&
                                  ap->strength > kept->strength);
        NmAccessPoint *loser   = replace ? kept : ap;
        if (replace)
            node->data = ap;

        g_free (loser->ssid);
        g_free (loser->object_path);
        g_free (loser->bssid);
        g_free (loser);
    }

    g_slist_free (list);
    return out;
}

GSList *
nm_get_access_points (GDBusConnection *conn, const gchar *device_path)
{
    GVariant    *paths_v;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;
    gchar       *active_path;

    active_path = get_active_ap_path (conn, device_path);

    /* Propiedad "AccessPoints" en vez del método GetAllAccessPoints: mismo
     * contenido útil (los AP ocultos traen SSID vacío y se saltean igual),
     * pero al ser propiedad se resuelve gratis desde la instantánea. */
    paths_v = get_property (conn, device_path, NM_WIFI_IFACE, "AccessPoints");
    if (!paths_v) {
        g_free (active_path);
        return NULL;
    }
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        /* Una sola llamada GetAll por AP en vez de seis Get por propiedad. */
        GVariant *props = get_all_properties (conn, paths[i], NM_AP_IFACE);
        if (!props) continue;

        GVariant *ssid_v = g_variant_lookup_value (props, "Ssid",
                                                   G_VARIANT_TYPE ("ay"));
        if (!ssid_v) {
            g_variant_unref (props);
            continue;
        }

        gsize         len;
        const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
        gchar        *raw   = g_strndup ((const gchar *) bytes, len);
        g_variant_unref (ssid_v);

        if (!raw || !*raw) {
            g_free (raw);
            g_variant_unref (props);
            continue;
        }

        /* SSIDs son bytes sin codificación garantizada: forzar UTF-8 válido
         * para que el dibujado de etiquetas nunca se rompa. */
        gchar *ssid = g_utf8_make_valid (raw, -1);
        g_free (raw);

        guchar       strength = 0;
        guint32      wpa = 0, rsn = 0, freq = 0;
        const gchar *bssid = NULL;
        g_variant_lookup (props, "Strength",  "y",  &strength);
        g_variant_lookup (props, "WpaFlags",  "u",  &wpa);
        g_variant_lookup (props, "RsnFlags",  "u",  &rsn);
        g_variant_lookup (props, "Frequency", "u",  &freq);
        g_variant_lookup (props, "HwAddress", "&s", &bssid);

        NmAccessPoint *ap = g_new0 (NmAccessPoint, 1);
        ap->ssid        = ssid;
        ap->object_path = g_strdup (paths[i]);
        ap->bssid       = g_strdup (bssid);
        ap->strength    = (gint) strength;
        ap->frequency   = freq;
        ap->wpa_flags   = wpa;
        ap->rsn_flags   = rsn;
        ap->secure      = ((wpa | rsn) != 0);
        ap->active      = (active_path && strcmp (paths[i], active_path) == 0);

        g_variant_unref (props);

        list = g_slist_append (list, ap);
    }

    g_free (active_path);
    g_free (paths);
    g_variant_unref (paths_v);

    /* Agrupar repetidores: una sola fila por SSID+banda. */
    return ap_list_dedupe (list);
}

void
nm_ap_list_free (GSList *list)
{
    GSList *l;
    for (l = list; l; l = l->next) {
        NmAccessPoint *ap = l->data;
        g_free (ap->ssid);
        g_free (ap->object_path);
        g_free (ap->bssid);
        g_free (ap);
    }
    g_slist_free (list);
}

/* ---------- detalles de conexión activa (IP, gateway, DNS) ---------- */

NmConnectionDetails *
nm_get_connection_details (GDBusConnection *conn, const gchar *device_path)
{
    NmConnectionDetails *d = g_new0 (NmConnectionDetails, 1);

    /* IP4Config */
    GVariant *ip4_v = get_property (conn, device_path,
                                    "org.freedesktop.NetworkManager.Device",
                                    "Ip4Config");
    if (ip4_v) {
        const gchar *ip4_path = g_variant_get_string (ip4_v, NULL);
        if (ip4_path && g_strcmp0 (ip4_path, "/") != 0) {
            /* Addresses: array of (array of uint32) */
            GVariant *addr_v = get_property (conn, ip4_path,
                "org.freedesktop.NetworkManager.IP4Config", "AddressData");
            if (addr_v) {
                if (g_variant_n_children (addr_v) > 0) {
                    GVariant *first = g_variant_get_child_value (addr_v, 0);
                    GVariant *addr_str = g_variant_lookup_value (first, "address",
                                            G_VARIANT_TYPE_STRING);
                    GVariant *prefix_v = g_variant_lookup_value (first, "prefix",
                                            G_VARIANT_TYPE_UINT32);
                    if (addr_str)
                        d->ip4_address = g_strdup (g_variant_get_string (addr_str, NULL));
                    if (prefix_v)
                        d->ip4_prefix = g_strdup_printf ("%u",
                                            g_variant_get_uint32 (prefix_v));
                    if (addr_str) g_variant_unref (addr_str);
                    if (prefix_v) g_variant_unref (prefix_v);
                    g_variant_unref (first);
                }
                g_variant_unref (addr_v);
            }
            GVariant *gw_v = get_property (conn, ip4_path,
                "org.freedesktop.NetworkManager.IP4Config", "Gateway");
            if (gw_v) {
                d->ip4_gateway = g_strdup (g_variant_get_string (gw_v, NULL));
                g_variant_unref (gw_v);
            }
            GVariant *dns_v = get_property (conn, ip4_path,
                "org.freedesktop.NetworkManager.IP4Config", "NameserverData");
            if (dns_v) {
                GString *dns_str = g_string_new (NULL);
                for (gsize i = 0; i < g_variant_n_children (dns_v); i++) {
                    GVariant *ns = g_variant_get_child_value (dns_v, i);
                    GVariant *ns_addr = g_variant_lookup_value (ns, "address",
                                            G_VARIANT_TYPE_STRING);
                    if (ns_addr) {
                        if (dns_str->len > 0) g_string_append (dns_str, ", ");
                        g_string_append (dns_str, g_variant_get_string (ns_addr, NULL));
                        g_variant_unref (ns_addr);
                    }
                    g_variant_unref (ns);
                }
                d->ip4_dns = g_string_free (dns_str, FALSE);
                g_variant_unref (dns_v);
            }
        }
        g_variant_unref (ip4_v);
    }

    /* IP6Config — solo primera dirección global */
    GVariant *ip6_v = get_property (conn, device_path,
                                    "org.freedesktop.NetworkManager.Device",
                                    "Ip6Config");
    if (ip6_v) {
        const gchar *ip6_path = g_variant_get_string (ip6_v, NULL);
        if (ip6_path && g_strcmp0 (ip6_path, "/") != 0) {
            GVariant *a6_v = get_property (conn, ip6_path,
                "org.freedesktop.NetworkManager.IP6Config", "AddressData");
            if (a6_v) {
                for (gsize i = 0; i < g_variant_n_children (a6_v); i++) {
                    GVariant *entry = g_variant_get_child_value (a6_v, i);
                    GVariant *addr6 = g_variant_lookup_value (entry, "address",
                                          G_VARIANT_TYPE_STRING);
                    if (addr6) {
                        const gchar *s = g_variant_get_string (addr6, NULL);
                        /* Preferir dirección global (no link-local fe80::) */
                        if (!d->ip6_address ||
                            (g_str_has_prefix (d->ip6_address, "fe80") &&
                             !g_str_has_prefix (s, "fe80"))) {
                            g_free (d->ip6_address);
                            d->ip6_address = g_strdup (s);
                        }
                        g_variant_unref (addr6);
                    }
                    g_variant_unref (entry);
                }
                g_variant_unref (a6_v);
            }
        }
        g_variant_unref (ip6_v);
    }

    return d;
}

void
nm_connection_details_free (NmConnectionDetails *d)
{
    if (!d) return;
    g_free (d->ip4_address);
    g_free (d->ip4_prefix);
    g_free (d->ip4_gateway);
    g_free (d->ip4_dns);
    g_free (d->ip6_address);
    g_free (d);
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

/* ---------- conexiones guardadas (perfiles) ----------
 *
 * Leer los perfiles guardados es caro: una llamada para la lista y otra
 * GetSettings por cada perfil. Antes eso se repetía varias veces en cada
 * refresco del popup (redes guardadas por adaptador, lista de VPN dos veces,
 * "conectar automáticamente" una vez por fila).
 *
 * Ahora la lista se lee una vez y queda en memoria (profiles_cache). Se
 * descarta sola cuando NetworkManager avisa que un perfil se agregó, se borró
 * o se modificó (señales de Settings, suscriptas en nm_subscribe_signals), y
 * también después de cada escritura propia (borrar, modificar, crear).
 *
 * Seguridad: si no hay ninguna suscripción activa a señales (profiles_watchers
 * en 0), nadie avisaría de cambios, así que en ese caso la lista NO se
 * conserva y cada consulta lee de nuevo, como antes.
 *
 * Regla para quien la use: obtener la lista con profiles_get() y copiar lo
 * necesario ANTES de llamar a cualquier otra función que pueda volver a
 * leerla o descartarla. */

typedef struct {
    gchar    *path;          /* ruta del objeto del perfil en el bus */
    gchar    *type;          /* connection.type: "802-11-wireless", "vpn", ... */
    gchar    *id;            /* connection.id: nombre visible del perfil */
    gchar    *uuid;          /* connection.uuid */
    gchar    *ssid;          /* nombre de red Wi-Fi, o NULL si no es Wi-Fi */
    gchar    *key_mgmt;      /* 802-11-wireless-security.key-mgmt, NULL = abierta */
    gchar    *mode;          /* 802-11-wireless.mode: "infrastructure", "ap"... */
    gboolean  autoconnect;   /* connection.autoconnect (NM asume TRUE si falta) */
    gboolean  has_iface;     /* connection.interface-name fijado y no vacío */
} ProfileInfo;

static GSList   *profiles_cache    = NULL;
static gboolean  profiles_valid    = FALSE;
static gint      profiles_watchers = 0;

static void
profile_info_free (gpointer p)
{
    ProfileInfo *pi = p;
    g_free (pi->path);
    g_free (pi->type);
    g_free (pi->id);
    g_free (pi->uuid);
    g_free (pi->ssid);
    g_free (pi->key_mgmt);
    g_free (pi->mode);
    g_free (pi);
}

/* Descarta la lista en memoria; la próxima consulta la vuelve a leer. */
static void
profiles_invalidate (void)
{
    g_slist_free_full (profiles_cache, profile_info_free);
    profiles_cache = NULL;
    profiles_valid = FALSE;
}

/* Lectura completa desde el bus (ListConnections + GetSettings por perfil). */
static GSList *
profiles_read (GDBusConnection *conn)
{
    GSList       *list = NULL;
    GError       *err  = NULL;
    GVariant     *result, *paths_v;
    const gchar **paths;
    gsize         n, i;

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

    for (i = 0; i < n; i++) {
        GVariant *settings = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, paths[i], NM_CONN_IFACE,
            "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
        if (!settings) continue;

        GVariant    *outer = g_variant_get_child_value (settings, 0);
        ProfileInfo *pi    = g_new0 (ProfileInfo, 1);
        pi->path        = g_strdup (paths[i]);
        pi->autoconnect = TRUE;

        GVariant *conn_dict = NULL;
        g_variant_lookup (outer, "connection", "@a{sv}", &conn_dict);
        if (conn_dict) {
            const gchar *s = NULL;
            gboolean     b = TRUE;
            if (g_variant_lookup (conn_dict, "type", "&s", &s)) pi->type = g_strdup (s);
            if (g_variant_lookup (conn_dict, "id",   "&s", &s)) pi->id   = g_strdup (s);
            if (g_variant_lookup (conn_dict, "uuid", "&s", &s)) pi->uuid = g_strdup (s);
            if (g_variant_lookup (conn_dict, "autoconnect", "b", &b))
                pi->autoconnect = b;
            if (g_variant_lookup (conn_dict, "interface-name", "&s", &s) && s && *s)
                pi->has_iface = TRUE;
            g_variant_unref (conn_dict);
        }

        GVariant *wifi_dict = NULL;
        g_variant_lookup (outer, "802-11-wireless", "@a{sv}", &wifi_dict);
        if (wifi_dict) {
            GVariant *ssid_v = NULL;
            g_variant_lookup (wifi_dict, "ssid", "@ay", &ssid_v);
            if (ssid_v) {
                gsize         len;
                const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
                gchar        *raw   = g_strndup ((const gchar *) bytes, len);
                pi->ssid = g_utf8_make_valid (raw, -1);
                g_free (raw);
                g_variant_unref (ssid_v);
            }
            const gchar *m = NULL;
            if (g_variant_lookup (wifi_dict, "mode", "&s", &m))
                pi->mode = g_strdup (m);
            g_variant_unref (wifi_dict);
        }

        GVariant *sec_dict = NULL;
        g_variant_lookup (outer, "802-11-wireless-security", "@a{sv}", &sec_dict);
        if (sec_dict) {
            const gchar *k = NULL;
            if (g_variant_lookup (sec_dict, "key-mgmt", "&s", &k))
                pi->key_mgmt = g_strdup (k);
            g_variant_unref (sec_dict);
        }

        g_variant_unref (outer);
        g_variant_unref (settings);
        list = g_slist_prepend (list, pi);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    g_variant_unref (result);
    /* Conservar el orden en que NM los listó. */
    return g_slist_reverse (list);
}

/* Devuelve la lista de perfiles (propiedad de este módulo: NO liberar). */
static GSList *
profiles_get (GDBusConnection *conn)
{
    if (profiles_valid && profiles_watchers > 0)
        return profiles_cache;

    profiles_invalidate ();
    profiles_cache = profiles_read (conn);
    profiles_valid = (profiles_watchers > 0);
    return profiles_cache;
}

/* ¿Es un perfil para CONECTARSE a una red Wi-Fi con ese nombre? Deja afuera
 * los perfiles de punto de acceso propio (modo "ap", el hotspot): que exista
 * un hotspot llamado igual que una red no la convierte en "guardada", ni
 * olvidar la red debe borrar el hotspot. */
static gboolean
profile_is_client_for (const ProfileInfo *pi, const gchar *ssid)
{
    return pi->ssid && g_strcmp0 (pi->ssid, ssid) == 0 &&
           g_strcmp0 (pi->mode, "ap") != 0;
}

static gchar *
find_connection_path_by_ssid (GDBusConnection *conn, const gchar *ssid)
{
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (profile_is_client_for (pi, ssid))
            return g_strdup (pi->path);
    }
    return NULL;
}

/* Busca un perfil por su ruta en la lista en memoria (o NULL). */
static ProfileInfo *
profile_by_path (GDBusConnection *conn, const gchar *path)
{
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (g_strcmp0 (pi->path, path) == 0)
            return pi;
    }
    return NULL;
}

/* ¿El tipo de seguridad del perfil sirve para este punto de acceso?
 * Mismas banderas que usa el popup (WpaFlags/RsnFlags). */
#define AP_KM_PSK    0x00000100
#define AP_KM_8021X  0x00000200
#define AP_KM_SAE    0x00000400

static gboolean
profile_fits_ap (const ProfileInfo *pi, guint32 wpa_flags, guint32 rsn_flags)
{
    guint32      f = wpa_flags | rsn_flags;
    const gchar *k = pi->key_mgmt;

    if (f == 0)                                      /* abierta (o WEP) */
        return k == NULL || g_strcmp0 (k, "none") == 0;
    if ((f & AP_KM_8021X) && !(f & (AP_KM_PSK | AP_KM_SAE)))   /* empresarial */
        return g_strcmp0 (k, "wpa-eap") == 0 || g_strcmp0 (k, "ieee8021x") == 0;
    if (f & AP_KM_PSK) {
        if (g_strcmp0 (k, "wpa-psk") == 0)
            return TRUE;
        return (rsn_flags & AP_KM_SAE) && g_strcmp0 (k, "sae") == 0;  /* mixta */
    }
    if (rsn_flags & AP_KM_SAE)                       /* WPA3 puro */
        return g_strcmp0 (k, "sae") == 0;
    return FALSE;
}

gchar *
nm_find_profile_for_ap (GDBusConnection *conn, const gchar *ssid,
                        guint32 wpa_flags, guint32 rsn_flags)
{
    const gchar *fallback = NULL;
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (!profile_is_client_for (pi, ssid))
            continue;
        if (profile_fits_ap (pi, wpa_flags, rsn_flags))
            return g_strdup (pi->path);
        if (!fallback)
            fallback = pi->path;
    }
    /* Ninguno con seguridad compatible: el primero con ese nombre, como
     * antes (mejor intentar que no ofrecer nada). */
    return fallback ? g_strdup (fallback) : NULL;
}

gchar *
nm_get_device_active_profile (GDBusConnection *conn, const gchar *device_path)
{
    GVariant *ac_v = get_property (conn, device_path, NM_DEVICE_IFACE,
                                   "ActiveConnection");
    if (!ac_v) return NULL;
    const gchar *ac_path = g_variant_get_string (ac_v, NULL);
    gchar       *result  = NULL;
    if (ac_path && g_strcmp0 (ac_path, "/") != 0) {
        GVariant *c_v = get_property (conn, ac_path,
            "org.freedesktop.NetworkManager.Connection.Active", "Connection");
        if (c_v) {
            const gchar *p = g_variant_get_string (c_v, NULL);
            if (p && g_strcmp0 (p, "/") != 0)
                result = g_strdup (p);
            g_variant_unref (c_v);
        }
    }
    g_variant_unref (ac_v);
    return result;
}

gboolean
nm_delete_profile (GDBusConnection *conn, const gchar *profile_path)
{
    if (!profile_path) return FALSE;
    GError   *err = NULL;
    GVariant *r   = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, profile_path, NM_CONN_IFACE,
        "Delete", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (!r) {
        g_warning ("nm-dbus: Delete %s: %s", profile_path, err->message);
        g_error_free (err);
        return FALSE;
    }
    g_variant_unref (r);
    profiles_invalidate ();
    return TRUE;
}

gboolean
nm_has_saved_connection (GDBusConnection *conn, const gchar *ssid)
{
    gchar    *path  = find_connection_path_by_ssid (conn, ssid);
    gboolean  found = (path != NULL);
    g_free (path);
    return found;
}

GHashTable *
nm_get_saved_wifi_ssids (GDBusConnection *conn)
{
    GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, NULL);
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (pi->ssid && g_strcmp0 (pi->mode, "ap") != 0)
            g_hash_table_add (set, g_strdup (pi->ssid));
    }
    return set;
}

/* Devuelve copias de las rutas de todos los perfiles cuya red coincide con
 * `ssid`. Liberar con g_slist_free_full (lista, g_free). */
static GSList *
list_connection_paths_by_ssid (GDBusConnection *conn, const gchar *ssid)
{
    GSList *matches = NULL;
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (profile_is_client_for (pi, ssid))
            matches = g_slist_prepend (matches, g_strdup (pi->path));
    }
    return g_slist_reverse (matches);
}

gboolean
nm_forget_connection (GDBusConnection *conn, const gchar *ssid)
{
    /* Copiar las rutas ANTES de borrar: borrar descarta la lista en memoria. */
    GSList   *paths       = list_connection_paths_by_ssid (conn, ssid);
    gboolean  deleted_any = FALSE;

    for (GSList *l = paths; l; l = l->next) {
        GError   *derr = NULL;
        GVariant *dres = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, (const gchar *) l->data, NM_CONN_IFACE,
            "Delete", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &derr);
        if (dres) {
            g_variant_unref (dres);
            deleted_any = TRUE;
        } else {
            g_warning ("nm-dbus: Delete %s: %s",
                       (const gchar *) l->data, derr->message);
            g_error_free (derr);
        }
    }

    g_slist_free_full (paths, g_free);
    if (deleted_any)
        profiles_invalidate ();
    return deleted_any;
}

/* ---------- Migración: quitar interface-name de perfiles viejos ---------- */

/* Devuelve TRUE si el perfil en `conn_path` tiene `connection.interface-name`
 * fijado (no vacío). */
static gboolean
connection_has_interface_name (GDBusConnection *conn, const gchar *conn_path)
{
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (g_strcmp0 (pi->path, conn_path) == 0)
            return pi->has_iface;
    }
    return FALSE;
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
    profiles_invalidate ();
    return TRUE;
}

gboolean
nm_profile_cleanup (GDBusConnection *conn, const gchar *profile_path)
{
    ProfileInfo *me = profile_by_path (conn, profile_path);
    if (!me || !me->ssid)
        return FALSE;

    /* Copiar lo necesario antes de borrar (borrar descarta la lista). */
    gchar   *ssid     = g_strdup (me->ssid);
    gchar   *key_mgmt = g_strdup (me->key_mgmt);
    gboolean changed  = FALSE;

    /* Duplicados de verdad: misma red, mismo tipo de seguridad (los que se
     * creaban antes, uno por adaptador). Un perfil de la misma red con otra
     * seguridad NO es un duplicado y se respeta. */
    GSList *dups = NULL;
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (g_strcmp0 (pi->path, profile_path) != 0 &&
            profile_is_client_for (pi, ssid) &&
            g_strcmp0 (pi->key_mgmt, key_mgmt) == 0)
            dups = g_slist_prepend (dups, g_strdup (pi->path));
    }
    for (GSList *l = dups; l; l = l->next)
        if (nm_delete_profile (conn, l->data))
            changed = TRUE;
    g_slist_free_full (dups, g_free);

    /* Al perfil elegido se le saca interface-name si lo tiene. */
    if (connection_has_interface_name (conn, profile_path) &&
        connection_strip_interface_name (conn, profile_path))
        changed = TRUE;

    g_free (ssid);
    g_free (key_mgmt);
    return changed;
}

gboolean
nm_strip_interface_name (GDBusConnection *conn, const gchar *ssid)
{
    gchar   *path    = find_connection_path_by_ssid (conn, ssid);
    gboolean changed = path ? nm_profile_cleanup (conn, path) : FALSE;
    g_free (path);
    return changed;
}

/* ---------- ACCIÓN ASYNC: activar conexión guardada ---------- */

void
nm_activate_profile_async (GDBusConnection *conn,
                           const gchar     *profile_path,
                           const gchar     *device_path,
                           const gchar     *ap_path)
{
    if (!profile_path) return;
    g_dbus_connection_call (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "ActivateConnection",
        g_variant_new ("(ooo)", profile_path, device_path,
                       ap_path ? ap_path : "/"),
        G_VARIANT_TYPE ("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_async_done, "ActivateConnection");
}

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
    nm_activate_profile_async (conn, conn_path, device_path, ap_path);
    g_free (conn_path);
}

/* ---------- ACCIÓN ASYNC: añadir y activar conexión nueva ---------- */

/* Al terminar AddAndActivate hay un perfil nuevo: descartar la lista de
 * perfiles en memoria (la señal de NM también llega, esto es por las dudas). */
static void
on_add_and_activate_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    profiles_invalidate ();
    on_async_done (src, res, user_data);
}

void
nm_add_and_activate_connection_async (GDBusConnection *conn,
                                      const gchar     *device_path,
                                      const gchar     *ap_path,
                                      const gchar     *ssid,
                                      const gchar     *password,
                                      const gchar     *key_mgmt,
                                      gboolean         autoconnect,
                                      gboolean         hidden)
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
    /* Red oculta: sin esta marca NM no sale a buscarla activamente y muchos
     * routers nunca contestan. */
    if (hidden)
        g_variant_builder_add (&wifi_builder, "{sv}", "hidden",
                               g_variant_new_boolean (TRUE));

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
        /* "wpa-psk" sirve para WPA2 y para redes mixtas WPA2/WPA3; "sae" es
         * obligatorio en redes WPA3 puro (con wpa-psk fallarían siempre). */
        const gchar *km = (key_mgmt && *key_mgmt) ? key_mgmt : "wpa-psk";
        GVariantBuilder sec_builder;
        g_variant_builder_init (&sec_builder, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&sec_builder, "{sv}", "key-mgmt",
                               g_variant_new_string (km));
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
        on_add_and_activate_done, "AddAndActivateConnection");
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

guint32
nm_get_device_state (GDBusConnection *conn, const gchar *device_path)
{
    if (!device_path) return 0;
    GVariant *v = get_property (conn, device_path, NM_DEVICE_IFACE, "State");
    if (!v) return 0;
    guint32 state = g_variant_get_uint32 (v);
    g_variant_unref (v);
    return state;
}

gboolean
nm_get_device_enabled (GDBusConnection *conn, const gchar *device_path)
{
    /* State 20 = UNAVAILABLE (managed pero sin radio), 10 = UNMANAGED */
    return (nm_get_device_state (conn, device_path) > 20);
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
#define NM_DEVICE_STATE_UNMANAGED 10
#define NM_WIRED_IFACE            "org.freedesktop.NetworkManager.Device.Wired"

GSList *
nm_get_ethernet_devices (GDBusConnection *conn)
{
    GVariant    *paths_v;
    GSList      *list = NULL;
    gsize        n, i;
    const gchar **paths;

    /* Propiedad "Devices" en vez del método GetDevices: mismo contenido,
     * pero al ser propiedad se resuelve gratis desde la instantánea. */
    paths_v = get_property (conn, NM_OBJECT_PATH, NM_IFACE, "Devices");
    if (!paths_v)
        return NULL;
    paths = g_variant_get_objv (paths_v, &n);

    for (i = 0; i < n; i++) {
        /* Una sola llamada GetAll por dispositivo (DeviceType, State e
         * Interface juntos) en vez de un Get por propiedad. */
        GVariant *props = get_all_properties (conn, paths[i], NM_DEVICE_IFACE);
        if (!props) continue;

        guint32      dev_type = 0, state = 0;
        const gchar *iface    = NULL;
        g_variant_lookup (props, "DeviceType", "u",  &dev_type);
        g_variant_lookup (props, "State",      "u",  &state);
        g_variant_lookup (props, "Interface",  "&s", &iface);

        if (dev_type != NM_DEVICE_TYPE_ETHERNET || !iface) {
            g_variant_unref (props);
            continue;
        }

        /* Mostrar la sección si: está conectado (state 100), o el usuario lo
         * apagó (state 10 = no gestionado) para que el switch siga visible y
         * se pueda reactivar, o hay cable enchufado (propiedad Carrier de la
         * interfaz Wired) aunque todavía no esté conectado. Si está gestionado
         * pero sin cable (state 20), se oculta como antes. */
        gboolean show = (state == NM_DEVICE_STATE_ACTIVATED ||
                         state == NM_DEVICE_STATE_UNMANAGED);
        if (!show) {
            GVariant *carrier_v = get_property (conn, paths[i],
                                                NM_WIRED_IFACE, "Carrier");
            if (carrier_v) {
                show = g_variant_get_boolean (carrier_v);
                g_variant_unref (carrier_v);
            }
        }
        if (!show) {
            g_variant_unref (props);
            continue;
        }

        NmDevice *dev    = g_new0 (NmDevice, 1);
        dev->iface       = g_strdup (iface);
        dev->object_path = g_strdup (paths[i]);
        dev->description = nm_get_device_description (dev->iface);
        g_variant_unref (props);

        list = g_slist_append (list, dev);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    return list;
}

/* ---------- Ethernet conectado ---------- */

gboolean
nm_any_ethernet_activated (GDBusConnection *conn)
{
    gboolean      found = FALSE;
    gsize         n;
    GVariant     *paths_v = get_property (conn, NM_OBJECT_PATH, NM_IFACE, "Devices");
    if (!paths_v)
        return FALSE;
    const gchar **paths = g_variant_get_objv (paths_v, &n);

    for (gsize i = 0; i < n && !found; i++) {
        GVariant *props = get_all_properties (conn, paths[i], NM_DEVICE_IFACE);
        if (!props) continue;
        guint32 dev_type = 0, state = 0;
        g_variant_lookup (props, "DeviceType", "u", &dev_type);
        g_variant_lookup (props, "State",      "u", &state);
        if (dev_type == NM_DEVICE_TYPE_ETHERNET &&
            state == NM_DEVICE_STATE_ACTIVATED)
            found = TRUE;
        g_variant_unref (props);
    }

    g_free (paths);
    g_variant_unref (paths_v);
    return found;
}

/* ---------- autoconexión de perfiles Wi-Fi ---------- */

gboolean
nm_get_profile_autoconnect (GDBusConnection *conn, const gchar *profile_path)
{
    ProfileInfo *pi = profile_path ? profile_by_path (conn, profile_path) : NULL;
    return pi ? pi->autoconnect : TRUE;  /* sin perfil: el valor por defecto de NM */
}

gboolean
nm_get_autoconnect_by_ssid (GDBusConnection *conn, const gchar *ssid)
{
    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (profile_is_client_for (pi, ssid))
            return pi->autoconnect;
    }
    return TRUE; /* sin perfil: el valor por defecto de NM */
}

gboolean
nm_set_autoconnect_by_ssid (GDBusConnection *conn,
                             const gchar     *ssid,
                             gboolean         autoconnect)
{
    gchar   *path = find_connection_path_by_ssid (conn, ssid);
    gboolean ok   = path ? nm_set_profile_autoconnect (conn, path, autoconnect) : FALSE;
    g_free (path);
    return ok;
}

gboolean
nm_set_profile_autoconnect (GDBusConnection *conn,
                            const gchar     *profile_path,
                            gboolean         autoconnect)
{
    if (!profile_path) return FALSE;
    gchar *path = g_strdup (profile_path);

    GVariant *settings = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, path, NM_CONN_IFACE,
        "GetSettings", NULL, G_VARIANT_TYPE ("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!settings) { g_free (path); return FALSE; }

    GVariant *outer = g_variant_get_child_value (settings, 0);
    g_variant_unref (settings);

    GVariantBuilder top;
    g_variant_builder_init (&top, G_VARIANT_TYPE ("a{sa{sv}}"));

    GVariantIter iter_top;
    g_variant_iter_init (&iter_top, outer);
    const gchar *section;
    GVariant    *section_dict;
    gboolean     wrote = FALSE;

    while (g_variant_iter_next (&iter_top, "{s@a{sv}}", &section, &section_dict)) {
        GVariantBuilder sec_b;
        g_variant_builder_init (&sec_b, G_VARIANT_TYPE ("a{sv}"));
        GVariantIter iter_sec;
        g_variant_iter_init (&iter_sec, section_dict);
        const gchar *key;
        GVariant    *val;
        while (g_variant_iter_next (&iter_sec, "{sv}", &key, &val)) {
            if (g_strcmp0 (section, "connection") == 0 &&
                g_strcmp0 (key, "autoconnect") == 0) {
                g_variant_builder_add (&sec_b, "{sv}", "autoconnect",
                                       g_variant_new_boolean (autoconnect));
                wrote = TRUE;
                g_variant_unref (val);
            } else {
                g_variant_builder_add (&sec_b, "{sv}", key, val);
                g_variant_unref (val);
            }
            g_free ((gchar *) key);
        }
        if (g_strcmp0 (section, "connection") == 0 && !wrote) {
            g_variant_builder_add (&sec_b, "{sv}", "autoconnect",
                                   g_variant_new_boolean (autoconnect));
            wrote = TRUE;
        }
        g_variant_builder_add (&top, "{sa{sv}}", section, &sec_b);
        g_variant_unref (section_dict);
        g_free ((gchar *) section);
    }
    g_variant_unref (outer);

    GError   *err = NULL;
    GVariant *res = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, path, NM_CONN_IFACE,
        "Update",
        g_variant_new ("(a{sa{sv}})", &top),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
    g_free (path);
    if (res) { g_variant_unref (res); profiles_invalidate (); return TRUE; }
    if (err) { g_warning ("nm_set_profile_autoconnect: %s", err->message); g_error_free (err); }
    return FALSE;
}

/* ---------- VPN ---------- */

/* Conjunto de UUIDs (identificadores únicos) de las conexiones activas.
 * Liberar con g_hash_table_destroy(). */
static GHashTable *
get_active_uuids (GDBusConnection *conn)
{
    GHashTable *uuids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, NULL);
    GVariant   *array = get_property (conn, NM_OBJECT_PATH, NM_IFACE,
                                      "ActiveConnections");
    if (!array)
        return uuids;

    gsize an = g_variant_n_children (array);
    for (gsize ai = 0; ai < an; ai++) {
        GVariant    *path_v = g_variant_get_child_value (array, ai);
        const gchar *path   = g_variant_get_string (path_v, NULL);
        GVariant    *uuid_v = get_property (conn, path,
            "org.freedesktop.NetworkManager.Connection.Active", "Uuid");
        if (uuid_v) {
            g_hash_table_add (uuids,
                              g_strdup (g_variant_get_string (uuid_v, NULL)));
            g_variant_unref (uuid_v);
        }
        g_variant_unref (path_v);
    }
    g_variant_unref (array);
    return uuids;
}

GSList *
nm_get_vpn_connections (GDBusConnection *conn)
{
    GSList     *list         = NULL;
    GHashTable *active_uuids = get_active_uuids (conn);

    for (GSList *l = profiles_get (conn); l; l = l->next) {
        ProfileInfo *pi = l->data;
        if (!pi->type || !pi->id || !pi->uuid)
            continue;
        if (g_strcmp0 (pi->type, "wireguard") != 0 &&
            g_strcmp0 (pi->type, "vpn") != 0)
            continue;

        NmVpnConnection *vpn = g_new0 (NmVpnConnection, 1);
        vpn->name      = g_strdup (pi->id);
        vpn->uuid      = g_strdup (pi->uuid);
        vpn->conn_path = g_strdup (pi->path);
        vpn->active    = g_hash_table_contains (active_uuids, pi->uuid);
        list = g_slist_append (list, vpn);
    }

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
    gboolean  found = FALSE;
    /* Propiedad leída con get_property: si hay instantánea activa (refresco
     * del panel), no toca el bus. */
    GVariant *array = get_property (conn, NM_OBJECT_PATH, NM_IFACE,
                                    "ActiveConnections");
    if (!array) return FALSE;

    gsize n = g_variant_n_children (array);
    for (gsize i = 0; i < n && !found; i++) {
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

    g_variant_unref (array);
    return found;
}

/* ---------- contraseña del perfil guardado ---------- */

gchar *
nm_get_profile_password (GDBusConnection *conn, const gchar *profile_path)
{
    GVariant *secrets, *outer, *sec_dict = NULL, *psk_v = NULL;
    gchar    *password = NULL;

    if (!profile_path) return NULL;

    secrets = g_dbus_connection_call_sync (
        conn, NM_BUS_NAME, profile_path, NM_CONN_IFACE,
        "GetSecrets",
        g_variant_new ("(s)", "802-11-wireless-security"),
        G_VARIANT_TYPE ("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (!secrets) return NULL;

    outer = g_variant_get_child_value (secrets, 0);
    g_variant_lookup (outer, "802-11-wireless-security", "@a{sv}", &sec_dict);

    if (sec_dict) {
        /* psk_v arranca en NULL: si el perfil no tiene clave (WEP, abierta),
         * la búsqueda no lo toca. Antes quedaba sin inicializar y se leía
         * basura, con riesgo de cierre del plugin. */
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

gchar *
nm_get_saved_password (GDBusConnection *conn, const gchar *ssid)
{
    gchar *path = find_connection_path_by_ssid (conn, ssid);
    gchar *pw   = nm_get_profile_password (conn, path);
    g_free (path);
    return pw;
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

/* Señales de perfiles guardados (alta, baja, modificación): primero se
 * descarta la lista de perfiles en memoria y después se avisa igual que con
 * cualquier otro cambio. */
static void
on_settings_signal (GDBusConnection *conn,
                    const gchar     *sender,
                    const gchar     *object_path,
                    const gchar     *iface,
                    const gchar     *signal_name,
                    GVariant        *params,
                    gpointer         user_data)
{
    profiles_invalidate ();
    on_nm_signal (conn, sender, object_path, iface, signal_name, params,
                  user_data);
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

    /* 8-10. Perfiles guardados: alta (NewConnection), baja
     *       (ConnectionRemoved) y modificación (Updated, en cualquier perfil).
     *       Cubre lo que hace este plugin y lo que se haga desde afuera
     *       (nmcli, editor de conexiones). Antes no se escuchaban: borrar un
     *       perfil no refrescaba el popup por sí solo. */
    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_SETTINGS_IFACE,
        "NewConnection", NM_SETTINGS_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_settings_signal, sd, NULL);

    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_SETTINGS_IFACE,
        "ConnectionRemoved", NM_SETTINGS_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_settings_signal, sd, NULL);

    ids[n++] = g_dbus_connection_signal_subscribe (
        conn, NM_BUS_NAME, NM_CONN_IFACE,
        "Updated", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_settings_signal, sd, NULL);

    /* Mientras haya alguien escuchando, la lista de perfiles se puede
     * conservar en memoria (alguien va a avisar cuando cambie). */
    profiles_watchers++;

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

    /* Sin nadie escuchando, nadie avisaría de cambios: descartar la lista. */
    if (profiles_watchers > 0)
        profiles_watchers--;
    if (profiles_watchers == 0)
        profiles_invalidate ();
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

/* ---------- conectividad (portal cautivo) ---------- */

guint32
nm_get_connectivity (GDBusConnection *conn)
{
    GVariant *v = get_property (conn, NM_OBJECT_PATH, NM_IFACE, "Connectivity");
    if (!v) return NM_CONN_STATE_UNKNOWN;
    guint32 c = g_variant_get_uint32 (v);
    g_variant_unref (v);
    return c;
}

gchar *
nm_get_connectivity_check_uri (GDBusConnection *conn)
{
    GVariant *v = get_property (conn, NM_OBJECT_PATH, NM_IFACE,
                                "ConnectivityCheckUri");
    if (!v) return NULL;
    const gchar *s   = g_variant_get_string (v, NULL);
    gchar       *uri = (s && *s) ? g_strdup (s) : NULL;
    g_variant_unref (v);
    return uri;
}

void
nm_check_connectivity_async (GDBusConnection *conn)
{
    g_dbus_connection_call (
        conn, NM_BUS_NAME, NM_OBJECT_PATH, NM_IFACE,
        "CheckConnectivity", NULL, G_VARIANT_TYPE ("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
        on_async_done, "CheckConnectivity");
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

