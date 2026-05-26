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
    GtkWidget *status_stack;
    GtkWidget *status_icon;
    GtkWidget *status_spinner;
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
    guint      pending_timeout_id;
    gboolean   show_separators;

    /* SSIDs cuya última operación CONNECT falló. Se setea en op_timeout_cb
     * y se limpia al conectar exitosamente o al olvidar la red manualmente.
     * Sirve para que el expand de red guardada muestre campo "Reescribir contraseña"
     * después de un fallo, sin obligar al usuario a clickear Olvidar y reabrir. */
    GHashTable *failed_ssids;

    /* SSIDs con intentos de conexión en curso al cerrar el popup. Indexada
     * por SSID (gchar*) → timestamp monotonic (gint64*). Al reabrir el popup
     * consultamos esta tabla: si el SSID no está conectado en ningún adapter,
     * lo marcamos en failed_ssids; si está conectado, lo descartamos.
     * Sirve para no perder el rastro de fallos cuando el usuario cierra
     * el popup antes de los 20s del timeout. */
    GHashTable *pending_attempts;

    /* Puntero opaco al NetPlugin del panel. Usado para controlar el spinner. */
    gpointer plugin_ref;

    /* Filtro GDK para detectar clicks en el área vacía del panel (cuando grab falla). */
    gboolean event_filter_active;
    gboolean show_forget_active;   /* Mostrar botón Olvidar en redes activas. */
    GtkWidget *hotspot_btn;        /* Botón Hotspot (NULL si hw no lo soporta). */
    GtkWidget *hotspot_arrow;      /* Flecha arriba/abajo del botón Hotspot. */
    gchar     *ap_capable_device;  /* device_path del adaptador AP-capable, o NULL. */
    GtkWidget *hotspot_section;    /* Sección scrolleable del hotspot. */
    GtkWidget *hotspot_ssid_entry; /* Entry SSID dentro de la sección. */
    GtkWidget *hotspot_pass_entry; /* Entry contraseña dentro de la sección. */
    GtkWidget *hotspot_switch;     /* Switch ON/OFF del hotspot. */

    /* Lista de NmAutoconnectState con los perfiles cuya autoconexion fue
     * deshabilitada al activar el hotspot. NULL cuando el hotspot esta apagado. */
    GSList    *autoconnect_states;
} NetPopup;

/* Crea un widget con ícono de señal Wi-Fi. Si secure=TRUE superpone un candado. */
GtkWidget *make_signal_icon (gint strength, gboolean secure, gint icon_size);

/* Controlado desde popup.c para mostrar/ocultar el spinner del panel. */
void net_plugin_set_connecting (gpointer np_ptr, gboolean connecting);

gchar *net_plugin_get_config_path         (gpointer np_ptr);
void net_plugin_save_autoconnect_restore  (gpointer np_ptr, GSList *states);
void net_plugin_clear_autoconnect_restore (gpointer np_ptr);

NetPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void      popup_show    (NetPopup *popup, XfcePanelPlugin *plugin,
                         GtkWidget *button, GDBusConnection *conn,
                         gint popup_width, gint popup_height);
void      popup_hide    (NetPopup *popup);
void      popup_destroy (NetPopup *popup);

#endif /* POPUP_H */
