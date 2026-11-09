/* popup-private.h — declaraciones internas compartidas entre los módulos
 * del popup (popup.c, popup-rows.c, popup-sections.c). NO incluir desde
 * plugin.c: la interfaz pública sigue siendo popup.h. */
#ifndef POPUP_PRIVATE_H
#define POPUP_PRIVATE_H

#include "popup.h"
#include <gdk/gdkkeysyms.h>
/* gi18n-lib: _() busca siempre en NUESTRO dominio de traducción
 * (GETTEXT_PACKAGE). En Wayland todos los plugins corren dentro del
 * proceso del panel y comparten el dominio "por defecto": con gi18n.h
 * nuestros textos se buscaban en el de otro y salían en inglés. */
#include <glib/gi18n-lib.h>

#define NM_BUS_NAME    "org.freedesktop.NetworkManager"
#define NM_OBJECT_PATH "/org/freedesktop/NetworkManager"
#define NM_IFACE       "org.freedesktop.NetworkManager"
#define NM_WIFI_IFACE  "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_IFACE    "org.freedesktop.NetworkManager.AccessPoint"

/* Separación del popup flotante: del panel y de los bordes de la pantalla
 * (en todas las direcciones o en ninguna). */
#define POPUP_FLOAT_GAP 6
/* Modo automático: desde cuántos píxeles de radio el popover del tema se ve
 * redondeado. Casi ningún tema da 0 (Numix 2, Arc 3, Qogir 3, Greybird 5),
 * y a esos tamaños las esquinas se ven rectas; Adwaita da 9. */
#define POPUP_ROUND_MIN 6

/* Margen izquierdo y derecho del contenido de las filas de la lista. Al
 * derecho se le suma el ancho de la barra de desplazamiento, que se dibuja
 * encima de la lista (popup_end_margin). */
#define POPUP_ROW_MARGIN 12

/* Cooldown del botón Actualizar (ms) entre escaneos. */
#define SCAN_COOLDOWN_MS 5000

/* Bits de gestión de claves en WpaFlags/RsnFlags del gestor de red. */
#define NM_AP_SEC_KEY_MGMT_PSK    0x00000100
#define NM_AP_SEC_KEY_MGMT_802_1X 0x00000200
#define NM_AP_SEC_KEY_MGMT_SAE    0x00000400
#define NM_AP_SEC_KEY_MGMT_SUITEB 0x00002000

/* Los intentos de conexión los sigue attempts.c (incluido por popup.h):
 * estado de cada red en cada adaptador, motivo del fallo (failure_text) y
 * qué deshacer si falla. */

typedef struct {
    GDBusConnection *conn;
    gchar           *device_path;
    gchar           *iface;      /* nombre de la interfaz (para recordar si quedó apagado) */
    NetPopup        *popup;
} DeviceSwitchData;

typedef struct {
    NetPopup        *popup;
    GtkWidget       *expand_box;
    GtkWidget       *action_btn;     /* Conectar / Desconectar / Reintentar con la contraseña guardada */
    gchar           *ssid;
    gchar           *ap_path;
    gboolean         secure;
    gboolean         active;
    gboolean         saved;
    const gchar     *key_mgmt;       /* "wpa-psk" o "sae" según el AP. Cadena estática, no liberar. */
    gchar           *device_path;
    gchar           *profile_path;   /* Perfil guardado que usa esta fila (ruta en el bus), o NULL. */
    guint32          wpa_flags;      /* Banderas de seguridad del AP, para volver a */
    guint32          rsn_flags;      /* buscar el perfil justo al conectar. */
    GtkWidget       *pass_entry;
    GtkWidget       *autoconnect_check;

    /* Solo presentes en expand de red guardada con último intento fallido. */
    GtkWidget       *state_a_box;    /* Contenedor de botones del estado A. */
    GtkWidget       *state_b_box;    /* Contenedor del entry + botones del estado B. */
    GtkWidget       *retry_btn;      /* Estado A: "Reintentar con la contraseña guardada" */
    GtkWidget       *try_other_btn;  /* Estado A: "Probar otra contraseña" */
    GtkWidget       *back_btn;       /* Estado B: "Volver" */
    GtkWidget       *connect_btn_b;  /* Estado B: "Conectar" */
    GtkWidget       *forget_btn;     /* Botón "Olvidar" (solo en estado A para red con fallo). */
    GtkWidget       *confirm_box;    /* Contenedor de confirmación "¿Eliminar red guardada?" */
    GtkWidget       *action_row;     /* Fila con botones Conectar/Olvidar — se oculta al confirmar. */
    GtkWidget       *qr_drawing_area;  /* GtkDrawingArea con el QR (solo en red activa+segura). */
    GtkWidget       *qr_btn;           /* Botón "Mostrar QR" — se oculta al confirmar Olvidar. */
    GtkWidget       *qr_lbl;           /* Label dentro del botón QR — se actualiza sin destruir el ícono. */
    GtkWidget       *active_forget_btn; /* Botón "Olvidar" en red activa. */
    GtkWidget       *details_btn;       /* Botón "Detalles" con flecha. */
    GtkWidget       *details_box;       /* Panel de detalles expandible. */
    GtkWidget       *details_arrow;     /* Ícono flecha arriba/abajo. */
    GtkWidget       *row_box;           /* El event_box de la fila, para marcar realce "abierta". */
} RowData;

