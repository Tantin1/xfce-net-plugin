#include <libxfce4panel/libxfce4panel.h>
#include <gtk/gtk.h>
/* gi18n-lib: _() busca siempre en NUESTRO dominio de traducción
 * (GETTEXT_PACKAGE). En Wayland todos los plugins corren dentro del
 * proceso del panel y comparten el dominio "por defecto": con gi18n.h
 * nuestros textos se buscaban en el de otro y salían en inglés. */
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <xfconf/xfconf.h>
#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif
#include "popup.h"
#include "nm-dbus.h"
#include "secret-agent.h"
#include "attempts.h"

#define DEFAULT_SQUARE_BUTTON   FALSE
#define DEFAULT_PLUGIN_SIZE     24
#define DEFAULT_USE_PANEL_ICON  FALSE
#define DEFAULT_ICON_SIZE       16
#define DEFAULT_POPUP_WIDTH     400
#define DEFAULT_POPUP_HEIGHT    340
#define DEFAULT_SHOW_SEPARATORS    TRUE
#define DEFAULT_SHOW_NOTIFICATIONS FALSE
#define DEFAULT_SHOW_FORGET_ACTIVE  FALSE
#define DEFAULT_SECRET_AGENT        TRUE
#define DEFAULT_REMEMBER_DISABLED   FALSE
#define DEFAULT_PREFER_NM_ICONS     FALSE
#define DEFAULT_POPUP_STYLE         NET_POPUP_STYLE_AUTO

/* Declaración forward: el cuerpo vive en popup.c y arma el candado con
 * fallback nm-vpn-active-lock → nm-secure-lock → recurso embebido en el .so.
 * Usa verificación robusta (lookup + g_file_test) para no caer en enlaces
 * simbólicos rotos como los de RedmondX/RedmondX-Light. */
GtkWidget *make_lock_overlay_image (gint icon_size);

/* Recurso embebido generado por glib-compile-resources con c_name=lock_icons.
 * Lo registramos en net_plugin_construct para que gtk_image_new_from_resource
 * pueda cargar el SVG del candado sin depender de archivos del sistema. */
GResource *lock_icons_get_resource (void);

/* Versión: la define el sistema de compilación desde el archivo VERSION. */
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "dev"
#endif


typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *button;
    GtkWidget       *icon_box;
    GtkWidget       *stack;
    GtkWidget       *spinner;
    NetPopup        *popup;
    GDBusConnection *conn;

    gboolean        square_button;
    gint            plugin_size;
    gboolean        use_panel_icon;
    gint            icon_size;
    gint            popup_width;
    gint            popup_height;
    gboolean        show_separators;

    GtkCssProvider *button_css;

    /* Configuración en Xfconf, propia de ESTA copia del plugin (bajo
     * /plugins/plugin-N del canal xfce4-panel). NULL si Xfconf no respondió. */
    XfconfChannel  *channel;

    guint *signal_ids;
    guint  refresh_timeout_id;   /* refresco agendado del ícono (100 ms) */

    gboolean        connecting;
    gboolean        notif_ready;
    guint           attempts_listener;   /* avisos del seguidor de intentos */
#ifdef HAVE_LIBNOTIFY
    /* Una sola notificación, que se reemplaza en el lugar: los avisos
     * nuevos no se apilan. NULL hasta el primer aviso. */
    NotifyNotification *notif;
#endif
    gboolean        show_notifications;
    gboolean        show_forget_active;
    gboolean        prefer_nm_icons;   /* íconos de nm-applet antes que los estándar */
    gint            popup_style;       /* NET_POPUP_STYLE_*: automático, flotante o pegada */

    /* Agente de secretos: responde cuando NM pide una contraseña.
     * NULL si está apagado en Propiedades o no se pudo publicar. */
    gboolean        secret_agent_enabled;
    NetSecretAgent *agent;

    /* Adaptadores apagados a mano: si remember_disabled está activo, se
     * guardan (por nombre de interfaz) y se vuelven a apagar al iniciar. */
    gboolean        remember_disabled;
    GHashTable     *disabled_ifaces;   /* conjunto de nombres de interfaz */

    gint     last_strength;
    gboolean last_secure;
    gboolean last_connected;
    gboolean last_vpn;
    gboolean last_wired;
    gchar   *last_ssid;     /* red Wi-Fi del último refresco (para avisar cambios) */
    gboolean last_portal;   /* el último refresco vio un portal cautivo */
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

    np->last_strength  = strength;
    np->last_secure    = secure;
    np->last_connected = connected;
    np->last_vpn       = vpn;
    np->last_wired     = wired;

    GList *kids = gtk_container_get_children (GTK_CONTAINER (np->icon_box));
    for (GList *w = kids; w; w = w->next) gtk_widget_destroy (GTK_WIDGET (w->data));
    g_list_free (kids);

    GtkWidget *new_icon;

    if (!connected) {
        if (wired) {
            if (vpn) {
                GtkWidget *overlay  = gtk_overlay_new ();
                GIcon     *wired_gicon = net_themed_icon_new ("network-wired-symbolic", "nm-device-wired");
                GtkWidget *base_img    = gtk_image_new_from_gicon (wired_gicon, GTK_ICON_SIZE_BUTTON);
                g_object_unref (wired_gicon);
                gtk_image_set_pixel_size (GTK_IMAGE (base_img), icon_px);

                /* Candado de VPN activa con fallback robusto. */
                GtkWidget *lock_img = make_lock_overlay_image (icon_px);

                gtk_container_add (GTK_CONTAINER (overlay), base_img);
                gtk_overlay_add_overlay (GTK_OVERLAY (overlay), lock_img);
                gtk_widget_set_size_request (overlay, icon_px, icon_px);
                gtk_widget_show_all (overlay);
                new_icon = overlay;
            } else {
                {
                    GIcon *wired_gicon = net_themed_icon_new ("network-wired-symbolic", "nm-device-wired");
                    new_icon = gtk_image_new_from_gicon (wired_gicon, GTK_ICON_SIZE_BUTTON);
                    g_object_unref (wired_gicon);
                }
                gtk_image_set_pixel_size (GTK_IMAGE (new_icon), icon_px);
            }
        } else {
            {
                GIcon *disc_gicon = net_themed_icon_new ("network-wireless-disconnected-symbolic", "nm-no-connection");
                new_icon = gtk_image_new_from_gicon (disc_gicon, GTK_ICON_SIZE_BUTTON);
                g_object_unref (disc_gicon);
            }
            gtk_image_set_pixel_size (GTK_IMAGE (new_icon), icon_px);
        }
    } else {
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

    {
        gchar *css;
        /* Lado de UNA fila: con el panel en varias filas, el tamaño del
         * panel es el de todas juntas. El plugin está marcado como "chico"
         * (ocupa una sola fila), así que el botón cuadrado mide una fila. */
        gint   rows = (gint) xfce_panel_plugin_get_nrows (plugin);
        gint   ps   = xfce_panel_plugin_get_size (plugin) / MAX (rows, 1);

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

    update_panel_icon (np,
                       np->last_connected, np->last_strength,
                       np->last_secure, np->last_vpn, np->last_wired);
}

/* ---------- leer / guardar configuración ----------
 *
 * Igual que los plugins nativos de Xfce: la configuración vive en Xfconf,
 * canal "xfce4-panel", bajo la ruta propia de cada copia del plugin
 * (/plugins/plugin-N, la da xfce_panel_plugin_get_property_base). Así:
 *   - cada copia (por ejemplo, una en cada panel) tiene la suya;
 *   - al quitar el plugin, el PANEL borra esa ruta entera, y si se lo vuelve
 *     a agregar arranca con los valores por defecto. El plugin no tiene que
 *     hacer nada para eso.
 *
 * Antes (hasta nov-2026) todo iba a ~/.config/xfce4/panel/xfce-net-plugin.ini,
 * compartido por todas las copias y que sobrevivía a quitar el plugin. La
 * primera copia que arranca sin configuración en Xfconf lo importa una vez y
 * lo borra (si quedara, cada copia nueva volvería a leerlo). */

#define CONFIG_PATH "xfce4/panel/xfce-net-plugin.ini"   /* solo para importar */

static gchar *
get_config_path (void)
{
    return g_build_filename (g_get_user_config_dir (), CONFIG_PATH, NULL);
}

static void save_config (NetPlugin *np);

static void
config_defaults (NetPlugin *np)
{
    np->square_button  = DEFAULT_SQUARE_BUTTON;
    np->plugin_size    = DEFAULT_PLUGIN_SIZE;
    np->use_panel_icon = DEFAULT_USE_PANEL_ICON;
    np->icon_size      = DEFAULT_ICON_SIZE;
    np->popup_width    = DEFAULT_POPUP_WIDTH;
    np->popup_height   = DEFAULT_POPUP_HEIGHT;
    np->show_separators    = DEFAULT_SHOW_SEPARATORS;
    np->show_notifications = DEFAULT_SHOW_NOTIFICATIONS;
    np->show_forget_active = DEFAULT_SHOW_FORGET_ACTIVE;
    np->secret_agent_enabled = DEFAULT_SECRET_AGENT;
    np->remember_disabled    = DEFAULT_REMEMBER_DISABLED;
    np->prefer_nm_icons      = DEFAULT_PREFER_NM_ICONS;
    np->popup_style          = DEFAULT_POPUP_STYLE;
    if (!np->disabled_ifaces)
        np->disabled_ifaces = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, NULL);
    g_hash_table_remove_all (np->disabled_ifaces);
}

