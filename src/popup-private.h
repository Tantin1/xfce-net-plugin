/* popup-private.h — declaraciones internas compartidas entre los módulos
 * del popup (popup.c, popup-rows.c, popup-sections.c). NO incluir desde
 * plugin.c: la interfaz pública sigue siendo popup.h. */
#ifndef POPUP_PRIVATE_H
#define POPUP_PRIVATE_H

#include "popup.h"
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#define NM_BUS_NAME    "org.freedesktop.NetworkManager"
#define NM_OBJECT_PATH "/org/freedesktop/NetworkManager"
#define NM_IFACE       "org.freedesktop.NetworkManager"
#define NM_WIFI_IFACE  "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_IFACE    "org.freedesktop.NetworkManager.AccessPoint"

/* Timeout de seguridad (ms) para operaciones conectar/desconectar.
 * Si NM no confirma en este tiempo, asumimos fallo. */
#define OP_TIMEOUT_MS 20000

/* Cooldown del botón Actualizar (ms) entre escaneos. */
#define SCAN_COOLDOWN_MS 5000

/* Edad máxima (en segundos) de un intento "pendiente" para que cuente como
 * candidato a marcar fallo al reabrir el popup. Si pasó más tiempo que esto,
 * descartamos el intento sin marcar — asumimos que ya pasó suficiente y
 * cualquier marca tardía sería confusa. */
#define PENDING_ATTEMPT_MAX_AGE_SECS 60

/* Bits de gestión de claves en WpaFlags/RsnFlags del gestor de red. */
#define NM_AP_SEC_KEY_MGMT_PSK    0x00000100
#define NM_AP_SEC_KEY_MGMT_802_1X 0x00000200
#define NM_AP_SEC_KEY_MGMT_SAE    0x00000400

/* ================================================================
 * Operaciones en curso
 *
 * Cuando el usuario aprieta Conectar o Desconectar, registramos
 * la operación en popup->ops_in_progress, indexada por device_path.
 * El handler reactivo, cuando llega una señal DBus, mira la tabla
 * y decide si tiene que cerrar el expand, restaurar el botón, etc.
 * ================================================================ */

typedef enum {
    OP_CONNECT,
    OP_DISCONNECT
} OpKind;

typedef struct {
    OpKind     kind;
    gchar     *ssid;          /* SSID destino (para CONNECT) o el actualmente conectado (DISCONNECT) */
    gchar     *device_path;
    GtkWidget *expand_box;    /* Expand a cerrar al confirmar (puede ser NULL si la fila se destruye antes). */
    GtkWidget *action_btn;    /* Botón a restaurar si falla. */
    gchar     *action_label;  /* Texto original del action_btn a restaurar en caso de fallo. */
    GtkWidget *pass_entry;    /* Entry de contraseña, si aplica (para mostrar error inline). */
    GtkWidget *error_label;   /* Label de error reusable, si se crea. */
    GSList    *extra_disabled;/* Lista de GtkWidget* extra que se deshabilitaron y hay que rehabilitar al fallar. */
    guint      timeout_id;    /* Fuente de timeout de 20s. */
    gboolean   extended;      /* TRUE si ya se le dio la prórroga única por "sigue intentando". */
    NetPopup  *popup;
} OpInProgress;

typedef struct {
    GDBusConnection *conn;
    gchar           *device_path;
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
gboolean  deferred_refresh_cb       (gpointer user_data);
gchar    *get_primary_ssid          (GDBusConnection *conn);
gboolean  op_timeout_cb             (gpointer user_data);
gboolean  device_is_activated      (GDBusConnection *conn, const gchar *device_path);
gboolean  device_is_disconnected   (GDBusConnection *conn, const gchar *device_path);
gboolean  device_is_connecting     (GDBusConnection *conn, const gchar *device_path);
gboolean  is_ssid_connected_anywhere (GDBusConnection *conn, const gchar *ssid);
void      process_pending_attempts (NetPopup *popup);
void      check_ops_progress       (NetPopup *popup);
gboolean  on_wifi_switch_toggled   (GtkSwitch *sw, gboolean state, gpointer user_data);
void      device_switch_data_free  (DeviceSwitchData *d);
gboolean  on_device_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data);
void      on_refresh_clicked       (GtkWidget *btn, NetPopup *popup);
void      on_eye_clicked           (GtkWidget *btn, GtkEntry *entry);
void      on_hidden_network_clicked (GtkWidget *btn, NetPopup *popup);
void      on_advanced_clicked      (GtkWidget *btn, gpointer user_data);

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

#endif /* POPUP_PRIVATE_H */
