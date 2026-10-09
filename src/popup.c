/* popup.c — núcleo del popup: ventana (crear/mostrar/ocultar/destruir),
 * posicionamiento y grab, operaciones en curso e intentos pendientes,
 * helpers compartidos, diálogos (red oculta) y manejadores de la ventana.
 * La fila de red vive en popup-rows.c; las secciones y el refresco, en
 * popup-sections.c; lo compartido se declara en popup-private.h. */

#include "popup-private.h"
#include <string.h>

typedef struct {
    NetPopup  *popup;
    GtkWidget *button;
} GrabData;

/* Prototipos locales de este módulo. */
static void     op_free         (gpointer p);
static gboolean on_button_press (GtkWidget *widget, GdkEventButton *event, NetPopup *popup);
static gboolean on_focus_out    (GtkWidget *widget, GdkEventFocus *ev, NetPopup *popup);

/* Libera una cadena sensible (contraseña) sobreescribiéndola primero con
 * ceros, para que el texto no quede flotando en la memoria liberada.
 * El puntero volátil evita que el compilador elimine el borrado. */
void
secure_wipe_free (gchar *s)
{
    if (!s) return;
    volatile gchar *p = s;
    while (*p) { *p = '\0'; p++; }
    g_free (s);
}

/* ---------- grab diferido al mapear la ventana ---------- */

static gboolean
on_window_mapped (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void) event;
    GrabData  *gd   = user_data;
    NetPopup  *popup = gd->popup;
    GdkDisplay *display = gtk_widget_get_display (widget);
    GdkSeat    *seat    = gdk_display_get_default_seat (display);

    /* Desconectarse inmediatamente — solo ejecutar una vez por apertura. */
    g_signal_handlers_disconnect_by_func (widget, on_window_mapped, gd);

    GdkGrabStatus grab_status = gdk_seat_grab (
            seat,
            gtk_widget_get_window (widget),
            GDK_SEAT_CAPABILITY_ALL,
            TRUE, NULL, NULL, NULL, NULL);

    popup->grab_active = (grab_status == GDK_GRAB_SUCCESS);
    if (grab_status == GDK_GRAB_SUCCESS) {
        popup->press_handler =
            g_signal_connect (popup->window, "button-press-event",
                              G_CALLBACK (on_button_press), popup);
    } else {
        popup->press_handler =
            g_signal_connect (popup->window, "focus-out-event",
                              G_CALLBACK (on_focus_out), popup);
    }

    g_free (gd);
    return FALSE; /* no consumir el evento */
}

/* ---------- menú contextual: devolver la captura al cerrarse ----------
 *
 * Solo puede haber una captura del mouse a la vez: el menú contextual se la
 * lleva al abrirse y la suelta al cerrarse, y el popup se queda sin ella
 * (dejaría de cerrarse al clickear afuera). Por eso, al cerrarse el menú se
 * vuelve a pedir. Se hace diferido (idle) porque GTK emite "deactivate"
 * ANTES de soltar su captura y ANTES de ejecutar la opción elegida; así la
 * opción corre con los datos de la fila todavía vivos y recién después se
 * destruye el menú y se refrescan las filas. */

static gboolean
menu_closed_idle (gpointer user_data)
{
    NetPopup *popup = user_data;
    popup->menu_idle_id = 0;

    if (popup->ctx_menu) {
        gtk_widget_destroy (popup->ctx_menu);
        popup->ctx_menu = NULL;
    }

    if (popup->grab_active && gtk_widget_get_visible (popup->window)) {
        GdkSeat *seat = gdk_display_get_default_seat (
                            gtk_widget_get_display (popup->window));
        if (gdk_seat_grab (seat, gtk_widget_get_window (popup->window),
                           GDK_SEAT_CAPABILITY_ALL, TRUE,
                           NULL, NULL, NULL, NULL) != GDK_GRAB_SUCCESS)
            g_warning ("xfce-net-plugin: no se pudo recuperar la captura "
                       "del mouse tras cerrar el menú contextual");
    }

    /* El refresco estuvo congelado mientras el menú estaba abierto. */
    schedule_refresh_ui (popup);
    return G_SOURCE_REMOVE;
}

void
popup_menu_closed (NetPopup *popup)
{
    if (!popup->menu_idle_id)
        popup->menu_idle_id = g_idle_add (menu_closed_idle, popup);
}

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

gchar *
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

/* Venció el tiempo de espera de una operación (conectar o desconectar).
 * Si era una conexión que fracasó, la red queda marcada como fallida, se
 * reconstruyen las filas y se reabre el expand de esa red en estado A, sin
 * que el usuario tenga que volver a clickearla. */
