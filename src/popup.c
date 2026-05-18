#include "popup.h"
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#define NM_BUS_NAME    "org.freedesktop.NetworkManager"
#define NM_OBJECT_PATH "/org/freedesktop/NetworkManager"
#define NM_IFACE       "org.freedesktop.NetworkManager"
#define NM_WIFI_IFACE  "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_IFACE    "org.freedesktop.NetworkManager.AccessPoint"

/* Timeout de seguridad (ms) para operaciones conectar/desconectar.
 * Si NM no confirma en este tiempo, asumimos fallo. */
#define OP_TIMEOUT_MS 20000

/* Cooldown del botón Actualizar (ms) entre escaneos. */
#define SCAN_COOLDOWN_MS 5000

/* Edad máxima (en segundos) de un intento "pendiente" para que cuente como
 * candidato a marcar fallo al reabrir el popup. Si pasó más tiempo que esto,
 * descartamos el intento sin marcar — asumimos que ya pasó suficiente y
 * cualquier marca tardía sería confusa. */
#define PENDING_ATTEMPT_MAX_AGE_SECS 60

/* ================================================================
 * Operaciones en curso
 *
 * Cuando el usuario aprieta Conectar o Desconectar, registramos
 * la operación en popup->ops_in_progress, indexada por device_path.
 * El handler reactivo, cuando llega una señal DBus, mira la tabla
 * y decide si tiene que cerrar el expand, restaurar el botón, etc.
 * ================================================================ */

typedef enum {
    OP_CONNECT,
    OP_DISCONNECT
} OpKind;

typedef struct {
    OpKind     kind;
    gchar     *ssid;          /* SSID destino (para CONNECT) o el actualmente conectado (DISCONNECT) */
    gchar     *device_path;
    GtkWidget *expand_box;    /* Expand a cerrar al confirmar (puede ser NULL si la fila se destruye antes). */
    GtkWidget *action_btn;    /* Botón a restaurar si falla. */
    gchar     *action_label;  /* Texto original del action_btn a restaurar en caso de fallo. */
    GtkWidget *pass_entry;    /* Entry de contraseña, si aplica (para mostrar error inline). */
    GtkWidget *error_label;   /* Label de error reusable, si se crea. */
    GSList    *extra_disabled;/* Lista de GtkWidget* extra que se deshabilitaron y hay que rehabilitar al fallar. */
    guint      timeout_id;    /* Fuente de timeout de 20s. */
    NetPopup  *popup;
} OpInProgress;

static void op_free (gpointer p);
static gboolean op_timeout_cb (gpointer user_data);
static void schedule_refresh_ui (NetPopup *popup);

/* ---------- prototipos anticipados ---------- */

static void rebuild_ui (NetPopup *popup);
static void update_top_status (NetPopup *popup);
static void update_eth_section (NetPopup *popup);
static void update_vpn_section (NetPopup *popup);
static void update_devices_section (NetPopup *popup);
static GtkWidget *make_ap_row (NmAccessPoint *ap, NetPopup *popup,
                               const gchar *device_path);
static void on_forget_clicked  (GtkWidget *btn, gpointer rd_ptr);
static void on_connect_clicked (GtkWidget *btn, gpointer rd_ptr);
static void on_nm_signal_popup (gpointer user_data);
static void on_eye_clicked     (GtkWidget *btn, GtkEntry *entry);

/* ---------- posicionamiento ---------- */

static void
position_popup (NetPopup *popup, GtkWidget *button)
{
    GdkWindow      *gdk_win;
    GdkMonitor     *monitor;
    GdkRectangle    workarea;
    GtkRequisition  preferred;
    gint bx, by, bw, bh, pw, ph, x, y;

    gdk_win = gtk_widget_get_window (button);
    gdk_window_get_origin (gdk_win, &bx, &by);
    bw = gtk_widget_get_allocated_width  (button);
    bh = gtk_widget_get_allocated_height (button);

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);
    pw = preferred.width;
    ph = preferred.height;

    monitor = gdk_display_get_monitor_at_point (
                  gtk_widget_get_display (button), bx + bw / 2, by + bh / 2);
    gdk_monitor_get_workarea (monitor, &workarea);

    x = bx;
    y = by + bh;

    if (x + pw > workarea.x + workarea.width)
        x = workarea.x + workarea.width - pw;

    if (y + ph > workarea.y + workarea.height)
        y = by - ph;

    gtk_window_move (GTK_WINDOW (popup->window), x, y);
}

/* ---------- SSID de la conexión primaria ---------- */

static gchar *
get_primary_ssid (GDBusConnection *conn)
{
    GSList      *devices, *d;
    gchar       *result       = NULL;
    const gchar *primary_dev  = NULL;
    GVariant    *v, *inner;

    v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, NM_OBJECT_PATH,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)", NM_IFACE, "PrimaryConnection"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (v) {
        g_variant_get (v, "(v)", &inner);
        const gchar *primary_path = g_variant_get_string (inner, NULL);

        if (primary_path && g_strcmp0 (primary_path, "/") != 0) {
            GVariant *dev_v = g_dbus_connection_call_sync (
                    conn, NM_BUS_NAME, primary_path,
                    "org.freedesktop.DBus.Properties", "Get",
                    g_variant_new ("(ss)",
                        "org.freedesktop.NetworkManager.Connection.Active",
                        "Devices"),
                    G_VARIANT_TYPE ("(v)"),
                    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
            if (dev_v) {
                GVariant *dev_inner;
                g_variant_get (dev_v, "(v)", &dev_inner);
                if (g_variant_n_children (dev_inner) > 0) {
                    GVariant *dev_path_v = g_variant_get_child_value (dev_inner, 0);
                    primary_dev = g_variant_get_string (dev_path_v, NULL);

                    devices = nm_get_wifi_devices (conn);
                    for (d = devices; d && !result; d = d->next) {
                        NmDevice *dev = d->data;
                        if (g_strcmp0 (dev->object_path, primary_dev) == 0) {
                            GSList *aps = nm_get_access_points (conn, dev->object_path);
                            for (GSList *a = aps; a; a = a->next) {
                                NmAccessPoint *ap = a->data;
                                if (ap->active && ap->ssid && *ap->ssid) {
                                    result = g_strdup (ap->ssid);
                                    break;
                                }
                            }
                            nm_ap_list_free (aps);
                        }
                    }
                    nm_device_list_free (devices);
                    g_variant_unref (dev_path_v);
                }
                g_variant_unref (dev_inner);
                g_variant_unref (dev_v);
            }
        }
        g_variant_unref (inner);
        g_variant_unref (v);
    }

    /* Fallback: cualquier adaptador Wi-Fi con AP activo */
    if (!result) {
        devices = nm_get_wifi_devices (conn);
        for (d = devices; d && !result; d = d->next) {
            NmDevice *dev = d->data;
            GSList   *aps = nm_get_access_points (conn, dev->object_path);
            for (GSList *a = aps; a; a = a->next) {
                NmAccessPoint *ap = a->data;
                if (ap->active && ap->ssid && *ap->ssid) {
                    result = g_strdup (ap->ssid);
                    break;
                }
            }
            nm_ap_list_free (aps);
        }
        nm_device_list_free (devices);
    }

    return result;
}

/* ---------- Operaciones en curso: helpers ---------- */

static void
op_free (gpointer p)
{
    OpInProgress *op = p;
    if (!op) return;
    if (op->timeout_id) {
        g_source_remove (op->timeout_id);
        op->timeout_id = 0;
    }
    g_free (op->ssid);
    g_free (op->device_path);
    g_free (op->action_label);
    g_slist_free (op->extra_disabled);
    g_free (op);
}

/* Si todavía existe el botón y el error_label, restaurar el botón y mostrar error. */
static void
op_show_error (OpInProgress *op)
{
    if (op->action_btn && GTK_IS_BUTTON (op->action_btn)) {
        gtk_widget_set_sensitive (op->action_btn, TRUE);
        const gchar *label = op->action_label ? op->action_label
                            : (op->kind == OP_CONNECT ? _("Connect") : _("Disconnect"));
        gtk_button_set_label (GTK_BUTTON (op->action_btn), label);
    }
    if (op->pass_entry && GTK_IS_ENTRY (op->pass_entry))
        gtk_widget_set_sensitive (op->pass_entry, TRUE);
    /* Rehabilitar widgets extras (botones Probar otra / Volver / Olvidar
     * deshabilitados durante la conexión). */
    for (GSList *l = op->extra_disabled; l; l = l->next) {
        GtkWidget *w = l->data;
        if (w && GTK_IS_WIDGET (w))
            gtk_widget_set_sensitive (w, TRUE);
    }
    if (op->kind == OP_CONNECT && op->expand_box && GTK_IS_WIDGET (op->expand_box)) {
        GtkWidget *err_label = g_object_get_data (G_OBJECT (op->expand_box), "error-label");
        if (!err_label) {
            err_label = gtk_label_new (_("Incorrect password"));
            gtk_style_context_add_class (
                gtk_widget_get_style_context (err_label), "error");
            gtk_label_set_xalign (GTK_LABEL (err_label), 0.0);
            gtk_box_pack_start (GTK_BOX (op->expand_box),
                                err_label, FALSE, FALSE, 0);
            gtk_box_reorder_child (GTK_BOX (op->expand_box), err_label, 0);
            g_object_set_data (G_OBJECT (op->expand_box),
                               "error-label", err_label);
        }
        gtk_widget_show (err_label);
        if (op->pass_entry && GTK_IS_ENTRY (op->pass_entry))
            gtk_entry_set_text (GTK_ENTRY (op->pass_entry), "");
    }
}

/* Recorre las filas reconstruidas y reabre el expand de la fila cuyo SSID
 * coincide con failed_ssid. Se llama después de update_devices_section para
 * que, tras un fallo de conexión, el expand quede abierto en estado A sin
 * que el usuario tenga que volver a clickear la fila. */
static void reopen_expand_for_ssid (NetPopup *popup, const gchar *ssid, const gchar *device_path);
static gboolean
op_timeout_cb (gpointer user_data)
{
    OpInProgress *op = user_data;
    op->timeout_id = 0;
    /* Apagar spinner: operación falló. */
    if (op->kind == OP_CONNECT && op->popup && op->popup->plugin_ref)
        net_plugin_set_connecting (op->popup->plugin_ref, FALSE);
    op_show_error (op);
    /* Paso 1: ya NO borramos el perfil automáticamente. Si la clave estaba mal,
     * el perfil queda guardado con clave incorrecta — el usuario decide qué hacer
     * (reintentar con la clave guardada o clickear "Olvidar" manualmente).
     * Esto evita perder el perfil cuando el driver miente sobre la causa real
     * del fallo (típico en módems USB con bug de roaming entre 2.4G y 5G). */
    /* Paso 2: registrar el SSID como "fallido" para que el próximo expand
     * de esta red guardada muestre campo "Reescribir contraseña". */
    NetPopup *popup_ref = op->popup;
    gchar    *failed_ssid = NULL;
    if (op->kind == OP_CONNECT && op->ssid) {
        g_hash_table_replace (op->popup->failed_ssids,
                              g_strdup (op->ssid), GINT_TO_POINTER (1));
        /* Ya procesamos visualmente el fallo, no es "pending". */
        {
            gchar *attempt_key = g_strdup_printf ("%s|%s", op->ssid, op->device_path);
            g_hash_table_remove (op->popup->pending_attempts, attempt_key);
            g_free (attempt_key);
        }
        failed_ssid = g_strdup (op->ssid);
    }
    /* Cerrar el expand actual (si quedó abierto) antes de reconstruir, así
     * la regla 1A no bloquea el refresh. */
    if (failed_ssid && popup_ref->current_expand_box) {
        gtk_widget_hide (popup_ref->current_expand_box);
        popup_ref->current_expand_box = NULL;
    }
    gchar *failed_dev = (op->kind == OP_CONNECT && op->device_path)
                        ? g_strdup (op->device_path) : NULL;
    g_hash_table_remove (op->popup->ops_in_progress, op->device_path);
    /* Reconstruir secciones para que esta red, ahora "guardada con fallo",
     * tenga el expand con interfaz A/B en lugar de la interfaz vieja.
     * Después reabrir el expand de esa misma fila para que el usuario vea
     * el estado A directamente, sin tener que reclickear la red. */
    if (failed_ssid) {
        if (popup_ref->current_expand_box) {
            gtk_widget_hide (popup_ref->current_expand_box);
            popup_ref->current_expand_box = NULL;
        }
        update_devices_section (popup_ref);
        reopen_expand_for_ssid (popup_ref, failed_ssid, failed_dev);
        g_free (failed_dev);
        g_free (failed_ssid);
    }
    return G_SOURCE_REMOVE;
}

