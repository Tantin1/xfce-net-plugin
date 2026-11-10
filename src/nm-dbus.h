/* SPDX-FileCopyrightText: 2026 Tantin1 <juanperez33840@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NM_DBUS_H
#define NM_DBUS_H

#include <gio/gio.h>

typedef struct {
    gchar    *iface;
    gchar    *object_path;
    gchar    *description;  /* Nombre legible del hardware (puede ser NULL). Liberar con g_free(). */
    gchar    *perm_hw;      /* Dirección física de fábrica (PermHwAddress), en
                             * mayúsculas, o NULL si el adaptador no informa una.
                             * A diferencia del nombre (wlan0, wlan1), no cambia
                             * entre reinicios. */
} NmDevice;

typedef struct {
    gchar   *ssid;
    gchar   *object_path;
    gchar   *bssid;        /* MAC del punto de acceso (HwAddress) */
    gint     strength;
    guint    frequency;
    guint    wpa_flags;    /* WpaFlags de NM */
    guint    rsn_flags;    /* RsnFlags de NM */
    gboolean secure;
    gboolean active;
} NmAccessPoint;

GSList *nm_get_wifi_devices  (GDBusConnection *conn);
gchar  *nm_get_device_description (const gchar *iface);  /* Lee /run/udev/data/ sin DBus. Liberar con g_free(). */
void    nm_device_list_free  (GSList *list);

GSList *nm_get_access_points (GDBusConnection *conn, const gchar *device_path);
void    nm_ap_list_free      (GSList *list);
/* Información del AP activo: una sola lectura para ícono + tooltip + notificaciones.
 * Liberar con nm_active_ap_info_free(). */
typedef struct {
    gboolean  connected;
    gint      strength;
    gboolean  secure;
    gchar    *ssid;   /* liberar con g_free() */
    gchar    *band;   /* "2.4G" / "5G" / "6G" — liberar con g_free() */
} NmActiveApInfo;

NmActiveApInfo *nm_get_active_ap_info  (GDBusConnection *conn);
void            nm_active_ap_info_free (NmActiveApInfo  *info);


GDBusConnection *nm_dbus_connect         (void);

/* Instantánea (ObjectManager): nm_cache_begin trae TODOS los objetos del
 * gestor de red en una sola llamada al bus; mientras está activa, las
 * funciones de consulta de este módulo resuelven desde memoria sin tocar
 * el bus. nm_cache_end la libera. Anidable. Usar solo alrededor de bloques
 * cortos de lectura (armar/refrescar la UI). */
void nm_cache_begin (GDBusConnection *conn);
void nm_cache_end   (void);

gboolean         nm_has_saved_connection (GDBusConnection *conn, const gchar *ssid);
gboolean         nm_forget_connection    (GDBusConnection *conn, const gchar *ssid);

/* Devuelve un set (tabla hash) con las SSIDs de todos los perfiles Wi-Fi
 * guardados. Una sola enumeración de perfiles, pensada para consultar muchas
 * SSIDs sin repetir la enumeración. Liberar con g_hash_table_destroy(). */
GHashTable *nm_get_saved_wifi_ssids (GDBusConnection *conn);

/* ---------- Perfiles por identificador ----------
 * Un mismo nombre de red puede tener varios perfiles guardados (por ejemplo,
 * uno WPA2 viejo y uno WPA3 nuevo, o un hotspot propio con el mismo nombre).
 * Estas funciones trabajan sobre UN perfil concreto, identificado por su ruta
 * en el bus (profile_path), en vez de "el primero con ese nombre". */

/* El perfil guardado que corresponde a este punto de acceso desde ESTE
 * adaptador (device_path): misma red, modo cliente (no hotspot), que el
 * adaptador pueda usar (libre o atado a él; nunca uno atado a otro
 * adaptador) y con seguridad compatible con las banderas del AP. Prefiere
 * uno atado a este adaptador, después uno libre. Si ninguno es compatible,
 * el primero usable con ese nombre. NULL si no hay. Liberar con g_free(). */