/* Valores fuera de rango (configuración editada a mano o de una versión
 * futura) vuelven a los de por defecto. */
static void
config_validate (NetPlugin *np)
{
    if (np->popup_style < NET_POPUP_STYLE_AUTO || np->popup_style > NET_POPUP_STYLE_ATTACHED)
        np->popup_style = DEFAULT_POPUP_STYLE;
    if (np->plugin_size  <= 0) np->plugin_size  = DEFAULT_PLUGIN_SIZE;
    if (np->icon_size    <= 0) np->icon_size    = DEFAULT_ICON_SIZE;
    if (np->popup_width  <= 0) np->popup_width  = DEFAULT_POPUP_WIDTH;
    if (np->popup_height <= 0) np->popup_height = DEFAULT_POPUP_HEIGHT;
}

/* Lee el .ini viejo (solo para importarlo). FALSE si no existe. */
static gboolean
load_config_ini (NetPlugin *np, const gchar *path)
{
    GKeyFile *kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free (kf);
        return FALSE;
    }

#define INI_BOOL(grp, key, field) \
    if (g_key_file_has_key (kf, grp, key, NULL)) \
        np->field = g_key_file_get_boolean (kf, grp, key, NULL)
#define INI_INT(grp, key, field) \
    if (g_key_file_has_key (kf, grp, key, NULL)) \
        np->field = g_key_file_get_integer (kf, grp, key, NULL)

    INI_BOOL ("appearance", "square_button",      square_button);
    INI_INT  ("appearance", "plugin_size",        plugin_size);
    INI_BOOL ("appearance", "use_panel_icon",     use_panel_icon);
    INI_INT  ("appearance", "icon_size",          icon_size);
    INI_INT  ("appearance", "popup_width",        popup_width);
    INI_INT  ("appearance", "popup_height",       popup_height);
    INI_BOOL ("appearance", "show_separators",    show_separators);
    INI_BOOL ("appearance", "show_notifications", show_notifications);
    INI_BOOL ("appearance", "show_forget_active", show_forget_active);
    INI_BOOL ("appearance", "prefer_nm_icons",    prefer_nm_icons);
    INI_INT  ("appearance", "popup_style",        popup_style);
    INI_BOOL ("network",    "secret_agent",       secret_agent_enabled);
    INI_BOOL ("network",    "remember_disabled",  remember_disabled);
#undef INI_BOOL
#undef INI_INT

    gsize   n = 0;
    gchar **names = g_key_file_get_string_list (kf, "network", "disabled_devices",
                                                &n, NULL);
    for (gsize i = 0; names && i < n; i++)
        if (names[i] && *names[i])
            g_hash_table_add (np->disabled_ifaces, g_strdup (names[i]));
    g_strfreev (names);

    g_key_file_free (kf);
    config_validate (np);
    return TRUE;
}