gboolean
op_timeout_cb (gpointer user_data)
{
    OpInProgress *op = user_data;
    op->timeout_id = 0;

    /* Prórroga única: si es un intento de conexión y la antena TODAVÍA está
     * intentando (autenticando, pidiendo IP, etc.), no declaramos fallo aún.
     * Le damos una sola tanda extra de OP_TIMEOUT_MS. Si al vencer esa tanda
     * sigue sin conectar, ahí sí se declara el fallo (este mismo callback
     * vuelve a entrar, pero op->extended ya estará en TRUE). */
    if (op->kind == OP_CONNECT && !op->extended && op->popup &&
        device_is_connecting (op->popup->conn, op->device_path)) {
        op->extended   = TRUE;
        op->timeout_id = g_timeout_add (OP_TIMEOUT_MS, op_timeout_cb, op);
        return G_SOURCE_REMOVE;
    }

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
                              make_ssid_dev_key (op->ssid, op->device_path),
                              GINT_TO_POINTER (1));
        /* Ya procesamos visualmente el fallo, no es "pending". */
        {
            gchar *attempt_key = make_ssid_dev_key (op->ssid, op->device_path);
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
        nm_cache_begin (popup_ref->conn);
        update_devices_section (popup_ref);
        reopen_expand_for_ssid (popup_ref, failed_ssid, failed_dev);
        nm_cache_end ();
        g_free (failed_dev);
        g_free (failed_ssid);
    }
    return G_SOURCE_REMOVE;
}

/* Las tres preguntas sobre el estado del adaptador leen el mismo dato con
 * nm_get_device_state (leer estado del dispositivo). Antes eran tres copias
 * casi idénticas de la misma llamada al bus. */

/* ¿Conectado de verdad? (estado 100 = activado) */
gboolean
device_is_activated (GDBusConnection *conn, const gchar *device_path)
{
    return nm_get_device_state (conn, device_path) == 100;
}

/* ¿Desconectado? 30 = desconectado, 20 = no disponible, 10 = no gestionado.
 * (0 = no se pudo leer: no se considera desconectado.) */
gboolean
device_is_disconnected (GDBusConnection *conn, const gchar *device_path)
{
    guint32 state = nm_get_device_state (conn, device_path);
    return (state > 0 && state <= 30);
}

/* ¿TODAVÍA está intentando conectar?
 * Estados 40-90: 40=PREPARE 50=CONFIG 60=NEED_AUTH 70=IP_CONFIG
 * 80=IP_CHECK 90=SECONDARIES. En ese rango la conexión sigue en curso,
 * así que no debemos declarar fallo todavía: merece la prórroga única. */
gboolean
device_is_connecting (GDBusConnection *conn, const gchar *device_path)
{
    guint32 state = nm_get_device_state (conn, device_path);
    return (state >= 40 && state <= 90);
}

/* ---------- Intentos pendientes (popup cerrado durante conexión) ---------- */

/* Procesa la tabla de intentos pendientes. Para cada intento ("ssid|adaptador"):
 *   - Menos de 20s → todavía en espera, no se toca.
 *   - Entre 20s y 60s → si el adaptador no quedó conectado, la red pasa a
 *     failed_ssids (interfaz A/B).
 *   - Más de 60s → se descarta sin marcar fallo (ya no se distingue un fallo
 *     de una desconexión manual hecha desde afuera).
 * Al final elimina todos los intentos de más de 20s. */
void
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
        gint64      *ts   = value;
        gint64       age_secs = (now - *ts) / G_USEC_PER_SEC;

        if (age_secs > PENDING_ATTEMPT_MAX_AGE_SECS) {
            /* Intento viejo: a esta altura ya no se puede distinguir un fallo
             * real de una desconexión manual hecha desde afuera del plugin
             * (nmtui, terminal). Descartar sin marcar fallo — cualquier marca
             * tardía sería confusa. La limpieza al final lo elimina. */
            continue;
        }

        if (age_secs * 1000 < OP_TIMEOUT_MS)
            continue;  /* todavía dentro del tiempo de espera, no declarar fallo */

        /* La clave es "ssid|device_path" y es la misma que usa failed_ssids,
         * así que se reutiliza tal cual. Siempre trae adaptador: todos los
         * intentos se registran desde una fila, que conoce su adaptador. */
        const gchar *k   = key;
        const gchar *sep = strrchr (k, '|');
        if (!sep || !sep[1])
            continue;   /* clave mal formada: la limpieza final la elimina */
        if (!device_is_activated (popup->conn, sep + 1))
            to_mark_failed = g_slist_prepend (to_mark_failed, g_strdup (k));
    }

    for (GSList *l = to_mark_failed; l; l = l->next) {
        g_hash_table_replace (popup->failed_ssids,
                              g_strdup ((const gchar *) l->data),
                              GINT_TO_POINTER (1));
    }
    g_slist_free_full (to_mark_failed, g_free);

    /* Eliminar los intentos cuya ventana de 20s ya expiró: a esta altura ya
     * fueron evaluados (marcados como fallo o descartados). Los más recientes
     * se conservan para que make_ap_row pueda forzar "no conectada" durante
     * ese período (NM puede mentir sobre el estado activo hasta que el AP
     * rechace la auth). Antes el umbral era de 60s por error: las entradas
     * de 20-60s quedaban dando vueltas sin servir para nada. */
    GHashTableIter iter2;
    gpointer       key2, value2;
    g_hash_table_iter_init (&iter2, popup->pending_attempts);
    while (g_hash_table_iter_next (&iter2, &key2, &value2)) {
        gint64 *ts = value2;
        gint64  age_ms = (now - *ts) / 1000;
        if (age_ms >= (gint64) OP_TIMEOUT_MS)
            g_hash_table_iter_remove (&iter2);
    }
}