gchar   *nm_find_profile_for_ap       (GDBusConnection *conn, const gchar *ssid,
                                       guint32 wpa_flags, guint32 rsn_flags,
                                       const gchar *device_path);
/* El perfil que el adaptador tiene activo ahora (o NULL). Liberar con g_free(). */
gchar   *nm_get_device_active_profile (GDBusConnection *conn, const gchar *device_path);
gboolean nm_delete_profile            (GDBusConnection *conn, const gchar *profile_path);
gchar   *nm_get_profile_password      (GDBusConnection *conn, const gchar *profile_path);
gboolean nm_get_profile_autoconnect   (GDBusConnection *conn, const gchar *profile_path);
gboolean nm_set_profile_autoconnect   (GDBusConnection *conn, const gchar *profile_path,
                                       gboolean autoconnect);
/* Cambia solo la contraseña del perfil (WPA2/WPA3 o WEP) y conserva el
 * resto de sus ajustes. FALSE si no se pudo; el perfil queda intacto. */
gboolean nm_set_profile_password      (GDBusConnection *conn, const gchar *profile_path,
                                       const gchar *password);
/* Marca (o desmarca) el perfil como red oculta, sin tocar nada más. */
gboolean nm_set_profile_hidden        (GDBusConnection *conn, const gchar *profile_path,
                                       gboolean hidden);
gboolean nm_get_profile_hidden        (GDBusConnection *conn, const gchar *profile_path);
/* ¿La seguridad del perfil sirve para un AP con estas banderas? */
gboolean nm_profile_fits_ap           (GDBusConnection *conn, const gchar *profile_path,
                                       guint32 wpa_flags, guint32 rsn_flags);
/* Rutas de todos los perfiles de cliente de esa red (no hotspot).
 * Liberar con g_slist_free_full (lista, g_free). */
GSList  *nm_list_wifi_profiles        (GDBusConnection *conn, const gchar *ssid);

/* Clasificación de una red por sus banderas (WpaFlags/RsnFlags del AP).
 * need_password: hay que pedir clave o usuario (lleva candado). Las redes
 *   abiertas cifradas ("Enhanced Open"/OWE) NO la necesitan.
 * enhanced_open: red abierta solo cifrada; su perfil va con key-mgmt "owe". */
gboolean nm_ap_flags_need_password (guint32 wpa_flags, guint32 rsn_flags);
gboolean nm_ap_flags_enhanced_open (guint32 wpa_flags, guint32 rsn_flags);

gboolean nm_get_wifi_enabled    (GDBusConnection *conn);
void     nm_set_wifi_enabled    (GDBusConnection *conn, gboolean enabled);

/* Estado del adaptador individual */
gboolean nm_get_device_enabled  (GDBusConnection *conn, const gchar *device_path);

/* Estado crudo del adaptador (10 = no gestionado, 20 = no disponible,
 * 30 = desconectado, 40-90 = conectando, 100 = activado, 110 = desactivando,
 * 120 = falló). Devuelve 0 si no se pudo leer. Usa la instantánea si hay
 * una activa. */
guint32  nm_get_device_state    (GDBusConnection *conn, const gchar *device_path);

/* Devuelve lista de NmDevice Ethernet para mostrar en el popup: los
 * conectados (estado 100), los apagados por el usuario (estado 10, para que
 * su interruptor siga visible) y los que tienen cable enchufado aunque
 * todavía no estén conectados. NO sirve para saber si hay conexión por cable:
 * para eso usar nm_any_ethernet_activated. */
GSList *nm_get_ethernet_devices (GDBusConnection *conn);

/* TRUE solo si algún adaptador Ethernet está conectado de verdad (estado 100). */
gboolean nm_any_ethernet_activated (GDBusConnection *conn);

/* Devuelve la contraseña del perfil guardado para un SSID, o NULL. Liberar con g_free(). */
gchar *nm_get_saved_password (GDBusConnection *conn, const gchar *ssid);

/* Devuelve TRUE si hay una conexión VPN activa. */
gboolean nm_get_vpn_active (GDBusConnection *conn);

