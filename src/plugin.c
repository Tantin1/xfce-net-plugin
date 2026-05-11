#include <libxfce4panel/libxfce4panel.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include "popup.h"
#include "nm-dbus.h"

#define DEFAULT_SQUARE_BUTTON   FALSE
#define DEFAULT_PLUGIN_SIZE     24
#define DEFAULT_USE_PANEL_ICON  FALSE
#define DEFAULT_ICON_SIZE       16
#define DEFAULT_POPUP_WIDTH     400
#define DEFAULT_POPUP_HEIGHT    340

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *button;
    GtkWidget       *icon_box;   /* contenedor del ícono en el botón */
    GtkWidget       *stack;
    GtkWidget       *spinner;
    NetPopup        *popup;
    GDBusConnection *conn;

    /* configuración */
    gboolean        square_button;
    gint            plugin_size;
    gboolean        use_panel_icon;
    gint            icon_size;
    gint            popup_width;
    gint            popup_height;

    /* CSS para tamaño del botón */
    GtkCssProvider *button_css;

    /* IDs de suscripción a señales DBus */
    guint *signal_ids;

    /* estado de conexión en progreso */
    gboolean        connecting;

    /* último ícono mostrado (para reconstruir al cambiar tamaño) */
    gint     last_strength;
    gboolean last_secure;
    gboolean last_connected;
    gboolean last_vpn;
    gboolean last_wired;
} NetPlugin;

/* ---------- actualizar ícono del botón del panel ---------- */

static void
update_panel_icon (NetPlugin *np,
                   gboolean connected, gint strength,
                   gboolean secure, gboolean vpn, gboolean wired)
{
    gint icon_px = np->use_panel_icon
                   ? xfce_panel_plugin_get_icon_size (np->plugin)
                   : np->icon_size;

    /* Guardar estado para poder reconstruir al cambiar tamaño */
    np->last_strength  = strength;
    np->last_secure    = secure;
    np->last_connected = connected;
    np->last_vpn       = vpn;
    np->last_wired     = wired;

    /* Limpiar contenedor */
    GList *kids = gtk_container_get_children (GTK_CONTAINER (np->icon_box));
    g_list_foreach (kids, (GFunc) gtk_widget_destroy, NULL);
    g_list_free (kids);

    GtkWidget *new_icon;

    if (!connected) {
        if (wired) {
            if (vpn) {
                GtkWidget *overlay  = gtk_overlay_new ();
                GtkWidget *base_img = gtk_image_new_from_icon_name (
                                          "network-wired-symbolic", GTK_ICON_SIZE_BUTTON);
                GtkWidget *lock_img = gtk_image_new_from_icon_name (
                                          "nm-secure-lock", GTK_ICON_SIZE_BUTTON);
                gtk_image_set_pixel_size (GTK_IMAGE (base_img), icon_px);
                gtk_image_set_pixel_size (GTK_IMAGE (lock_img), icon_px);
                gtk_widget_set_halign (lock_img, GTK_ALIGN_FILL);
                gtk_widget_set_valign (lock_img, GTK_ALIGN_FILL);
                gtk_container_add (GTK_CONTAINER (overlay), base_img);
                gtk_overlay_add_overlay (GTK_OVERLAY (overlay), lock_img);
                gtk_widget_set_size_request (overlay, icon_px, icon_px);
                gtk_widget_show_all (overlay);
                new_icon = overlay;
            } else {
                new_icon = gtk_image_new_from_icon_name (
                               "network-wired-symbolic", GTK_ICON_SIZE_BUTTON);
                gtk_image_set_pixel_size (GTK_IMAGE (new_icon), icon_px);
            }
        } else {
            new_icon = gtk_image_new_from_icon_name (
                           "network-wireless-disconnected-symbolic",
                           GTK_ICON_SIZE_BUTTON);
            gtk_image_set_pixel_size (GTK_IMAGE (new_icon), icon_px);
        }
    } else {
        /* Para el panel: secure solo si hay VPN */
        new_icon = make_signal_icon (strength, vpn, icon_px);
    }

    gtk_box_pack_start (GTK_BOX (np->icon_box), new_icon, TRUE, TRUE, 0);
    gtk_widget_show_all (np->icon_box);
}

/* ---------- aplicar configuración visual ---------- */

