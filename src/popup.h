#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "nm-dbus.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *top_box;
    GtkWidget *content_box;
    GtkWidget *scroll;
    gulong     press_handler;
    GtkWidget *current_expand_box;
    gboolean   scanning;
    gint       popup_width;
    gint       popup_height;
    GtkWidget *button;
    GSList    *device_switches;

    /* Conexión DBus prestada (no liberar). */
    GDBusConnection *conn;

    /* Construcción perezosa: TRUE si la UI ya fue armada al menos una vez. */
    gboolean         ui_built;

    /* Estado superior: ícono, label y switch global. Se actualizan in-place. */
    GtkWidget *status_icon;
    GtkWidget *status_label;
    GtkWidget *wifi_switch;
    gulong     wifi_switch_handler;

    /* Botón "Actualizar" + su label, para poder cambiar texto sin reconstruir. */
    GtkWidget *refresh_button;
    GtkWidget *refresh_label;

    /* Contenedor de la sección Ethernet (se llena/vacía in-place). */
    GtkWidget *eth_section;

    /* Contenedor de la sección VPN (se llena/vacía in-place). */
    GtkWidget *vpn_section;

    /* IDs de suscripción a señales DBus de NM. */
    guint     *signal_ids;

    /* Tabla de operaciones en curso, indexada por SSID o por device_path.
     * Cada entrada apunta a una OpInProgress con su timeout id y widgets afectados. */
    GHashTable *ops_in_progress;

    /* Fuente de timeout para reactivar el botón Actualizar. */
    guint      scan_timeout_id;
    gboolean   show_separators;
} NetPopup;

/* Crea un widget con ícono de señal Wi-Fi. Si secure=TRUE superpone un candado. */
GtkWidget *make_signal_icon (gint strength, gboolean secure, gint icon_size);

NetPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void      popup_show    (NetPopup *popup, XfcePanelPlugin *plugin,
                         GtkWidget *button, GDBusConnection *conn,
                         gint popup_width, gint popup_height);
void      popup_hide    (NetPopup *popup);
void      popup_destroy (NetPopup *popup);

#endif /* POPUP_H */
