#ifndef NM_DBUS_H
#define NM_DBUS_H

#include <gio/gio.h>

typedef struct {
    gchar    *iface;
    gchar    *object_path;
    gboolean  enabled;
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
void    nm_device_list_free  (GSList *list);

GSList *nm_get_access_points (GDBusConnection *conn, const gchar *device_path);
void    nm_ap_list_free      (GSList *list);

GDBusConnection *nm_dbus_connect         (void);
gboolean         nm_has_saved_connection (GDBusConnection *conn, const gchar *ssid);
gboolean         nm_forget_connection    (GDBusConnection *conn, const gchar *ssid);

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

/* Devuelve lista de NmDevice Ethernet con cable conectado (State == 100) */
GSList *nm_get_ethernet_devices (GDBusConnection *conn);

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

void nm_add_and_activate_connection_async (GDBusConnection *conn,
                                           const gchar     *device_path,
                                           const gchar     *ap_path,
                                           const gchar     *ssid,
                                           const gchar     *password,
                                           gboolean         autoconnect);

void nm_set_device_enabled_async (GDBusConnection *conn,
                                  const gchar     *device_path,
                                  gboolean         enabled);

void nm_activate_vpn_async   (GDBusConnection *conn, const gchar *conn_path);
void nm_deactivate_vpn_async (GDBusConnection *conn, const gchar *conn_path);

/* Suscripción a señales DBus de NetworkManager.
 * Llama a callback(user_data) cada vez que algo relevante cambia.
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

/* Solicita un escaneo Wi-Fi activo al adaptador. */
void nm_request_scan (GDBusConnection *conn, const gchar *device_path);

/* Devuelve TRUE si algún adaptador Wi-Fi está en proceso de conectar (estados 40-90). */
gboolean nm_any_wifi_device_connecting (GDBusConnection *conn);

/* Devuelve el device_path del primer adaptador Wi-Fi con capacidad AP, o NULL. */
gchar *nm_find_ap_capable_device (GDBusConnection *conn);

/* Estado del hotspot: devuelve TRUE si hay un perfil AP activo.
 * Si activo, rellena *ssid_out y *pass_out (liberar con g_free). */
gboolean nm_get_hotspot_state (GDBusConnection *conn,
                               gchar          **ssid_out,
                               gchar          **pass_out);

/* Crea y activa un hotspot WPA2 en el dispositivo indicado. Async.
 * config_path: ruta al .ini del plugin para persistir el UUID del perfil
 * y evitar duplicados en activaciones sucesivas. Puede ser NULL. */
void nm_create_hotspot_async (GDBusConnection *conn,
                              const gchar     *device_path,
                              const gchar     *ssid,
                              const gchar     *password,
                              const gchar     *config_path);

/* Devuelve TRUE si tiene sentido mostrar el boton hotspot:
 * 2+ adaptadores Wi-Fi, o 1 Wi-Fi con cap AP + Ethernet activo. */
gboolean nm_hotspot_should_show (GDBusConnection *conn);

/* Devuelve TRUE si NM está configurado con dns=dnsmasq (necesario para
 * que los clientes del hotspot reciban IP). */
gboolean nm_check_hotspot_prerequisites (void);

/* Detiene el hotspot activo. Async. */
void nm_stop_hotspot_async (GDBusConnection *conn);

typedef struct {
    gchar    *conn_path;
    gboolean  orig_autoconnect;
} NmAutoconnectState;

/* Lee autoconnect de todos los perfiles Wi-Fi que NO pertenecen a
 * ap_device_path, lo pone en FALSE y devuelve una GSList de
 * NmAutoconnectState con los valores originales. */
GSList *nm_disable_wifi_autoconnect (GDBusConnection *conn,
                                     const gchar     *ap_device_path);

/* Restaura cada perfil al valor original y libera la lista. */
void nm_restore_wifi_autoconnect (GDBusConnection *conn, GSList *states);

#endif /* NM_DBUS_H */