typedef struct {
    gchar    *name;
    gchar    *uuid;
    gchar    *conn_path;
    gboolean  active;
} NmVpnConnection;

/* Devuelve lista de NmVpnConnection con perfiles wireguard/vpn guardados. */
GSList *nm_get_vpn_connections  (GDBusConnection *conn);
void    nm_vpn_list_free        (GSList *list);

/* ============================================================
 * ACCIONES ASYNC
 *
 * Estas funciones no bloquean el hilo principal. Envían el pedido
 * a NM y vuelven enseguida.
 * ============================================================ */

void nm_disconnect_device_async (GDBusConnection *conn,
                                 const gchar     *device_path);

/* Resultado de pedir una conexión. active_path: la "conexión activa" que
 * representa este intento (seguirla con nm_watch_active_connection), o NULL
 * si NM rechazó el pedido; en ese caso error trae el motivo. new_profile: el
 * perfil que se creó para este intento (solo nm_add_and_activate_connection_async),
 * o NULL. Las cadenas valen solo durante la llamada. */
typedef void (*NmActivateCallback) (const gchar *active_path,
                                    const gchar *new_profile,
                                    const gchar *error,
                                    gpointer     user_data);

/* Activa un perfil guardado en device_path (ap_path puede ser NULL). */
void nm_activate_profile_async (GDBusConnection   *conn,
                                const gchar       *profile_path,
                                const gchar       *device_path,
                                const gchar       *ap_path,
                                NmActivateCallback cb,
                                gpointer           user_data);

/* Guarda un perfil nuevo LIBRE (sin atarlo a ningún adaptador) y después
 * lo activa en device_path. No usa "crear y activar" en un paso porque NM
 * ataría el perfil a ese adaptador.
 * key_mgmt: "wpa-psk" (WPA2 / mixto WPA2-WPA3), "sae" (WPA3 puro) o NULL.
 * Con contraseña y key_mgmt NULL se usa "wpa-psk". Con key_mgmt y sin
 * contraseña, NM pide la clave al agente de secretos al conectar.
 * hidden: TRUE para redes ocultas (que no anuncian su nombre). Marca el
 * perfil como oculto para que NM salga a buscarla activamente. */
void nm_add_and_activate_connection_async (GDBusConnection   *conn,
                                           const gchar       *device_path,
                                           const gchar       *ap_path,
                                           const gchar       *ssid,
                                           const gchar       *password,
                                           const gchar       *key_mgmt,
                                           gboolean           autoconnect,
                                           gboolean           hidden,
                                           NmActivateCallback cb,
                                           gpointer           user_data);

/* ---------- Conexión activa (un intento de conexión en curso) ----------
 * Estados (NMActiveConnectionState): 0 = desconocido o ya no existe,
 * 1 = activándose, 2 = activada, 3 = desactivándose, 4 = desactivada. */
#define NM_ACTIVE_STATE_UNKNOWN      0
#define NM_ACTIVE_STATE_ACTIVATING   1
#define NM_ACTIVE_STATE_ACTIVATED    2
#define NM_ACTIVE_STATE_DEACTIVATING 3
#define NM_ACTIVE_STATE_DEACTIVATED  4

guint32 nm_get_active_state (GDBusConnection *conn, const gchar *active_path);

/* Avisa cada cambio de estado de la conexión activa, con el motivo
 * (NMActiveConnectionStateReason). Devuelve un número para dejar de seguirla
 * con nm_unwatch_active_connection. */
typedef void (*NmActiveStateCallback) (const gchar *active_path,
                                       guint32      state,
                                       guint32      reason,
                                       gpointer     user_data);
guint nm_watch_active_connection   (GDBusConnection      *conn,
                                    const gchar          *active_path,
                                    NmActiveStateCallback cb,
                                    gpointer              user_data);
void  nm_unwatch_active_connection (GDBusConnection *conn, guint watch_id);

void nm_set_device_enabled_async (GDBusConnection *conn,
                                  const gchar     *device_path,
                                  gboolean         enabled);