static void
load_config_xfconf (NetPlugin *np)
{
    XfconfChannel *ch = np->channel;

    np->square_button   = xfconf_channel_get_bool (ch, "/square-button",   DEFAULT_SQUARE_BUTTON);
    np->plugin_size     = xfconf_channel_get_int  (ch, "/plugin-size",     DEFAULT_PLUGIN_SIZE);
    np->use_panel_icon  = xfconf_channel_get_bool (ch, "/use-panel-size",  DEFAULT_USE_PANEL_ICON);
    np->icon_size       = xfconf_channel_get_int  (ch, "/icon-size",       DEFAULT_ICON_SIZE);
    np->popup_width     = xfconf_channel_get_int  (ch, "/popup-width",     DEFAULT_POPUP_WIDTH);
    np->popup_height    = xfconf_channel_get_int  (ch, "/popup-height",    DEFAULT_POPUP_HEIGHT);
    np->show_separators = xfconf_channel_get_bool (ch, "/show-separators", DEFAULT_SHOW_SEPARATORS);
    np->show_notifications = xfconf_channel_get_bool (ch, "/show-notifications",
                                                      DEFAULT_SHOW_NOTIFICATIONS);
    np->show_forget_active = xfconf_channel_get_bool (ch, "/show-forget-active",
                                                      DEFAULT_SHOW_FORGET_ACTIVE);
    np->prefer_nm_icons = xfconf_channel_get_bool (ch, "/prefer-nm-icons", DEFAULT_PREFER_NM_ICONS);
    np->popup_style     = xfconf_channel_get_int  (ch, "/popup-style",     DEFAULT_POPUP_STYLE);
    np->secret_agent_enabled = xfconf_channel_get_bool (ch, "/secret-agent", DEFAULT_SECRET_AGENT);
    np->remember_disabled    = xfconf_channel_get_bool (ch, "/remember-disabled",
                                                        DEFAULT_REMEMBER_DISABLED);

    gchar **names = xfconf_channel_get_string_list (ch, "/disabled-devices");
    for (gchar **p = names; p && *p; p++)
        if (**p)
            g_hash_table_add (np->disabled_ifaces, g_strdup (*p));
    g_strfreev (names);

    config_validate (np);
}

static void
load_config (NetPlugin *np)
{
    config_defaults (np);

    gchar *path = get_config_path ();
    if (!np->channel) {
        /* Xfconf no respondió: usar al menos lo guardado antes, sin borrarlo.
         * Los cambios no se guardan hasta que Xfconf vuelva. */
        load_config_ini (np, path);
    } else if (xfconf_channel_has_property (np->channel, "/plugin-size")) {
        load_config_xfconf (np);
    } else if (load_config_ini (np, path)) {
        /* Primera vez con Xfconf: importar el .ini viejo y borrarlo, solo si
         * quedó guardado de verdad. */
        save_config (np);
        if (xfconf_channel_has_property (np->channel, "/plugin-size"))
            g_unlink (path);
    }
    g_free (path);
}

static void
save_config (NetPlugin *np)
{
    XfconfChannel *ch = np->channel;
    if (!ch)
        return;

    xfconf_channel_set_bool (ch, "/square-button",      np->square_button);
    xfconf_channel_set_int  (ch, "/plugin-size",        np->plugin_size);
    xfconf_channel_set_bool (ch, "/use-panel-size",     np->use_panel_icon);
    xfconf_channel_set_int  (ch, "/icon-size",          np->icon_size);
    xfconf_channel_set_int  (ch, "/popup-width",        np->popup_width);
    xfconf_channel_set_int  (ch, "/popup-height",       np->popup_height);
    xfconf_channel_set_bool (ch, "/show-separators",    np->show_separators);
    xfconf_channel_set_bool (ch, "/show-notifications", np->show_notifications);
    xfconf_channel_set_bool (ch, "/show-forget-active", np->show_forget_active);
    xfconf_channel_set_bool (ch, "/prefer-nm-icons",    np->prefer_nm_icons);
    xfconf_channel_set_int  (ch, "/popup-style",        np->popup_style);
    xfconf_channel_set_bool (ch, "/secret-agent",       np->secret_agent_enabled);
    xfconf_channel_set_bool (ch, "/remember-disabled",  np->remember_disabled);

    /* Xfconf no guarda listas vacías: sin adaptadores, se borra la clave. */
    if (np->disabled_ifaces && g_hash_table_size (np->disabled_ifaces) > 0) {
        GPtrArray     *arr = g_ptr_array_new ();
        GHashTableIter it;
        gpointer       key;
        g_hash_table_iter_init (&it, np->disabled_ifaces);
        while (g_hash_table_iter_next (&it, &key, NULL))
            g_ptr_array_add (arr, key);
        g_ptr_array_add (arr, NULL);
        xfconf_channel_set_string_list (ch, "/disabled-devices",
                                        (const gchar * const *) arr->pdata);
        g_ptr_array_free (arr, TRUE);
    } else {
        xfconf_channel_reset_property (ch, "/disabled-devices", FALSE);
    }
}

/* ---------- ventana de propiedades ---------- */

typedef struct {
    NetPlugin   *np;
    GtkWidget   *square_check;
    GtkWidget   *forget_active_check;
    GtkWidget   *nm_icons_check;
    GtkWidget   *style_combo;
    GtkWidget   *plugin_spin;
    GtkWidget   *panel_icon_check;
    GtkWidget   *icon_spin;
    GtkWidget   *popup_width_spin;
    GtkWidget   *popup_height_spin;
    GtkWidget   *separators_check;
    GtkWidget   *notifications_check;
    GtkWidget   *agent_check;
    GtkWidget   *remember_check;
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
on_separators_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->np->show_separators = gtk_toggle_button_get_active (btn);
    save_config (pd->np);
    /* Forzar reconstrucción del popup la próxima vez que se abra */
    pd->np->popup->ui_built = FALSE;
}

#ifdef HAVE_LIBNOTIFY
static void
on_notifications_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->np->show_notifications = gtk_toggle_button_get_active (btn);
    save_config (pd->np);
}
#endif

static void
on_forget_active_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->np->show_forget_active = gtk_toggle_button_get_active (btn);
    save_config (pd->np);
    pd->np->popup->ui_built = FALSE;
}

/* El popup toma el estilo nuevo en la próxima apertura. */
static void
on_popup_style_changed (GtkComboBox *combo, PropsDialog *pd)
{
    gint v = gtk_combo_box_get_active (combo);
    if (v < NET_POPUP_STYLE_AUTO || v > NET_POPUP_STYLE_ATTACHED)
        return;
    pd->np->popup_style = v;
    save_config (pd->np);
}

static void
on_nm_icons_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->np->prefer_nm_icons = gtk_toggle_button_get_active (btn);
    popup_set_prefer_nm_icons (pd->np->prefer_nm_icons);
    save_config (pd->np);
    apply_config (pd->np);              /* redibuja el ícono del panel */
    pd->np->popup->ui_built = FALSE;    /* el popup se rearma al abrirse */
}

/* ---------- agente de secretos ---------- */

/* Antes de mostrar el diálogo de contraseña se cierra el popup: tiene
 * capturado el teclado y el diálogo no recibiría lo que se tipea. */
static void
on_agent_prompt (gpointer user_data)
{
    NetPlugin *np = user_data;
    if (np->popup && gtk_widget_get_visible (np->popup->window))
        popup_hide (np->popup);
}