/* Verifica si un dispositivo está en state==100 (activado). */
static gboolean
device_is_activated (GDBusConnection *conn, const gchar *device_path)
{
    GVariant *v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, device_path,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)",
                "org.freedesktop.NetworkManager.Device", "State"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!v) return FALSE;
    GVariant *inner;
    g_variant_get (v, "(v)", &inner);
    guint32 state = g_variant_get_uint32 (inner);
    g_variant_unref (inner);
    g_variant_unref (v);
    return (state == 100);
}

/* Verifica si un dispositivo está desconectado (state < 100 y > 30 = "disconnected" o menos). */
static gboolean
device_is_disconnected (GDBusConnection *conn, const gchar *device_path)
{
    GVariant *v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, device_path,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)",
                "org.freedesktop.NetworkManager.Device", "State"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!v) return FALSE;
    GVariant *inner;
    g_variant_get (v, "(v)", &inner);
    guint32 state = g_variant_get_uint32 (inner);
    g_variant_unref (inner);
    g_variant_unref (v);
    /* 30 = DISCONNECTED, 20 = UNAVAILABLE, 10 = UNMANAGED */
    return (state <= 30);
}

/* ---------- Intentos pendientes (popup cerrado durante conexión) ---------- */

/* Devuelve TRUE si `ssid` está actualmente conectado en algún adapter Wi-Fi.
 * Recorre todos los devices Wi-Fi y mira sus access points buscando uno
 * marcado como activo cuya ssid coincida. */
static gboolean
is_ssid_connected_anywhere (GDBusConnection *conn, const gchar *ssid)
{
    gboolean  found = FALSE;
    GSList   *devs  = nm_get_wifi_devices (conn);
    for (GSList *l = devs; l && !found; l = l->next) {
        NmDevice *dev = l->data;
        GSList   *aps = nm_get_access_points (conn, dev->object_path);
        for (GSList *a = aps; a; a = a->next) {
            NmAccessPoint *ap = a->data;
            if (ap->active && g_strcmp0 (ap->ssid, ssid) == 0) {
                found = TRUE;
                break;
            }
        }
        nm_ap_list_free (aps);
    }
    nm_device_list_free (devs);
    return found;
}

/* Procesa la tabla de intentos pendientes. Para cada SSID:
 *   - Si está conectada en algún adapter → descartar (todo bien).
 *   - Si no está conectada y el intento es reciente → marcar como fallida.
 *   - Si no está conectada y el intento es viejo (>60s) → descartar igual.
 * Vacía la tabla al final. */
static void
process_pending_attempts (NetPopup *popup)
{
    if (!popup->pending_attempts) return;
    if (g_hash_table_size (popup->pending_attempts) == 0) return;

    gint64  now = g_get_monotonic_time ();  /* microsegundos */
    GSList *to_mark_failed = NULL;

    GHashTableIter iter;
    gpointer       key, value;
    g_hash_table_iter_init (&iter, popup->pending_attempts);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        const gchar *ssid = key;
        gint64      *ts   = value;
        gint64       age_secs = (now - *ts) / G_USEC_PER_SEC;

        if (age_secs > PENDING_ATTEMPT_MAX_AGE_SECS) {
            to_mark_failed = g_slist_prepend (to_mark_failed, g_strdup (ssid));
            continue;
        }

        if (age_secs * 1000 < OP_TIMEOUT_MS)
            continue;  /* todavía dentro del tiempo de espera, no declarar fallo */

        /* La clave es "ssid|device_path". Separar para verificar el adaptador correcto. */
        gchar **parts = g_strsplit ((const gchar *) key, "|", 2);
        const gchar *attempt_ssid = parts[0];
        const gchar *attempt_dev  = parts[1];
        gboolean connected = FALSE;
        if (attempt_dev) {
            connected = device_is_activated (popup->conn, attempt_dev);
        } else {
            connected = is_ssid_connected_anywhere (popup->conn, attempt_ssid);
        }
        if (!connected)
            to_mark_failed = g_slist_prepend (to_mark_failed, g_strdup (attempt_ssid));
        g_strfreev (parts);
    }

    for (GSList *l = to_mark_failed; l; l = l->next) {
        g_hash_table_replace (popup->failed_ssids,
                              g_strdup ((const gchar *) l->data),
                              GINT_TO_POINTER (1));
    }
    g_slist_free_full (to_mark_failed, g_free);

    /* Eliminar solo los SSIDs cuya ventana de 20s ya expiró. Los más recientes
     * se conservan para que make_ap_row pueda forzar "no conectada" durante
     * ese período (NM puede mentir sobre el estado activo hasta que el AP
     * rechace la auth). */
    GHashTableIter iter2;
    gpointer       key2, value2;
    g_hash_table_iter_init (&iter2, popup->pending_attempts);
    while (g_hash_table_iter_next (&iter2, &key2, &value2)) {
        gint64 *ts = value2;
        gint64  age_ms = (now - *ts) / 1000;
        if (age_ms >= (gint64) PENDING_ATTEMPT_MAX_AGE_SECS * G_USEC_PER_SEC / 1000)
            g_hash_table_iter_remove (&iter2);
    }
}

/* Revisa todas las operaciones en curso y resuelve las que ya se completaron. */
static void
check_ops_progress (NetPopup *popup)
{
    if (!popup->ops_in_progress) return;

    GHashTableIter iter;
    gpointer       key, value;
    GSList        *to_remove = NULL;

    g_hash_table_iter_init (&iter, popup->ops_in_progress);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        OpInProgress *op = value;
        const gchar  *device_path = key;

        if (op->kind == OP_CONNECT) {
            if (device_is_activated (popup->conn, device_path)) {
                /* Verificar que el AP activo coincida con el SSID que pedimos
                 * (puede que se haya conectado a otra red distinta por algún motivo). */
                gchar *active_ssid = NULL;
                GSList *aps = nm_get_access_points (popup->conn, device_path);
                for (GSList *a = aps; a; a = a->next) {
                    NmAccessPoint *ap = a->data;
                    if (ap->active) {
                        active_ssid = g_strdup (ap->ssid);
                        break;
                    }
                }
                nm_ap_list_free (aps);

                if (active_ssid && g_strcmp0 (active_ssid, op->ssid) == 0) {
                    /* Confirmado: marcamos para eliminar. */
                    to_remove = g_slist_append (to_remove, g_strdup (device_path));
                    /* Limpiar marca de fallo: la red conectó OK. */
                    g_hash_table_remove (popup->failed_ssids, op->ssid);
                    /* Limpiar intento pendiente: ya resuelto en este ciclo. */
                    {
                        gchar *attempt_key = g_strdup_printf ("%s|%s", op->ssid, op->device_path);
                        g_hash_table_remove (popup->pending_attempts, attempt_key);
                        g_free (attempt_key);
                    }
                    if (op->expand_box && GTK_IS_WIDGET (op->expand_box)) {
                        gtk_widget_hide (op->expand_box);
                        if (popup->current_expand_box == op->expand_box)
                            popup->current_expand_box = NULL;
                    }
                    /* Apagar spinner: conexión confirmada. */
                    if (popup->plugin_ref)
                        net_plugin_set_connecting (popup->plugin_ref, FALSE);
                }
                g_free (active_ssid);
            }
        } else { /* OP_DISCONNECT */
            if (device_is_disconnected (popup->conn, device_path)) {
                to_remove = g_slist_append (to_remove, g_strdup (device_path));
                if (op->expand_box && GTK_IS_WIDGET (op->expand_box)) {
                    gtk_widget_hide (op->expand_box);
                    if (popup->current_expand_box == op->expand_box)
                        popup->current_expand_box = NULL;
                }
            }
        }
    }

    for (GSList *l = to_remove; l; l = l->next) {
        g_hash_table_remove (popup->ops_in_progress, l->data);
        g_free (l->data);
    }
    g_slist_free (to_remove);
}

/* ---------- ícono de señal Wi-Fi (usado también por plugin.c) ---------- */

GtkWidget *
make_signal_icon (gint strength, gboolean secure, gint icon_size)
{
    const gchar *level;
    gchar        icon_name[64];

    if      (strength >= 80) level = "excellent";
    else if (strength >= 55) level = "good";
    else if (strength >= 30) level = "ok";
    else                     level = "weak";

    g_snprintf (icon_name, sizeof icon_name,
                "network-wireless-signal-%s-symbolic", level);

    GtkWidget *signal_img = gtk_image_new_from_icon_name (
                                icon_name, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size (GTK_IMAGE (signal_img), icon_size);

    if (!secure)
        return signal_img;

    GtkWidget *overlay  = gtk_overlay_new ();
    GtkWidget *lock_img = gtk_image_new_from_icon_name (
                              "nm-secure-lock", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size (GTK_IMAGE (lock_img), icon_size);
    gtk_widget_set_halign (lock_img, GTK_ALIGN_FILL);
    gtk_widget_set_valign (lock_img, GTK_ALIGN_FILL);

    gtk_container_add    (GTK_CONTAINER (overlay), signal_img);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), lock_img);
    gtk_widget_set_size_request (overlay, icon_size, icon_size);
    gtk_widget_show_all (overlay);

    return overlay;
}

/* ---------- callback del switch Wi-Fi global ---------- */

static gboolean
on_wifi_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    NetPopup *popup = user_data;
    nm_set_wifi_enabled (popup->conn, state);
    for (GSList *l = popup->device_switches; l; l = l->next)
        gtk_widget_set_sensitive (GTK_WIDGET (l->data), state);
    return FALSE;
}

/* ---------- callback del switch por adaptador ---------- */

typedef struct {
    GDBusConnection *conn;
    gchar           *device_path;
} DeviceSwitchData;

static void
device_switch_data_free (DeviceSwitchData *d)
{
    g_free (d->device_path);
    g_free (d);
}

static gboolean
on_device_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    DeviceSwitchData *d = user_data;
    nm_set_device_enabled_async (d->conn, d->device_path, state);
    return FALSE;
}

/* ---------- Actualizar (botón) ---------- */