void nm_activate_vpn_async   (GDBusConnection *conn, const gchar *conn_path);
void nm_deactivate_vpn_async (GDBusConnection *conn, const gchar *conn_path);

/* Suscripción a señales DBus de NetworkManager.
 * Llama a callback(user_data) cada vez que algo relevante cambia, incluidos
 * los perfiles guardados (alta, baja o modificación, hecha por este plugin
 * o por nmcli / el editor de conexiones).
 * Mientras haya al menos una suscripción activa, la lista de perfiles
 * guardados se mantiene en memoria y se descarta sola cuando cambia.
 * Devuelve un array de IDs de suscripción terminado en 0; liberar con nm_unsubscribe_signals(). */
/* Motivo del último fallo de conexión del adaptador (código de NM,
 * NMDeviceStateReason), si ocurrió en o después de `since`
 * (g_get_monotonic_time). 0 = no falló desde entonces. Solo se registra
 * mientras haya alguna suscripción a señales activa. */
#define NM_FAIL_UNKNOWN   1       /* falló sin motivo informado */
#define NM_FAIL_TOO_LONG  100000  /* propio del plugin: venció el tiempo */
guint32 nm_device_last_failure (const gchar *device_path, gint64 since);

typedef void (*NmSignalCallback) (gpointer user_data);

guint *nm_subscribe_signals  (GDBusConnection *conn,
                               NmSignalCallback callback,
                               gpointer         user_data);
void   nm_unsubscribe_signals (GDBusConnection *conn, guint *ids);

/* Detalles de la conexión activa: IP, gateway, DNS.
 * Liberar con nm_connection_details_free(). */
typedef struct {
    gchar  *ip4_address;
    gchar  *ip4_prefix;
    gchar  *ip4_gateway;
    gchar  *ip4_dns;
    gchar  *ip6_address;
} NmConnectionDetails;

NmConnectionDetails *nm_get_connection_details (GDBusConnection *conn,
                                                const gchar     *device_path);
void                 nm_connection_details_free (NmConnectionDetails *d);

/* Cambia el valor de autoconexión del perfil guardado para un SSID. */
gboolean nm_set_autoconnect_by_ssid (GDBusConnection *conn,
                                     const gchar     *ssid,
                                     gboolean         autoconnect);

/* Devuelve el valor de autoconexión del perfil guardado para un SSID. */
gboolean nm_get_autoconnect_by_ssid (GDBusConnection *conn,
                                     const gchar     *ssid);

/* Solicita un escaneo Wi-Fi activo al adaptador. */
void nm_request_scan (GDBusConnection *conn, const gchar *device_path);

/* Devuelve TRUE si algún adaptador Wi-Fi está en proceso de conectar (estados 40-90). */
gboolean nm_any_wifi_device_connecting (GDBusConnection *conn);

/* ---------- Conectividad (portal cautivo) ----------
 * Lo que NM averiguó sobre la salida a Internet. Requiere que la
 * comprobación de conectividad esté activada en la configuración de NM (en
 * Debian viene en el paquete network-manager-config-connectivity-debian);
 * si no, el valor queda en DESCONOCIDA y no se muestra nada. */
#define NM_CONN_STATE_UNKNOWN  0   /* desconocida / comprobación apagada */
#define NM_CONN_STATE_NONE     1   /* sin red */
#define NM_CONN_STATE_PORTAL   2   /* hay que iniciar sesión (bar, hotel...) */
#define NM_CONN_STATE_LIMITED  3   /* red sin salida a Internet */
#define NM_CONN_STATE_FULL     4   /* Internet completo */

guint32 nm_get_connectivity           (GDBusConnection *conn);
/* Dirección que usa NM para comprobar (o NULL). Liberar con g_free(). */
gchar  *nm_get_connectivity_check_uri (GDBusConnection *conn);
/* Pide a NM que vuelva a comprobar ya (el resultado llega por señal). */
void    nm_check_connectivity_async   (GDBusConnection *conn);


#endif /* NM_DBUS_H */