/* Revisa todas las operaciones en curso y resuelve las que ya se completaron. */
void
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
                    /* La misma clave sirve para ambas tablas. */
                    {
                        gchar *fk = make_ssid_dev_key (op->ssid, op->device_path);
                        g_hash_table_remove (popup->failed_ssids, fk);
                        /* Limpiar intento pendiente: ya resuelto en este ciclo. */
                        g_hash_table_remove (popup->pending_attempts, fk);
                        g_free (fk);
                    }
                    if (op->expand_box && GTK_IS_WIDGET (op->expand_box)) {
                        gtk_widget_hide (op->expand_box);
                        if (popup->current_expand_box == op->expand_box)
                            popup->current_expand_box = NULL;
                    }
                    clear_open_highlight (popup);
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
                clear_open_highlight (popup);
            }
        }
    }

    for (GSList *l = to_remove; l; l = l->next) {
        g_hash_table_remove (popup->ops_in_progress, l->data);
        g_free (l->data);
    }
    g_slist_free (to_remove);
}

/* ---------- verificación robusta de existencia de íconos del tema ----------
 *
 * gtk_icon_theme_has_icon devuelve TRUE en cuanto el nombre está declarado
 * en el index.theme del tema, sin comprobar que el archivo realmente exista.
 * Hay temas (notablemente RedmondX y RedmondX-Light) que declaran nombres
 * como nm-secure-lock pero el archivo en disco es un enlace simbólico roto.
 *
 * Esta función pide a GTK que resuelva el ícono al tamaño deseado, obtiene
 * la ruta del archivo final, y verifica con g_file_test que ese archivo
 * exista de verdad. Solo si las tres cosas (lookup OK + filename OK +
 * archivo presente) se cumplen, devuelve TRUE. */
static gboolean
theme_icon_exists_real (const gchar *name, gint size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    GtkIconInfo  *info  = gtk_icon_theme_lookup_icon (theme, name, size, 0);
    if (!info)
        return FALSE;

    const gchar *filename = gtk_icon_info_get_filename (info);
    gboolean ok = (filename != NULL) && g_file_test (filename, G_FILE_TEST_EXISTS);
    g_object_unref (info);
    return ok;
}

/* ---------- ícono de candado superpuesto (helper reutilizable) ----------
 *
 * Devuelve un GtkImage listo para superponer como overlay encima de otro
 * widget. Prueba en orden:
 *   1) nm-vpn-active-lock (existe en muchos temas que soportan nm-applet).
 *   2) nm-secure-lock     (el nombre histórico de nm-applet).
 *   3) /org/xfce/net-plugin/icons/nm-secure-lock.svg (recurso embebido en
 *      el propio .so del plugin, garantiza que siempre se vea algo).
 *
 * Usa theme_icon_exists_real para evitar caer en enlaces rotos. */
GtkWidget *
make_lock_overlay_image (gint icon_size)
{
    GtkWidget *lock_img = NULL;

    if (theme_icon_exists_real ("nm-vpn-active-lock", icon_size)) {
        lock_img = gtk_image_new_from_icon_name ("nm-vpn-active-lock",
                                                 GTK_ICON_SIZE_MENU);
    } else if (theme_icon_exists_real ("nm-secure-lock", icon_size)) {
        lock_img = gtk_image_new_from_icon_name ("nm-secure-lock",
                                                 GTK_ICON_SIZE_MENU);
    } else {
        /* Último recurso: SVG embebido en el binario del plugin. */
        lock_img = gtk_image_new_from_resource (
            "/org/xfce/net-plugin/icons/nm-secure-lock.svg");
    }

    gtk_image_set_pixel_size (GTK_IMAGE (lock_img), icon_size);
    gtk_widget_set_halign (lock_img, GTK_ALIGN_FILL);
    gtk_widget_set_valign (lock_img, GTK_ALIGN_FILL);
    return lock_img;
}

/* ---------- ícono de señal Wi-Fi (usado también por plugin.c) ----------
 *
 * Comportamiento:
 *   - Red abierta:  ícono de señal normal (nm-signal-* con fallback freedesktop).
 *   - Red segura:   ícono de señal normal + candado superpuesto via
 *                   make_lock_overlay_image (que tiene su propio fallback).
 *
 * Las variantes integradas nm-signal-{nivel}-secure se descartan porque,
 * cuando el tema activo no las provee, GTK cae a hicolor — y la versión de
 * hicolor (instalada con network-manager-applet) es genérica y en algunos
 * temas (notablemente Papirus en 16px) se ve mal. Usar siempre el candado
 * superpuesto garantiza un resultado predecible en todos los temas. */