static gboolean
on_refresh_reenable (gpointer popup_ptr)
{
    NetPopup *popup = popup_ptr;
    popup->scanning = FALSE;
    popup->scan_timeout_id = 0;
    if (popup->refresh_button && GTK_IS_WIDGET (popup->refresh_button)) {
        gtk_widget_set_sensitive (popup->refresh_button, TRUE);
        if (popup->refresh_label)
            gtk_label_set_text (GTK_LABEL (popup->refresh_label), _("Refresh"));
    }
    return G_SOURCE_REMOVE;
}

static void
on_refresh_clicked (GtkWidget *btn, NetPopup *popup)
{
    if (popup->scanning) return;

    popup->scanning = TRUE;
    gtk_widget_set_sensitive (btn, FALSE);
    if (popup->refresh_label)
        gtk_label_set_text (GTK_LABEL (popup->refresh_label), _("Updating…"));
    popup->scan_timeout_id =
        g_timeout_add (SCAN_COOLDOWN_MS, on_refresh_reenable, popup);

    GSList *devs = nm_get_wifi_devices (popup->conn);
    for (GSList *l = devs; l; l = l->next) {
        NmDevice *dev = l->data;
        nm_request_scan (popup->conn, dev->object_path);
    }
    nm_device_list_free (devs);
}

/* ---------- callbacks de filas de red ---------- */

typedef struct {
    NetPopup        *popup;
    GtkWidget       *expand_box;
    GtkWidget       *action_btn;     /* Conectar / Desconectar / Reintentar con la contraseña guardada */
    gchar           *ssid;
    gchar           *ap_path;
    gboolean         secure;
    gboolean         active;
    gboolean         saved;
    gchar           *device_path;
    GtkWidget       *pass_entry;
    GtkWidget       *autoconnect_check;

    /* Solo presentes en expand de red guardada con último intento fallido. */
    GtkWidget       *state_a_box;    /* Contenedor de botones del estado A. */
    GtkWidget       *state_b_box;    /* Contenedor del entry + botones del estado B. */
    GtkWidget       *retry_btn;      /* Estado A: "Reintentar con la contraseña guardada" */
    GtkWidget       *try_other_btn;  /* Estado A: "Probar otra contraseña" */
    GtkWidget       *back_btn;       /* Estado B: "Volver" */
    GtkWidget       *connect_btn_b;  /* Estado B: "Conectar" */
    GtkWidget       *forget_btn;     /* Botón "Olvidar" (solo en estado A para red con fallo). */
} RowData;

static void
reopen_expand_for_ssid (NetPopup *popup, const gchar *ssid, const gchar *device_path)
{
    if (!popup || !ssid) return;

    GList *sections = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *s = sections; s; s = s->next) {
        if (s->data == popup->vpn_section) continue;
        if (!GTK_IS_CONTAINER (s->data))    continue;

        GList *rows = gtk_container_get_children (GTK_CONTAINER (s->data));
        for (GList *r = rows; r; r = r->next) {
            if (!GTK_IS_EVENT_BOX (r->data)) continue;
            const gchar *row_ssid = g_object_get_data (G_OBJECT (r->data), "ssid");
            if (!row_ssid || g_strcmp0 (row_ssid, ssid) != 0) continue;
            const gchar *row_dev = g_object_get_data (G_OBJECT (r->data), "device-path");
            if (device_path && (!row_dev || g_strcmp0 (row_dev, device_path) != 0)) continue;

            GList *inner = gtk_container_get_children (GTK_CONTAINER (r->data));
            if (inner && inner->data) {
                RowData *rd = g_object_get_data (G_OBJECT (inner->data), "row-data");
                if (rd && rd->expand_box) {
                    gtk_widget_set_no_show_all (rd->expand_box, FALSE);
                    gtk_widget_show_all (rd->expand_box);
                    gtk_widget_set_no_show_all (rd->expand_box, TRUE);
                    if (rd->state_a_box && rd->state_b_box) {
                        gtk_widget_show (rd->state_a_box);
                        gtk_widget_hide (rd->state_b_box);
                    }
                    popup->current_expand_box = rd->expand_box;
                }
            }
            g_list_free (inner);
            g_list_free (rows);
            goto done;
        }
        g_list_free (rows);
    }
done:
    g_list_free (sections);
}

static void
row_data_free (RowData *rd)
{
    g_free (rd->ssid);
    g_free (rd->ap_path);
    g_free (rd->device_path);
    g_free (rd);
}

static void
on_row_clicked (GtkWidget *event_box, GdkEventButton *event, gpointer rd_ptr)
{
    (void) event;
    (void) event_box;
    RowData *rd = rd_ptr;

    gboolean ya_abierto = (rd->popup->current_expand_box == rd->expand_box);

    if (rd->popup->current_expand_box) {
        gtk_widget_hide (rd->popup->current_expand_box);
        rd->popup->current_expand_box = NULL;
    }

    if (!ya_abierto) {
        /* Si el expand tiene estados A/B y estamos abriendo desde cero,
         * asegurar que el estado visible es A (regla: cerrar sin accionar
         * vuelve al primer estado). */
        if (rd->state_a_box && rd->state_b_box) {
            gtk_widget_show (rd->state_a_box);
            gtk_widget_hide (rd->state_b_box);
            if (rd->pass_entry)
                gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
        }
        gtk_widget_show (rd->expand_box);
        rd->popup->current_expand_box = rd->expand_box;
    }
}

/* ---------- conectar / desconectar ---------- */

static void
on_disconnect_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;

    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Disconnecting…"));

    OpInProgress *op = g_new0 (OpInProgress, 1);
    op->kind        = OP_DISCONNECT;
    op->ssid        = g_strdup (rd->ssid);
    op->device_path = g_strdup (rd->device_path);
    op->expand_box  = rd->expand_box;
    op->action_btn  = btn;
    op->popup       = rd->popup;
    op->timeout_id  = g_timeout_add (OP_TIMEOUT_MS, op_timeout_cb, op);
    g_hash_table_replace (rd->popup->ops_in_progress,
                          g_strdup (rd->device_path), op);

    nm_disconnect_device_async (rd->popup->conn, rd->device_path);
}

static void
on_forget_response (GtkDialog *dialog, gint response, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    gtk_widget_destroy (GTK_WIDGET (dialog));
    if (response != GTK_RESPONSE_YES)
        return;
    nm_forget_connection (rd->popup->conn, rd->ssid);
    /* Limpiar marca de fallo: la red ya no está guardada. */
    g_hash_table_remove (rd->popup->failed_ssids, rd->ssid);
    /* Limpiar intento pendiente: la red fue olvidada explícitamente. */
    {
        gchar *attempt_key = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_remove (rd->popup->pending_attempts, attempt_key);
        g_free (attempt_key);
    }

    /* Cerrar el expand para que update_devices_section pueda reconstruir las
     * filas (regla 1A bloquea la reconstrucción mientras hay expand abierto).
     * NM no emite señal de device al borrar un perfil de conexión, así que
     * forzamos el refresh nosotros. */
    if (rd->expand_box) {
        gtk_widget_hide (rd->expand_box);
        if (rd->popup->current_expand_box == rd->expand_box)
            rd->popup->current_expand_box = NULL;
    }
    schedule_refresh_ui (rd->popup);
}

static void
on_forget_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;

    GtkWidget *dialog = gtk_message_dialog_new (
        NULL,
        0,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        _("Forget network \"%s\"?"), rd->ssid);
    gtk_window_set_title (GTK_WINDOW (dialog), _("Confirm"));
    gtk_window_set_deletable (GTK_WINDOW (dialog), FALSE);
    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
    g_signal_connect (dialog, "response", G_CALLBACK (on_forget_response), rd);
    gtk_widget_show_all (dialog);
}

static void
do_connect (RowData *rd)
{
    gchar       *saved_pw    = NULL;
    const gchar *password    = NULL;
    gboolean     autoconnect = TRUE;

    if (rd->pass_entry)
        password = gtk_entry_get_text (GTK_ENTRY (rd->pass_entry));
    if (!password || !*password)
        saved_pw = nm_get_saved_password (rd->popup->conn, rd->ssid);

    if (rd->autoconnect_check)
        autoconnect = gtk_toggle_button_get_active (
                          GTK_TOGGLE_BUTTON (rd->autoconnect_check));

    /* Ocultar label de error previo, si existía. */
    GtkWidget *prev_err = g_object_get_data (G_OBJECT (rd->expand_box), "error-label");
    if (prev_err)
        gtk_widget_hide (prev_err);

    /* Decidir cuál es el botón "principal" según el estado visible del expand.
     * Si estamos en estado A (state_a_box visible), el botón principal es
     * "Reintentar con la contraseña guardada"; si estamos en estado B (o en
     * el expand simple sin fallo), es el "Conectar" normal. */
    GtkWidget   *primary_btn  = rd->action_btn;
    const gchar *primary_text = NULL;
    if (rd->state_a_box && gtk_widget_get_visible (rd->state_a_box)) {
        primary_btn  = rd->retry_btn;
        primary_text = _("Retry with saved password");
    } else if (rd->connect_btn_b && rd->state_b_box &&
               gtk_widget_get_visible (rd->state_b_box)) {
        primary_btn  = rd->connect_btn_b;
        primary_text = _("Connect");
    } else {
        primary_text = _("Connect");
    }

    /* UI inmediata: botón a "Conectando…", deshabilitado. Entry también. */
    gtk_widget_set_sensitive (primary_btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (primary_btn), _("Connecting…"));
    if (rd->pass_entry)
        gtk_widget_set_sensitive (rd->pass_entry, FALSE);

    /* Deshabilitar los otros botones del expand para evitar acciones cruzadas. */
    GSList *extras = NULL;
    if (rd->try_other_btn && gtk_widget_get_sensitive (rd->try_other_btn)) {
        gtk_widget_set_sensitive (rd->try_other_btn, FALSE);
        extras = g_slist_prepend (extras, rd->try_other_btn);
    }
    if (rd->back_btn && gtk_widget_get_sensitive (rd->back_btn)) {
        gtk_widget_set_sensitive (rd->back_btn, FALSE);
        extras = g_slist_prepend (extras, rd->back_btn);
    }
    if (rd->forget_btn && gtk_widget_get_sensitive (rd->forget_btn)) {
        gtk_widget_set_sensitive (rd->forget_btn, FALSE);
        extras = g_slist_prepend (extras, rd->forget_btn);
    }

    /* Registrar operación en curso. */
    OpInProgress *op = g_new0 (OpInProgress, 1);
    op->kind        = OP_CONNECT;
    op->ssid        = g_strdup (rd->ssid);
    op->device_path = g_strdup (rd->device_path);
    op->expand_box  = rd->expand_box;
    op->action_btn  = primary_btn;
    op->action_label = g_strdup (primary_text);
    op->pass_entry  = rd->pass_entry;
    op->extra_disabled = extras;
    op->popup       = rd->popup;
    op->timeout_id  = g_timeout_add (OP_TIMEOUT_MS, op_timeout_cb, op);
    g_hash_table_replace (rd->popup->ops_in_progress,
                          g_strdup (rd->device_path), op);

    /* Registrar intento pendiente con timestamp. Si el usuario cierra el popup
     * antes de que la op se resuelva, al reabrir lo procesamos para decidir
     * si marcar como fallido o no según el estado real de la conexión. */
    {
        gint64 *ts = g_new (gint64, 1);
        *ts = g_get_monotonic_time ();
        gchar *attempt_key = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_replace (rd->popup->pending_attempts,
                              attempt_key, ts);
    }

    /* Activar spinner en el botón del panel mientras conecta. */
    if (rd->popup->plugin_ref)
        net_plugin_set_connecting (rd->popup->plugin_ref, TRUE);

    /* Si hay perfil guardado y no se ingresó password, usar ActivateConnection.
     * Si no, usar AddAndActivate. AddAndActivate crea un perfil nuevo en cada
     * llamada — si ya había uno guardado para este SSID, lo borramos antes para
     * evitar acumular perfiles duplicados tras varios intentos con clave mala. */
    if (saved_pw && (!password || !*password)) {
        /* Migración automática: si el perfil viejo está bindeado a un adapter
         * específico (campo interface-name fijado), se lo sacamos para que sirva
         * a cualquier wlanX. También borra duplicados si quedaron de antes. */
        nm_strip_interface_name (rd->popup->conn, rd->ssid);
        nm_activate_connection_async (rd->popup->conn, rd->device_path,
                                      rd->ap_path, rd->ssid);
    } else {
        if (password && *password)
            nm_forget_connection (rd->popup->conn, rd->ssid);
        nm_add_and_activate_connection_async (rd->popup->conn, rd->device_path,
                                              rd->ap_path, rd->ssid,
                                              saved_pw ? saved_pw : password,
                                              autoconnect);
    }

    g_free (saved_pw);
}

