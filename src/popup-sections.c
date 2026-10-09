/* popup-sections.c — secciones del popup (por adaptador, Ethernet, VPN),
 * funciones de actualización, refresco por diferencia, refresco coalescido
 * y reconstrucción completa de la UI.
 * Separado de popup.c sin cambios de lógica. */

#include "popup-private.h"
#include <string.h>

/* ---------- sección por adaptador ---------- */

/* Comparador de filas de red: activa primero, guardadas después, y dentro
 * de cada grupo por intensidad de señal descendente. user_data es el set
 * de SSIDs guardadas. */
static gint
ap_compare (gconstpointer a, gconstpointer b, gpointer user_data)
{
    const NmAccessPoint *apa   = a;
    const NmAccessPoint *apb   = b;
    GHashTable          *saved = user_data;

    if (apa->active != apb->active)
        return apa->active ? -1 : 1;

    gboolean sa = saved && g_hash_table_contains (saved, apa->ssid);
    gboolean sb = saved && g_hash_table_contains (saved, apb->ssid);
    if (sa != sb)
        return sa ? -1 : 1;

    return apb->strength - apa->strength;
}

static GtkWidget *
make_device_section (NetPopup *popup, NmDevice *dev, gboolean show_header)
{
    GtkWidget *section, *separator;
    GSList    *aps, *a;
    GDBusConnection *conn = popup->conn;

    section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    /* Etiqueta para que el refresco por diferencia pueda aparear cada
     * sección existente con su adaptador. */
    g_object_set_data_full (G_OBJECT (section), "device-path",
                            g_strdup (dev->object_path), g_free);

    if (popup->show_separators) {
        GtkWidget *mod_sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (mod_sep, -1, 2);
        gtk_style_context_add_class (gtk_widget_get_style_context (mod_sep), "module-sep");
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
        gtk_box_pack_start (GTK_BOX (header_row), header, FALSE, FALSE, 0);

        if (dev->description && *dev->description) {
            GtkWidget *desc_label = gtk_label_new (NULL);
            gchar *desc_markup = g_markup_printf_escaped ("<small>%s</small>", dev->description);
            gtk_label_set_markup (GTK_LABEL (desc_label), desc_markup);
            g_free (desc_markup);
            gtk_style_context_add_class (gtk_widget_get_style_context (desc_label), "dim-label");
            gtk_label_set_xalign (GTK_LABEL (desc_label), 0.0);
            gtk_widget_set_margin_start (desc_label, 6);
            gtk_box_pack_start (GTK_BOX (header_row), desc_label, TRUE, TRUE, 0);
        }

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

    /* Set de SSIDs guardadas: una sola enumeración de perfiles por sección,
     * en vez de una enumeración completa por cada fila como antes. */
    GHashTable *saved_ssids = nm_get_saved_wifi_ssids (conn);

    aps = nm_get_access_points (conn, dev->object_path);
    /* Orden: activa primero, después guardadas, después por señal. */
    aps = g_slist_sort_with_data (aps, ap_compare, saved_ssids);
    for (a = aps; a; a = a->next) {
        if (popup->show_separators) {
            separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
            gtk_box_pack_start (GTK_BOX (section), separator, FALSE, FALSE, 0);
        }
        GtkWidget *r = make_ap_row (a->data, popup, dev->object_path, saved_ssids);
        gtk_box_pack_start (GTK_BOX (section), r, FALSE, FALSE, 0);
    }
    nm_ap_list_free (aps);
    g_hash_table_destroy (saved_ssids);

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

/* ---------- secciones fijas: actualización en el lugar ----------
 *
 * Ethernet y VPN se arman de nuevo SOLO si cambia qué filas tienen (otro
 * adaptador, otro perfil de VPN, separadores prendidos/apagados). Si solo
 * cambia el estado (conectado, encendido), se actualizan los interruptores y
 * la etiqueta "Conectado" sin destruir nada: no hay parpadeo, no se pierde el
 * foco del teclado y un interruptor recién tocado no se reemplaza a mitad de
 * camino. Para eso cada sección guarda su "huella" (section-fp) y una tabla
 * ruta → interruptor (section-switches). */

static void
section_clear (GtkWidget *section)
{
    GList *kids = gtk_container_get_children (GTK_CONTAINER (section));
    for (GList *w = kids; w; w = w->next) gtk_widget_destroy (GTK_WIDGET (w->data));
    g_list_free (kids);
    g_object_set_data (G_OBJECT (section), "section-fp", NULL);
    g_object_set_data (G_OBJECT (section), "section-switches", NULL);
}

/* ¿La huella guardada coincide con la nueva? (no libera nada) */
static gboolean
section_fp_matches (GtkWidget *section, const gchar *fp)
{
    const gchar *old = g_object_get_data (G_OBJECT (section), "section-fp");
    return old && g_strcmp0 (old, fp) == 0;
}

/* Guarda la huella y una tabla nueva de interruptores, que devuelve. */
static GHashTable *
section_start (GtkWidget *section, gchar *fp_owned)
{
    GHashTable *sws = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_object_set_data_full (G_OBJECT (section), "section-fp", fp_owned, g_free);
    g_object_set_data_full (G_OBJECT (section), "section-switches", sws,
                            (GDestroyNotify) g_hash_table_destroy);
    return sws;
}

/* Pone un interruptor en `on` sin disparar su manejador (no es el usuario
 * quien lo cambia, es el reflejo del estado real). */
static void
switch_sync (GtkWidget *sw, GCallback handler, gpointer data, gboolean on)
{
    if (gtk_switch_get_active (GTK_SWITCH (sw)) == on &&
        gtk_switch_get_state  (GTK_SWITCH (sw)) == on)
        return;
    g_signal_handlers_block_by_func (sw, handler, data);
    gtk_switch_set_active (GTK_SWITCH (sw), on);
    gtk_switch_set_state  (GTK_SWITCH (sw), on);
    g_signal_handlers_unblock_by_func (sw, handler, data);
}

static void
fill_vpn_section (NetPopup *popup)
{
    GSList *vpns = nm_get_vpn_connections (popup->conn);
    if (!vpns) {
        section_clear (popup->vpn_section);
        gtk_widget_hide (popup->vpn_section);
        return;
    }

    /* Huella: separadores + qué perfiles (ruta y nombre), sin su estado. */
    GString *fp = g_string_new (popup->show_separators ? "s" : "-");
    for (GSList *l = vpns; l; l = l->next) {
        NmVpnConnection *vpn = l->data;
        g_string_append_printf (fp, "|%s=%s", vpn->conn_path, vpn->name);
    }

    if (section_fp_matches (popup->vpn_section, fp->str)) {
        GHashTable *sws = g_object_get_data (G_OBJECT (popup->vpn_section),
                                             "section-switches");
        for (GSList *l = vpns; l; l = l->next) {
            NmVpnConnection *vpn = l->data;
            GtkWidget *sw = sws ? g_hash_table_lookup (sws, vpn->conn_path) : NULL;
            if (sw)
                switch_sync (sw, G_CALLBACK (on_vpn_switch_toggled),
                             g_object_get_data (G_OBJECT (sw), "vpn-switch-data"),
                             vpn->active);
        }
        g_string_free (fp, TRUE);
        nm_vpn_list_free (vpns);
        return;
    }

    /* Cambió qué filas hay: armar de nuevo. */
    section_clear (popup->vpn_section);
    GHashTable *sws = section_start (popup->vpn_section, g_string_free (fp, FALSE));

    if (popup->show_separators) {
        GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (sep, -1, 2);
        gtk_style_context_add_class (gtk_widget_get_style_context (sep), "module-sep");
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

        GtkWidget *vpn_icon = gtk_image_new_from_icon_name ("network-vpn", GTK_ICON_SIZE_MENU);
        gtk_box_pack_start (GTK_BOX (row), vpn_icon, FALSE, FALSE, 4);

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
        g_hash_table_replace (sws, g_strdup (vpn->conn_path), sw);

        gtk_box_pack_start (GTK_BOX (popup->vpn_section), row, FALSE, FALSE, 0);
    }

    nm_vpn_list_free (vpns);
    gtk_widget_show_all (popup->vpn_section);
}

/* ---------- sección Ethernet ---------- */

static void
fill_eth_section (NetPopup *popup)
{
    GSList *eth_devices = nm_get_ethernet_devices (popup->conn);
    if (!eth_devices) {
        section_clear (popup->eth_section);
        gtk_widget_hide (popup->eth_section);
        return;
    }

    /* Huella: separadores + qué adaptadores, sin su estado. */
    GString *fp = g_string_new (popup->show_separators ? "s" : "-");
    for (GSList *d = eth_devices; d; d = d->next) {
        NmDevice *dev = d->data;
        g_string_append_printf (fp, "|%s=%s", dev->object_path, dev->iface);
    }

    if (section_fp_matches (popup->eth_section, fp->str)) {
        GHashTable *sws = g_object_get_data (G_OBJECT (popup->eth_section),
                                             "section-switches");
        for (GSList *d = eth_devices; d; d = d->next) {
            NmDevice  *dev = d->data;
            GtkWidget *sw  = sws ? g_hash_table_lookup (sws, dev->object_path) : NULL;
            if (!sw) continue;
            switch_sync (sw, G_CALLBACK (on_device_switch_toggled),
                         g_object_get_data (G_OBJECT (sw), "switch-data"),
                         nm_get_device_enabled (popup->conn, dev->object_path));
            GtkWidget *status = g_object_get_data (G_OBJECT (sw), "status-label");
            if (status)
                gtk_widget_set_visible (status,
                    device_is_activated (popup->conn, dev->object_path));
        }
        g_string_free (fp, TRUE);
        nm_device_list_free (eth_devices);
        return;
    }

    /* Cambió qué adaptadores hay: armar de nuevo. */
    section_clear (popup->eth_section);
    GHashTable *sws = section_start (popup->eth_section, g_string_free (fp, FALSE));

    for (GSList *d = eth_devices; d; d = d->next) {
        NmDevice *dev = d->data;

        if (popup->show_separators) {
            GtkWidget *eth_sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_size_request (eth_sep, -1, 2);
            gtk_style_context_add_class (gtk_widget_get_style_context (eth_sep), "module-sep");
            gtk_box_pack_start (GTK_BOX (popup->eth_section), eth_sep, FALSE, FALSE, 0);
        }

        GtkWidget *eth_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_margin_start  (eth_row, 12);
        gtk_widget_set_margin_end    (eth_row, 12);
        gtk_widget_set_margin_top    (eth_row, 8);
        gtk_widget_set_margin_bottom (eth_row, 8);

        const gchar *eth_icon_names[] = { "nm-device-wired", "network-wired-symbolic", NULL };
        GIcon     *eth_gicon = g_themed_icon_new_from_names ((gchar **) eth_icon_names, -1);
        GtkWidget *eth_icon  = gtk_image_new_from_gicon (eth_gicon, GTK_ICON_SIZE_MENU);
        g_object_unref (eth_gicon);
        gtk_box_pack_start (GTK_BOX (eth_row), eth_icon, FALSE, FALSE, 4);

        GtkWidget *eth_label = gtk_label_new (NULL);
        gchar *eth_markup = g_markup_printf_escaped ("<b>%s</b>", dev->iface);
        gtk_label_set_markup (GTK_LABEL (eth_label), eth_markup);
        g_free (eth_markup);
        gtk_label_set_xalign (GTK_LABEL (eth_label), 0.0);
        gtk_box_pack_start (GTK_BOX (eth_row), eth_label, TRUE, TRUE, 0);

        gboolean eth_enabled = nm_get_device_enabled (popup->conn, dev->object_path);

        GtkWidget *eth_switch = gtk_switch_new ();
        gtk_switch_set_active (GTK_SWITCH (eth_switch), eth_enabled);
        gtk_widget_set_valign (eth_switch, GTK_ALIGN_CENTER);

        DeviceSwitchData *dsd = g_new0 (DeviceSwitchData, 1);
        dsd->conn        = popup->conn;
        dsd->device_path = g_strdup (dev->object_path);
        g_object_set_data_full (G_OBJECT (eth_switch), "switch-data", dsd,
                                (GDestroyNotify) device_switch_data_free);
        g_signal_connect (eth_switch, "state-set",
                          G_CALLBACK (on_device_switch_toggled), dsd);
        gtk_box_pack_end (GTK_BOX (eth_row), eth_switch, FALSE, FALSE, 0);

        /* "Connected" solo si el estado es 100 (activado de verdad). La
         * etiqueta se crea siempre y se muestra u oculta, así el refresco en
         * el lugar puede cambiarla sin rearmar la fila. */
        GtkWidget *eth_status = gtk_label_new (_("Connected"));
        gtk_style_context_add_class (gtk_widget_get_style_context (eth_status),
                                     "dim-label");
        gtk_box_pack_end (GTK_BOX (eth_row), eth_status, FALSE, FALSE, 6);
        gtk_widget_set_no_show_all (eth_status, TRUE);
        gtk_widget_set_visible (eth_status,
                                device_is_activated (popup->conn, dev->object_path));
        g_object_set_data (G_OBJECT (eth_switch), "status-label", eth_status);
        g_hash_table_replace (sws, g_strdup (dev->object_path), eth_switch);

        gtk_box_pack_start (GTK_BOX (popup->eth_section), eth_row, FALSE, FALSE, 0);
    }
    nm_device_list_free (eth_devices);

    gtk_widget_show_all (popup->eth_section);
}

/* ---------- update functions: actualizan widgets in-place ---------- */

void
update_top_status (NetPopup *popup)
{
    if (!popup->ui_built) return;

    gchar *primary_ssid = get_primary_ssid (popup->conn);

    /* Si NM dice "conectado" pero hay un CONNECT en curso para ese mismo SSID,
     * ignorarlo — NM miente brevemente antes de confirmar el fallo de auth.
     * Verificar tanto ops_in_progress (popup abierto) como pending_attempts
     * (popup reabierto con op en curso). */
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
    if (primary_ssid && popup->pending_attempts &&
        g_hash_table_size (popup->pending_attempts) > 0) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, popup->pending_attempts);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            gint64 *ts  = value;
            gint64  age = (g_get_monotonic_time () - *ts) / 1000;
            if (age < OP_TIMEOUT_MS) {
                const gchar *k   = key;
                const gchar *sep = strrchr (k, '|');
                if (sep) {
                    gchar *pending_ssid = g_strndup (k, sep - k);
                    if (g_strcmp0 (pending_ssid, primary_ssid) == 0) {
                        g_free (primary_ssid);
                        primary_ssid = NULL;
                        g_free (pending_ssid);
                        break;
                    }
                    g_free (pending_ssid);
                }
            }
        }
    }

    /* Buscar SSID conectando: primero en ops_in_progress (popup abierto),
     * luego en pending_attempts (popup reabierto con op en curso). */
    const gchar *connecting_ssid = NULL;
    gchar       *connecting_ssid_pending = NULL; /* para liberar si viene de pending */

    if (g_hash_table_size (popup->ops_in_progress) > 0) {
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
    } else if (popup->pending_attempts &&
               g_hash_table_size (popup->pending_attempts) > 0) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, popup->pending_attempts);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            gint64 *ts  = value;
            gint64  age = (g_get_monotonic_time () - *ts) / 1000;
            if (age < OP_TIMEOUT_MS) {
                /* La clave es "ssid|device_path" — extraer ssid. */
                const gchar *k   = key;
                const gchar *sep = strrchr (k, '|');
                if (sep) {
                    connecting_ssid_pending = g_strndup (k, sep - k);
                    connecting_ssid = connecting_ssid_pending;
                }
                break;
            }
        }
    }

    /* Ícono superior: spinner si hay operación en curso, ícono si no. */
    if (connecting_ssid) {
        gtk_spinner_start (GTK_SPINNER (popup->status_spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (popup->status_stack), "spinner");
    } else {
        gtk_spinner_stop (GTK_SPINNER (popup->status_spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (popup->status_stack), "icon");
        if (primary_ssid) {
            const gchar *conn_names[] = { "network-wireless-symbolic", "nm-device-wireless", NULL };
            GIcon *conn_gicon = g_themed_icon_new_from_names ((gchar **) conn_names, -1);
            gtk_image_set_from_gicon (GTK_IMAGE (popup->status_icon), conn_gicon, GTK_ICON_SIZE_MENU);
            g_object_unref (conn_gicon);
        } else {
            const gchar *disc_names[] = { "network-wireless-disconnected-symbolic", "nm-no-connection", NULL };
            GIcon *disc_gicon = g_themed_icon_new_from_names ((gchar **) disc_names, -1);
            gtk_image_set_from_gicon (GTK_IMAGE (popup->status_icon), disc_gicon, GTK_ICON_SIZE_MENU);
            g_object_unref (disc_gicon);
        }
    }

    /* Etiqueta de estado */
    if (primary_ssid) {
        gchar *markup = g_markup_printf_escaped (
                            _("Connected to <b>%s</b>"), primary_ssid);
        gtk_label_set_markup (GTK_LABEL (popup->status_label), markup);
        g_free (markup);
    } else if (!nm_get_wifi_enabled (popup->conn)) {
        gtk_label_set_text (GTK_LABEL (popup->status_label), _("Disabled"));
    } else if (connecting_ssid) {
        gchar *markup = g_markup_printf_escaped (
                            _("Connecting to <b>%s</b>…"), connecting_ssid);
        gtk_label_set_markup (GTK_LABEL (popup->status_label), markup);
        g_free (markup);
    } else {
        gtk_label_set_text (GTK_LABEL (popup->status_label), _("Enabled – not connected"));
    }

    g_free (connecting_ssid_pending);

    g_free (primary_ssid);

    /* Portal cautivo: NM detectó que hay que iniciar sesión para navegar. */
    if (popup->portal_row)
        gtk_widget_set_visible (popup->portal_row,
            nm_get_connectivity (popup->conn) == NM_CONN_STATE_PORTAL);

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

void
update_eth_section (NetPopup *popup)
{
    if (!popup->ui_built) return;
    fill_eth_section (popup);
}

void
update_vpn_section (NetPopup *popup)
{
    if (!popup->ui_built) return;
    fill_vpn_section (popup);
}

/* ---------- refresco por diferencia ----------
 *
 * En vez de destruir y recrear TODAS las filas en cada refresco, se compara
 * la lista nueva de puntos de acceso con las filas existentes:
 *   - fila con la misma huella → se reutiliza (solo se actualizan la señal,
 *     la ruta del AP y la posición); el foco del teclado y los widgets
 *     sobreviven, y no hay parpadeo;
 *   - huella distinta → se recrea solo esa fila;
 *   - AP desaparecido → se elimina su fila; AP nuevo → se agrega su fila. */

/* Si hay una operación en curso cuyos widgets viven en una fila que está por
 * destruirse, descartar la entrada para no dejar punteros colgando. (La
 * reconstrucción completa hace lo mismo, pero con TODAS las operaciones.) */
static void
drop_op_for_row (NetPopup *popup, const gchar *device_path, const gchar *ssid)
{
    OpInProgress *op = g_hash_table_lookup (popup->ops_in_progress, device_path);
    if (op && g_strcmp0 (op->ssid, ssid) == 0)
        g_hash_table_remove (popup->ops_in_progress, device_path);
}

/* Actualiza en el lugar lo volátil de una fila reutilizada: la ruta del
 * objeto AP (rota entre escaneos aunque la red sea la misma; si quedara
 * vieja, el próximo Conectar fallaría), el ícono de señal y el texto
 * "Señal NN%" del expand. */
static void
row_update_in_place (GtkWidget *event_box, NmAccessPoint *ap)
{
    GList *inner = gtk_container_get_children (GTK_CONTAINER (event_box));
    if (inner && inner->data) {
        RowData *rd = g_object_get_data (G_OBJECT (inner->data), "row-data");
        if (rd && g_strcmp0 (rd->ap_path, ap->object_path) != 0) {
            g_free (rd->ap_path);
            rd->ap_path = g_strdup (ap->object_path);
        }
    }
    g_list_free (inner);

    gint old_strength = GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (event_box), "row-strength"));
    if (old_strength == ap->strength)
        return;

    GtkWidget *icon = g_object_get_data (G_OBJECT (event_box), "signal-icon");
    if (icon) {
        GtkWidget *hbox = gtk_widget_get_parent (icon);
        gtk_widget_destroy (icon);
        GtkWidget *fresh = make_signal_icon (ap->strength, ap->secure, 22);
        gtk_box_pack_start (GTK_BOX (hbox), fresh, FALSE, FALSE, 0);
        gtk_box_reorder_child (GTK_BOX (hbox), fresh, 0);
        gtk_widget_show_all (fresh);
        g_object_set_data (G_OBJECT (event_box), "signal-icon", fresh);
    }
    g_object_set_data (G_OBJECT (event_box), "row-strength",
                       GINT_TO_POINTER (ap->strength));

    GtkWidget *info = g_object_get_data (G_OBJECT (event_box), "info-label");
    if (info) {
        gboolean saved_row = GPOINTER_TO_INT (
            g_object_get_data (G_OBJECT (event_box), "info-saved"));
        gchar *txt = saved_row
            ? g_strdup_printf (_("Saved network · Signal %d%%"), ap->strength)
            : g_strdup_printf (_("Signal %d%%"), ap->strength);
        gtk_label_set_text (GTK_LABEL (info), txt);
        g_free (txt);
    }
}