GtkWidget *
make_signal_icon (gint strength, gboolean secure, gint icon_size)
{
    const gchar *nm_level;
    const gchar *fd_level;

    if      (strength >= 80) { nm_level = "100"; fd_level = "excellent"; }
    else if (strength >= 55) { nm_level = "75";  fd_level = "good";      }
    else if (strength >= 30) { nm_level = "50";  fd_level = "ok";        }
    else                     { nm_level = "25";  fd_level = "weak";      }

    gchar nm_name[64], fd_name[64];
    g_snprintf (nm_name, sizeof nm_name,
                "nm-signal-%s", nm_level);
    g_snprintf (fd_name, sizeof fd_name,
                "network-wireless-signal-%s-symbolic", fd_level);

    /* Ícono de señal: nm-signal-* con fallback freedesktop. */
    const gchar *signal_names[] = { fd_name, nm_name, NULL };
    GIcon     *signal_gicon = g_themed_icon_new_from_names ((gchar **) signal_names, -1);
    GtkWidget *signal_img   = gtk_image_new_from_gicon (signal_gicon, GTK_ICON_SIZE_MENU);
    g_object_unref (signal_gicon);
    gtk_image_set_pixel_size (GTK_IMAGE (signal_img), icon_size);

    if (!secure)
        return signal_img;

    /* Red segura: superponemos el candado encima del ícono de señal. */
    GtkWidget *overlay  = gtk_overlay_new ();
    GtkWidget *lock_img = make_lock_overlay_image (icon_size);

    gtk_container_add       (GTK_CONTAINER (overlay), signal_img);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), lock_img);
    gtk_widget_set_size_request (overlay, icon_size, icon_size);
    gtk_widget_show_all (overlay);

    return overlay;
}

/* ---------- callback del switch Wi-Fi global ---------- */

gboolean
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


void
device_switch_data_free (DeviceSwitchData *d)
{
    g_free (d->device_path);
    g_free (d);
}

gboolean
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

/* ---------- portal cautivo ---------- */

void
on_portal_signin_clicked (GtkWidget *btn, NetPopup *popup)
{
    (void) btn;
    /* Cualquier dirección http (sin s) sirve: el portal intercepta la
     * primera página que se pide y muestra su formulario. Se prefiere la que
     * usa NM para comprobar; si no hay, una pensada para esto. */
    gchar *uri = nm_get_connectivity_check_uri (popup->conn);
    if (!uri)
        uri = g_strdup ("http://neverssl.com/");

    popup_hide (popup);     /* el popup tiene capturado el mouse */

    GError *err = NULL;
    if (!gtk_show_uri_on_window (NULL, uri, GDK_CURRENT_TIME, &err)) {
        g_warning ("xfce-net-plugin: no se pudo abrir el navegador: %s",
                   err->message);
        g_error_free (err);
    }
    g_free (uri);
}

void
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

    /* Volver a comprobar la salida a Internet (por si ya se inició sesión
     * en el portal, o recién apareció). */
    nm_check_connectivity_async (popup->conn);
}

/* ---------- diálogo "Conectar a red oculta" ----------
 *
 * El diálogo queda abierto mientras se intenta la conexión: muestra
 * "Conectando…", se cierra solo si conecta, y si falla muestra el error y
 * deja corregir los datos. Para enterarse del resultado tiene su propia
 * suscripción a las señales de NM, que vive mientras el diálogo exista. */

#define HIDDEN_TIMEOUT_MS (OP_TIMEOUT_MS * 2)   /* igual que la prórroga: 40s */

typedef struct {
    GDBusConnection *conn;
    GtkWidget       *dialog;
    GtkWidget       *ssid_entry;
    GtkWidget       *sec_combo;
    GtkWidget       *pass_entry;
    GtkWidget       *status_label;
    gchar           *ssid;             /* red que se está intentando */
    gchar           *device_path;      /* adaptador usado */
    gboolean         remove_on_fail;   /* el perfil lo creamos nosotros */
    gboolean         seen_activating;  /* el adaptador ya pasó por 40-90 */
    guint           *signal_ids;       /* suscripción propia, NULL si no hay intento */
    guint            timeout_id;       /* límite de espera */
    guint            eval_idle_id;     /* evaluación agendada (coalescida) */
} HiddenData;

void
on_eye_clicked (GtkWidget *btn, GtkEntry *entry)
{
    (void) btn;
    gtk_entry_set_visibility (entry, !gtk_entry_get_visibility (entry));
}

/* Deja de seguir el intento en curso (suscripción, temporizadores). */
static void
hidden_stop_watch (HiddenData *hd)
{
    if (hd->signal_ids) {
        nm_unsubscribe_signals (hd->conn, hd->signal_ids);
        hd->signal_ids = NULL;
    }
    if (hd->timeout_id) {
        g_source_remove (hd->timeout_id);
        hd->timeout_id = 0;
    }
    if (hd->eval_idle_id) {
        g_source_remove (hd->eval_idle_id);
        hd->eval_idle_id = 0;
    }
}

static void
hidden_set_busy (HiddenData *hd, gboolean busy)
{
    gtk_widget_set_sensitive (hd->ssid_entry, !busy);
    gtk_widget_set_sensitive (hd->sec_combo,  !busy);
    gtk_widget_set_sensitive (hd->pass_entry, !busy);
    gtk_dialog_set_response_sensitive (GTK_DIALOG (hd->dialog),
                                       GTK_RESPONSE_OK, !busy);
}