/* Handler de "Reintentar con la contraseña guardada": vacía el entry de
 * contraseña para forzar el uso de la guardada, después llama a do_connect. */
static void
on_retry_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->pass_entry)
        gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
    do_connect (rd);
}

/* Handler de "Probar otra contraseña": pasa del estado A al B (oculta los
 * botones grandes, muestra el entry y los botones Volver/Conectar). */
static void
on_try_other_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->state_a_box) gtk_widget_hide (rd->state_a_box);
    if (rd->state_b_box) {
        /* show_all para que los hijos del state_b_box (entry, botones Volver/Conectar)
         * salgan visibles. set_no_show_all en el contenedor padre evita que un
         * gtk_widget_show_all externo lo reabra, pero acá lo forzamos a mano. */
        gtk_widget_set_no_show_all (rd->state_b_box, FALSE);
        gtk_widget_show_all (rd->state_b_box);
        gtk_widget_set_no_show_all (rd->state_b_box, TRUE);
    }
    if (rd->pass_entry) {
        gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
        gtk_widget_grab_focus (rd->pass_entry);
    }
    /* Conectar arranca deshabilitado: se habilita cuando el entry tenga texto. */
    if (rd->connect_btn_b)
        gtk_widget_set_sensitive (rd->connect_btn_b, FALSE);
}

/* Handler de "Volver": pasa del estado B al A. */
static void
on_back_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->state_b_box) gtk_widget_hide (rd->state_b_box);
    if (rd->state_a_box) gtk_widget_show (rd->state_a_box);
    if (rd->pass_entry)
        gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
}

/* Habilita el botón Conectar (estado B) solo si el entry tiene texto. */
static void
on_pass_entry_b_changed (GtkEditable *editable, gpointer rd_ptr)
{
    RowData     *rd   = rd_ptr;
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (editable));
    if (rd->connect_btn_b)
        gtk_widget_set_sensitive (rd->connect_btn_b, text && *text);
}

static void
on_connect_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    do_connect ((RowData *) rd_ptr);
}

static gboolean
on_pass_entry_key_press (GtkWidget *widget, GdkEventKey *event, gpointer rd_ptr)
{
    (void) widget;
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        do_connect ((RowData *) rd_ptr);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void
on_eye_pressed (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->pass_entry)
        gtk_entry_set_visibility (GTK_ENTRY (rd->pass_entry), TRUE);
}

static void
on_eye_released (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->pass_entry)
        gtk_entry_set_visibility (GTK_ENTRY (rd->pass_entry), FALSE);
}

/* ---------- construcción de una fila de red ---------- */

