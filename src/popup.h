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
    /* Tamaño con el que se ubicó la ventana (X11) y hacia dónde abre. Si al
     * aparecer el tamaño real es otro, se la vuelve a ubicar. */
    gint         placed_w, placed_h;
    GtkArrowType placed_arrow;
    GSList    *device_switches;

    /* Conexión DBus prestada (no liberar). */
    GDBusConnection *conn;

    /* Construcción perezosa: TRUE si la UI ya fue armada al menos una vez. */
    gboolean         ui_built;
    /* El tema cambió con la ventana cerrada: rearmar el contenido antes de
     * la próxima apertura (ver on_theme_changed en popup.c). */
    gboolean         theme_dirty;
    GtkSettings     *theme_settings;
    gulong           theme_handler;

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

    /* Clave anterior de un perfil al que se le cambió la clave para conectar.
     * Clave "ssid|device_path" → PwBackup (popup.c). Si el intento falla se
     * le devuelve; si conecta, se descarta. */
    GHashTable *pw_backups;

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

    /* Búsqueda de redes (oct-2026). Franja fija entre la zona superior y la
     * lista, oculta hasta que se empieza a escribir o se aprieta Ctrl+F. */
    GtkWidget *search_bar;      /* contenedor de la franja */
    GtkWidget *search_entry;    /* campo de búsqueda */
    GtkWidget *search_empty;    /* "No hay redes que coincidan" */

    /* Aspecto del popup (nov-2026, ver popup-moderno-notas.md). */
    GtkWidget *frame;           /* marco con nombre de componente "menu" (colores del menú del tema): todo el contenido va adentro */
    gint       popup_style;     /* NET_POPUP_STYLE_*: lo fija plugin.c antes de popup_show */
    gboolean   style_applied;   /* ya se eligió el visual al menos una vez */
    gboolean   rgba_active;     /* ventana con transparencia (esquinas del tema) */
    gint       float_gap;       /* separación del panel y de los bordes, en px (0 = pegada) */
} NetPopup;

/* Estilo de la ventana (Propiedades → Apariencia → "Popup style"). */
#define NET_POPUP_STYLE_AUTO      0   /* flota solo si hay transparencia y el tema redondea */
#define NET_POPUP_STYLE_FLOATING  1   /* siempre separada del panel */
#define NET_POPUP_STYLE_ATTACHED  2   /* pegada al panel, esquinas rectas: el aspecto de antes */

/* Arma la clave "ssid|device_path" que usan pending_attempts y failed_ssids.
 * Un solo lugar para armarla, así nunca queda inconsistente. Liberar con
 * g_free(). */
static inline gchar *
make_ssid_dev_key (const gchar *ssid, const gchar *device_path)
{
    return g_strdup_printf ("%s|%s", ssid ? ssid : "",
                            device_path ? device_path : "");
}

/* Íconos con reserva: arma un ícono que prueba dos nombres en orden. Por
 * defecto primero el estándar del tema (freedesktop) y después el de
 * nm-applet; con la opción "Preferir íconos de nm-applet" activada, al revés.
 * Vale para el panel y el popup. Liberar con g_object_unref(). */
GIcon *net_themed_icon_new       (const gchar *freedesktop_name, const gchar *nm_name);
void   popup_set_prefer_nm_icons (gboolean prefer);

/* Crea un widget con ícono de señal Wi-Fi. Si secure=TRUE superpone un candado. */
GtkWidget *make_signal_icon (gint strength, gboolean secure, gint icon_size);

/* Controlado desde popup.c para mostrar/ocultar el spinner del panel. */
void net_plugin_set_connecting (gpointer np_ptr, gboolean connecting);
/* Avisa al plugin que el usuario prendió o apagó un adaptador, para poder
 * recordarlo al reiniciar. */
void net_plugin_device_toggled (gpointer np_ptr, const gchar *iface, gboolean enabled);


NetPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void      popup_show    (NetPopup *popup, XfcePanelPlugin *plugin,
                         GtkWidget *button, GDBusConnection *conn,
                         gint popup_width, gint popup_height);
void      popup_hide    (NetPopup *popup);
void      popup_destroy (NetPopup *popup);

#endif /* POPUP_H */