/* Al destruirse el diálogo (Cancelar, cerrar ventana o éxito). Las acciones
 * ya enviadas a NM siguen su curso aunque el diálogo se cierre. */
static void
on_hidden_destroy (GtkWidget *dialog, HiddenData *hd)
{
    (void) dialog;
    hidden_stop_watch (hd);
    g_free (hd->ssid);
    g_free (hd->device_path);
    g_free (hd);
}

static void
hidden_fail (HiddenData *hd)
{
    hidden_stop_watch (hd);
    /* Un perfil de red oculta fallido con autoconexión quedaría reintentando
     * para siempre: si lo creamos nosotros, se borra. */
    if (hd->remove_on_fail && hd->ssid)
        nm_forget_connection (hd->conn, hd->ssid);

    GtkStyleContext *ctx = gtk_widget_get_style_context (hd->status_label);
    gtk_style_context_remove_class (ctx, "dim-label");
    gtk_style_context_add_class    (ctx, "error");
    gtk_label_set_text (GTK_LABEL (hd->status_label),
                        _("Last connection attempt failed"));
    gtk_widget_show (hd->status_label);
    hidden_set_busy (hd, FALSE);
}

/* TRUE si el adaptador está conectado (estado 100) a la red pedida. Se usa
 * además del "pasó por conectando" porque el refresco agrupado puede saltarse
 * los estados intermedios si la conexión es muy rápida. */
static gboolean
hidden_is_connected (HiddenData *hd, guint32 state)
{
    if (state != 100)
        return FALSE;
    if (hd->seen_activating)
        return TRUE;

    gboolean match = FALSE;
    GSList  *aps   = nm_get_access_points (hd->conn, hd->device_path);
    for (GSList *a = aps; a; a = a->next) {
        NmAccessPoint *ap = a->data;
        if (ap->active && g_strcmp0 (ap->ssid, hd->ssid) == 0) {
            match = TRUE;
            break;
        }
    }
    nm_ap_list_free (aps);
    return match;
}

static void
hidden_succeed (HiddenData *hd)
{
    hidden_stop_watch (hd);
    gtk_widget_destroy (hd->dialog);      /* libera hd vía on_hidden_destroy */
}

/* Mira el estado del adaptador y decide: sigue, conectó o falló.
 * Corre siempre fuera de la entrega de señales (desde un idle), así puede
 * desuscribirse y destruir el diálogo sin riesgo. */
static gboolean
hidden_eval_idle (gpointer user_data)
{
    HiddenData *hd = user_data;
    hd->eval_idle_id = 0;
    if (!hd->signal_ids)
        return G_SOURCE_REMOVE;

    guint32 state = nm_get_device_state (hd->conn, hd->device_path);

    if (state >= 40 && state <= 90)
        hd->seen_activating = TRUE;
    else if (hidden_is_connected (hd, state))
        hidden_succeed (hd);
    else if (hd->seen_activating && (state == 120 || state <= 30))
        hidden_fail (hd);
    return G_SOURCE_REMOVE;
}

static void
on_hidden_nm_signal (gpointer user_data)
{
    HiddenData *hd = user_data;
    if (!hd->eval_idle_id)
        hd->eval_idle_id = g_idle_add (hidden_eval_idle, hd);
}

static gboolean
hidden_timeout_cb (gpointer user_data)
{
    HiddenData *hd = user_data;
    hd->timeout_id = 0;
    /* Última mirada antes de declarar el fallo. */
    if (hidden_is_connected (hd, nm_get_device_state (hd->conn, hd->device_path)))
        hidden_succeed (hd);
    else
        hidden_fail (hd);
    return G_SOURCE_REMOVE;
}

/* Elige el adaptador: el primero que esté encendido; si ninguno, el primero. */
static gchar *
pick_wifi_device (GDBusConnection *conn)
{
    gchar  *chosen = NULL;
    GSList *devs   = nm_get_wifi_devices (conn);
    for (GSList *l = devs; l && !chosen; l = l->next) {
        NmDevice *dev = l->data;
        if (nm_get_device_enabled (conn, dev->object_path))
            chosen = g_strdup (dev->object_path);
    }
    if (!chosen && devs)
        chosen = g_strdup (((NmDevice *) devs->data)->object_path);
    nm_device_list_free (devs);
    return chosen;
}