/* Prende o apaga el agente según la preferencia. */
static void
secret_agent_apply (NetPlugin *np)
{
    if (np->secret_agent_enabled && !np->agent && np->conn)
        np->agent = net_secret_agent_new (np->conn, on_agent_prompt, np);
    else if (!np->secret_agent_enabled && np->agent) {
        net_secret_agent_free (np->agent);
        np->agent = NULL;
    }
}

/* ---------- recordar adaptadores apagados ---------- */

/* Cada vez que se arma una lista de adaptadores hay que liberar los dos
 * tipos; esto junta Wi-Fi y Ethernet en una sola lista de NmDevice. */
static GSList *
all_switchable_devices (GDBusConnection *conn)
{
    GSList *wifi = nm_get_wifi_devices (conn);
    GSList *eth  = nm_get_ethernet_devices (conn);
    return g_slist_concat (wifi, eth);
}

/* Cómo se identifica un adaptador en la lista de apagados.
 *
 * El nombre (wlan0, wlan1…) lo reparte el núcleo según el orden en que
 * encuentra los adaptadores al arrancar, y con adaptadores USB ese orden
 * puede cambiar entre reinicios: el plugin terminaba apagando el adaptador
 * equivocado. Por eso se guarda la dirección física de fábrica, que no
 * cambia. Solo si el adaptador no informa una se usa el nombre, como antes.
 * Una dirección física siempre tiene ':' y un nombre de interfaz no, así se
 * distinguen al leer la configuración. */
static const gchar *
device_key (NmDevice *dev)
{
    return dev->perm_hw ? dev->perm_hw : dev->iface;
}

/* ¿Este adaptador está en la lista de apagados? Si figura con el nombre
 * viejo y ahora tiene dirección física, se pasa la entrada a la dirección
 * (migración de configuraciones anteriores). *migrated queda en TRUE si
 * hubo que guardar el cambio. */
static gboolean
device_is_listed (NetPlugin *np, NmDevice *dev, gboolean *migrated)
{
    if (g_hash_table_contains (np->disabled_ifaces, device_key (dev)))
        return TRUE;
    if (dev->perm_hw && g_hash_table_contains (np->disabled_ifaces, dev->iface)) {
        g_hash_table_remove (np->disabled_ifaces, dev->iface);
        g_hash_table_add (np->disabled_ifaces, g_strdup (dev->perm_hw));
        if (migrated)
            *migrated = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* Vuelve a apagar los adaptadores guardados que NetworkManager haya
 * vuelto a gestionar (por ejemplo, tras reiniciar la PC). */
static void
disabled_devices_apply (NetPlugin *np)
{
    if (!np->remember_disabled || !np->conn || !np->disabled_ifaces ||
        g_hash_table_size (np->disabled_ifaces) == 0)
        return;

    gboolean migrated = FALSE;
    GSList *devs = all_switchable_devices (np->conn);
    for (GSList *l = devs; l; l = l->next) {
        NmDevice *dev = l->data;
        if (!device_is_listed (np, dev, &migrated))
            continue;
        guint32 state = nm_get_device_state (np->conn, dev->object_path);
        /* 0 = desconocido, 10 = ya está apagado (no gestionado) */
        if (state == 0 || state == 10)
            continue;
        nm_set_device_enabled_async (np->conn, dev->object_path, FALSE);
    }
    nm_device_list_free (devs);
    if (migrated)
        save_config (np);
}

/* Al activar la opción: guarda como "apagados" los adaptadores que lo estén ahora. */
static void
disabled_devices_snapshot (NetPlugin *np)
{
    if (!np->conn)
        return;
    g_hash_table_remove_all (np->disabled_ifaces);
    GSList *devs = all_switchable_devices (np->conn);
    for (GSList *l = devs; l; l = l->next) {
        NmDevice *dev = l->data;
        if (nm_get_device_state (np->conn, dev->object_path) == 10)
            g_hash_table_add (np->disabled_ifaces, g_strdup (device_key (dev)));
    }
    nm_device_list_free (devs);
}

/* El popup avisa con el nombre de la interfaz (dentro de una misma sesión el
 * nombre no cambia); acá se busca el adaptador para guardar su dirección. */
void
net_plugin_device_toggled (gpointer np_ptr, const gchar *iface, gboolean enabled)
{
    NetPlugin *np = np_ptr;
    if (!np || !iface || !np->disabled_ifaces)
        return;

    gchar  *hw   = NULL;
    GSList *devs = np->conn ? all_switchable_devices (np->conn) : NULL;
    for (GSList *l = devs; l; l = l->next) {
        NmDevice *dev = l->data;
        if (g_strcmp0 (dev->iface, iface) == 0) {
            hw = g_strdup (dev->perm_hw);
            break;
        }
    }
    nm_device_list_free (devs);

    if (enabled) {
        /* Borrar las dos formas: la dirección y un nombre viejo que quedara. */
        if (hw)
            g_hash_table_remove (np->disabled_ifaces, hw);
        g_hash_table_remove (np->disabled_ifaces, iface);
    } else {
        g_hash_table_remove (np->disabled_ifaces, iface);
        g_hash_table_add (np->disabled_ifaces, g_strdup (hw ? hw : iface));
    }
    g_free (hw);
    save_config (np);
}

static void
on_remember_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->np->remember_disabled = gtk_toggle_button_get_active (btn);
    if (pd->np->remember_disabled)
        disabled_devices_snapshot (pd->np);
    save_config (pd->np);
}

static void
on_agent_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->np->secret_agent_enabled = gtk_toggle_button_get_active (btn);
    save_config (pd->np);
    secret_agent_apply (pd->np);
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

    /* SIN padre (NULL) a propósito: en Wayland el panel es una capa
     * (layer-shell) y, con él como padre, GTK ubica mal las ventanitas del
     * diálogo (la lista desplegable salía en una esquina de la pantalla).
     * No "arreglarlo" poniéndole el panel como padre. */
    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Properties — Network"),
        NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        _("Close"), GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 12);

    /* Dos pestañas (Apariencia y Red) en vez de dos marcos apilados: la
     * ventana queda más baja y entra en pantallas chicas. */
    GtkWidget *notebook = gtk_notebook_new ();

    GtkWidget *grid  = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_container_set_border_width (GTK_CONTAINER (grid), 12);

    gint row = 0;

    pd->square_check = gtk_check_button_new_with_label (_("Square button"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->square_check),
                                  np->square_button);
    gtk_grid_attach (GTK_GRID (grid), pd->square_check, 0, row, 2, 1);
    row++;

    GtkWidget *plugin_size_label = gtk_label_new (_("Plugin size:"));
    gtk_label_set_xalign (GTK_LABEL (plugin_size_label), 0.0);
    gtk_widget_set_margin_start (plugin_size_label, 12);
    pd->plugin_spin = gtk_spin_button_new_with_range (16, 200, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->plugin_spin), np->plugin_size);
    gtk_widget_set_sensitive (pd->plugin_spin, !np->square_button);
    gtk_grid_attach (GTK_GRID (grid), plugin_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->plugin_spin,   1, row, 1, 1);
    row++;

    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top    (sep, 4);
    gtk_widget_set_margin_bottom (sep, 4);
    gtk_grid_attach (GTK_GRID (grid), sep, 0, row, 2, 1);
    row++;

    pd->panel_icon_check = gtk_check_button_new_with_label (_("Use panel size"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->panel_icon_check),
                                  np->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid), pd->panel_icon_check, 0, row, 2, 1);
    row++;

    GtkWidget *icon_size_label = gtk_label_new (_("Icon size:"));
    gtk_label_set_xalign (GTK_LABEL (icon_size_label), 0.0);
    gtk_widget_set_margin_start (icon_size_label, 12);
    pd->icon_spin = gtk_spin_button_new_with_range (8, 64, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->icon_spin), np->icon_size);
    gtk_widget_set_sensitive (pd->icon_spin, !np->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid), icon_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->icon_spin,   1, row, 1, 1);
    row++;

    GtkWidget *sep2 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top    (sep2, 4);
    gtk_widget_set_margin_bottom (sep2, 4);
    gtk_grid_attach (GTK_GRID (grid), sep2, 0, row, 2, 1);
    row++;

    GtkWidget *popup_width_label = gtk_label_new (_("Popup width:"));
    gtk_label_set_xalign (GTK_LABEL (popup_width_label), 0.0);
    gtk_widget_set_margin_start (popup_width_label, 12);
    pd->popup_width_spin = gtk_spin_button_new_with_range (300, 800, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_width_spin), np->popup_width);
    gtk_grid_attach (GTK_GRID (grid), popup_width_label,    0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->popup_width_spin, 1, row, 1, 1);
    row++;

    GtkWidget *popup_height_label = gtk_label_new (_("Popup height:"));
    gtk_label_set_xalign (GTK_LABEL (popup_height_label), 0.0);
    gtk_widget_set_margin_start (popup_height_label, 12);
    pd->popup_height_spin = gtk_spin_button_new_with_range (200, 600, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_height_spin), np->popup_height);
    gtk_grid_attach (GTK_GRID (grid), popup_height_label,    0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->popup_height_spin, 1, row, 1, 1);
    row++;

    GtkWidget *sep3 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top    (sep3, 4);
    gtk_widget_set_margin_bottom (sep3, 4);
    gtk_grid_attach (GTK_GRID (grid), sep3, 0, row, 2, 1);
    row++;

    pd->separators_check = gtk_check_button_new_with_label (_("Show separators"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->separators_check),
                                  np->show_separators);
    gtk_grid_attach (GTK_GRID (grid), pd->separators_check, 0, row, 2, 1);
    row++;

    pd->forget_active_check = gtk_check_button_new_with_label (_("Show Forget button on active networks"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->forget_active_check),
                                  np->show_forget_active);
    gtk_grid_attach (GTK_GRID (grid), pd->forget_active_check, 0, row, 2, 1);
    row++;

    pd->nm_icons_check = gtk_check_button_new_with_label (_("Prefer nm-applet icons"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->nm_icons_check),
                                  np->prefer_nm_icons);
    gtk_widget_set_tooltip_text (pd->nm_icons_check,
        _("Use the icons your theme provides for nm-applet, if any, instead of the standard network icons."));
    gtk_grid_attach (GTK_GRID (grid), pd->nm_icons_check, 0, row, 2, 1);
    row++;

    /* Estilo de la ventana emergente. El orden de las opciones es el de
     * NET_POPUP_STYLE_* (0, 1, 2). */
    GtkWidget *style_label = gtk_label_new (_("Popup style:"));
    gtk_label_set_xalign (GTK_LABEL (style_label), 0.0);
    pd->style_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->style_combo), _("Automatic"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->style_combo), _("Floating"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->style_combo), _("Attached to the panel"));
    gtk_combo_box_set_active (GTK_COMBO_BOX (pd->style_combo), np->popup_style);
    gtk_widget_set_tooltip_text (pd->style_combo,
        _("Automatic: rounded corners and a small gap from the panel when the theme has rounded popovers and a compositor is running; otherwise attached to the panel."));
    gtk_grid_attach (GTK_GRID (grid), style_label,     0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pd->style_combo, 1, row, 1, 1);
    row++;

