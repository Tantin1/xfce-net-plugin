#include "popup.h"
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#define NM_BUS_NAME    "org.freedesktop.NetworkManager"
#define NM_OBJECT_PATH "/org/freedesktop/NetworkManager"
#define NM_IFACE       "org.freedesktop.NetworkManager"
#define NM_WIFI_IFACE  "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_IFACE    "org.freedesktop.NetworkManager.AccessPoint"

/* ---------- posicionamiento: solo se llama al abrir ---------- */

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
    GVariant    *v, *inner;
    const gchar *primary_path;
    gchar       *result = NULL;

    v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, NM_OBJECT_PATH,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)", NM_IFACE, "PrimaryConnection"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!v) return NULL;

    g_variant_get (v, "(v)", &inner);
    primary_path = g_variant_get_string (inner, NULL);

    if (!primary_path || g_strcmp0 (primary_path, "/") == 0) {
        g_variant_unref (inner);
        g_variant_unref (v);
        return NULL;
    }

    GVariant *dev_v = g_dbus_connection_call_sync (
            conn, NM_BUS_NAME, primary_path,
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new ("(ss)",
                "org.freedesktop.NetworkManager.Connection.Active",
                "Devices"),
            G_VARIANT_TYPE ("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

    if (dev_v) {
        GVariant    *dev_inner, *dev_array;
        g_variant_get (dev_v, "(v)", &dev_inner);
        dev_array = dev_inner;

        if (g_variant_n_children (dev_array) > 0) {
            GVariant    *dev_path_v = g_variant_get_child_value (dev_array, 0);
            const gchar *dev_path   = g_variant_get_string (dev_path_v, NULL);

            GVariant *ap_v = g_dbus_connection_call_sync (
                    conn, NM_BUS_NAME, dev_path,
                    "org.freedesktop.DBus.Properties", "Get",
                    g_variant_new ("(ss)", NM_WIFI_IFACE, "ActiveAccessPoint"),
                    G_VARIANT_TYPE ("(v)"),
                    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

            if (ap_v) {
                GVariant    *ap_inner;
                const gchar *ap_path;
                g_variant_get (ap_v, "(v)", &ap_inner);
                ap_path = g_variant_get_string (ap_inner, NULL);

                if (ap_path && g_strcmp0 (ap_path, "/") != 0) {
                    GVariant *ssid_v = g_dbus_connection_call_sync (
                            conn, NM_BUS_NAME, ap_path,
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", NM_AP_IFACE, "Ssid"),
                            G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

                    if (ssid_v) {
                        GVariant     *ssid_inner;
                        GVariantIter *iter;
                        GString      *ssid_str = g_string_new (NULL);
                        guchar        c;

                        g_variant_get (ssid_v, "(v)", &ssid_inner);
                        iter = g_variant_iter_new (ssid_inner);
                        while (g_variant_iter_next (iter, "y", &c))
                            g_string_append_c (ssid_str, (gchar) c);
                        g_variant_iter_free (iter);

                        if (ssid_str->len > 0)
                            result = g_string_free (ssid_str, FALSE);
                        else
                            g_string_free (ssid_str, TRUE);

                        g_variant_unref (ssid_inner);
                        g_variant_unref (ssid_v);
                    }
                }
                g_variant_unref (ap_inner);
                g_variant_unref (ap_v);
            }
            g_variant_unref (dev_path_v);
        }
        g_variant_unref (dev_inner);
        g_variant_unref (dev_v);
    }

    g_variant_unref (inner);
    g_variant_unref (v);
    return result;
}

/* ---------- callback del switch de Wi-Fi ---------- */

static gboolean
on_wifi_switch_toggled (GtkSwitch *sw, gboolean state, gpointer conn)
{
    (void) sw;
    nm_set_wifi_enabled ((GDBusConnection *) conn, state);
    return FALSE;
}

typedef struct {
    NetPopup        *popup;
    GDBusConnection *conn;
    GtkWidget       *button;
    XfcePanelPlugin *plugin;
    GSList          *devices;
} RefreshData;

static gboolean
on_refresh_reenable (gpointer popup_ptr)
{
    NetPopup *popup = popup_ptr;
    popup->scanning = FALSE;
    return G_SOURCE_REMOVE;
}

static void
on_refresh_clicked (GtkWidget *btn, NetPopup *popup)
{
    GDBusConnection *conn = g_object_get_data (G_OBJECT (popup->window), "conn");
    if (!conn) return;

    popup->scanning = TRUE;
    gtk_widget_set_sensitive (btn, FALSE);
    GtkWidget *lbl = g_object_get_data (G_OBJECT (btn), "label");
    if (lbl) gtk_label_set_text (GTK_LABEL (lbl), _("Updating…"));
    g_timeout_add_seconds (5, on_refresh_reenable, popup);

    GSList *devs = nm_get_wifi_devices (conn);
    for (GSList *l = devs; l; l = l->next) {
        NmDevice *dev = l->data;
        nm_request_scan (conn, dev->object_path);
    }
    nm_device_list_free (devs);
}

/* ---------- datos para el callback de expansion ---------- */

typedef struct {
    NetPopup        *popup;
    GtkWidget       *expand_box;
    gchar           *ssid;
    gchar           *ap_path;
    gboolean         secure;
    gboolean         active;
    gboolean         saved;
    gchar           *device_path;
    GDBusConnection *conn;
    GtkWidget       *pass_entry;
    GtkWidget       *autoconnect_check;
} RowData;

static void
row_data_free (RowData *rd)
{
    g_free (rd->ssid);
    g_free (rd->ap_path);
    g_free (rd->device_path);
    g_free (rd);
}

static void
on_row_clicked (GtkWidget *event_box, GdkEventButton *event, RowData *rd)
{
    (void) event;
    (void) event_box;

    gboolean ya_abierto = (rd->popup->current_expand_box == rd->expand_box);

    if (rd->popup->current_expand_box) {
        gtk_widget_hide (rd->popup->current_expand_box);
        rd->popup->current_expand_box = NULL;
    }

    if (!ya_abierto) {
        gtk_widget_show (rd->expand_box);
        rd->popup->current_expand_box = rd->expand_box;
    }
}

/* ---------- prototipos anticipados ---------- */

static GtkWidget *make_ap_row (NmAccessPoint *ap, NetPopup *popup,
                                const gchar *device_path, GDBusConnection *conn);
static void on_forget_clicked  (GtkWidget *btn, RowData *rd);
static void on_connect_clicked (GtkWidget *btn, RowData *rd);

/* ---------- helper: reemplaza una fila en su contenedor ---------- */

static void
refresh_row (RowData *rd)
{
    GSList        *aps;
    NmAccessPoint *updated_ap = NULL;

    aps = nm_get_access_points (rd->conn, rd->device_path);
    for (GSList *a = aps; a; a = a->next) {
        NmAccessPoint *ap = a->data;
        if (g_strcmp0 (ap->ssid, rd->ssid) == 0) {
            updated_ap = ap;
            break;
        }
    }

    if (!updated_ap) {
        nm_ap_list_free (aps);
        return;
    }

    GtkWidget *old_row = gtk_widget_get_parent (rd->expand_box);
    while (old_row && !GTK_IS_BOX (gtk_widget_get_parent (old_row)))
        old_row = gtk_widget_get_parent (old_row);

    GtkWidget *section = gtk_widget_get_parent (old_row);
    gint position = 0;
    GList *children = gtk_container_get_children (GTK_CONTAINER (section));
    for (GList *l = children; l; l = l->next, position++)
        if (l->data == old_row) break;
    g_list_free (children);

    GtkWidget *new_row = make_ap_row (updated_ap, rd->popup,
                                      rd->device_path, rd->conn);
    gtk_box_pack_start (GTK_BOX (section), new_row, FALSE, FALSE, 0);
    gtk_box_reorder_child (GTK_BOX (section), new_row, position);
    gtk_widget_show_all (new_row);
    gtk_widget_destroy (old_row);

    nm_ap_list_free (aps);
}

/* ---------- callbacks de botones ---------- */

static void
on_disconnect_clicked (GtkWidget *btn, RowData *rd)
{
    (void) btn;
    nm_disconnect_device (rd->conn, rd->device_path);
    g_usleep (1000000);
    refresh_row (rd);
}

static void
on_forget_response (GtkDialog *dialog, gint response, RowData *rd)
{
    gtk_widget_destroy (GTK_WIDGET (dialog));
    if (response != GTK_RESPONSE_YES)
        return;
    nm_forget_connection (rd->conn, rd->ssid);
    g_usleep (500000);
    refresh_row (rd);
}

static void
on_forget_clicked (GtkWidget *btn, RowData *rd)
{
    (void) btn;

    GtkWidget *dialog = gtk_message_dialog_new (
        NULL,
        0,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        _("Forget network \"%s\"?"), rd->ssid);
    gtk_window_set_title (GTK_WINDOW (dialog), _("Confirm"));
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
        saved_pw = nm_get_saved_password (rd->conn, rd->ssid);

    if (rd->autoconnect_check)
        autoconnect = gtk_toggle_button_get_active (
                          GTK_TOGGLE_BUTTON (rd->autoconnect_check));

    /* Deshabilitar botón Conectar mientras espera */
    GtkWidget *connect_btn = NULL;
    GtkWidget *action_row  = gtk_widget_get_parent (rd->expand_box);
    (void) action_row;
    {
        GList *kids = gtk_container_get_children (GTK_CONTAINER (rd->expand_box));
        for (GList *k = kids; k; k = k->next) {
            if (GTK_IS_BOX (k->data)) {
                GList *row_kids = gtk_container_get_children (GTK_CONTAINER (k->data));
                for (GList *r = row_kids; r; r = r->next) {
                    if (GTK_IS_BUTTON (r->data)) {
                        const gchar *lbl = gtk_button_get_label (GTK_BUTTON (r->data));
                        if (g_strcmp0 (lbl, _("Connect")) == 0)
                            connect_btn = r->data;
                    }
                }
                g_list_free (row_kids);
            }
        }
        g_list_free (kids);
    }
    if (connect_btn)
        gtk_widget_set_sensitive (connect_btn, FALSE);

    

    gboolean ok = nm_add_and_activate_connection (rd->conn, rd->device_path,
                                                   rd->ap_path, rd->ssid,
                                                   saved_pw ? saved_pw : password,
                                                   autoconnect);

    if (ok)
        ok = nm_wait_for_connected (rd->conn, rd->device_path);

    

    if (!ok) {
        /* Borrar perfil mal guardado */
        nm_forget_connection (rd->conn, rd->ssid);

        /* Mostrar error en el expand */
        if (rd->pass_entry) {
            gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");

            GtkWidget *err_label = g_object_get_data (
                                       G_OBJECT (rd->expand_box), "error-label");
            if (!err_label) {
                err_label = gtk_label_new (_("Incorrect password"));
                gtk_style_context_add_class (
                    gtk_widget_get_style_context (err_label), "error");
                gtk_label_set_xalign (GTK_LABEL (err_label), 0.0);
                gtk_box_pack_start (GTK_BOX (rd->expand_box),
                                    err_label, FALSE, FALSE, 0);
                gtk_box_reorder_child (GTK_BOX (rd->expand_box), err_label, 0);
                g_object_set_data (G_OBJECT (rd->expand_box),
                                   "error-label", err_label);
            }
            gtk_widget_show (err_label);
        }

        if (connect_btn)
            gtk_widget_set_sensitive (connect_btn, TRUE);

        g_free (saved_pw);
        return;
    }

    g_free (saved_pw);
    refresh_row (rd);
}

static void
on_connect_clicked (GtkWidget *btn, RowData *rd)
{
    (void) btn;
    do_connect (rd);
}

static gboolean
on_pass_entry_key_press (GtkWidget *widget, GdkEventKey *event, RowData *rd)
{
    (void) widget;
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        do_connect (rd);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void
on_eye_pressed (GtkWidget *btn, RowData *rd)
{
    (void) btn;
    if (rd->pass_entry)
        gtk_entry_set_visibility (GTK_ENTRY (rd->pass_entry), TRUE);
}

static void
on_eye_released (GtkWidget *btn, RowData *rd)
{
    (void) btn;
    if (rd->pass_entry)
        gtk_entry_set_visibility (GTK_ENTRY (rd->pass_entry), FALSE);
}

/* ---------- construccion de filas de red ---------- */

static GtkWidget *
make_signal_icon (gint strength, gboolean secure)
{
    const gchar *level;
    gchar        icon_name[80];
    GtkWidget   *img;

    if (strength >= 80)
        level = "excellent";
    else if (strength >= 55)
        level = "good";
    else if (strength >= 30)
        level = "ok";
    else
        level = "weak";

    if (secure) {
        GtkIconTheme *theme = gtk_icon_theme_get_default ();
        g_snprintf (icon_name, sizeof (icon_name),
                    "network-wireless-secure-signal-%s-symbolic", level);
        if (!gtk_icon_theme_has_icon (theme, icon_name)) {
            g_snprintf (icon_name, sizeof (icon_name),
                        "network-wireless-secure-signal-%s", level);
        }
        if (!gtk_icon_theme_has_icon (theme, icon_name)) {
            g_snprintf (icon_name, sizeof (icon_name),
                        "network-wireless-signal-%s-symbolic", level);
        }
        img = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_MENU);
    } else {
        g_snprintf (icon_name, sizeof (icon_name),
                    "network-wireless-signal-%s-symbolic", level);
        img = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_MENU);
    }

    return img;
}

static GtkWidget *
make_ap_row (NmAccessPoint   *ap,
             NetPopup        *popup,
             const gchar     *device_path,
             GDBusConnection *conn)
{
    GtkWidget *outer, *event_box, *row, *ssid_label, *signal_icon;
    GtkWidget *expand_box, *action_btn, *action_row;
    GtkWidget *forget_btn = NULL;
    GtkWidget *pass_entry = NULL;
    GtkWidget *autoconnect_check_widget = NULL;

    outer     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    event_box = gtk_event_box_new ();
    gtk_event_box_set_above_child (GTK_EVENT_BOX (event_box), FALSE);
    gtk_container_add (GTK_CONTAINER (event_box), outer);

    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (row, 12);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 8);
    gtk_widget_set_margin_bottom (row, 8);

    signal_icon = make_signal_icon (ap->strength, ap->secure);
    gtk_box_pack_start (GTK_BOX (row), signal_icon, FALSE, FALSE, 0);

    ssid_label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (ssid_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (ssid_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (ssid_label), 24);

    {
        const gchar *band;
        if (ap->frequency >= 5925)
            band = "6G";
        else if (ap->frequency >= 5000)
            band = "5G";
        else
            band = "2.4G";

        if (ap->active) {
            gchar *markup = g_markup_printf_escaped ("<b>%s</b>  <small><span alpha='60%%'>%s</span></small>",
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
        } else {
            gchar *markup = g_markup_printf_escaped ("%s  <small><span alpha='60%%'>%s</span></small>",
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

    if (ap->active) {
        action_btn = gtk_button_new_with_label (_("Disconnect"));
    } else {
        gboolean saved = nm_has_saved_connection (conn, ap->ssid);

        if (saved) {
            GtkWidget *saved_label = gtk_label_new (_("Saved network"));
            gtk_style_context_add_class (gtk_widget_get_style_context (saved_label),
                                         "dim-label");
            gtk_label_set_xalign (GTK_LABEL (saved_label), 0.0);
            gtk_box_pack_start (GTK_BOX (expand_box), saved_label, FALSE, FALSE, 0);
            gtk_widget_show (saved_label);

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

        action_btn = gtk_button_new_with_label (_("Connect"));
    }

    gtk_widget_set_halign (action_btn, GTK_ALIGN_END);

    action_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start (GTK_BOX (action_row), action_btn, FALSE, FALSE, 0);
    if (!ap->active && forget_btn) {
        gtk_box_pack_end (GTK_BOX (action_row), forget_btn, FALSE, FALSE, 0);
        gtk_widget_show (forget_btn);
    }
    gtk_box_pack_start (GTK_BOX (expand_box), action_row, FALSE, FALSE, 0);
    gtk_widget_show (action_btn);
    gtk_widget_show (action_row);

    gtk_widget_set_no_show_all (expand_box, TRUE);
    gtk_widget_hide (expand_box);
    gtk_box_pack_start (GTK_BOX (outer), expand_box, FALSE, FALSE, 0);

    /* ---- RowData ---- */
    RowData *rd     = g_new0 (RowData, 1);
    rd->popup       = popup;
    rd->expand_box  = expand_box;
    rd->ssid        = g_strdup (ap->ssid);
    rd->ap_path     = g_strdup (ap->object_path);
    rd->secure      = ap->secure;
    rd->active      = ap->active;
    rd->saved       = ap->active ? FALSE : nm_has_saved_connection (conn, ap->ssid);
    rd->device_path = g_strdup (device_path);
    rd->conn              = conn;
    rd->pass_entry        = pass_entry;
    rd->autoconnect_check = autoconnect_check_widget;

    g_object_set_data_full (G_OBJECT (outer), "row-data", rd,
                            (GDestroyNotify) row_data_free);

    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_row_clicked), rd);

    if (ap->active) {
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
    nm_set_device_enabled (d->conn, d->device_path, state);
    return FALSE;
}

static GtkWidget *
make_device_section (GDBusConnection *conn, NmDevice *dev, NetPopup *popup,
                     gboolean show_header)
{
    GtkWidget *section, *separator;
    GSList    *aps, *a;

    section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

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

        DeviceSwitchData *d = g_new0 (DeviceSwitchData, 1);
        d->conn        = conn;
        d->device_path = g_strdup (dev->object_path);
        g_object_set_data_full (G_OBJECT (dev_switch), "switch-data", d,
                                (GDestroyNotify) device_switch_data_free);

        g_signal_connect (dev_switch, "state-set",
                          G_CALLBACK (on_device_switch_toggled), d);
        gtk_box_pack_end (GTK_BOX (header_row), dev_switch, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (section), header_row, FALSE, FALSE, 0);
    }

    separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (section), separator, FALSE, FALSE, 0);

    aps = nm_get_access_points (conn, dev->object_path);
    for (a = aps; a; a = a->next) {
        GtkWidget *r = make_ap_row (a->data, popup, dev->object_path, conn);
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
        nm_activate_vpn   (d->conn, d->conn_path);
    else
        nm_deactivate_vpn (d->conn, d->conn_path);
    return FALSE;
}

static GtkWidget *
make_vpn_section (GDBusConnection *conn)
{
    GSList *vpns = nm_get_vpn_connections (conn);
    if (!vpns) return NULL;

    GtkWidget *section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (section), sep, FALSE, FALSE, 0);

    GtkWidget *header_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start  (header_row, 12);
    gtk_widget_set_margin_end    (header_row, 12);
    gtk_widget_set_margin_top    (header_row, 8);
    gtk_widget_set_margin_bottom (header_row, 4);

    GtkWidget *header = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (header), "<b><small>VPN</small></b>");
    gtk_label_set_xalign (GTK_LABEL (header), 0.0);
    gtk_box_pack_start (GTK_BOX (header_row), header, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (section), header_row, FALSE, FALSE, 0);

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
        d->conn      = conn;
        d->conn_path = g_strdup (vpn->conn_path);
        g_object_set_data_full (G_OBJECT (sw), "vpn-switch-data", d,
                                (GDestroyNotify) vpn_switch_data_free);

        g_signal_connect (sw, "state-set",
                          G_CALLBACK (on_vpn_switch_toggled), d);
        gtk_box_pack_end (GTK_BOX (row), sw, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (section), row, FALSE, FALSE, 0);
    }

    nm_vpn_list_free (vpns);
    return section;
}

/* ---------- event handlers ---------- */

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
                nm_add_and_activate_connection (conn, dev->object_path,
                                                "/", ssid,
                                                secure ? password : NULL,
                                                TRUE);
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

    GDBusConnection *conn = g_object_get_data (G_OBJECT (popup->window), "conn");
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

    /* Guardar referencias a los widgets en el diálogo para el callback */
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
    return popup;
}

void
popup_show (NetPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button,
            GSList *devices, GDBusConnection *conn,
            gint popup_width, gint popup_height)
{
    GdkDisplay *display;
    GdkSeat    *seat;
    GSList     *d;
    (void) plugin;

    /* Limpiar zona scrolleable */
    GList *children = gtk_container_get_children (
                          GTK_CONTAINER (popup->content_box));
    g_list_foreach (children, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (children);
    popup->current_expand_box = NULL;
    popup->popup_width  = popup_width;
    popup->popup_height = popup_height;
    gtk_widget_set_size_request (popup->window, popup_width, -1);
    gtk_widget_set_size_request (popup->scroll, popup_width, popup_height);

    /* Limpiar y reconstruir zona fija superior */
    GList *top_children = gtk_container_get_children (
                              GTK_CONTAINER (popup->top_box));
    g_list_foreach (top_children, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (top_children);

    /* ---- Barra superior fija ---- */
    GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (top_row, 12);
    gtk_widget_set_margin_end    (top_row, 12);
    gtk_widget_set_margin_top    (top_row, 8);
    gtk_widget_set_margin_bottom (top_row, 8);

    /* Ícono de señal o desconectado */
    gchar       *primary_ssid = get_primary_ssid (conn);
    GtkWidget   *top_icon;
    if (primary_ssid)
        top_icon = gtk_image_new_from_icon_name (
                       "network-wireless-symbolic", GTK_ICON_SIZE_MENU);
    else
        top_icon = gtk_image_new_from_icon_name (
                       "network-wireless-disconnected-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_valign (top_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (top_icon, 22);
    gtk_box_pack_start (GTK_BOX (top_row), top_icon, FALSE, FALSE, 0);

    /* Columna central: "Wi-Fi [switch]" arriba, estado abajo */
    GtkWidget *center_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start (GTK_BOX (top_row), center_box, TRUE, TRUE, 0);

    GtkWidget *refresh_btn   = gtk_button_new ();
    GtkWidget *refresh_box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *refresh_icon  = gtk_image_new_from_icon_name (
                                   "view-refresh-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *refresh_label = gtk_label_new (popup->scanning ? _("Updating…") : _("Refresh"));
    gtk_box_pack_start (GTK_BOX (refresh_box), refresh_icon,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (refresh_box), refresh_label, FALSE, FALSE, 0);
    gtk_container_add  (GTK_CONTAINER (refresh_btn), refresh_box);
    gtk_button_set_relief (GTK_BUTTON (refresh_btn), GTK_RELIEF_NONE);
    gtk_widget_set_valign (refresh_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (refresh_btn, 16);
    gtk_widget_set_sensitive (refresh_btn, !popup->scanning);
    g_object_set_data (G_OBJECT (refresh_btn), "label", refresh_label);
    g_signal_connect (refresh_btn, "clicked",
                      G_CALLBACK (on_refresh_clicked), popup);
    gtk_box_pack_end (GTK_BOX (top_row), refresh_btn, FALSE, FALSE, 0);

    /* Fila Wi-Fi + switch */
    GtkWidget *wifi_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start (GTK_BOX (center_box), wifi_row, FALSE, FALSE, 0);

    GtkWidget *wifi_label = gtk_label_new (_("Wi-Fi"));
    gtk_label_set_xalign (GTK_LABEL (wifi_label), 0.0);
    gtk_box_pack_start (GTK_BOX (wifi_row), wifi_label, FALSE, FALSE, 0);

    GtkWidget *wifi_switch = gtk_switch_new ();
    gtk_switch_set_active (GTK_SWITCH (wifi_switch),
                           nm_get_wifi_enabled (conn));
    g_signal_connect (wifi_switch, "state-set",
                      G_CALLBACK (on_wifi_switch_toggled), conn);
    gtk_box_pack_start (GTK_BOX (wifi_row), wifi_switch, FALSE, FALSE, 0);

    

    /* Texto de estado debajo */
    GtkWidget *status_label = gtk_label_new (NULL);
    if (primary_ssid) {
        gchar *markup = g_markup_printf_escaped (
                            _("Connected to <b>%s</b>"), primary_ssid);
        gtk_label_set_markup (GTK_LABEL (status_label), markup);
        g_free (markup);
    } else {
        gtk_label_set_text (GTK_LABEL (status_label), _("Not connected"));
    }
    g_free (primary_ssid);
    gtk_label_set_xalign (GTK_LABEL (status_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (status_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start (GTK_BOX (center_box), status_label, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (popup->top_box), top_row, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (popup->top_box), sep, FALSE, FALSE, 0);
    /* -------------------------------- */

    gint n_devices = g_slist_length (devices);
    for (d = devices; d; d = d->next) {
        GtkWidget *section = make_device_section (conn, d->data, popup, n_devices > 1);
        gtk_box_pack_start (GTK_BOX (popup->content_box), section,
                            FALSE, FALSE, 0);
    }

    /* ---- Sección Ethernet (solo si hay cable) ---- */
    GSList *eth_devices = nm_get_ethernet_devices (conn);
    for (d = eth_devices; d; d = d->next) {
        NmDevice  *dev = d->data;

        GtkWidget *eth_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start (GTK_BOX (popup->content_box), eth_sep, FALSE, FALSE, 0);

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

        gtk_box_pack_start (GTK_BOX (popup->content_box), eth_row, FALSE, FALSE, 0);
    }
    nm_device_list_free (eth_devices);
    /* ---------------------------------------------- */

    /* ---- Sección VPN (solo si hay perfiles configurados) ---- */
    GtkWidget *vpn_section = make_vpn_section (conn);
    if (vpn_section)
        gtk_box_pack_start (GTK_BOX (popup->content_box), vpn_section,
                            FALSE, FALSE, 0);
    /* --------------------------------------------------------- */

    

    g_object_set_data (G_OBJECT (popup->window), "conn", conn);

    gtk_widget_show_all (popup->window);
    position_popup (popup, button);
    gtk_window_present (GTK_WINDOW (popup->window));

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
    gtk_widget_destroy (popup->window);
    g_free (popup);
}
