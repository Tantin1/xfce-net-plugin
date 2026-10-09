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

    /* Tabla de operaciones en curso, indexada por device_path (ruta del
     * adaptador). Cada entrada apunta a una OpInProgress con su temporizador
     * y los widgets afectados. */
    GHashTable *ops_in_progress;

    /* Temporizadores propios. Todos se cancelan en popup_hide y en
     * popup_destroy, para que ninguno dispare con el popup ya liberado. */
    guint      scan_timeout_id;     /* reactivar el botón Actualizar */
    guint      pending_timeout_id;  /* evaluar intentos pendientes al reabrir */
    guint      passive_timeout_id;  /* expand pasivo "Conectando…" */
    guint      refresh_idle_id;     /* refresco agendado (coalescido) */
    gboolean   show_separators;

    /* Redes cuya última operación CONNECT falló, clave "ssid|device_path"
     * (ver make_ssid_dev_key). Se setea en op_timeout_cb
     * y se limpia al conectar exitosamente o al olvidar la red manualmente.
     * Sirve para que el expand de red guardada muestre campo "Reescribir contraseña"
     * después de un fallo, sin obligar al usuario a clickear Olvidar y reabrir. */
    GHashTable *failed_ssids;

    /* Intentos de conexión en curso. Clave "ssid|device_path" (ver
     * make_ssid_dev_key) → marca de tiempo monotónica (gint64*). Al reabrir
     * el popup se consulta: si el adaptador no quedó conectado, la red pasa a
     * failed_ssids; si quedó conectado, se descarta. Sirve para no perder el
     * rastro de fallos cuando el usuario cierra el popup antes de los 20s. */
    GHashTable *pending_attempts;

    /* Puntero opaco al NetPlugin del panel. Usado para controlar el spinner. */
    gpointer plugin_ref;

    /* Filtro GDK para detectar clicks en el área vacía del panel (cuando grab falla). */
    gboolean event_filter_active;
    gboolean show_forget_active;   /* Mostrar botón Olvidar en redes activas. */

    /* Captura del mouse/teclado: TRUE si el popup la tiene (gdk_seat_grab
     * funcionó). El menú contextual se la lleva al abrirse; al cerrarse se
     * vuelve a pedir solo si estaba activa. */
    gboolean   grab_active;
    /* Menú contextual abierto (NULL si no hay). Mientras existe, el refresco
     * no reconstruye filas y la pérdida de foco no cierra el popup. */
    GtkWidget *ctx_menu;
    guint      menu_idle_id;        /* limpieza diferida al cerrar el menú */

    /* Franja "esta red pide iniciar sesión" (portal cautivo), en la zona
     * fija superior. Oculta salvo que NM informe conectividad PORTAL. */
    GtkWidget *portal_row;
} NetPopup;

/* Arma la clave "ssid|device_path" que usan pending_attempts y failed_ssids.
 * Un solo lugar para armarla, así nunca queda inconsistente. Liberar con
 * g_free(). */
static inline gchar *
make_ssid_dev_key (const gchar *ssid, const gchar *device_path)
{
    return g_strdup_printf ("%s|%s", ssid ? ssid : "",
                            device_path ? device_path : "");
}

/* Crea un widget con ícono de señal Wi-Fi. Si secure=TRUE superpone un candado. */
GtkWidget *make_signal_icon (gint strength, gboolean secure, gint icon_size);

/* Controlado desde popup.c para mostrar/ocultar el spinner del panel. */
void net_plugin_set_connecting (gpointer np_ptr, gboolean connecting);


NetPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void      popup_show    (NetPopup *popup, XfcePanelPlugin *plugin,
                         GtkWidget *button, GDBusConnection *conn,
                         gint popup_width, gint popup_height);
void      popup_hide    (NetPopup *popup);
void      popup_destroy (NetPopup *popup);

#endif /* POPUP_H */