static GtkWidget *
make_ap_row (NmAccessPoint *ap, NetPopup *popup, const gchar *device_path)
{
    GtkWidget *outer, *event_box, *row, *ssid_label, *signal_icon;
    GtkWidget *expand_box, *action_btn, *action_row;
    GtkWidget *forget_btn = NULL;
    GtkWidget *pass_entry = NULL;
    GtkWidget *autoconnect_check_widget = NULL;
    GDBusConnection *conn = popup->conn;

    /* Si hay un intento de conexión pendiente para este SSID y aún estamos
     * dentro de la ventana del timeout (OP_TIMEOUT_MS), NO confiar en lo que
     * dice NM sobre ap->active: NM puede reportar "activado" varios segundos
     * antes de que el AP rechace la auth con clave mala. Forzamos active=FALSE
     * para que la fila no muestre "Conectado" falsamente. */
    gboolean ap_active = ap->active;
    if (ap_active && popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS)
                ap_active = FALSE;
        }
    }

    outer     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    event_box = gtk_event_box_new ();
    gtk_event_box_set_above_child (GTK_EVENT_BOX (event_box), FALSE);
    gtk_container_add (GTK_CONTAINER (event_box), outer);

    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (row, 12);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 8);
    gtk_widget_set_margin_bottom (row, 8);

    signal_icon = make_signal_icon (ap->strength, ap->secure, 16);
    gtk_box_pack_start (GTK_BOX (row), signal_icon, FALSE, FALSE, 0);

    ssid_label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (ssid_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (ssid_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (ssid_label), 24);

    {
        const gchar *band;
        if (ap->frequency >= 5925)      band = "6G";
        else if (ap->frequency >= 5000) band = "5G";
        else                            band = "2.4G";

        /* is_connecting: hay un intento de conexión a este SSID iniciado en otra
     * apertura del popup, y aún estamos dentro de la ventana de 20s. */
    gboolean is_connecting = FALSE;
    if (!ap_active && popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS)
                is_connecting = TRUE;
        }
    }

    if (ap_active) {
            gchar *markup = g_markup_printf_escaped (
                "<b>%s</b>  <small><span alpha='60%%'>%s</span></small>",
                ap->ssid, band);
            gtk_label_set_markup (GTK_LABEL (ssid_label), markup);
            g_free (markup);
            GtkWidget *check = gtk_image_new_from_icon_name (
                                   "emblem-default-symbolic", GTK_ICON_SIZE_MENU);
            GtkWidget *connected_right = gtk_label_new (_("Connected"));
            gtk_style_context_add_class (gtk_widget_get_style_context (connected_right),
                                         "dim-label");
            gtk_label_set_ellipsize (GTK_LABEL (connected_right), PANGO_ELLIPSIZE_NONE);
            gtk_widget_set_halign (connected_right, GTK_ALIGN_END);
            gtk_widget_set_hexpand (connected_right, FALSE);
            gtk_box_pack_end (GTK_BOX (row), check, FALSE, FALSE, 0);
            gtk_box_pack_end (GTK_BOX (row), connected_right, FALSE, FALSE, 4);
        } else if (is_connecting) {
            gchar *markup = g_markup_printf_escaped (
                "%s  <small><span alpha='60%%'>%s</span></small>",
                ap->ssid, band);
            gtk_label_set_markup (GTK_LABEL (ssid_label), markup);
            g_free (markup);
            GtkWidget *connecting_right = gtk_label_new (_("Connecting…"));
            gtk_style_context_add_class (gtk_widget_get_style_context (connecting_right),
                                         "dim-label");
            gtk_label_set_ellipsize (GTK_LABEL (connecting_right), PANGO_ELLIPSIZE_NONE);
            gtk_widget_set_halign (connecting_right, GTK_ALIGN_END);
            gtk_widget_set_hexpand (connecting_right, FALSE);
            gtk_box_pack_end (GTK_BOX (row), connecting_right, FALSE, FALSE, 4);
        } else {
            gchar *markup = g_markup_printf_escaped (
                "%s  <small><span alpha='60%%'>%s</span></small>",
                ap->ssid, band);
            gtk_label_set_markup (GTK_LABEL (ssid_label), markup);
            g_free (markup);
        }
    }

    gtk_box_pack_start (GTK_BOX (row), ssid_label, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (outer), row, FALSE, FALSE, 0);

    /* ---- área expandible ---- */
    expand_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start  (expand_box, 12);
    gtk_widget_set_margin_end    (expand_box, 12);
    gtk_widget_set_margin_top    (expand_box, 4);
    gtk_widget_set_margin_bottom (expand_box, 8);
    

    /* is_connecting: hay un intento de conexión a este SSID iniciado en otra
     * apertura del popup, y aún estamos dentro de la ventana de 20s. El expand
     * muestra solo un label "Conectando…", sin botones, para no confundir al
     * usuario con un "Conectar" cuando ya hay un intento en curso. */
    gboolean is_connecting = FALSE;
    if (!ap_active && popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS)
                is_connecting = TRUE;
        }
    }

    if (ap_active) {
        action_btn = gtk_button_new_with_label (_("Disconnect"));
    } else if (is_connecting) {
        GtkWidget *connecting_label = gtk_label_new (_("Connecting…"));
        gtk_label_set_xalign (GTK_LABEL (connecting_label), 0.0);
        gtk_box_pack_start (GTK_BOX (expand_box), connecting_label, FALSE, FALSE, 0);
        gtk_widget_show (connecting_label);
        /* Botón "Connect" creado oculto: no rompe la lógica de action_row. */
        action_btn = gtk_button_new_with_label (_("Connect"));
        gtk_widget_set_no_show_all (action_btn, TRUE);
        /* Marca: este expand es pasivo (solo muestra "Conectando…"), no hay
         * interacción del usuario que proteger. update_devices_section puede
         * reconstruirlo libremente y reabrirlo después. */
        g_object_set_data (G_OBJECT (expand_box), "passive-connecting",
                           GINT_TO_POINTER (1));
        /* Agendar refresh para cuando expire la ventana de pending_attempts.
         * Si NM no emitió ninguna señal hasta entonces, queremos transicionar
         * el expand a estado A sin esperar a que el usuario interactúe. */
        {
            gint64 *ts = g_hash_table_lookup (popup->pending_attempts, ap->ssid);
            if (ts) {
                gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
                gint64 remaining = OP_TIMEOUT_MS - age_ms;
                if (remaining < 100) remaining = 100;
                g_timeout_add_full (G_PRIORITY_DEFAULT, (guint) remaining,
                                    (GSourceFunc) (void *) schedule_refresh_ui,
                                    popup, NULL);
            }
        }
    } else {
        gboolean saved = nm_has_saved_connection (conn, ap->ssid);
        gboolean had_failure = saved && ap->secure &&
            g_hash_table_contains (popup->failed_ssids, ap->ssid);

        if (saved) {
            GtkWidget *saved_label = gtk_label_new (_("Saved network"));
            gtk_style_context_add_class (gtk_widget_get_style_context (saved_label),
                                         "dim-label");
            gtk_label_set_xalign (GTK_LABEL (saved_label), 0.0);
            gtk_box_pack_start (GTK_BOX (expand_box), saved_label, FALSE, FALSE, 0);
            gtk_widget_show (saved_label);

            if (had_failure) {
                /* Mensaje "El último intento falló al conectar", visible siempre. */
                GtkWidget *fail_label = gtk_label_new (
                    _("Last connection attempt failed"));
                gtk_style_context_add_class (
                    gtk_widget_get_style_context (fail_label), "error");
                gtk_label_set_xalign (GTK_LABEL (fail_label), 0.0);
                gtk_box_pack_start (GTK_BOX (expand_box), fail_label,
                                    FALSE, FALSE, 0);
                gtk_widget_show (fail_label);
            }

            GtkWidget *autoconnect_check_saved =
                gtk_check_button_new_with_label (_("Connect automatically"));
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (autoconnect_check_saved), TRUE);
            gtk_box_pack_start (GTK_BOX (expand_box), autoconnect_check_saved, FALSE, FALSE, 0);
            gtk_widget_show (autoconnect_check_saved);
            autoconnect_check_widget = autoconnect_check_saved;

            forget_btn = gtk_button_new_with_label (_("Forget"));
            gtk_widget_set_halign (forget_btn, GTK_ALIGN_END);
        } else if (ap->secure) {
            GtkWidget *pass_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

            pass_entry = gtk_entry_new ();
            gtk_entry_set_placeholder_text (GTK_ENTRY (pass_entry), _("Password"));
            gtk_entry_set_visibility       (GTK_ENTRY (pass_entry), FALSE);
            gtk_box_pack_start (GTK_BOX (pass_row), pass_entry, TRUE, TRUE, 0);

            GtkWidget *eye_btn  = gtk_button_new ();
            GtkWidget *eye_icon = gtk_image_new_from_icon_name (
                                      "view-reveal-symbolic", GTK_ICON_SIZE_MENU);
            gtk_button_set_image  (GTK_BUTTON (eye_btn), eye_icon);
            gtk_button_set_relief (GTK_BUTTON (eye_btn), GTK_RELIEF_NONE);
            gtk_box_pack_start (GTK_BOX (pass_row), eye_btn, FALSE, FALSE, 0);

            gtk_box_pack_start (GTK_BOX (expand_box), pass_row, FALSE, FALSE, 0);
            gtk_widget_show_all (pass_row);

            GtkWidget *autoconnect_check =
                gtk_check_button_new_with_label (_("Connect automatically"));
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (autoconnect_check), TRUE);
            gtk_box_pack_start (GTK_BOX (expand_box), autoconnect_check, FALSE, FALSE, 0);
            gtk_widget_show (autoconnect_check);
            autoconnect_check_widget = autoconnect_check;
        }

        /* action_btn de uso general: "Connect" simple para red guardada sin
         * fallo o para red nueva. En el caso "red guardada con fallo" no lo
         * usamos como botón principal (creamos retry_btn / connect_btn_b
         * abajo), pero lo dejamos creado y oculto para no romper la lógica
         * existente de action_row / RowData. */
        action_btn = gtk_button_new_with_label (_("Connect"));
        if (had_failure)
            gtk_widget_set_no_show_all (action_btn, TRUE);
    }

    gtk_widget_set_halign (action_btn, GTK_ALIGN_END);

    action_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start (GTK_BOX (action_row), action_btn, FALSE, FALSE, 0);
    if (!ap_active && forget_btn) {
        gtk_box_pack_end (GTK_BOX (action_row), forget_btn, FALSE, FALSE, 0);
        gtk_widget_show (forget_btn);
    }
    gtk_box_pack_start (GTK_BOX (expand_box), action_row, FALSE, FALSE, 0);
    /* Si action_btn tiene no_show_all (caso red guardada con fallo, donde el
     * botón principal pasa a ser retry_btn / connect_btn_b), no lo mostramos.
     * Sin esto el "Connect" residual queda visible en el estado A. */
    if (!gtk_widget_get_no_show_all (action_btn))
        gtk_widget_show (action_btn);
    gtk_widget_show (action_row);

    gtk_widget_set_no_show_all (expand_box, TRUE);
    gtk_widget_hide (expand_box);
    gtk_box_pack_start (GTK_BOX (outer), expand_box, FALSE, FALSE, 0);

    /* ---- RowData ---- */
    RowData *rd     = g_new0 (RowData, 1);
    rd->popup       = popup;
    rd->expand_box  = expand_box;
    rd->action_btn  = action_btn;
    rd->ssid        = g_strdup (ap->ssid);
    rd->ap_path     = g_strdup (ap->object_path);
    rd->secure      = ap->secure;
    rd->active      = ap_active;
    rd->saved       = ap_active ? FALSE : nm_has_saved_connection (conn, ap->ssid);
    rd->device_path = g_strdup (device_path);
    rd->pass_entry        = pass_entry;
    rd->autoconnect_check = autoconnect_check_widget;
    rd->forget_btn        = forget_btn;

    /* Si es red guardada con fallo previo, construir los dos sub-bloques
     * (estado A = botones de reintentar/probar otra; estado B = entry de
     * reescribir contraseña + Volver/Conectar). Se intercalan después del
     * autoconnect_check y antes del action_row (que solo tiene "Olvidar"). */
    if (!ap_active && rd->saved && ap->secure &&
        g_hash_table_contains (popup->failed_ssids, ap->ssid)) {

        /* ---- Estado A ---- */
        rd->state_a_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        rd->retry_btn = gtk_button_new_with_label (
            _("Retry with saved password"));
        gtk_widget_set_hexpand (rd->retry_btn, FALSE);
        gtk_widget_set_halign  (rd->retry_btn, GTK_ALIGN_FILL);
        gtk_box_pack_start (GTK_BOX (rd->state_a_box), rd->retry_btn,
                            TRUE, TRUE, 0);

        rd->try_other_btn = gtk_button_new_with_label (
            _("Try a different password"));
        gtk_widget_set_hexpand (rd->try_other_btn, FALSE);
        gtk_widget_set_halign  (rd->try_other_btn, GTK_ALIGN_FILL);
        gtk_box_pack_start (GTK_BOX (rd->state_a_box), rd->try_other_btn,
                            TRUE, TRUE, 0);

        /* Insertar state_a_box justo antes del action_row. */
        gtk_box_pack_start (GTK_BOX (expand_box), rd->state_a_box,
                            FALSE, FALSE, 0);
        gtk_widget_show_all (rd->state_a_box);

        /* ---- Estado B (oculto inicialmente) ---- */
        rd->state_b_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        GtkWidget *pass_row_b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        pass_entry = gtk_entry_new ();
        gtk_entry_set_placeholder_text (GTK_ENTRY (pass_entry),
                                        _("Rewrite password"));
        gtk_entry_set_visibility       (GTK_ENTRY (pass_entry), FALSE);
        gtk_box_pack_start (GTK_BOX (pass_row_b), pass_entry, TRUE, TRUE, 0);

        GtkWidget *eye_btn_b  = gtk_button_new ();
        GtkWidget *eye_icon_b = gtk_image_new_from_icon_name (
                                    "view-reveal-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_image  (GTK_BUTTON (eye_btn_b), eye_icon_b);
        gtk_button_set_relief (GTK_BUTTON (eye_btn_b), GTK_RELIEF_NONE);
        gtk_box_pack_start (GTK_BOX (pass_row_b), eye_btn_b, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (rd->state_b_box), pass_row_b,
                            FALSE, FALSE, 0);

        GtkWidget *btns_row_b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign (btns_row_b, GTK_ALIGN_END);

        rd->back_btn = gtk_button_new_with_label (_("Back"));
        gtk_box_pack_start (GTK_BOX (btns_row_b), rd->back_btn,
                            FALSE, FALSE, 0);

        rd->connect_btn_b = gtk_button_new_with_label (_("Connect"));
        gtk_widget_set_sensitive (rd->connect_btn_b, FALSE);
        gtk_box_pack_start (GTK_BOX (btns_row_b), rd->connect_btn_b,
                            FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (rd->state_b_box), btns_row_b,
                            FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (expand_box), rd->state_b_box,
                            FALSE, FALSE, 0);

        gtk_widget_set_no_show_all (rd->state_b_box, TRUE);
        gtk_widget_hide (rd->state_b_box);

        /* Reordenar para que action_row (con el botón Olvidar) quede al final. */
        gtk_box_reorder_child (GTK_BOX (expand_box), action_row, -1);

        /* Apuntamos el pass_entry de RowData al de B (necesario para do_connect). */
        rd->pass_entry = pass_entry;

        /* Conectar señales propias de A y B. */
        g_signal_connect (rd->retry_btn, "clicked",
                          G_CALLBACK (on_retry_clicked), rd);
        g_signal_connect (rd->try_other_btn, "clicked",
                          G_CALLBACK (on_try_other_clicked), rd);
        g_signal_connect (rd->back_btn, "clicked",
                          G_CALLBACK (on_back_clicked), rd);
        g_signal_connect (rd->connect_btn_b, "clicked",
                          G_CALLBACK (on_connect_clicked), rd);
        g_signal_connect (pass_entry, "changed",
                          G_CALLBACK (on_pass_entry_b_changed), rd);
        g_signal_connect_swapped (eye_btn_b, "clicked",
                                  G_CALLBACK (on_eye_clicked), pass_entry);
    }

    g_object_set_data_full (G_OBJECT (outer), "row-data", rd,
                            (GDestroyNotify) row_data_free);
    /* Marca el SSID y device_path en el event_box para identificar la fila desde afuera. */
    g_object_set_data_full (G_OBJECT (event_box), "ssid",
                            g_strdup (ap->ssid), g_free);
    g_object_set_data_full (G_OBJECT (event_box), "device-path",
                            g_strdup (device_path), g_free);

    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_row_clicked), rd);

    if (ap_active) {
        g_signal_connect (action_btn, "clicked",
                          G_CALLBACK (on_disconnect_clicked), rd);
    } else {
        g_signal_connect (action_btn, "clicked",
                          G_CALLBACK (on_connect_clicked), rd);

        if (forget_btn)
            g_signal_connect (forget_btn, "clicked",
                              G_CALLBACK (on_forget_clicked), rd);

        if (pass_entry) {
            g_signal_connect (pass_entry, "key-press-event",
                              G_CALLBACK (on_pass_entry_key_press), rd);

            GtkWidget *pass_row = gtk_widget_get_parent (pass_entry);
            GList     *kids     = gtk_container_get_children (GTK_CONTAINER (pass_row));
            for (GList *k = kids; k; k = k->next) {
                if (GTK_IS_BUTTON (k->data)) {
                    g_signal_connect (k->data, "pressed",
                                      G_CALLBACK (on_eye_pressed), rd);
                    g_signal_connect (k->data, "released",
                                      G_CALLBACK (on_eye_released), rd);
                    break;
                }
            }
            g_list_free (kids);
        }
    }

    return event_box;
}

/* ---------- sección por adaptador ---------- */

