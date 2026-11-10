/* SPDX-FileCopyrightText: 2026 Tantin1 <juanperez33840@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "nm-dbus.h"
#include "attempts.h"

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

    /* Temporizadores propios. Todos se cancelan en popup_hide y en
     * popup_destroy, para que ninguno dispare con el popup ya liberado. */
    guint      scan_timeout_id;     /* reactivar el botón Actualizar */
    guint      refresh_idle_id;     /* refresco agendado (coalescido) */
    guint      reserve_idle_id;     /* reacomodo del hueco de la barra de desplazamiento */
    gboolean   show_separators;

    /* Seguimiento de intentos de conexión: lo lleva el seguidor común
     * (attempts.c). El popup solo escucha sus avisos para redibujar. */
    guint      attempts_listener;
    /* Fila cuyo expand hay que reabrir después del próximo refresco (un
     * intento que falló con su expand abierto: se reabre en estado A). */
    gchar     *reopen_ssid;
    gchar     *reopen_dev;

    /* Puntero opaco al NetPlugin del panel (para avisarle de adaptadores
     * prendidos o apagados a mano). */
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

/* Íconos con reserva: arma un ícono que prueba dos nombres en orden. Por
 * defecto primero el estándar del tema (freedesktop) y después el de
 * nm-applet; con la opción "Preferir íconos de nm-applet" activada, al revés.
 * Vale para el panel y el popup. Liberar con g_object_unref(). */
GIcon *net_themed_icon_new       (const gchar *freedesktop_name, const gchar *nm_name);
void   popup_set_prefer_nm_icons (gboolean prefer);

/* Crea un widget con ícono de señal Wi-Fi. Si secure=TRUE superpone un candado. */
GtkWidget *make_signal_icon (gint strength, gboolean secure, gint icon_size);

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