/* Si el expand abierto vive dentro de una fila que está por destruirse,
 * soltar el puntero para no dejarlo colgando. */
static void
forget_expand_if_inside (NetPopup *popup, GtkWidget *row)
{
    if (popup->current_expand_box &&
        gtk_widget_is_ancestor (popup->current_expand_box, row))
        popup->current_expand_box = NULL;
}

/* Refresco por diferencia de UNA sección de adaptador. */
static void
diff_refresh_one_section (NetPopup *popup, GtkWidget *section, NmDevice *dev)
{
    GHashTable *saved_ssids = nm_get_saved_wifi_ssids (popup->conn);
    GSList     *aps = nm_get_access_points (popup->conn, dev->object_path);
    aps = g_slist_sort_with_data (aps, ap_compare, saved_ssids);

    /* Inventario de filas existentes, clave "ssid|banda" → event_box. El
     * separador propio de cada fila (el GtkSeparator inmediatamente anterior)
     * se anota en la fila. También se cuenta el prefijo de widgets fijos
     * (separador de módulo, encabezado) que precede a la zona de filas. */
    GHashTable *existing = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
    GList     *kids = gtk_container_get_children (GTK_CONTAINER (section));
    gint       prefix = 0;
    gboolean   in_rows = FALSE;
    GtkWidget *pending_sep = NULL;

    for (GList *k = kids; k; k = k->next) {
        GtkWidget *w = k->data;
        if (GTK_IS_SEPARATOR (w)) {
            in_rows = TRUE;
            pending_sep = w;
            continue;
        }
        if (GTK_IS_EVENT_BOX (w)) {
            in_rows = TRUE;
            const gchar *ssid = g_object_get_data (G_OBJECT (w), "ssid");
            const gchar *band = g_object_get_data (G_OBJECT (w), "band");
            if (ssid) {
                g_object_set_data (G_OBJECT (w), "own-separator", pending_sep);
                g_hash_table_replace (existing,
                                      g_strdup_printf ("%s|%s", ssid,
                                                       band ? band : ""),
                                      w);
            }
            pending_sep = NULL;
            continue;
        }
        if (!in_rows)
            prefix++;
    }
    g_list_free (kids);

    /* Recorrer la lista nueva en orden, reutilizando o recreando. */
    gint pos = prefix;
    for (GSList *a = aps; a; a = a->next) {
        NmAccessPoint *ap = a->data;
        const gchar   *band = (ap->frequency >= 5925) ? "6G"
                            : (ap->frequency >= 5000) ? "5G" : "2.4G";
        gchar     *key     = g_strdup_printf ("%s|%s", ap->ssid, band);
        GtkWidget *old_row = g_hash_table_lookup (existing, key);
        gchar     *fp_new  = row_fingerprint (popup, ap, dev->object_path,
                                              saved_ssids);
        GtkWidget *row_w = NULL;
        GtkWidget *sep_w = NULL;

        if (old_row) {
            const gchar *fp_old = g_object_get_data (G_OBJECT (old_row), "row-fp");
            sep_w = g_object_get_data (G_OBJECT (old_row), "own-separator");
            if (fp_old && g_strcmp0 (fp_old, fp_new) == 0) {
                row_update_in_place (old_row, ap);
                row_w = old_row;
            } else {
                /* El estado cambió: recrear solo esta fila. */
                drop_op_for_row (popup, dev->object_path, ap->ssid);
                forget_expand_if_inside (popup, old_row);
                gtk_widget_destroy (old_row);
                row_w = make_ap_row (ap, popup, dev->object_path, saved_ssids);
                gtk_box_pack_start (GTK_BOX (section), row_w, FALSE, FALSE, 0);
                gtk_widget_show_all (row_w);
            }
            g_hash_table_remove (existing, key);   /* marcada como usada */
        } else {
            /* Red nueva. */
            row_w = make_ap_row (ap, popup, dev->object_path, saved_ssids);
            gtk_box_pack_start (GTK_BOX (section), row_w, FALSE, FALSE, 0);
            gtk_widget_show_all (row_w);
        }

        /* Separador propio: crear si falta. */
        if (popup->show_separators && !sep_w) {
            sep_w = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
            gtk_box_pack_start (GTK_BOX (section), sep_w, FALSE, FALSE, 0);
            gtk_widget_show (sep_w);
        }
        if (sep_w) {
            g_object_set_data (G_OBJECT (row_w), "own-separator", sep_w);
            gtk_box_reorder_child (GTK_BOX (section), sep_w, pos++);
        }
        gtk_box_reorder_child (GTK_BOX (section), row_w, pos++);

        g_free (fp_new);
        g_free (key);
    }

    /* Filas sobrantes: redes que ya no están al alcance. */
    GHashTableIter it;
    gpointer       k2, v2;
    g_hash_table_iter_init (&it, existing);
    while (g_hash_table_iter_next (&it, &k2, &v2)) {
        GtkWidget   *row      = v2;
        const gchar *row_ssid = g_object_get_data (G_OBJECT (row), "ssid");
        if (row_ssid)
            drop_op_for_row (popup, dev->object_path, row_ssid);
        forget_expand_if_inside (popup, row);
        GtkWidget *sep = g_object_get_data (G_OBJECT (row), "own-separator");
        if (sep)
            gtk_widget_destroy (sep);
        gtk_widget_destroy (row);
    }

    g_hash_table_destroy (existing);
    nm_ap_list_free (aps);
    g_hash_table_destroy (saved_ssids);
}