static void
apply_config (NetPlugin *np)
{
    XfcePanelPlugin *plugin = np->plugin;

    /* ---- tamaño del botón via CSS ---- */
    {
        gchar *css;
        gint   ps = xfce_panel_plugin_get_size (plugin);

        if (np->square_button)
            css = g_strdup_printf (
                "button{padding:0;margin:0;min-width:%dpx;min-height:%dpx;border:none;}",
                ps, ps);
        else {
            GtkOrientation orient = xfce_panel_plugin_get_orientation (plugin);
            if (orient == GTK_ORIENTATION_HORIZONTAL)
                css = g_strdup_printf (
                    "button{padding:0;margin:0;min-width:%dpx;min-height:0;border:none;}",
                    np->plugin_size);
            else
                css = g_strdup_printf (
                    "button{padding:0;margin:0;min-width:0;min-height:%dpx;border:none;}",
                    np->plugin_size);
        }

        if (!np->button_css) {
            np->button_css = gtk_css_provider_new ();
            gtk_style_context_add_provider (
                gtk_widget_get_style_context (np->button),
                GTK_STYLE_PROVIDER (np->button_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        gtk_css_provider_load_from_data (np->button_css, css, -1, NULL);
        g_free (css);
    }

    /* Reconstruir ícono con el nuevo tamaño */
    update_panel_icon (np,
                       np->last_connected, np->last_strength,
                       np->last_secure, np->last_vpn, np->last_wired);
}

/* ---------- leer / guardar configuración ---------- */

#define CONFIG_PATH "xfce4/panel/xfce-net-plugin.ini"

static gchar *
get_config_path (void)
{
    return g_build_filename (g_get_user_config_dir (), CONFIG_PATH, NULL);
}

static void
load_config (NetPlugin *np)
{
    GKeyFile *kf   = g_key_file_new ();
    gchar    *path = get_config_path ();
    GError   *err  = NULL;

    np->square_button  = DEFAULT_SQUARE_BUTTON;
    np->plugin_size    = DEFAULT_PLUGIN_SIZE;
    np->use_panel_icon = DEFAULT_USE_PANEL_ICON;
    np->icon_size      = DEFAULT_ICON_SIZE;
    np->popup_width    = DEFAULT_POPUP_WIDTH;
    np->popup_height   = DEFAULT_POPUP_HEIGHT;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        goto out;
    }

    np->square_button  = g_key_file_get_boolean (kf, "appearance", "square_button",  NULL);
    np->plugin_size    = g_key_file_get_integer  (kf, "appearance", "plugin_size",    NULL);
    np->use_panel_icon = g_key_file_get_boolean  (kf, "appearance", "use_panel_icon", NULL);
    np->icon_size      = g_key_file_get_integer  (kf, "appearance", "icon_size",      NULL);
    np->popup_width    = g_key_file_get_integer  (kf, "appearance", "popup_width",    NULL);
    np->popup_height   = g_key_file_get_integer  (kf, "appearance", "popup_height",   NULL);

    if (np->plugin_size  == 0) np->plugin_size  = DEFAULT_PLUGIN_SIZE;
    if (np->icon_size    == 0) np->icon_size    = DEFAULT_ICON_SIZE;
    if (np->popup_width  == 0) np->popup_width  = DEFAULT_POPUP_WIDTH;
    if (np->popup_height == 0) np->popup_height = DEFAULT_POPUP_HEIGHT;

out:
    g_key_file_free (kf);
    g_free (path);
}

static void
save_config (NetPlugin *np)
{
    GKeyFile *kf   = g_key_file_new ();
    gchar    *path = get_config_path ();
    gchar    *dir  = g_path_get_dirname (path);
    GError   *err  = NULL;

    g_mkdir_with_parents (dir, 0755);

    g_key_file_set_boolean (kf, "appearance", "square_button",  np->square_button);
    g_key_file_set_integer  (kf, "appearance", "plugin_size",    np->plugin_size);
    g_key_file_set_boolean (kf, "appearance", "use_panel_icon", np->use_panel_icon);
    g_key_file_set_integer  (kf, "appearance", "icon_size",      np->icon_size);
    g_key_file_set_integer  (kf, "appearance", "popup_width",    np->popup_width);
    g_key_file_set_integer  (kf, "appearance", "popup_height",   np->popup_height);

    if (!g_key_file_save_to_file (kf, path, &err)) {
        g_warning ("xfce-net-plugin: no se pudo guardar config: %s", err->message);
        g_error_free (err);
    }

    g_key_file_free (kf);
    g_free (dir);
    g_free (path);
}

/* ---------- ventana de propiedades ---------- */

typedef struct {
    NetPlugin   *np;
    GtkWidget   *square_check;
    GtkWidget   *plugin_spin;
    GtkWidget   *panel_icon_check;
    GtkWidget   *icon_spin;
    GtkWidget   *popup_width_spin;
    GtkWidget   *popup_height_spin;
} PropsDialog;

static void
on_square_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    gboolean active = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->plugin_spin, !active);
    pd->np->square_button = active;
    save_config (pd->np);
    apply_config (pd->np);
}