static GtkWidget *
make_device_section (NetPopup *popup, NmDevice *dev, gboolean show_header)
{
    GtkWidget *section, *separator;
    GSList    *aps, *a;
    GDBusConnection *conn = popup->conn;

    section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    if (popup->show_separators) {
        GtkWidget *mod_sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (mod_sep, -1, 2);
        gtk_style_context_add_class (gtk_widget_get_style_context (mod_sep), "module-sep");
        GtkCssProvider *sc = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (sc, ".module-sep { background-color: mix(@theme_bg_color, @theme_fg_color, 0.3); min-height: 2px; }", -1, NULL);
        gtk_style_context_add_provider (gtk_widget_get_style_context (mod_sep),
            GTK_STYLE_PROVIDER (sc), GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_unref (sc);
        gtk_box_pack_start (GTK_BOX (section), mod_sep, FALSE, FALSE, 0);
    }

    if (show_header) {
        GtkWidget *header_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_margin_start  (header_row, 12);
        gtk_widget_set_margin_end    (header_row, 12);
        gtk_widget_set_margin_top    (header_row, 8);
        gtk_widget_set_margin_bottom (header_row, 4);

        GtkWidget *header = gtk_label_new (NULL);
        gchar *markup = g_markup_printf_escaped ("<b><small>%s</small></b>", dev->iface);
        gtk_label_set_markup (GTK_LABEL (header), markup);
        g_free (markup);
        gtk_label_set_xalign (GTK_LABEL (header), 0.0);
        gtk_box_pack_start (GTK_BOX (header_row), header, TRUE, TRUE, 0);

        GtkWidget *dev_switch = gtk_switch_new ();
        gtk_switch_set_active (GTK_SWITCH (dev_switch),
                               nm_get_device_enabled (conn, dev->object_path));
        gtk_widget_set_sensitive (dev_switch, nm_get_wifi_enabled (conn));

        DeviceSwitchData *d = g_new0 (DeviceSwitchData, 1);
        d->conn        = conn;
        d->device_path = g_strdup (dev->object_path);
        g_object_set_data_full (G_OBJECT (dev_switch), "switch-data", d,
                                (GDestroyNotify) device_switch_data_free);

        g_signal_connect (dev_switch, "state-set",
                          G_CALLBACK (on_device_switch_toggled), d);
        gtk_box_pack_end (GTK_BOX (header_row), dev_switch, FALSE, FALSE, 0);
        popup->device_switches = g_slist_append (popup->device_switches, dev_switch);

        gtk_box_pack_start (GTK_BOX (section), header_row, FALSE, FALSE, 0);
    }

    aps = nm_get_access_points (conn, dev->object_path);
    for (a = aps; a; a = a->next) {
        if (popup->show_separators) {
            separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
            gtk_box_pack_start (GTK_BOX (section), separator, FALSE, FALSE, 0);
        }
        GtkWidget *r = make_ap_row (a->data, popup, dev->object_path);
        gtk_box_pack_start (GTK_BOX (section), r, FALSE, FALSE, 0);
    }
    nm_ap_list_free (aps);

    return section;
}

/* ---------- sección VPN ---------- */

typedef struct {
    GDBusConnection *conn;
    gchar           *conn_path;
} VpnSwitchData;

static void
vpn_switch_data_free (VpnSwitchData *d)
{
    g_free (d->conn_path);
    g_free (d);
}

static gboolean
on_vpn_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    VpnSwitchData *d = user_data;
    if (state)
        nm_activate_vpn_async   (d->conn, d->conn_path);
    else
        nm_deactivate_vpn_async (d->conn, d->conn_path);
    return FALSE;
}

static void
fill_vpn_section (NetPopup *popup)
{
    /* Vacía y rellena popup->vpn_section. */
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->vpn_section));
    g_list_foreach (kids, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (kids);

    GSList *vpns = nm_get_vpn_connections (popup->conn);
    if (!vpns) {
        gtk_widget_hide (popup->vpn_section);
        return;
    }

    if (popup->show_separators) {
        GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (sep, -1, 2);
        gtk_style_context_add_class (gtk_widget_get_style_context (sep), "module-sep");
        GtkCssProvider *sc = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (sc, ".module-sep { background-color: mix(@theme_bg_color, @theme_fg_color, 0.3); min-height: 2px; }", -1, NULL);
        gtk_style_context_add_provider (gtk_widget_get_style_context (sep),
            GTK_STYLE_PROVIDER (sc), GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_unref (sc);
        gtk_box_pack_start (GTK_BOX (popup->vpn_section), sep, FALSE, FALSE, 0);
    }

    GtkWidget *header_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start  (header_row, 12);
    gtk_widget_set_margin_end    (header_row, 12);
    gtk_widget_set_margin_top    (header_row, 8);
    gtk_widget_set_margin_bottom (header_row, 4);

    GtkWidget *header = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (header), "<b><small>VPN</small></b>");
    gtk_label_set_xalign (GTK_LABEL (header), 0.0);
    gtk_box_pack_start (GTK_BOX (header_row), header, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (popup->vpn_section), header_row, FALSE, FALSE, 0);

    for (GSList *l = vpns; l; l = l->next) {
        NmVpnConnection *vpn = l->data;

        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start  (row, 12);
        gtk_widget_set_margin_end    (row, 12);
        gtk_widget_set_margin_top    (row, 4);
        gtk_widget_set_margin_bottom (row, 4);

        GtkWidget *name_label = gtk_label_new (vpn->name);
        gtk_label_set_xalign   (GTK_LABEL (name_label), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars (GTK_LABEL (name_label), 28);
        gtk_box_pack_start (GTK_BOX (row), name_label, TRUE, TRUE, 0);

        GtkWidget *sw = gtk_switch_new ();
        gtk_switch_set_active (GTK_SWITCH (sw), vpn->active);

        VpnSwitchData *d = g_new0 (VpnSwitchData, 1);
        d->conn      = popup->conn;
        d->conn_path = g_strdup (vpn->conn_path);
        g_object_set_data_full (G_OBJECT (sw), "vpn-switch-data", d,
                                (GDestroyNotify) vpn_switch_data_free);

        g_signal_connect (sw, "state-set",
                          G_CALLBACK (on_vpn_switch_toggled), d);
        gtk_box_pack_end (GTK_BOX (row), sw, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (popup->vpn_section), row, FALSE, FALSE, 0);
    }

    nm_vpn_list_free (vpns);
    gtk_widget_show_all (popup->vpn_section);
}

/* ---------- sección Ethernet ---------- */

static void
fill_eth_section (NetPopup *popup)
{
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->eth_section));
    g_list_foreach (kids, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (kids);

    GSList *eth_devices = nm_get_ethernet_devices (popup->conn);
    if (!eth_devices) {
        gtk_widget_hide (popup->eth_section);
        return;
    }

    for (GSList *d = eth_devices; d; d = d->next) {
        NmDevice *dev = d->data;

        if (popup->show_separators) {
            GtkWidget *eth_sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_size_request (eth_sep, -1, 2);
            gtk_style_context_add_class (gtk_widget_get_style_context (eth_sep), "module-sep");
            GtkCssProvider *sc = gtk_css_provider_new ();
            gtk_css_provider_load_from_data (sc, ".module-sep { background-color: mix(@theme_bg_color, @theme_fg_color, 0.3); min-height: 2px; }", -1, NULL);
            gtk_style_context_add_provider (gtk_widget_get_style_context (eth_sep),
                GTK_STYLE_PROVIDER (sc), GTK_STYLE_PROVIDER_PRIORITY_USER);
            g_object_unref (sc);
            gtk_box_pack_start (GTK_BOX (popup->eth_section), eth_sep, FALSE, FALSE, 0);
        }

        GtkWidget *eth_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_margin_start  (eth_row, 12);
        gtk_widget_set_margin_end    (eth_row, 12);
        gtk_widget_set_margin_top    (eth_row, 8);
        gtk_widget_set_margin_bottom (eth_row, 8);

        GtkWidget *eth_icon = gtk_image_new_from_icon_name (
                                  "network-wired-symbolic", GTK_ICON_SIZE_MENU);
        gtk_box_pack_start (GTK_BOX (eth_row), eth_icon, FALSE, FALSE, 4);

        GtkWidget *eth_label = gtk_label_new (NULL);
        gchar *eth_markup = g_markup_printf_escaped ("<b>%s</b>", dev->iface);
        gtk_label_set_markup (GTK_LABEL (eth_label), eth_markup);
        g_free (eth_markup);
        gtk_label_set_xalign (GTK_LABEL (eth_label), 0.0);
        gtk_box_pack_start (GTK_BOX (eth_row), eth_label, TRUE, TRUE, 0);

        GtkWidget *eth_status = gtk_label_new (_("Connected"));
        gtk_style_context_add_class (gtk_widget_get_style_context (eth_status),
                                     "dim-label");
        gtk_box_pack_end (GTK_BOX (eth_row), eth_status, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (popup->eth_section), eth_row, FALSE, FALSE, 0);
    }
    nm_device_list_free (eth_devices);

    gtk_widget_show_all (popup->eth_section);
}

/* ---------- update functions: actualizan widgets in-place ---------- */

static void
update_top_status (NetPopup *popup)
{
    if (!popup->ui_built) return;

    gchar *primary_ssid = get_primary_ssid (popup->conn);

    /* Si NM dice "conectado" pero hay un CONNECT en curso para ese mismo SSID,
     * ignorarlo — NM miente brevemente antes de confirmar el fallo de auth. */
    if (primary_ssid && g_hash_table_size (popup->ops_in_progress) > 0) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, popup->ops_in_progress);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            OpInProgress *op = value;
            if (op->kind == OP_CONNECT &&
                g_strcmp0 (op->ssid, primary_ssid) == 0) {
                g_free (primary_ssid);
                primary_ssid = NULL;
                break;
            }
        }
    }

    /* Ícono superior: spinner si hay operación en curso en la tabla, ícono si no. */
    if (g_hash_table_size (popup->ops_in_progress) > 0) {
        gtk_spinner_start (GTK_SPINNER (popup->status_spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (popup->status_stack), "spinner");
    } else {
        gtk_spinner_stop (GTK_SPINNER (popup->status_spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (popup->status_stack), "icon");
        if (primary_ssid)
            gtk_image_set_from_icon_name (GTK_IMAGE (popup->status_icon),
                                          "network-wireless-symbolic", GTK_ICON_SIZE_MENU);
        else
            gtk_image_set_from_icon_name (GTK_IMAGE (popup->status_icon),
                                          "network-wireless-disconnected-symbolic", GTK_ICON_SIZE_MENU);
    }

    /* Label de estado */
    if (primary_ssid) {
        gchar *markup = g_markup_printf_escaped (
                            _("Connected to <b>%s</b>"), primary_ssid);
        gtk_label_set_markup (GTK_LABEL (popup->status_label), markup);
        g_free (markup);
    } else if (!nm_get_wifi_enabled (popup->conn)) {
        gtk_label_set_text (GTK_LABEL (popup->status_label), _("Disabled"));
    } else {
        /* Buscar si hay un CONNECT en curso en la tabla */
        const gchar *connecting_ssid = NULL;
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, popup->ops_in_progress);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            OpInProgress *op = value;
            if (op->kind == OP_CONNECT && op->ssid) {
                connecting_ssid = op->ssid;
                break;
            }
        }
        if (connecting_ssid) {
            gchar *markup = g_markup_printf_escaped (
                                _("Connecting to <b>%s</b>…"), connecting_ssid);
            gtk_label_set_markup (GTK_LABEL (popup->status_label), markup);
            g_free (markup);
        } else {
            gtk_label_set_text (GTK_LABEL (popup->status_label), _("Enabled – not connected"));
        }
    }

    g_free (primary_ssid);

    /* Switch global: sincronizar sin disparar handler */
    g_signal_handler_block (popup->wifi_switch, popup->wifi_switch_handler);
    gboolean wifi_on = nm_get_wifi_enabled (popup->conn);
    gtk_switch_set_active (GTK_SWITCH (popup->wifi_switch), wifi_on);
    gtk_switch_set_state  (GTK_SWITCH (popup->wifi_switch), wifi_on);
    g_signal_handler_unblock (popup->wifi_switch, popup->wifi_switch_handler);

    /* Switches por adaptador: sensitividad según switch global */
    for (GSList *l = popup->device_switches; l; l = l->next)
        gtk_widget_set_sensitive (GTK_WIDGET (l->data), wifi_on);
}

static void
update_eth_section (NetPopup *popup)
{
    if (!popup->ui_built) return;
    fill_eth_section (popup);
}

