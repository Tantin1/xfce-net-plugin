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
    gint     strength;
    guint    frequency;
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

/* Solicita un escaneo Wi-Fi activo al adaptador. */
void nm_request_scan (GDBusConnection *conn, const gchar *device_path);

#endif /* NM_DBUS_H */