/* Intenta el refresco por diferencia de todas las secciones de adaptadores.
 * Devuelve FALSE si el conjunto (u orden) de adaptadores cambió respecto de
 * las secciones existentes: en ese caso el llamador debe hacer la
 * reconstrucción completa de siempre. */
static gboolean
diff_refresh_sections (NetPopup *popup)
{
    GSList *devices  = nm_get_wifi_devices (popup->conn);
    GList  *sections = NULL;
    GList  *kids = gtk_container_get_children (GTK_CONTAINER (popup->content_box));

    for (GList *k = kids; k; k = k->next) {
        if (k->data == popup->vpn_section || k->data == popup->eth_section)
            continue;
        sections = g_list_append (sections, k->data);
    }
    g_list_free (kids);

    gboolean match =
        (g_list_length (sections) == g_slist_length (devices));
    if (match) {
        GList  *s = sections;
        GSList *d = devices;
        for (; s && d; s = s->next, d = d->next) {
            const gchar *sec_dev = g_object_get_data (G_OBJECT (s->data),
                                                      "device-path");
            if (!sec_dev ||
                g_strcmp0 (sec_dev,
                           ((NmDevice *) d->data)->object_path) != 0) {
                match = FALSE;
                break;
            }
        }
    }

    if (match) {
        GList  *s = sections;
        GSList *d = devices;
        for (; s && d; s = s->next, d = d->next)
            diff_refresh_one_section (popup, GTK_WIDGET (s->data),
                                      (NmDevice *) d->data);
    }

    g_list_free (sections);
    nm_device_list_free (devices);
    return match;
}