static void
update_vpn_section (NetPopup *popup)
{
    if (!popup->ui_built) return;
    fill_vpn_section (popup);
}

static void
update_devices_section (NetPopup *popup)
{
    if (!popup->ui_built) return;

    /* Si hay un expand abierto NO reconstruimos la lista de filas
     * (regla 1A: congelar filas mientras el usuario interactúa).
     * EXCEPCIÓN: si el expand actual es "pasivo" (solo muestra "Conectando…"
     * sin botones ni entry), no hay nada que el usuario esté tocando, así
     * que sí reconstruimos y después reabrimos esa misma fila. */
    gchar    *passive_ssid = NULL;
    gchar    *passive_dev  = NULL;
    gboolean  passive      = FALSE;
    if (popup->current_expand_box) {
        passive = (g_object_get_data (G_OBJECT (popup->current_expand_box),
                                      "passive-connecting") != NULL);
        if (!passive) {
            check_ops_progress (popup);
            return;
        }
        /* Sacar ssid + device_path del expand abierto antes de destruirlo.
         * El expand_box está dentro de outer, que está dentro del event_box. */
        GtkWidget *outer_w = gtk_widget_get_parent (popup->current_expand_box);
        if (outer_w) {
            GtkWidget *evb = gtk_widget_get_parent (outer_w);
            if (evb) {
                const gchar *s = g_object_get_data (G_OBJECT (evb), "ssid");
                const gchar *d = g_object_get_data (G_OBJECT (evb), "device-path");
                if (s) passive_ssid = g_strdup (s);
                if (d) passive_dev  = g_strdup (d);
            }
        }
    }

    /* Limpiar y reconstruir todas las secciones de adaptadores. */
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *k = kids; k; k = k->next) {
        /* Saltar la sección VPN, que tiene tag */
        if (k->data == popup->vpn_section) continue;
        gtk_widget_destroy (GTK_WIDGET (k->data));
    }
    g_list_free (kids);

    g_slist_free (popup->device_switches);
    popup->device_switches = NULL;

    GSList *devices = nm_get_wifi_devices (popup->conn);
    gint    n_devices = g_slist_length (devices);
    for (GSList *d = devices; d; d = d->next) {
        GtkWidget *section = make_device_section (popup, d->data, n_devices > 1);
        gtk_box_pack_start (GTK_BOX (popup->content_box), section, FALSE, FALSE, 0);
        gtk_box_reorder_child (GTK_BOX (popup->content_box), section, -1);
    }
    nm_device_list_free (devices);

    /* La sección VPN debe quedar al final. */
    gtk_box_reorder_child (GTK_BOX (popup->content_box), popup->vpn_section, -1);

    gtk_widget_show_all (popup->content_box);

    /* Tras reconstruir, los expand_boxes vuelven a no_show_all + hidden por make_ap_row,
     * así que no quedan abiertos. */
    popup->current_expand_box = NULL;

    /* Las operaciones en curso ya no tienen sus widgets antiguos, las descartamos. */
    g_hash_table_remove_all (popup->ops_in_progress);

    /* Si veníamos de un expand "Conectando…" pasivo, reabrir esa misma fila
     * para que el usuario siga viendo el estado actualizado sin reclickear. */
    if (passive_ssid) {
        reopen_expand_for_ssid (popup, passive_ssid, passive_dev);
        g_free (passive_ssid);
        g_free (passive_dev);
    }
}

/* ---------- refresh coalescido ---------- */

/* Idle source: una sola pasada de actualización por ráfaga de señales. */
static gboolean
refresh_ui_cb (gpointer user_data)
{
    NetPopup *popup = user_data;
    g_object_set_data (G_OBJECT (popup->window), "refresh-pending", NULL);

    if (!popup->ui_built) return G_SOURCE_REMOVE;
    if (!gtk_widget_get_visible (popup->window)) return G_SOURCE_REMOVE;

    /* Primero chequear si alguna operación en curso ya se confirmó. */
    check_ops_progress (popup);

    /* Promover a failed_ssids los SSIDs cuya ventana de pending_attempts
     * ya expiró sin que NM haya confirmado conexión. Así el próximo
     * update_devices_section construye estado A en vez de "Conectando…". */
    process_pending_attempts (popup);

    /* Después refrescar las partes que no dependen del expand abierto. */
    update_top_status     (popup);
    update_eth_section    (popup);
    update_vpn_section    (popup);
    update_devices_section (popup);

    return G_SOURCE_REMOVE;
}

static void
schedule_refresh_ui (NetPopup *popup)
{
    if (!popup) return;
    if (!gtk_widget_get_visible (popup->window)) return;
    /* Coalescer: si ya hay un refresh pendiente, no encolar otro. */
    if (g_object_get_data (G_OBJECT (popup->window), "refresh-pending"))
        return;
    g_object_set_data (G_OBJECT (popup->window), "refresh-pending",
                       GINT_TO_POINTER (1));
    g_idle_add (refresh_ui_cb, popup);
}

/* Callback que NM dispara: agendamos refresh idle (coalescido). */
static void
on_nm_signal_popup (gpointer user_data)
{
    NetPopup *popup = user_data;
    schedule_refresh_ui (popup);
}

/* ---------- construcción inicial de la UI ---------- */

static void
rebuild_ui (NetPopup *popup)
{
    GDBusConnection *conn = popup->conn;

    /* Limpiar zonas (por si rebuild_ui se llama dos veces) */
    GList *children = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    g_list_foreach (children, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (children);

    GList *top_children = gtk_container_get_children (GTK_CONTAINER (popup->top_box));
    g_list_foreach (top_children, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (top_children);

    g_slist_free (popup->device_switches);
    popup->device_switches = NULL;
    popup->current_expand_box = NULL;

    /* ---- Barra superior fija ---- */
    GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (top_row, 6);
    gtk_widget_set_margin_end    (top_row, 12);
    gtk_widget_set_margin_top    (top_row, 8);
    gtk_widget_set_margin_bottom (top_row, 8);

    popup->status_icon = gtk_image_new_from_icon_name (
                             "network-wireless-disconnected-symbolic",
                             GTK_ICON_SIZE_MENU);
    gtk_widget_set_valign (popup->status_icon, GTK_ALIGN_CENTER);

    popup->status_spinner = gtk_spinner_new ();
    gtk_widget_set_valign (popup->status_spinner, GTK_ALIGN_CENTER);

    popup->status_stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (popup->status_stack),
                                   GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named (GTK_STACK (popup->status_stack), popup->status_icon,    "icon");
    gtk_stack_add_named (GTK_STACK (popup->status_stack), popup->status_spinner, "spinner");
    gtk_stack_set_visible_child_name (GTK_STACK (popup->status_stack), "icon");
    gtk_widget_set_margin_bottom (popup->status_stack, 22);
    gtk_box_pack_start (GTK_BOX (top_row), popup->status_stack, FALSE, FALSE, 0);

    GtkWidget *center_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start (GTK_BOX (top_row), center_box, TRUE, TRUE, 0);

    /* Botón Actualizar */
    popup->refresh_button = gtk_button_new ();
    GtkWidget *refresh_box  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *refresh_icon = gtk_image_new_from_icon_name (
                                  "view-refresh-symbolic", GTK_ICON_SIZE_MENU);
    popup->refresh_label = gtk_label_new (popup->scanning ? _("Updating…") : _("Refresh"));
    gtk_box_pack_start (GTK_BOX (refresh_box), refresh_icon,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (refresh_box), popup->refresh_label, FALSE, FALSE, 0);
    gtk_container_add  (GTK_CONTAINER (popup->refresh_button), refresh_box);
    gtk_button_set_relief (GTK_BUTTON (popup->refresh_button), GTK_RELIEF_NONE);
    gtk_widget_set_valign (popup->refresh_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (popup->refresh_button, 16);
    gtk_widget_set_sensitive (popup->refresh_button, !popup->scanning);
    g_signal_connect (popup->refresh_button, "clicked",
                      G_CALLBACK (on_refresh_clicked), popup);
    gtk_box_pack_end (GTK_BOX (top_row), popup->refresh_button, FALSE, FALSE, 0);

    /* Fila Wi-Fi + switch */
    GtkWidget *wifi_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start (GTK_BOX (center_box), wifi_row, FALSE, FALSE, 0);

    GtkWidget *wifi_label = gtk_label_new (_("Wi-Fi"));
    gtk_label_set_xalign (GTK_LABEL (wifi_label), 0.0);
    gtk_box_pack_start (GTK_BOX (wifi_row), wifi_label, FALSE, FALSE, 0);

    popup->wifi_switch = gtk_switch_new ();
    gtk_switch_set_active (GTK_SWITCH (popup->wifi_switch),
                           nm_get_wifi_enabled (conn));
    popup->wifi_switch_handler = g_signal_connect (
        popup->wifi_switch, "state-set",
        G_CALLBACK (on_wifi_switch_toggled), popup);
    gtk_box_pack_start (GTK_BOX (wifi_row), popup->wifi_switch, FALSE, FALSE, 0);

    /* Label de estado */
    popup->status_label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (popup->status_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (popup->status_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (popup->status_label), 28);
    gtk_box_pack_start (GTK_BOX (center_box), popup->status_label, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (popup->top_box), top_row, FALSE, FALSE, 0);


    /* Contenedor sección Ethernet (vacío por defecto, se llena en update_eth_section). */
    popup->eth_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (popup->top_box), popup->eth_section, FALSE, FALSE, 0);

    /* Contenedor sección VPN (al fondo del content_box, se llena en update_vpn_section). */
    popup->vpn_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_end (GTK_BOX (popup->content_box), popup->vpn_section, FALSE, FALSE, 0);

    popup->ui_built = TRUE;

    /* Llenar las secciones por primera vez. */
    update_top_status      (popup);
    update_eth_section     (popup);
    update_devices_section (popup);
    update_vpn_section     (popup);
}

/* ---------- diálogo "Conectar a red oculta" ---------- */

static void
on_eye_clicked (GtkWidget *btn, GtkEntry *entry)
{
    (void) btn;
    gtk_entry_set_visibility (entry, !gtk_entry_get_visibility (entry));
}

static void
on_hidden_response (GtkDialog *dialog, gint response, GDBusConnection *conn)
{
    if (response == GTK_RESPONSE_OK) {
        GtkWidget *ssid_entry = g_object_get_data (G_OBJECT (dialog), "ssid_entry");
        GtkWidget *sec_combo  = g_object_get_data (G_OBJECT (dialog), "sec_combo");
        GtkWidget *pass_entry = g_object_get_data (G_OBJECT (dialog), "pass_entry");

        const gchar *ssid     = gtk_entry_get_text (GTK_ENTRY (ssid_entry));
        const gchar *password = gtk_entry_get_text (GTK_ENTRY (pass_entry));
        gboolean     secure   = gtk_combo_box_get_active (GTK_COMBO_BOX (sec_combo)) == 1;

        if (ssid && *ssid) {
            GSList *devs = nm_get_wifi_devices (conn);
            if (devs) {
                NmDevice *dev = devs->data;
                nm_add_and_activate_connection_async (
                    conn, dev->object_path, "/", ssid,
                    secure ? password : NULL, TRUE);
                nm_device_list_free (devs);
            }
        }
    }
    gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
on_hidden_network_clicked (GtkWidget *btn, NetPopup *popup)
{
    (void) btn;
    GDBusConnection *conn = popup->conn;
    if (!conn) return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Connect to hidden network"),
        NULL,
        0,
        _("Cancel"), GTK_RESPONSE_CANCEL,
        _("Connect"), GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

    GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (area), 12);

    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
    gtk_box_pack_start (GTK_BOX (area), grid, TRUE, TRUE, 0);

    GtkWidget *ssid_label = gtk_label_new (_("Network name:"));
    gtk_label_set_xalign (GTK_LABEL (ssid_label), 0.0);
    GtkWidget *ssid_entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (ssid_entry), TRUE);
    gtk_grid_attach (GTK_GRID (grid), ssid_label, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), ssid_entry, 1, 0, 1, 1);

    GtkWidget *sec_label = gtk_label_new (_("Security:"));
    gtk_label_set_xalign (GTK_LABEL (sec_label), 0.0);
    GtkWidget *sec_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (sec_combo), _("None"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (sec_combo), "WPA2/WPA3");
    gtk_combo_box_set_active (GTK_COMBO_BOX (sec_combo), 1);
    gtk_grid_attach (GTK_GRID (grid), sec_label, 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), sec_combo, 1, 1, 1, 1);

    GtkWidget *pass_label = gtk_label_new (_("Password:"));
    gtk_label_set_xalign (GTK_LABEL (pass_label), 0.0);
    GtkWidget *pass_box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *pass_entry = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (pass_entry), FALSE);
    gtk_entry_set_activates_default (GTK_ENTRY (pass_entry), TRUE);
    GtkWidget *eye_btn  = gtk_button_new ();
    GtkWidget *eye_icon = gtk_image_new_from_icon_name ("view-reveal-symbolic",
                                                         GTK_ICON_SIZE_MENU);
    gtk_button_set_image  (GTK_BUTTON (eye_btn), eye_icon);
    gtk_button_set_relief (GTK_BUTTON (eye_btn), GTK_RELIEF_NONE);
    g_signal_connect (eye_btn, "clicked", G_CALLBACK (on_eye_clicked), pass_entry);
    gtk_box_pack_start (GTK_BOX (pass_box), pass_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (pass_box), eye_btn,    FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), pass_label, 0, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pass_box,   1, 2, 1, 1);

    g_object_set_data (G_OBJECT (dialog), "ssid_entry", ssid_entry);
    g_object_set_data (G_OBJECT (dialog), "sec_combo",  sec_combo);
    g_object_set_data (G_OBJECT (dialog), "pass_entry", pass_entry);

    g_signal_connect (dialog, "response", G_CALLBACK (on_hidden_response), conn);
    gtk_widget_show_all (dialog);
}