#ifdef HAVE_LIBNOTIFY
    pd->notifications_check = gtk_check_button_new_with_label (_("Show notifications"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->notifications_check),
                                  np->show_notifications);
    gtk_grid_attach (GTK_GRID (grid), pd->notifications_check, 0, row, 2, 1);
    row++;

#endif

    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), grid,
                              gtk_label_new (_("Appearance")));

    /* Pestaña "Red": comportamiento, no apariencia. */
    GtkWidget *net_grid  = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (net_grid), 6);
    gtk_container_set_border_width (GTK_CONTAINER (net_grid), 12);

    pd->agent_check = gtk_check_button_new_with_label (_("Respond to password requests"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->agent_check),
                                  np->secret_agent_enabled);
    gtk_grid_attach (GTK_GRID (net_grid), pd->agent_check, 0, 0, 1, 1);

    GtkWidget *agent_hint = gtk_label_new (_("Turn this off if another network applet (such as nm-applet) is also running."));
    gtk_label_set_xalign (GTK_LABEL (agent_hint), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (agent_hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (agent_hint), 35);
    gtk_widget_set_margin_start (agent_hint, 24);
    gtk_style_context_add_class (gtk_widget_get_style_context (agent_hint), "dim-label");
    gtk_grid_attach (GTK_GRID (net_grid), agent_hint, 0, 1, 1, 1);

    pd->remember_check = gtk_check_button_new_with_label (_("Remember disabled adapters"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->remember_check),
                                  np->remember_disabled);
    gtk_widget_set_margin_top (pd->remember_check, 6);
    gtk_grid_attach (GTK_GRID (net_grid), pd->remember_check, 0, 2, 1, 1);

    GtkWidget *remember_hint = gtk_label_new (_("Adapters you turn off stay off after restarting."));
    gtk_label_set_xalign (GTK_LABEL (remember_hint), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (remember_hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (remember_hint), 35);
    gtk_widget_set_margin_start (remember_hint, 24);
    gtk_style_context_add_class (gtk_widget_get_style_context (remember_hint), "dim-label");
    gtk_grid_attach (GTK_GRID (net_grid), remember_hint, 0, 3, 1, 1);

    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), net_grid,
                              gtk_label_new (_("Network")));
    gtk_box_pack_start (GTK_BOX (content), notebook, TRUE, TRUE, 0);
    g_signal_connect (pd->agent_check, "toggled",
                      G_CALLBACK (on_agent_toggled), pd);
    g_signal_connect (pd->remember_check, "toggled",
                      G_CALLBACK (on_remember_toggled), pd);

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
    g_signal_connect (pd->separators_check, "toggled",
                      G_CALLBACK (on_separators_toggled), pd);
    g_signal_connect (pd->forget_active_check, "toggled",
                      G_CALLBACK (on_forget_active_toggled), pd);
    g_signal_connect (pd->style_combo, "changed",
                      G_CALLBACK (on_popup_style_changed), pd);
    g_signal_connect (pd->nm_icons_check, "toggled",
                      G_CALLBACK (on_nm_icons_toggled), pd);