static void
on_plugin_size_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->np->plugin_size = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->np);
    apply_config (pd->np);
}

static void
on_panel_icon_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    gboolean active = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->icon_spin, !active);
    pd->np->use_panel_icon = active;
    save_config (pd->np);
    apply_config (pd->np);
}

static void
on_icon_size_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->np->icon_size = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->np);
    apply_config (pd->np);
}

static void
on_popup_width_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->np->popup_width = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->np);
}

static void
on_popup_height_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->np->popup_height = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->np);
}

static void
on_props_dialog_destroy (GtkWidget *win, PropsDialog *pd)
{
    (void) win;
    g_free (pd);
}

static void
on_configure_plugin (XfcePanelPlugin *plugin, NetPlugin *np)
{
    (void) plugin;

    PropsDialog *pd = g_new0 (PropsDialog, 1);
    pd->np = np;

    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Properties — Network"),
        NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        _("Close"), GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 12);

    /* ---- frame Apariencia ---- */
    GtkWidget *frame = gtk_frame_new (_("Appearance"));
    GtkWidget *grid  = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_container_set_border_width (GTK_CONTAINER (grid), 8);
    gtk_container_add (GTK_CONTAINER (frame), grid);

    gint row = 0;

    /* botón cuadrado */
    pd->square_check = gtk_check_button_new_with_label (_("Square button"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->square_check),
                                  np->square_button);
    gtk_grid_attach (GTK_GRID (grid), pd->square_check, 0, row, 2, 1);
    row++;

    /* tamaño del plugin */
    GtkWidget *plugin_size_label = gtk_label_new (_("Plugin size:"));
    gtk_label_set_xalign (GTK_LABEL (plugin_size_label), 0.0);
    gtk_widget_set_margin_start (plugin_size_label, 12);
    pd->plugin_spin = gtk_spin_button_new_with_range (16, 200, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->plugin_spin), np->plugin_size);
    gtk_widget_set_sensitive (pd->plugin_spin, !np->square_button);
    gtk_grid_attach (GTK_GRID (grid), plugin_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->plugin_spin,   1, row, 1, 1);
    row++;

    /* separador */
    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top    (sep, 4);
    gtk_widget_set_margin_bottom (sep, 4);
    gtk_grid_attach (GTK_GRID (grid), sep, 0, row, 2, 1);
    row++;

    /* usar tamaño del panel */
    pd->panel_icon_check = gtk_check_button_new_with_label (_("Use panel size"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->panel_icon_check),
                                  np->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid), pd->panel_icon_check, 0, row, 2, 1);
    row++;

    /* tamaño personalizado del ícono */
    GtkWidget *icon_size_label = gtk_label_new (_("Icon size:"));
    gtk_label_set_xalign (GTK_LABEL (icon_size_label), 0.0);
    gtk_widget_set_margin_start (icon_size_label, 12);
    pd->icon_spin = gtk_spin_button_new_with_range (8, 64, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->icon_spin), np->icon_size);
    gtk_widget_set_sensitive (pd->icon_spin, !np->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid), icon_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->icon_spin,   1, row, 1, 1);
    row++;

    /* separador */
    GtkWidget *sep2 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top    (sep2, 4);
    gtk_widget_set_margin_bottom (sep2, 4);
    gtk_grid_attach (GTK_GRID (grid), sep2, 0, row, 2, 1);
    row++;

    /* ancho del popup */
    GtkWidget *popup_width_label = gtk_label_new (_("Popup width:"));
    gtk_label_set_xalign (GTK_LABEL (popup_width_label), 0.0);
    gtk_widget_set_margin_start (popup_width_label, 12);
    pd->popup_width_spin = gtk_spin_button_new_with_range (300, 800, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_width_spin), np->popup_width);
    gtk_grid_attach (GTK_GRID (grid), popup_width_label,    0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->popup_width_spin, 1, row, 1, 1);
    row++;

    /* alto del popup */
    GtkWidget *popup_height_label = gtk_label_new (_("Popup height:"));
    gtk_label_set_xalign (GTK_LABEL (popup_height_label), 0.0);
    gtk_widget_set_margin_start (popup_height_label, 12);
    pd->popup_height_spin = gtk_spin_button_new_with_range (200, 600, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_height_spin), np->popup_height);
    gtk_grid_attach (GTK_GRID (grid), popup_height_label,    0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->popup_height_spin, 1, row, 1, 1);

    gtk_box_pack_start (GTK_BOX (content), frame, FALSE, FALSE, 0);

    /* señales */
    g_signal_connect (pd->square_check,     "toggled",
                      G_CALLBACK (on_square_toggled),      pd);
    g_signal_connect (pd->plugin_spin,      "value-changed",
                      G_CALLBACK (on_plugin_size_changed), pd);
    g_signal_connect (pd->panel_icon_check, "toggled",
                      G_CALLBACK (on_panel_icon_toggled),  pd);
    g_signal_connect (pd->icon_spin,         "value-changed",
                      G_CALLBACK (on_icon_size_changed),    pd);
    g_signal_connect (pd->popup_width_spin,  "value-changed",
                      G_CALLBACK (on_popup_width_changed),  pd);
    g_signal_connect (pd->popup_height_spin, "value-changed",
                      G_CALLBACK (on_popup_height_changed), pd);
    g_signal_connect (dialog, "destroy",
                      G_CALLBACK (on_props_dialog_destroy), pd);

    xfce_panel_plugin_block_menu (plugin);
    g_signal_connect_swapped (dialog, "response",
                              G_CALLBACK (gtk_widget_destroy), dialog);
    g_signal_connect_swapped (dialog, "destroy",
                              G_CALLBACK (xfce_panel_plugin_unblock_menu), plugin);

    gtk_widget_show_all (dialog);
}

