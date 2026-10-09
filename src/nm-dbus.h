#ifndef NM_DBUS_H
#define NM_DBUS_H

#include <gio/gio.h>

typedef struct {
    gchar    *iface;
    gchar    *object_path;
    gchar    *description;  /* Nombre legible del hardware (puede ser NULL). Liberar con g_free(). */
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

/* El perfil guardado que corresponde a este punto de acceso: misma red, modo
 * cliente (no hotspot) y seguridad compatible con las banderas del AP. Si
 * ninguno es compatible, el primero con ese nombre. NULL si no hay.
 * Liberar con g_free(). */
gchar   *nm_find_profile_for_ap       (GDBusConnection *conn, const gchar *ssid,
                                       guint32 wpa_flags, guint32 rsn_flags);
/* El perfil que el adaptador tiene activo ahora (o NULL). Liberar con g_free(). */
gchar   *nm_get_device_active_profile (GDBusConnection *conn, const gchar *device_path);
gboolean nm_delete_profile            (GDBusConnection *conn, const gchar *profile_path);
gchar   *nm_get_profile_password      (GDBusConnection *conn, const gchar *profile_path);
gboolean nm_get_profile_autoconnect   (GDBusConnection *conn, const gchar *profile_path);
gboolean nm_set_profile_autoconnect   (GDBusConnection *conn, const gchar *profile_path,
                                       gboolean autoconnect);
void     nm_activate_profile_async    (GDBusConnection *conn, const gchar *profile_path,
                                       const gchar *device_path, const gchar *ap_path);
/* Migración: borra duplicados de verdad del perfil (misma red y MISMA
 * seguridad; otro perfil de la misma red con otra seguridad se respeta) y le
 * saca interface-name si lo tiene. Devuelve TRUE si cambió algo. */
gboolean nm_profile_cleanup           (GDBusConnection *conn, const gchar *profile_path);

/* Migra perfiles viejos con `connection.interface-name` fijado:
 *   - Si hay varios perfiles con la misma SSID, deja uno y borra los demás.
 *   - Al perfil que queda, le saca el binding de interface-name (lo vacía),
 *     así NM lo deja usar con cualquier adapter Wi-Fi.
 * Idempotente: si ya está limpio, no hace nada.
 * Devuelve TRUE si modificó algo. */
gboolean nm_strip_interface_name (GDBusConnection *conn, const gchar *ssid);

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
 * a NM y vuelven enseguida. Si querés saber si tuvo éxito o falló,
 * suscribite a las señales DBus de NM y reaccionar al cambio de
 * estado del dispositivo o de la conexión.
 * ============================================================ */

void nm_disconnect_device_async (GDBusConnection *conn,
                                 const gchar     *device_path);

void nm_activate_connection_async (GDBusConnection *conn,
                                   const gchar     *device_path,
                                   const gchar     *ap_path,
                                   const gchar     *ssid);

/* key_mgmt: "wpa-psk" (WPA2 / mixto WPA2-WPA3), "sae" (WPA3 puro) o NULL
 * (equivale a "wpa-psk"). Solo se usa si hay contraseña.
 * hidden: TRUE para redes ocultas (que no anuncian su nombre). Marca el
 * perfil como oculto para que NM salga a buscarla activamente. */
void nm_add_and_activate_connection_async (GDBusConnection *conn,
                                           const gchar     *device_path,
                                           const gchar     *ap_path,
                                           const gchar     *ssid,
                                           const gchar     *password,
                                           const gchar     *key_mgmt,
                                           gboolean         autoconnect,
                                           gboolean         hidden);

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