#ifdef HAVE_LIBNOTIFY
    g_signal_connect (pd->notifications_check, "toggled",
                      G_CALLBACK (on_notifications_toggled), pd);
#endif
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

static void
net_plugin_set_connecting (NetPlugin *np, gboolean connecting)
{
    np->connecting = connecting;
    if (connecting) {
        gtk_spinner_start (GTK_SPINNER (np->spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (np->stack), "spinner");
    } else {
        gtk_spinner_stop (GTK_SPINNER (np->spinner));
        gtk_stack_set_visible_child_name (GTK_STACK (np->stack), "icon");
    }
}

/* ---------- notificaciones ----------
 *
 * Una sola notificación que se reemplaza en el lugar (antes cada aviso era
 * una nueva y se apilaban). Los cambios que llegan juntos en un mismo
 * refresco van en un solo mensaje, una línea por cambio.
 *
 * Con el popup abierto no se muestra nada: el usuario ya está viendo el
 * estado (y casi siempre lo provocó desde ahí), y así la notificación nunca
 * tapa el popup. No se lo cierra para mostrarla, como hace nm-applet: se
 * cortaría lo que el usuario estuviera haciendo (escribir una contraseña,
 * leer los detalles). */

#ifdef HAVE_LIBNOTIFY
typedef struct {
    const gchar *summary;   /* "Wi-Fi", "Ethernet", "VPN" */
    gchar       *text;      /* "Conectado a Casa" */
    const gchar *icon;
} NotifyEvent;

typedef struct {
    NotifyEvent ev[6];
    gint        n;
} NotifyBatch;

static void
batch_add (NotifyBatch *b, const gchar *summary, const gchar *text,
           const gchar *icon)
{
    if (b->n >= (gint) G_N_ELEMENTS (b->ev))
        return;
    b->ev[b->n].summary = summary;
    b->ev[b->n].text    = g_strdup (text);
    b->ev[b->n].icon    = icon;
    b->n++;
}

static gboolean
popup_is_open (NetPlugin *np)
{
    return np->popup && gtk_widget_get_visible (np->popup->window);
}

static void
notify_show (NetPlugin *np, const gchar *summary, const gchar *body,
             const gchar *icon)
{
    if (!np->show_notifications || popup_is_open (np))
        return;
    if (!np->notif)
        np->notif = notify_notification_new (summary, body, icon);
    else
        notify_notification_update (np->notif, summary, body, icon);
    notify_notification_show (np->notif, NULL);
}

/* Un cambio: su título y su texto. Varios: título general ("Red") y una
 * línea por cambio con su título ("Wi-Fi: Conectado a Casa"). */
static void
batch_send (NetPlugin *np, NotifyBatch *b)
{
    if (b->n == 1) {
        notify_show (np, b->ev[0].summary, b->ev[0].text, b->ev[0].icon);
    } else if (b->n > 1) {
        GString *body = g_string_new (NULL);
        for (gint i = 0; i < b->n; i++)
            g_string_append_printf (body, "%s%s: %s", i ? "\n" : "",
                                    b->ev[i].summary, b->ev[i].text);
        notify_show (np, _("Network"), body->str, b->ev[0].icon);
        g_string_free (body, TRUE);
    }
    for (gint i = 0; i < b->n; i++)
        g_free (b->ev[i].text);
    b->n = 0;
}
#endif

/* ---------- callback de refresco por señales DBus ---------- */



static gboolean
on_nm_changed_cb (gpointer user_data)
{
    NetPlugin *np = user_data;
    np->refresh_timeout_id = 0;
    if (!np->conn)
        return G_SOURCE_REMOVE;

    /* Si hay adaptadores guardados como apagados que NM volvió a encender,
     * se apagan de nuevo (antes del refresco, que ya verá el estado final). */
    disabled_devices_apply (np);

    /* Instantánea: todas las lecturas de este refresco salen de una sola
     * foto del bus en vez de decenas de llamadas sueltas. */
    nm_cache_begin (np->conn);

    /* Una sola lectura del AP activo: ícono, tooltip y notificaciones
     * reusan este resultado sin repetir la enumeración completa. */
    NmActiveApInfo *ai = nm_get_active_ap_info (np->conn);
    gboolean connected = ai->connected;
    gint     strength  = ai->strength;
    gboolean secure    = ai->secure;

    gboolean vpn = nm_get_vpn_active (np->conn);
    /* "Hay cable" = algún Ethernet conectado de verdad (estado 100).
     * Antes se usaba la lista de la sección Ethernet del popup, que también
     * incluye el adaptador apagado y el cable enchufado sin conexión: con
     * Ethernet apagado el panel mostraba el ícono de cable y avisaba "Cable
     * conectado". Además solo se calculaba sin Wi-Fi, así que desenchufar
     * el cable con Wi-Fi activo no avisaba nada. Ahora se calcula siempre;
     * el ícono sigue mostrando Wi-Fi primero (ver update_panel_icon). */
    gboolean wired = nm_any_ethernet_activated (np->conn);
    /* Portal cautivo: hay red, pero hay que iniciar sesión para navegar. */
    gboolean portal = (connected || wired) &&
                      nm_get_connectivity (np->conn) == NM_CONN_STATE_PORTAL;

#ifdef HAVE_LIBNOTIFY
    gboolean prev_connected = np->last_connected;
    gboolean prev_vpn       = np->last_vpn;
    gboolean prev_wired     = np->last_wired;
#endif

    /* Spinner: un intento de conexión pedido desde el plugin (lo sigue
     * attempts.c) o cualquier adaptador conectando por su cuenta (por
     * ejemplo, la conexión automática al arrancar). */
    gboolean any_connecting = attempts_any_connecting () ||
                              nm_any_wifi_device_connecting (np->conn);
    if (any_connecting != np->connecting)
        net_plugin_set_connecting (np, any_connecting);

    update_panel_icon (np, connected, strength, secure, vpn, wired);

#ifdef HAVE_LIBNOTIFY
    {
        NotifyBatch b = { 0 };
        if (np->notif_ready) {
            /* Wi-Fi y Ethernet se evalúan por separado: un cambio de Wi-Fi
             * no tapa uno de cable que ocurra al mismo tiempo. */
            if (connected && (!prev_connected ||
                              (ai->ssid && np->last_ssid &&
                               g_strcmp0 (ai->ssid, np->last_ssid) != 0))) {
                /* Conectado, o pasó de una red a otra sin quedar desconectado. */
                gchar *t = g_strdup_printf (_("Connected to %s"),
                                            ai->ssid ? ai->ssid : "Wi-Fi");
                batch_add (&b, _("Wi-Fi"), t, "network-wireless-symbolic");
                g_free (t);
            } else if (!connected && prev_connected) {
                batch_add (&b, _("Wi-Fi"), _("Disconnected"),
                           "network-wireless-disconnected-symbolic");
            }
            if (wired && !prev_wired)
                batch_add (&b, _("Ethernet"), _("Cable connected"), "network-wired-symbolic");
            else if (!wired && prev_wired)
                batch_add (&b, _("Ethernet"), _("Cable disconnected"),
                           "network-wired-disconnected-symbolic");
            if (vpn && !prev_vpn)
                batch_add (&b, _("VPN"), _("Connected"), "network-vpn-symbolic");
            else if (!vpn && prev_vpn)
                batch_add (&b, _("VPN"), _("Disconnected"), "network-vpn-symbolic");
            if (portal && !np->last_portal)
                batch_add (&b, connected ? _("Wi-Fi") : _("Ethernet"),
                           _("This network requires you to sign in"),
                           "dialog-warning-symbolic");
        } else {
            /* Estado al arrancar. */
            if (connected) {
                gchar *t = g_strdup_printf (_("Connected to %s"),
                                            ai->ssid ? ai->ssid : "Wi-Fi");
                batch_add (&b, _("Wi-Fi"), t, "network-wireless-symbolic");
                g_free (t);
            }
            if (wired)
                batch_add (&b, _("Ethernet"), _("Cable connected"), "network-wired-symbolic");
            if (vpn)
                batch_add (&b, _("VPN"), _("Connected"), "network-vpn-symbolic");
        }
        batch_send (np, &b);
    }
    np->notif_ready = TRUE;
#endif

    /* Tooltip del botón */
    {
        GString *tip = g_string_new (NULL);

        if (!connected) {
            g_string_append (tip, _("Not connected"));
        } else {
            g_string_append_printf (tip, _("Connected to %s (%s)"),
                                    ai->ssid ? ai->ssid : "Wi-Fi",
                                    ai->band  ? ai->band  : "");
        }

        if (vpn) {
            if (tip->len > 0)
                g_string_append (tip, _(" · VPN active"));
            else
                g_string_append (tip, _("VPN active"));
        }
        if (portal)
            g_string_append (tip, _(" · Sign-in required"));

        gtk_widget_set_tooltip_text (np->button, tip->str);
        g_string_free (tip, TRUE);
    }

    np->last_portal = portal;

    /* Recordar la red actual para detectar el próximo cambio de red. */
    g_free (np->last_ssid);
    np->last_ssid = (connected && ai->ssid) ? g_strdup (ai->ssid) : NULL;

    nm_active_ap_info_free (ai);
    nm_cache_end ();

    /* IMPORTANTE: ya no reconstruimos el popup desde acá. El popup tiene su
     * propia suscripción a señales DBus mientras está abierto. Esta función
     * solo se ocupa del ícono y tooltip del botón del panel. */
    return G_SOURCE_REMOVE;
}

static void
on_nm_changed (gpointer user_data)
{
    NetPlugin *np = user_data;
    /* Agrupar ráfagas de señales en un solo refresco. El número queda
     * guardado para cancelarlo si el plugin se quita del panel. */
    if (np->refresh_timeout_id)
        return;
    np->refresh_timeout_id = g_timeout_add (100, on_nm_changed_cb, np);
}

/* Aviso del seguidor de intentos: actualizar el spinner y, si un intento
 * falló con el popup cerrado, avisar por qué. Los fallos del diálogo de red
 * oculta no se notifican: el diálogo ya lo muestra. */
static void
on_plugin_attempt (const gchar *ssid, const gchar *device_path,
                   AttemptEvent event, gpointer user_data)
{
    NetPlugin *np = user_data;
    on_nm_changed (np);
#ifdef HAVE_LIBNOTIFY
    Attempt a;
    if (event == ATTEMPT_EVENT_FAILED &&
        attempts_lookup (ssid, device_path, &a) && !a.from_hidden) {
        gchar *body = g_strdup_printf ("%s\n%s", ssid, failure_text (a.reason));
        notify_show (np, _("Could not connect"), body,
                     "network-wireless-disconnected-symbolic");
        g_free (body);
    }
#else
    (void) ssid; (void) device_path; (void) event;
#endif
}

/* ---------- callbacks del panel ---------- */

static void
on_button_toggled (GtkToggleButton *btn, NetPlugin *np)
{
    (void) btn;
    if (!np->conn) {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (np->button), FALSE);
        return;
    }
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (np->button))) {
        np->popup->show_separators   = np->show_separators;
        np->popup->show_forget_active = np->show_forget_active;
        np->popup->popup_style        = np->popup_style;
        popup_show (np->popup, np->plugin, np->button, np->conn,
                    np->popup_width, np->popup_height);
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

/* Cambió la cantidad de filas del panel: el tamaño del panel no cambia,
 * pero sí el de cada fila (botón cuadrado e ícono automático). */
static void
on_nrows_changed (XfcePanelPlugin *plugin, guint rows, NetPlugin *np)
{
    (void) plugin;
    (void) rows;
    apply_config (np);
}

/* Cambió el modo del panel (horizontal, vertical o barra de escritorio). */
static void
on_mode_changed (XfcePanelPlugin *plugin, XfcePanelPluginMode mode, NetPlugin *np)
{
    (void) plugin;
    (void) mode;
    apply_config (np);
}

static NetPlugin *
net_plugin_new (XfcePanelPlugin *plugin)
{
    NetPlugin *np = g_new0 (NetPlugin, 1);
    np->plugin    = plugin;
    np->conn      = nm_dbus_connect ();

    np->last_strength  = 0;
    np->last_secure    = FALSE;
    np->last_connected = FALSE;
    np->last_vpn       = FALSE;
    np->last_wired     = FALSE;
    np->notif_ready    = FALSE;

    /* Canal de Xfconf con la ruta propia de esta copia del plugin. */
    {
        GError *xerr = NULL;
        if (xfconf_init (&xerr))
            np->channel = xfconf_channel_new_with_property_base (
                              "xfce4-panel", xfce_panel_plugin_get_property_base (plugin));
        else {
            g_warning ("xfce-net-plugin: Xfconf no responde, la configuración "
                       "no se va a guardar: %s", xerr ? xerr->message : "?");
            g_clear_error (&xerr);
        }
    }

    load_config (np);
    popup_set_prefer_nm_icons (np->prefer_nm_icons);


    np->button = gtk_toggle_button_new ();
    gtk_button_set_relief (GTK_BUTTON (np->button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (np->button, _("Networks"));
    xfce_panel_plugin_set_shrink (plugin, TRUE);
    xfce_panel_plugin_set_small  (plugin, TRUE);

    np->icon_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    GIcon     *init_disc_gicon = net_themed_icon_new ("network-wireless-disconnected-symbolic", "nm-no-connection");
    GtkWidget *init_icon       = gtk_image_new_from_gicon (init_disc_gicon, GTK_ICON_SIZE_BUTTON);
    g_object_unref (init_disc_gicon);
    gtk_image_set_pixel_size (GTK_IMAGE (init_icon), np->icon_size);
    gtk_box_pack_start (GTK_BOX (np->icon_box), init_icon, TRUE, TRUE, 0);

    np->spinner = gtk_spinner_new ();
    np->stack   = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (np->stack), np->icon_box, "icon");
    gtk_stack_add_named (GTK_STACK (np->stack), np->spinner,  "spinner");
    gtk_stack_set_visible_child_name (GTK_STACK (np->stack), "icon");
    gtk_container_add (GTK_CONTAINER (np->button), np->stack);
    gtk_widget_show_all (np->button);

    /* Seguidor de intentos de conexión: antes que el popup, que también lo
     * escucha. Es uno solo por proceso, compartido por todas las copias. */
    attempts_acquire (np->conn);
    np->attempts_listener = attempts_add_listener (on_plugin_attempt, np);

    np->popup = popup_create (plugin, np->button);
    np->popup->plugin_ref = np;

    g_signal_connect (plugin, "size-changed",
                      G_CALLBACK (on_size_changed), np);
    g_signal_connect (plugin, "nrows-changed",
                      G_CALLBACK (on_nrows_changed), np);
    g_signal_connect (plugin, "mode-changed",
                      G_CALLBACK (on_mode_changed), np);
    g_signal_connect (np->button, "toggled",
                      G_CALLBACK (on_button_toggled), np);

    np->signal_ids = nm_subscribe_signals (np->conn, on_nm_changed, np);
    secret_agent_apply (np);
    g_signal_connect (np->popup->window, "hide",
                      G_CALLBACK (on_popup_hidden), np);

    on_nm_changed (np);

    xfce_panel_plugin_add_action_widget (plugin, np->button);
    gtk_container_add (GTK_CONTAINER (plugin), np->button);

    return np;
}

static void
net_plugin_about (XfcePanelPlugin *plugin, NetPlugin *np)
{
    (void) plugin;
    (void) np;
    const gchar *authors[] = { "Tantin", NULL };
    GdkPixbuf *logo = gtk_icon_theme_load_icon (
                          gtk_icon_theme_get_default (),
                          "network-wireless-symbolic",
                          48, 0, NULL);
    gtk_show_about_dialog (NULL,
        "program-name", "xfce-net-plugin",
        "version",      PACKAGE_VERSION,
        "comments",     _("Network manager plugin for the XFCE panel"),
        "authors",      authors,
        "license-type", GTK_LICENSE_GPL_2_0,
        "website",      "https://github.com/Tantin1/xfce-net-plugin",
        "logo",         logo,
        NULL);
    if (logo)
        g_object_unref (logo);
}

static void
net_plugin_free (XfcePanelPlugin *plugin, NetPlugin *np)
{
    (void) plugin;
    if (np->refresh_timeout_id) {
        g_source_remove (np->refresh_timeout_id);
        np->refresh_timeout_id = 0;
    }
    if (np->signal_ids)
        nm_unsubscribe_signals (np->conn, np->signal_ids);
    if (np->agent) {
        net_secret_agent_free (np->agent);
        np->agent = NULL;
    }
    attempts_remove_listener (np->attempts_listener);
    popup_destroy (np->popup);
    attempts_release ();
#ifdef HAVE_LIBNOTIFY
    g_clear_object (&np->notif);
#endif
    if (np->conn)
        g_object_unref (np->conn);
    if (np->button_css)
        g_object_unref (np->button_css);
    g_free (np->last_ssid);
    if (np->disabled_ifaces)
        g_hash_table_destroy (np->disabled_ifaces);
    if (np->channel) {
        g_object_unref (np->channel);
        xfconf_shutdown ();
    }
    g_free (np);
}

static void
net_plugin_construct (XfcePanelPlugin *plugin)
{
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    /* Sin textdomain(): cambia el dominio por defecto de TODO el proceso del
     * panel (y de los demás plugins). _() ya nombra nuestro dominio. */
#ifdef HAVE_LIBNOTIFY
    notify_init ("xfce-net-plugin");
#endif

    /* Registrar el recurso embebido para que el candado fallback funcione
     * desde gtk_image_new_from_resource. Se llama una sola vez por instancia. */
    g_resources_register (lock_icons_get_resource ());

    NetPlugin *np = net_plugin_new (plugin);
    xfce_panel_plugin_menu_show_configure (plugin);
    xfce_panel_plugin_menu_show_about (plugin);
    g_signal_connect (plugin, "about",
                      G_CALLBACK (net_plugin_about),     np);
    g_signal_connect (plugin, "free-data",
                      G_CALLBACK (net_plugin_free),     np);
    g_signal_connect (plugin, "configure-plugin",
                      G_CALLBACK (on_configure_plugin),  np);
    /* OJO: "size-changed" ya se conecta en net_plugin_new; conectarla otra
     * vez acá hacía que apply_config corriera dos veces por cada cambio. */
}

XFCE_PANEL_PLUGIN_REGISTER (net_plugin_construct);