/* ---------- control del spinner del panel ---------- */

void
net_plugin_set_connecting (gpointer np_ptr, gboolean connecting)
{
    NetPlugin *np = np_ptr;
    np->connecting = connecting;
    if (connecting) {
        gtk_spinner_start (GTK_SPINNER (np->spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (np->stack), "spinner");
    } else {
        gtk_spinner_stop (GTK_SPINNER (np->spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (np->stack), "icon");
    }
}

/* ---------- callback de refresco por señales DBus ---------- */

static void
on_nm_changed (gpointer user_data)
{
    NetPlugin *np = user_data;

    gboolean connected = FALSE;
    gint     strength  = 0;
    gboolean secure    = FALSE;

    GSList *devices = nm_get_wifi_devices (np->conn);
    for (GSList *l = devices; l; l = l->next) {
        NmDevice *dev = l->data;
        GSList   *aps = nm_get_access_points (np->conn, dev->object_path);
        for (GSList *a = aps; a; a = a->next) {
            NmAccessPoint *ap = a->data;
            if (ap->active) {
                connected = TRUE;
                strength  = ap->strength;
                secure    = ap->secure;
            }
        }
        nm_ap_list_free (aps);
    }
    nm_device_list_free (devices);

    gboolean vpn   = nm_get_vpn_active (np->conn);
    gboolean wired = FALSE;
    if (!connected) {
        GSList *eths = nm_get_ethernet_devices (np->conn);
        wired = (eths != NULL);
        nm_device_list_free (eths);
    }

    update_panel_icon (np, connected, strength, secure, vpn, wired);

    /* Tooltip del botón */
    {
        GString *tip = g_string_new (NULL);

        if (!connected) {
            g_string_append (tip, _("Not connected"));
        } else {
            GSList *devs2 = nm_get_wifi_devices (np->conn);
            for (GSList *l = devs2; l; l = l->next) {
                NmDevice *dev = l->data;
                GSList   *aps = nm_get_access_points (np->conn, dev->object_path);
                for (GSList *a = aps; a; a = a->next) {
                    NmAccessPoint *ap = a->data;
                    if (ap->active) {
                        const gchar *band;
                        if (ap->frequency >= 5925)      band = "6G";
                        else if (ap->frequency >= 5000) band = "5G";
                        else                            band = "2.4G";
                        g_string_append_printf (tip, _("Connected to %s (%s)"),
                                                ap->ssid, band);
                    }
                }
                nm_ap_list_free (aps);
            }
            nm_device_list_free (devs2);
        }

        if (vpn) {
            if (tip->len > 0)
                g_string_append (tip, _(" · VPN active"));
            else
                g_string_append (tip, _("VPN active"));
        }

        gtk_widget_set_tooltip_text (np->button, tip->str);
        g_string_free (tip, TRUE);
    }

    /* Si el popup está abierto y no hay expand activo, reconstruirlo */
    if (gtk_widget_get_visible (np->popup->window) &&
        np->popup->current_expand_box == NULL) {
        GSList *devs = nm_get_wifi_devices (np->conn);
        popup_show (np->popup, np->plugin, np->button, devs, np->conn,
                    np->popup_width, np->popup_height);
        nm_device_list_free (devs);
    }
}

/* ---------- callbacks del panel ---------- */

static void
on_button_toggled (GtkToggleButton *btn, NetPlugin *np)
{
    (void) btn;
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (np->button))) {
        GSList *devices = nm_get_wifi_devices (np->conn);
        popup_show (np->popup, np->plugin, np->button, devices, np->conn,
                    np->popup_width, np->popup_height);
        nm_device_list_free (devices);
    } else {
        popup_hide (np->popup);
    }
}

