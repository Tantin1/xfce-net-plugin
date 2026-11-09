/* attempts.h — seguidor de intentos de conexión (nov-2026).
 *
 * Un solo lugar sabe qué intento de conexión (o desconexión) hay en curso,
 * cuál terminó y por qué. Lo usan el panel (spinner, notificaciones), el
 * popup (filas, zona superior, menú) y el diálogo de red oculta.
 *
 * Reemplaza a las cuatro tablas y cuatro relojes que vivían en el popup
 * (operaciones en curso, intentos pendientes, redes fallidas y respaldos de
 * contraseña). A diferencia de aquellas, vive en el plugin del panel, que
 * está siempre vivo: cerrar y reabrir el popup no pierde nada ni obliga a
 * adivinar qué pasó mirando la hora.
 *
 * Cómo sabe qué pasó: al pedir una conexión, NetworkManager devuelve la
 * "conexión activa" de ese intento, que avisa cada cambio de estado con su
 * motivo. El seguidor la escucha. El único reloj es uno de seguridad, largo,
 * por si NetworkManager no avisara nunca nada.
 *
 * Un registro por red y adaptador (clave "red|adaptador", ver
 * make_ssid_dev_key). Un adaptador tiene a lo sumo un intento en curso;
 * los fallos de distintas redes se conservan hasta que el usuario hace algo
 * con esa red (reintenta, la olvida) o se conecta bien. */
#ifndef ATTEMPTS_H
#define ATTEMPTS_H

#include <gio/gio.h>

typedef enum {
    ATTEMPT_CONNECTING,     /* pedida, NetworkManager trabajando */
    ATTEMPT_CONNECTED,      /* conectó; se sigue mirando un rato por si se cae */
    ATTEMPT_FAILED,         /* falló; `reason` dice por qué */
    ATTEMPT_DISCONNECTING   /* se pidió desconectar */
} AttemptState;

/* Qué acaba de pasar, para quien escucha los avisos. */
typedef enum {
    ATTEMPT_EVENT_STARTED,       /* empezó un intento (conectar o desconectar) */
    ATTEMPT_EVENT_CONNECTED,     /* conectó */
    ATTEMPT_EVENT_FAILED,        /* falló */
    ATTEMPT_EVENT_DISCONNECTED,  /* terminó la desconexión pedida */
    ATTEMPT_EVENT_CLEARED        /* se borró el registro (sin novedad visible) */
} AttemptEvent;

/* Vista de solo lectura de un registro. Vale hasta el próximo aviso. */
typedef struct {
    const gchar  *ssid;
    const gchar  *device_path;
    AttemptState  state;
    guint32       reason;        /* motivo del fallo (ver failure_text) */
    gboolean      from_hidden;   /* lo pidió el diálogo de red oculta */
} Attempt;

/* Pedido de conexión. Quien llama prepara el perfil (elegirlo, cambiarle la
 * clave, marcarlo como oculto) y le dice al seguidor cómo deshacerlo si el
 * intento falla. */
typedef struct {
    const gchar *ssid;
    const gchar *device_path;
    const gchar *ap_path;            /* punto de acceso, o NULL */
    /* Perfil existente a activar. Si es NULL se crea uno nuevo con: */
    const gchar *profile_path;
    const gchar *password;           /* clave del perfil nuevo, o NULL */
    const gchar *key_mgmt;           /* "wpa-psk", "sae", "owe" o NULL */
    gboolean     autoconnect;
    gboolean     hidden;             /* red oculta */
    gboolean     from_hidden;        /* lo pide el diálogo de red oculta */
    /* Qué deshacer si falla: */
    const gchar *restore_password;   /* clave anterior de profile_path */
    gboolean     restore_unhide;     /* sacarle a profile_path la marca "oculta" */
    gboolean     delete_new_profile; /* borrar el perfil creado para el intento */
} AttemptRequest;

/* Arma la clave "ssid|device_path". Un solo lugar para armarla, así nunca
 * queda inconsistente. Liberar con g_free(). */
static inline gchar *
make_ssid_dev_key (const gchar *ssid, const gchar *device_path)
{
    return g_strdup_printf ("%s|%s", ssid ? ssid : "",
                            device_path ? device_path : "");
}

/* El seguidor es uno solo por proceso, compartido por todas las copias del
 * plugin. Cada copia lo pide al arrancar y lo suelta al quitarse. */
void attempts_acquire (GDBusConnection *conn);
void attempts_release (void);

/* Conectar / desconectar a través del seguidor. */
void attempts_connect    (const AttemptRequest *req);
void attempts_disconnect (const gchar *ssid, const gchar *device_path);

/* Registro de esa red en ese adaptador, o NULL si no hay. */
gboolean attempts_lookup (const gchar *ssid, const gchar *device_path,
                          Attempt *out);
/* ¿Hay un intento de conexión en curso en el adaptador (cualquier red)? */
gboolean attempts_device_busy (const gchar *device_path);
/* Red de algún intento de conexión en curso, o NULL. No liberar. */
const gchar *attempts_connecting_ssid (void);
/* ¿Hay algún intento de conexión en curso? (para el spinner del panel) */
gboolean attempts_any_connecting (void);

/* Borra el registro (por ejemplo, la marca de fallo) de esa red en ese
 * adaptador, o de esa red en todos los adaptadores (al olvidarla). */
void attempts_clear      (const gchar *ssid, const gchar *device_path);
void attempts_clear_ssid (const gchar *ssid);

/* Avisos: `cb` se llama con la red, el adaptador y qué pasó. Para saber el
 * estado nuevo, attempts_lookup. Se puede quitar el aviso desde adentro del
 * mismo aviso. */
typedef void (*AttemptsListener) (const gchar  *ssid,
                                  const gchar  *device_path,
                                  AttemptEvent  event,
                                  gpointer      user_data);
guint attempts_add_listener    (AttemptsListener cb, gpointer user_data);
void  attempts_remove_listener (guint id);

/* Texto corto para el usuario según el motivo del fallo (código de NM o
 * NM_FAIL_*). Nunca NULL. */
const gchar *failure_text (guint32 reason);

#endif /* ATTEMPTS_H */