/* ---- popup.c (núcleo: ventana, operaciones, helpers) ---- */
void      secure_wipe_free          (gchar *s);
gchar    *get_primary_ssid          (GDBusConnection *conn);
gboolean  device_is_activated      (GDBusConnection *conn, const gchar *device_path);
gboolean  on_wifi_switch_toggled   (GtkSwitch *sw, gboolean state, gpointer user_data);
void      device_switch_data_free  (DeviceSwitchData *d);
gboolean  on_device_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data);
void      on_refresh_clicked       (GtkWidget *btn, NetPopup *popup);
void      on_eye_clicked           (GtkWidget *btn, GtkEntry *entry);
void      on_hidden_network_clicked (GtkWidget *btn, NetPopup *popup);
void      on_advanced_clicked      (GtkWidget *btn, gpointer user_data);
/* Llamar cuando se cierra el menú contextual de una fila: destruye el menú
 * (diferido), devuelve la captura al popup y agenda el refresco postergado. */
void      popup_menu_closed        (NetPopup *popup);
/* Botón "Iniciar sesión" del portal cautivo: cierra el popup y abre el
 * navegador en la dirección de comprobación de NM. */
void      on_portal_signin_clicked (GtkWidget *btn, NetPopup *popup);
/* Botón de la lupa: abre la búsqueda, o la cierra si ya estaba abierta. */
void      on_search_button_clicked (GtkWidget *btn, NetPopup *popup);
/* Ajusta el alto de la lista para que la franja de búsqueda (y su aviso)
 * no agranden el popup: el alto total queda siempre igual. */
void      search_sync_height       (NetPopup *popup);
/* Margen derecho del contenido de una fila de la lista: POPUP_ROW_MARGIN
 * más el ancho de la barra de desplazamiento (0 con barras flotantes). Así
 * la barra, que va encima de la lista, nunca tapa botones ni textos. */
gint      popup_end_margin         (NetPopup *popup);

/* ---- popup-rows.c (fila de red, expand y sus callbacks) ---- */
gchar    *row_fingerprint (NetPopup *popup, NmAccessPoint *ap,
                           const gchar *device_path, GHashTable *saved_ssids);
GtkWidget *make_ap_row    (NmAccessPoint *ap, NetPopup *popup,
                           const gchar *device_path, GHashTable *saved_ssids);
void      reopen_expand_for_ssid (NetPopup *popup, const gchar *ssid,
                                  const gchar *device_path);
void      clear_open_highlight   (NetPopup *popup);

/* ---- popup-sections.c (secciones, refresco y reconstrucción) ---- */
void      rebuild_ui             (NetPopup *popup);
void      update_top_status      (NetPopup *popup);
void      update_eth_section     (NetPopup *popup);
void      update_vpn_section     (NetPopup *popup);
void      update_devices_section (NetPopup *popup);
void      schedule_refresh_ui    (NetPopup *popup);
void      on_nm_signal_popup     (gpointer user_data);
/* Aviso del seguidor de intentos (attempts.c): cierra o reabre el expand de
 * la red que terminó y agenda el refresco. */
void      on_attempt_event       (const gchar *ssid, const gchar *device_path,
                                  AttemptEvent event, gpointer user_data);
/* Búsqueda: oculta las filas Wi-Fi cuyo nombre no contiene el texto del
 * campo de búsqueda. Se llama al escribir y al final de cada refresco. */
void      apply_search_filter    (NetPopup *popup);

#endif /* POPUP_PRIVATE_H */