/* Reordena las secciones fijas (Ethernet y VPN) según estén activas o no.
 * Se usa tanto en el refresco por diferencia como en la reconstrucción. */
static void
reorder_fixed_sections (NetPopup *popup)
{
    gboolean eth_active = gtk_widget_get_visible (popup->eth_section);
    gboolean vpn_active = FALSE;
    GSList  *vpns = nm_get_vpn_connections (popup->conn);
    for (GSList *v = vpns; v && !vpn_active; v = v->next)
        vpn_active = ((NmVpnConnection *) v->data)->active;
    nm_vpn_list_free (vpns);

    /* Orden: Ethernet(0) → VPN(1) → Wi-Fi; los inactivos van al fondo. */
    gint pos = 0;
    if (eth_active)
        gtk_box_reorder_child (GTK_BOX (popup->content_box),
                               popup->eth_section, pos++);
    else
        gtk_box_reorder_child (GTK_BOX (popup->content_box),
                               popup->eth_section, -1);

    if (vpn_active)
        gtk_box_reorder_child (GTK_BOX (popup->content_box),
                               popup->vpn_section, pos);
    else
        gtk_box_reorder_child (GTK_BOX (popup->content_box),
                               popup->vpn_section, -1);
}

void
update_devices_section (NetPopup *popup)
{
    if (!popup->ui_built) return;

    /* Si hay un expand abierto NO reconstruimos la lista de filas
     * (regla 1A: congelar filas mientras el usuario interactúa).
     * EXCEPCIÓN: si el expand actual es "pasivo" (solo muestra "Conectando…"
     * sin botones ni entry), no hay nada que el usuario esté tocando, así
     * que sí reconstruimos y después reabrimos esa misma fila. */
    /* Menú contextual abierto: sus opciones apuntan a los datos de una fila,
     * así que no se reconstruye nada hasta que se cierre (al cerrarse se
     * agenda un refresco). */
    if (popup->ctx_menu) {
        check_ops_progress (popup);
        return;
    }

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

    /* ---- Refresco por diferencia ----
     * Si el conjunto de adaptadores no cambió, actualizar las filas en el
     * lugar: sin parpadeo, sin perder el foco del teclado y mucho más
     * barato. En el camino se conservan los switches por adaptador y las
     * operaciones en curso de las filas que sobreviven (la diferencia
     * descarta puntualmente las de filas destruidas). */
    if (diff_refresh_sections (popup)) {
        reorder_fixed_sections (popup);
        /* Si veníamos de un expand "Conectando…" pasivo y su fila fue
         * recreada (cambió de estado), reabrirlo en la fila nueva. Si la
         * fila sobrevivió, el expand sigue abierto y no hay nada que hacer. */
        if (passive_ssid && !popup->current_expand_box)
            reopen_expand_for_ssid (popup, passive_ssid, passive_dev);
        g_free (passive_ssid);
        g_free (passive_dev);
        return;
    }

    /* ---- Reconstrucción completa ----
     * Solo cuando cambió el conjunto de adaptadores (o todavía no hay
     * secciones etiquetadas, p. ej. la primera vez). */

    /* Limpiar y reconstruir todas las secciones de adaptadores. */
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *k = kids; k; k = k->next) {
        /* Saltar secciones fijas (VPN y Ethernet) que se reordenan aparte. */
        if (k->data == popup->vpn_section)     continue;
        if (k->data == popup->eth_section)     continue;
        gtk_widget_destroy (GTK_WIDGET (k->data));
    }
    g_list_free (kids);

    g_slist_free (popup->device_switches);
    popup->device_switches = NULL;

    GSList *devices = nm_get_wifi_devices (popup->conn);
    gint    n_devices = g_slist_length (devices);
    for (GSList *d = devices; d; d = d->next) {
        NmDevice *dev = d->data;
        GtkWidget *section = make_device_section (popup, dev, n_devices > 1);
        gtk_box_pack_start (GTK_BOX (popup->content_box), section, FALSE, FALSE, 0);
        gtk_box_reorder_child (GTK_BOX (popup->content_box), section, -1);
    }
    nm_device_list_free (devices);

    /* Reordenar secciones fijas (Ethernet/VPN) según estén activas. */
    reorder_fixed_sections (popup);

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
    popup->refresh_idle_id = 0;

    if (!popup->ui_built) return G_SOURCE_REMOVE;
    if (!gtk_widget_get_visible (popup->window)) return G_SOURCE_REMOVE;

    /* Instantánea: todo el refresco lee de una sola foto del bus. */
    nm_cache_begin (popup->conn);

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

    nm_cache_end ();

    return G_SOURCE_REMOVE;
}