static void
on_popup_hidden (GtkWidget *win, NetPlugin *np)
{
    (void) win;
    g_signal_handlers_block_by_func (np->button, on_button_toggled, np);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (np->button), FALSE);
    g_signal_handlers_unblock_by_func (np->button, on_button_toggled, np);
}

/* ---------- construcción del plugin ---------- */

static gboolean
on_size_changed (XfcePanelPlugin *plugin, gint size, NetPlugin *np)
{
    (void) plugin;
    (void) size;
    apply_config (np);
    return TRUE;
}

static NetPlugin *
net_plugin_new (XfcePanelPlugin *plugin)
{
    NetPlugin *np = g_new0 (NetPlugin, 1);
    np->plugin    = plugin;
    np->conn      = nm_dbus_connect ();

    /* Valores iniciales del último estado */
    np->last_strength  = 0;
    np->last_secure    = FALSE;
    np->last_connected = FALSE;
    np->last_vpn       = FALSE;
    np->last_wired     = FALSE;

    load_config (np);

    np->button = gtk_toggle_button_new ();
    gtk_button_set_relief (GTK_BUTTON (np->button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (np->button, _("Networks"));
    xfce_panel_plugin_set_shrink (plugin, TRUE);
    xfce_panel_plugin_set_small  (plugin, TRUE);

    /* Contenedor del ícono — se reconstruye en update_panel_icon */
    np->icon_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    /* Ícono inicial: desconectado */
    GtkWidget *init_icon = gtk_image_new_from_icon_name (
                               "network-wireless-disconnected-symbolic",
                               GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size (GTK_IMAGE (init_icon), np->icon_size);
    gtk_box_pack_start (GTK_BOX (np->icon_box), init_icon, TRUE, TRUE, 0);

    np->spinner = gtk_spinner_new ();
    np->stack   = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (np->stack), np->icon_box, "icon");
    gtk_stack_add_named (GTK_STACK (np->stack), np->spinner,  "spinner");
    gtk_stack_set_visible_child_name (GTK_STACK (np->stack), "icon");
    gtk_container_add (GTK_CONTAINER (np->button), np->stack);
    gtk_widget_show_all (np->button);

    np->popup = popup_create (plugin, np->button);

    g_signal_connect (plugin, "size-changed",
                      G_CALLBACK (on_size_changed), np);
    g_signal_connect (np->button, "toggled",
                      G_CALLBACK (on_button_toggled), np);

    np->signal_ids = nm_subscribe_signals (np->conn, on_nm_changed, np);
    g_signal_connect (np->popup->window, "hide",
                      G_CALLBACK (on_popup_hidden), np);

    xfce_panel_plugin_add_action_widget (plugin, np->button);
    gtk_container_add (GTK_CONTAINER (plugin), np->button);

    return np;
}

static void
net_plugin_free (XfcePanelPlugin *plugin, NetPlugin *np)
{
    (void) plugin;
    if (np->signal_ids)
        nm_unsubscribe_signals (np->conn, np->signal_ids);
    popup_destroy (np->popup);
    if (np->conn)
        g_object_unref (np->conn);
    if (np->button_css)
        g_object_unref (np->button_css);
    g_free (np);
}

static void
net_plugin_construct (XfcePanelPlugin *plugin)
{
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    NetPlugin *np = net_plugin_new (plugin);
    xfce_panel_plugin_menu_show_configure (plugin);
    g_signal_connect (plugin, "free-data",
                      G_CALLBACK (net_plugin_free),     np);
    g_signal_connect (plugin, "configure-plugin",
                      G_CALLBACK (on_configure_plugin),  np);
    g_signal_connect (plugin, "size-changed",
                      G_CALLBACK (on_size_changed),       np);
}

XFCE_PANEL_PLUGIN_REGISTER (net_plugin_construct);