static void
on_hidden_response (GtkDialog *dialog, gint response, HiddenData *hd)
{
    if (response != GTK_RESPONSE_OK) {
        gtk_widget_destroy (GTK_WIDGET (dialog));
        return;
    }
    if (hd->signal_ids)
        return;   /* ya hay un intento en curso */

    const gchar *ssid     = gtk_entry_get_text (GTK_ENTRY (hd->ssid_entry));
    const gchar *password = gtk_entry_get_text (GTK_ENTRY (hd->pass_entry));
    gboolean     secure   = gtk_combo_box_get_active (
                                GTK_COMBO_BOX (hd->sec_combo)) == 1;

    if (!ssid || !*ssid) {
        gtk_widget_grab_focus (hd->ssid_entry);
        return;
    }
    if (secure && (!password || !*password)) {
        gtk_widget_grab_focus (hd->pass_entry);
        return;
    }

    gchar *device_path = pick_wifi_device (hd->conn);
    if (!device_path)
        return;

    g_free (hd->ssid);
    g_free (hd->device_path);
    hd->ssid            = g_strdup (ssid);
    hd->device_path     = device_path;
    hd->seen_activating = FALSE;

    /* Igual que en la fila: con clave nueva se borra el perfil viejo para no
     * acumular duplicados. Así, todo perfil que quede es nuestro. */
    gboolean had_profile = nm_has_saved_connection (hd->conn, ssid);
    if (secure && had_profile)
        nm_forget_connection (hd->conn, ssid);
    hd->remove_on_fail = secure || !had_profile;

    GtkStyleContext *ctx = gtk_widget_get_style_context (hd->status_label);
    gtk_style_context_remove_class (ctx, "error");
    gtk_style_context_add_class    (ctx, "dim-label");
    gtk_label_set_text (GTK_LABEL (hd->status_label), _("Connecting…"));
    gtk_widget_show (hd->status_label);
    hidden_set_busy (hd, TRUE);

    /* Suscribirse ANTES de pedir la conexión, para no perder ningún cambio. */
    hd->signal_ids = nm_subscribe_signals (hd->conn, on_hidden_nm_signal, hd);
    hd->timeout_id = g_timeout_add (HIDDEN_TIMEOUT_MS, hidden_timeout_cb, hd);

    nm_add_and_activate_connection_async (
        hd->conn, hd->device_path, "/", hd->ssid,
        secure ? password : NULL, NULL, TRUE, TRUE);
}

void
on_hidden_network_clicked (GtkWidget *btn, NetPopup *popup)
{
    (void) btn;
    GDBusConnection *conn = popup->conn;
    if (!conn) return;

    HiddenData *hd = g_new0 (HiddenData, 1);
    hd->conn = conn;

    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Connect to hidden network"),
        NULL,
        0,
        _("Cancel"), GTK_RESPONSE_CANCEL,
        _("Connect"), GTK_RESPONSE_OK,
        NULL);
    hd->dialog = dialog;
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
    hd->ssid_entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (hd->ssid_entry), TRUE);
    gtk_grid_attach (GTK_GRID (grid), ssid_label,     0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), hd->ssid_entry, 1, 0, 1, 1);

    GtkWidget *sec_label = gtk_label_new (_("Security:"));
    gtk_label_set_xalign (GTK_LABEL (sec_label), 0.0);
    hd->sec_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (hd->sec_combo), _("None"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (hd->sec_combo), "WPA2/WPA3");
    gtk_combo_box_set_active (GTK_COMBO_BOX (hd->sec_combo), 1);
    gtk_grid_attach (GTK_GRID (grid), sec_label,     0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), hd->sec_combo, 1, 1, 1, 1);

    GtkWidget *pass_label = gtk_label_new (_("Password:"));
    gtk_label_set_xalign (GTK_LABEL (pass_label), 0.0);
    GtkWidget *pass_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    hd->pass_entry = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (hd->pass_entry), FALSE);
    gtk_entry_set_activates_default (GTK_ENTRY (hd->pass_entry), TRUE);
    GtkWidget *eye_btn  = gtk_button_new ();
    GtkWidget *eye_icon = gtk_image_new_from_icon_name ("view-reveal-symbolic",
                                                         GTK_ICON_SIZE_MENU);
    gtk_button_set_image  (GTK_BUTTON (eye_btn), eye_icon);
    gtk_button_set_relief (GTK_BUTTON (eye_btn), GTK_RELIEF_NONE);
    g_signal_connect (eye_btn, "clicked", G_CALLBACK (on_eye_clicked), hd->pass_entry);
    gtk_box_pack_start (GTK_BOX (pass_box), hd->pass_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (pass_box), eye_btn,        FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), pass_label, 0, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pass_box,   1, 2, 1, 1);

    /* Línea de estado: "Conectando…" o el error. Oculta hasta el primer intento. */
    hd->status_label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (hd->status_label), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (hd->status_label),
                                 "dim-label");
    gtk_grid_attach (GTK_GRID (grid), hd->status_label, 0, 3, 2, 1);

    g_signal_connect (dialog, "response", G_CALLBACK (on_hidden_response), hd);
    g_signal_connect (dialog, "destroy",  G_CALLBACK (on_hidden_destroy),  hd);
    gtk_widget_show_all (dialog);
    gtk_widget_hide (hd->status_label);
}