void
schedule_refresh_ui (NetPopup *popup)
{
    if (!popup) return;
    if (!gtk_widget_get_visible (popup->window)) return;
    /* Coalescer: si ya hay un refresh pendiente, no encolar otro. El número
     * queda en refresh_idle_id para poder cancelarlo al ocultar o destruir. */
    if (popup->refresh_idle_id)
        return;
    popup->refresh_idle_id = g_idle_add (refresh_ui_cb, popup);
}

/* Callback que NM dispara: agendamos refresh idle (coalescido). */
void
on_nm_signal_popup (gpointer user_data)
{
    NetPopup *popup = user_data;
    schedule_refresh_ui (popup);
}

/* ---------- construcción inicial de la UI ---------- */

void
rebuild_ui (NetPopup *popup)
{
    GDBusConnection *conn = popup->conn;

    /* Instantánea: toda la construcción lee de una sola foto del bus. */
    nm_cache_begin (conn);

    /* Limpiar zonas (por si rebuild_ui se llama dos veces) */
    GList *children = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *w = children; w; w = w->next) gtk_widget_destroy (GTK_WIDGET (w->data));
    g_list_free (children);

    GList *top_children = gtk_container_get_children (GTK_CONTAINER (popup->top_box));
    for (GList *w = top_children; w; w = w->next) gtk_widget_destroy (GTK_WIDGET (w->data));
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

    {
        const gchar *disc_names[] = { "nm-no-connection", "network-wireless-disconnected-symbolic", NULL };
        GIcon *disc_gicon = g_themed_icon_new_from_names ((gchar **) disc_names, -1);
        popup->status_icon = gtk_image_new_from_gicon (disc_gicon, GTK_ICON_SIZE_MENU);
        g_object_unref (disc_gicon);
    }
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
    gtk_box_pack_start (GTK_BOX (refresh_box), refresh_icon, FALSE, FALSE, 0);
    popup->refresh_label = gtk_label_new (popup->scanning ? _("Updating…") : _("Refresh"));
    gtk_box_pack_start (GTK_BOX (refresh_box), popup->refresh_label, FALSE, FALSE, 0);
    gtk_container_add  (GTK_CONTAINER (popup->refresh_button), refresh_box);
    gtk_button_set_relief (GTK_BUTTON (popup->refresh_button), GTK_RELIEF_NONE);
    gtk_widget_set_valign (popup->refresh_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (popup->refresh_button, 16);
    gtk_widget_set_sensitive (popup->refresh_button, !popup->scanning);
    g_signal_connect (popup->refresh_button, "clicked",
                      G_CALLBACK (on_refresh_clicked), popup);

    gtk_box_pack_end (GTK_BOX (top_row), popup->refresh_button, FALSE, FALSE, 0);
    gtk_widget_show (popup->refresh_button);

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

    /* Franja de portal cautivo (oculta hasta que NM lo informe). */
    {
        GtkWidget *prow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start  (prow, 12);
        gtk_widget_set_margin_end    (prow, 12);
        gtk_widget_set_margin_bottom (prow, 8);

        GtkWidget *picon = gtk_image_new_from_icon_name ("dialog-warning-symbolic",
                                                         GTK_ICON_SIZE_MENU);
        gtk_box_pack_start (GTK_BOX (prow), picon, FALSE, FALSE, 0);

        GtkWidget *plabel = gtk_label_new (_("This network requires you to sign in"));
        gtk_label_set_xalign (GTK_LABEL (plabel), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (plabel), TRUE);
        gtk_box_pack_start (GTK_BOX (prow), plabel, TRUE, TRUE, 0);

        GtkWidget *pbtn = gtk_button_new_with_label (_("Sign in"));
        gtk_widget_set_valign (pbtn, GTK_ALIGN_CENTER);
        g_signal_connect (pbtn, "clicked",
                          G_CALLBACK (on_portal_signin_clicked), popup);
        gtk_box_pack_end (GTK_BOX (prow), pbtn, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (popup->top_box), prow, FALSE, FALSE, 0);
        gtk_widget_show_all (prow);
        gtk_widget_set_no_show_all (prow, TRUE);
        gtk_widget_hide (prow);
        popup->portal_row = prow;
    }


    /* Contenedor sección Ethernet (en content_box, se llena en update_eth_section). */
    popup->eth_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (popup->content_box), popup->eth_section, FALSE, FALSE, 0);


    /* Contenedor sección VPN (al fondo del content_box, se llena en update_vpn_section). */
    popup->vpn_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (popup->content_box), popup->vpn_section, FALSE, FALSE, 0);

    popup->ui_built = TRUE;

    /* Llenar las secciones por primera vez. */
    update_top_status      (popup);
    update_eth_section     (popup);
    update_devices_section (popup);
    update_vpn_section     (popup);

    nm_cache_end ();
}