static void
on_advanced_clicked (GtkWidget *btn, gpointer user_data)
{
    (void) btn; (void) user_data;

    if (g_find_program_in_path ("nm-connection-editor")) {
        g_spawn_command_line_async ("nm-connection-editor", NULL);
        return;
    }
    if (g_find_program_in_path ("cmst")) {
        g_spawn_command_line_async ("cmst", NULL);
        return;
    }
    if (g_find_program_in_path ("connman-gtk")) {
        g_spawn_command_line_async ("connman-gtk", NULL);
        return;
    }
    if (g_find_program_in_path ("nmtui")) {
        g_spawn_command_line_async ("xterm -e nmtui", NULL);
        return;
    }
}

/* ---------- event handlers de la ventana ---------- */

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, NetPopup *popup)
{
    (void) widget;
    if (event->keyval == GDK_KEY_Escape) {
        popup_hide (popup);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static gboolean
on_focus_out (GtkWidget *widget, GdkEventFocus *ev, NetPopup *popup)
{
    (void) widget; (void) ev;
    popup_hide (popup);
    return FALSE;
}

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *event, NetPopup *popup)
{
    gint x, y, w, h;
    (void) widget;

    gtk_window_get_position (GTK_WINDOW (popup->window), &x, &y);
    gtk_window_get_size     (GTK_WINDOW (popup->window), &w, &h);

    if (event->x_root < x || event->x_root > x + w ||
        event->y_root < y || event->y_root > y + h)
        popup_hide (popup);

    return GDK_EVENT_PROPAGATE;
}

/* ---------- API publica ---------- */

NetPopup *
popup_create (XfcePanelPlugin *plugin, GtkWidget *button)
{
    (void) plugin; (void) button;
    NetPopup  *popup = g_new0 (NetPopup, 1);
    GtkWidget *win, *outer_box, *scroll;

    win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated        (GTK_WINDOW (win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW (win), TRUE);
    gtk_window_set_skip_pager_hint  (GTK_WINDOW (win), TRUE);
    gtk_window_set_resizable        (GTK_WINDOW (win), FALSE);
    gtk_window_set_type_hint        (GTK_WINDOW (win),
                                     GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_widget_set_size_request     (win, -1, -1);

    outer_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (outer_box), "popup");
    gtk_container_add (GTK_CONTAINER (win), outer_box);

    popup->top_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (outer_box), popup->top_box, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (outer_box), scroll, TRUE, TRUE, 0);
    popup->scroll = scroll;

    popup->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (scroll), popup->content_box);

    /* Zona fija inferior */
    GtkWidget *bottom_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_end (GTK_BOX (outer_box), bottom_sep, FALSE, FALSE, 0);

    GtkWidget *adv_btn = gtk_button_new_with_label (_("Advanced settings…"));
    gtk_button_set_relief (GTK_BUTTON (adv_btn), GTK_RELIEF_NONE);
    gtk_widget_set_margin_start  (adv_btn, 8);
    gtk_widget_set_margin_end    (adv_btn, 8);
    gtk_widget_set_margin_top    (adv_btn, 4);
    gtk_widget_set_margin_bottom (adv_btn, 4);
    g_signal_connect (adv_btn, "clicked",
                      G_CALLBACK (on_advanced_clicked), NULL);
    gtk_box_pack_end (GTK_BOX (outer_box), adv_btn, FALSE, FALSE, 0);

    GtkWidget *hidden_btn = gtk_button_new_with_label (_("Connect to hidden network…"));
    gtk_button_set_relief (GTK_BUTTON (hidden_btn), GTK_RELIEF_NONE);
    gtk_widget_set_margin_start  (hidden_btn, 8);
    gtk_widget_set_margin_end    (hidden_btn, 8);
    gtk_widget_set_margin_top    (hidden_btn, 0);
    gtk_widget_set_margin_bottom (hidden_btn, 4);
    g_signal_connect (hidden_btn, "clicked",
                      G_CALLBACK (on_hidden_network_clicked), popup);
    gtk_box_pack_end (GTK_BOX (outer_box), hidden_btn, FALSE, FALSE, 0);

    g_signal_connect (win, "key-press-event", G_CALLBACK (on_key_press), popup);

    popup->window = win;
    popup->ops_in_progress = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, op_free);
    popup->failed_ssids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
    popup->pending_attempts = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, g_free);
    return popup;
}

void
popup_show (NetPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button,
            GDBusConnection *conn,
            gint popup_width, gint popup_height)
{
    GdkDisplay *display;
    GdkSeat    *seat;
    (void) plugin;

    popup->conn         = conn;
    popup->popup_width  = popup_width;
    popup->popup_height = popup_height;
    gtk_widget_set_size_request (popup->window, popup_width, -1);
    gtk_widget_set_size_request (popup->scroll, popup_width, popup_height);

    /* Procesar intentos pendientes de la sesión anterior del popup: para cada
     * SSID que tenía un intento de conexión sin resolver al cerrarse, mirar
     * si terminó conectada o no, y marcarla en failed_ssids si corresponde.
     * Esto debe ocurrir ANTES de rebuild_ui / update_devices_section, así
     * cuando se arman las filas ya tienen la información correcta. */
    process_pending_attempts (popup);

    /* Construir UI si es la primera vez. */
    if (!popup->ui_built) {
        rebuild_ui (popup);
    } else {
        /* Ya estaba construida; refrescar estado por las dudas. */
        update_top_status      (popup);
        update_eth_section     (popup);
        update_devices_section (popup);
        update_vpn_section     (popup);
    }

    /* Suscribirse a señales DBus de NM (las gestiona el popup mientras está abierto). */
    if (!popup->signal_ids)
        popup->signal_ids = nm_subscribe_signals (conn, on_nm_signal_popup, popup);

    gtk_widget_realize (popup->window);
    position_popup (popup, button);
    gtk_widget_show_all (popup->window);
    gtk_window_present (GTK_WINDOW (popup->window));

    /* Pedir escaneo al abrir. */
    {
        GSList *devs = nm_get_wifi_devices (conn);
        for (GSList *l = devs; l; l = l->next) {
            NmDevice *dev = l->data;
            nm_request_scan (conn, dev->object_path);
        }
        nm_device_list_free (devs);
    }

    display = gtk_widget_get_display (popup->window);
    seat    = gdk_display_get_default_seat (display);
    popup->button = button;
    {
        GdkGrabStatus grab_status;
        grab_status = gdk_seat_grab (seat,
                                     gtk_widget_get_window (popup->window),
                                     GDK_SEAT_CAPABILITY_ALL_POINTING,
                                     TRUE, NULL, NULL, NULL, NULL);
        if (grab_status == GDK_GRAB_SUCCESS) {
            popup->press_handler =
                g_signal_connect (popup->window, "button-press-event",
                                  G_CALLBACK (on_button_press), popup);
        } else {
            popup->press_handler =
                g_signal_connect (popup->window, "focus-out-event",
                                  G_CALLBACK (on_focus_out), popup);
        }
    }
}

void
popup_hide (NetPopup *popup)
{
    GdkDisplay *display;
    GdkSeat    *seat;

    if (!gtk_widget_get_visible (popup->window))
        return;

    display = gtk_widget_get_display (popup->window);
    seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);

    if (popup->press_handler) {
        g_signal_handler_disconnect (popup->window, popup->press_handler);
        popup->press_handler = 0;
    }

    /* Desuscribirse de las señales DBus mientras está cerrado: el plugin
     * sigue suscripto por su cuenta para mantener el ícono del panel al día. */
    if (popup->signal_ids) {
        nm_unsubscribe_signals (popup->conn, popup->signal_ids);
        popup->signal_ids = NULL;
    }

    /* Cancelar timeouts de operaciones en curso. Las acciones DBus ya enviadas
     * a NM siguen su curso (NM las ejecutará), pero la UI no espera más. */
    g_hash_table_remove_all (popup->ops_in_progress);

    /* Cancelar cooldown del botón Actualizar. */
    if (popup->scan_timeout_id) {
        g_source_remove (popup->scan_timeout_id);
        popup->scan_timeout_id = 0;
    }
    popup->scanning = FALSE;

    /* Cerrar expand abierto para que al reabrir el popup esté limpio. */
    if (popup->current_expand_box) {
        gtk_widget_hide (popup->current_expand_box);
        popup->current_expand_box = NULL;
    }

    if (popup->button)
        g_signal_handlers_block_matched (popup->button,
            G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, popup->button);
    gtk_widget_hide (popup->window);
    if (popup->button)
        g_signal_handlers_unblock_matched (popup->button,
            G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, popup->button);
}

void
popup_destroy (NetPopup *popup)
{
    if (!popup) return;
    popup_hide (popup);
    if (popup->ops_in_progress)
        g_hash_table_destroy (popup->ops_in_progress);
    if (popup->failed_ssids)
        g_hash_table_destroy (popup->failed_ssids);
    if (popup->pending_attempts)
        g_hash_table_destroy (popup->pending_attempts);
    gtk_widget_destroy (popup->window);
    g_free (popup);
}