void
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
        /* Abrir nmtui en una terminal. Primero la terminal preferida de Xfce
         * (exo-open respeta la que eligió el usuario); después las comunes.
         * Antes era siempre xterm, que casi nadie tiene en Xfce. */
        static const gchar *terms[][2] = {
            { "exo-open",           "exo-open --launch TerminalEmulator nmtui" },
            { "xfce4-terminal",     "xfce4-terminal -e nmtui" },
            { "x-terminal-emulator","x-terminal-emulator -e nmtui" },
            { "xterm",              "xterm -e nmtui" },
        };
        for (gsize i = 0; i < G_N_ELEMENTS (terms); i++) {
            gchar *found = g_find_program_in_path (terms[i][0]);
            if (found) {
                g_free (found);
                g_spawn_command_line_async (terms[i][1], NULL);
                return;
            }
        }
        g_warning ("xfce-net-plugin: nmtui está instalado pero no se encontró "
                   "ninguna terminal para abrirlo");
        return;
    }
    g_warning ("xfce-net-plugin: no se encontró ningún gestor de red avanzado "
               "(nm-connection-editor, cmst, connman-gtk o nmtui)");
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

    /* Navegación con teclado dentro del popup.
     *
     * La ventana del popup es de tipo "menú emergente": el gestor de ventanas
     * no la trata como ventana con foco de teclado activo, así que GTK no
     * dispara solo su navegación con Tab. Las teclas SÍ llegan (Escape anda),
     * por eso movemos el foco nosotros con gtk_widget_child_focus, que recorre
     * los widgets enfocables (filas, botones del expand, switches, entry).
     *
     * Solo interceptamos Tab y Shift+Tab. Las flechas se dejan pasar a propósito
     * para no romper el movimiento del cursor dentro del campo de contraseña. */
    if (event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_KP_Tab) {
        gtk_widget_child_focus (popup->window, GTK_DIR_TAB_FORWARD);
        return GDK_EVENT_STOP;
    }
    if (event->keyval == GDK_KEY_ISO_Left_Tab) {   /* Shift+Tab */
        gtk_widget_child_focus (popup->window, GTK_DIR_TAB_BACKWARD);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

static gboolean
on_focus_out (GtkWidget *widget, GdkEventFocus *ev, NetPopup *popup)
{
    (void) widget; (void) ev;
    /* Abrir el menú contextual le quita el foco al popup: eso no es
     * "clickear afuera", no hay que cerrar. */
    if (popup->ctx_menu || popup->menu_idle_id)
        return FALSE;
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

static gboolean
pending_timeout_cb (gpointer user_data)
{
    NetPopup *popup = user_data;
    popup->pending_timeout_id = 0;

    if (!popup->ui_built) return G_SOURCE_REMOVE;
    if (!gtk_widget_get_visible (popup->window)) return G_SOURCE_REMOVE;

    nm_cache_begin (popup->conn);
    process_pending_attempts (popup);
    update_top_status      (popup);
    update_devices_section (popup);
    nm_cache_end ();

    return G_SOURCE_REMOVE;
}

NetPopup *
popup_create (XfcePanelPlugin *plugin, GtkWidget *button)
{
    (void) plugin; (void) button;
    NetPopup  *popup = g_new0 (NetPopup, 1);
    GtkWidget *win, *outer_box, *scroll;

    /* Proveedor de estilos ÚNICO para todo el plugin (filas y separadores),
     * registrado a nivel de pantalla una sola vez. Antes se creaba un
     * proveedor nuevo por cada fila y por cada separador en cada refresco.
     * Valores de mix() calibrados a ojo en tema oscuro: foco 0.03, abierta
     * 0.05, abierta con foco 0.05 (al partir popup.c se habían perdido). */
    {
        static gboolean css_installed = FALSE;
        if (!css_installed) {
            GtkCssProvider *prov = gtk_css_provider_new ();
            gtk_css_provider_load_from_data (prov,
                ".net-row:focus { background-color: mix(@theme_bg_color, @theme_fg_color, 0.03); }"
                ".net-row-open { background-color: mix(@theme_bg_color, @theme_fg_color, 0.05); }"
                ".net-row-open:focus { background-color: mix(@theme_bg_color, @theme_fg_color, 0.05); }"
                ".module-sep { background-color: mix(@theme_bg_color, @theme_fg_color, 0.3); min-height: 2px; }",
                -1, NULL);
            gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                GTK_STYLE_PROVIDER (prov),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref (prov);
            css_installed = TRUE;
        }
    }

    win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated        (GTK_WINDOW (win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW (win), TRUE);
    gtk_window_set_skip_pager_hint  (GTK_WINDOW (win), TRUE);
    gtk_window_set_resizable        (GTK_WINDOW (win), FALSE);
    gtk_window_set_type_hint        (GTK_WINDOW (win),
                                     GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_keep_above       (GTK_WINDOW (win), TRUE);
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

    /* Si quedan intentos pendientes vigentes, agendar temporizador para
     * evaluarlos cuando expire el período de espera. */
    if (popup->pending_attempts && g_hash_table_size (popup->pending_attempts) > 0) {
        if (popup->pending_timeout_id)
            g_source_remove (popup->pending_timeout_id);
        popup->pending_timeout_id = g_timeout_add (OP_TIMEOUT_MS,
                                                   pending_timeout_cb, popup);
    }

    /* Limpiar pending_attempts de conexiones que ya se confirmaron exitosas
     * mientras el popup estaba cerrado. */
    if (popup->pending_attempts && g_hash_table_size (popup->pending_attempts) > 0) {
        GHashTableIter iter;
        gpointer key, value;
        GSList *to_remove = NULL;
        g_hash_table_iter_init (&iter, popup->pending_attempts);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            const gchar *k   = key;
            const gchar *sep = strrchr (k, '|');
            if (!sep) continue;
            gchar *attempt_ssid = g_strndup (k, sep - k);
            gchar *attempt_dev  = g_strdup (sep + 1);
            if (device_is_activated (popup->conn, attempt_dev)) {
                gchar *active_ssid = NULL;
                GSList *aps = nm_get_access_points (popup->conn, attempt_dev);
                for (GSList *a = aps; a; a = a->next) {
                    NmAccessPoint *ap = a->data;
                    if (ap->active) { active_ssid = g_strdup (ap->ssid); break; }
                }
                nm_ap_list_free (aps);
                if (g_strcmp0 (active_ssid, attempt_ssid) == 0)
                    to_remove = g_slist_prepend (to_remove, g_strdup ((const gchar *) key));
                g_free (active_ssid);
            }
            g_free (attempt_ssid);
            g_free (attempt_dev);
        }
        for (GSList *l = to_remove; l; l = l->next)
            g_hash_table_remove (popup->pending_attempts, l->data);
        g_slist_free_full (to_remove, g_free);
    }

    /* Construir UI si es la primera vez. */
    nm_cache_begin (conn);
    if (!popup->ui_built) {
        rebuild_ui (popup);
    } else {
        /* Ya estaba construida; refrescar estado por las dudas. */
        update_top_status      (popup);
        update_eth_section     (popup);
        update_devices_section (popup);
        update_vpn_section     (popup);
    }
    nm_cache_end ();

    /* Suscribirse a señales DBus de NM (las gestiona el popup mientras está abierto). */
    if (!popup->signal_ids)
        popup->signal_ids = nm_subscribe_signals (conn, on_nm_signal_popup, popup);

    gtk_widget_realize (popup->window);
    position_popup (popup, button);
    gtk_widget_show_all (popup->window);

    popup->button = button;
    {
        /* Diferir el grab a cuando la ventana esté realmente visible. */
        GrabData *gd = g_new0 (GrabData, 1);
        gd->popup  = popup;
        gd->button = button;
        g_signal_connect (popup->window, "map-event",
                          G_CALLBACK (on_window_mapped), gd);
    }

    /* Pedir escaneo al abrir. */
    {
        GSList *devs = nm_get_wifi_devices (conn);
        for (GSList *l = devs; l; l = l->next) {
            NmDevice *dev = l->data;
            nm_request_scan (conn, dev->object_path);
        }
        nm_device_list_free (devs);
    }
}

void
popup_hide (NetPopup *popup)
{
    GdkDisplay *display;
    GdkSeat    *seat;

    if (!gtk_widget_get_visible (popup->window))
        return;

    /* Menú contextual abierto o a medio cerrar: descartarlo. */
    if (popup->menu_idle_id) {
        g_source_remove (popup->menu_idle_id);
        popup->menu_idle_id = 0;
    }
    if (popup->ctx_menu) {
        gtk_widget_destroy (popup->ctx_menu);
        popup->ctx_menu = NULL;
    }

    display = gtk_widget_get_display (popup->window);
    seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);
    popup->grab_active = FALSE;

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
    if (popup->pending_timeout_id) {
        g_source_remove (popup->pending_timeout_id);
        popup->pending_timeout_id = 0;
    }
    if (popup->passive_timeout_id) {
        g_source_remove (popup->passive_timeout_id);
        popup->passive_timeout_id = 0;
    }
    if (popup->refresh_idle_id) {
        g_source_remove (popup->refresh_idle_id);
        popup->refresh_idle_id = 0;
    }
    popup->scanning = FALSE;

    /* Cerrar expand abierto para que al reabrir el popup esté limpio. */
    if (popup->current_expand_box) {
        gtk_widget_hide (popup->current_expand_box);
        popup->current_expand_box = NULL;
    }
    clear_open_highlight (popup);

    /* El bloqueo de señales que había acá no matcheaba ningún manejador:
     * los del botón están registrados con el complemento como dato, no con
     * el botón mismo, así que el block/unblock era código muerto. La
     * protección real contra la recursión vive en on_popup_hidden de
     * plugin.c. */
    gtk_widget_hide (popup->window);
}

void
popup_destroy (NetPopup *popup)
{
    if (!popup) return;
    popup_hide (popup);
    /* popup_hide no hace nada si el popup ya estaba oculto, así que los
     * temporizadores se cancelan también acá: ninguno debe disparar con el
     * popup liberado (quitar el plugin del panel). */
    guint *timers[] = { &popup->scan_timeout_id, &popup->pending_timeout_id,
                        &popup->passive_timeout_id, &popup->refresh_idle_id,
                        &popup->menu_idle_id };
    for (gsize i = 0; i < G_N_ELEMENTS (timers); i++) {
        if (*timers[i]) {
            g_source_remove (*timers[i]);
            *timers[i] = 0;
        }
    }
    if (popup->ops_in_progress)
        g_hash_table_destroy (popup->ops_in_progress);
    if (popup->failed_ssids)
        g_hash_table_destroy (popup->failed_ssids);
    if (popup->pending_attempts)
        g_hash_table_destroy (popup->pending_attempts);
    gtk_widget_destroy (popup->window);
    g_free (popup);
}
