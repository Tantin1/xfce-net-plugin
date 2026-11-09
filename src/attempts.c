/* attempts.c — seguidor de intentos de conexión. Ver attempts.h.
 *
 * Ciclo de un intento de conexión:
 *
 *   attempts_connect ──► CONECTANDO ──(conexión activa: "activada")──► CONECTADO
 *                           │                                             │
 *                           │ "desactivada", NM rechazó el pedido,        │ pasan
 *                           │ o el reloj de seguridad                     │ SETTLE_S
 *                           ▼                                             ▼
 *                         FALLÓ (con motivo; se deshace lo pedido)   registro borrado
 *
 * Si la conexión se cae mientras está CONECTADO (dentro de SETTLE_S), pasa a
 * FALLÓ igual: algunos adaptadores asocian y se caen enseguida.
 *
 * Desconectar: DESCONECTANDO hasta que el adaptador queda desconectado (o
 * vence DISCONNECT_S); después se borra el registro. */

#include "attempts.h"
#include "nm-dbus.h"
/* gi18n-lib: _() busca siempre en NUESTRO dominio de traducción
 * (GETTEXT_PACKAGE), también en Wayland, donde todos los plugins comparten
 * el proceso del panel. */
#include <glib/gi18n-lib.h>

/* Reloj de seguridad: solo por si NetworkManager no avisa nunca nada. Él
 * mismo da por fallido un intento que no progresa mucho antes de esto. */
#define SAFETY_S      180
/* Después de conectar, cuánto se sigue mirando por si se cae enseguida. */
#define SETTLE_S       10
/* Desconectar: si en este tiempo el adaptador no quedó desconectado, se
 * abandona (la fila vuelve a mostrar el estado real). */
#define DISCONNECT_S   20

/* Estados del adaptador que interesan acá. */
#define DEV_STATE_DISCONNECTED 30
#define DEV_STATE_NEED_AUTH    60
#define DEV_STATE_ACTIVATED   100

typedef struct {
    gchar        *key;            /* "ssid|device_path" */
    gchar        *ssid;
    gchar        *device_path;
    AttemptState  state;
    guint32       reason;
    gboolean      from_hidden;
    guint         serial;         /* identifica el intento ante respuestas tardías */
    gint64        started;        /* g_get_monotonic_time() */
    gchar        *active_path;    /* conexión activa que devolvió NM */
    guint         watch_id;       /* aviso de cambios de esa conexión activa */
    guint         timer_id;       /* reloj del estado actual */
    /* Qué deshacer si falla. */
    gchar        *undo_profile;
    gchar        *undo_password;
    gboolean      undo_unhide;
    gboolean      delete_new;
    gchar        *new_profile;    /* perfil creado para este intento */
} Rec;

typedef struct {
    guint            id;
    AttemptsListener cb;
    gpointer         user_data;
} Listener;

static struct {
    gint             refs;
    GDBusConnection *conn;
    GHashTable      *recs;          /* key → Rec* */
    guint           *signal_ids;
    guint            eval_idle_id;
    GList           *listeners;     /* Listener* */
    guint            next_listener_id;
    guint            next_serial;
} T;

/* ---------- utilidades ---------- */

/* Borra una contraseña de la memoria antes de liberarla. */
static void
wipe_free (gchar *s)
{
    if (!s) return;
    volatile gchar *p = s;
    while (*p) { *p = '\0'; p++; }
    g_free (s);
}

static void
rec_stop_watching (Rec *r)
{
    if (r->watch_id) {
        nm_unwatch_active_connection (T.conn, r->watch_id);
        r->watch_id = 0;
    }
    if (r->timer_id) {
        g_source_remove (r->timer_id);
        r->timer_id = 0;
    }
}

static void
rec_forget_undo (Rec *r)
{
    g_clear_pointer (&r->undo_profile, g_free);
    g_clear_pointer (&r->undo_password, wipe_free);
    r->undo_unhide = FALSE;
    r->delete_new  = FALSE;
}

static void
rec_free (gpointer p)
{
    Rec *r = p;
    rec_stop_watching (r);
    rec_forget_undo (r);
    g_free (r->key);
    g_free (r->ssid);
    g_free (r->device_path);
    g_free (r->active_path);
    g_free (r->new_profile);
    g_free (r);
}

static Rec *
rec_by_serial (guint serial)
{
    if (!T.recs) return NULL;
    GHashTableIter it;
    gpointer       v;
    g_hash_table_iter_init (&it, T.recs);
    while (g_hash_table_iter_next (&it, NULL, &v))
        if (((Rec *) v)->serial == serial)
            return v;
    return NULL;
}

/* Avisa a todos. Se trabaja sobre una copia de los números de aviso: un
 * aviso puede quitarse a sí mismo (o a otro) mientras se recorre. */
static void
emit (const gchar *ssid, const gchar *device_path, AttemptEvent ev)
{
    gchar  *s = g_strdup (ssid);
    gchar  *d = g_strdup (device_path);
    GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint));
    for (GList *l = T.listeners; l; l = l->next)
        g_array_append_val (ids, ((Listener *) l->data)->id);

    for (guint i = 0; i < ids->len; i++) {
        guint id = g_array_index (ids, guint, i);
        for (GList *l = T.listeners; l; l = l->next) {
            Listener *ls = l->data;
            if (ls->id == id) {
                ls->cb (s, d, ev, ls->user_data);
                break;
            }
        }
    }
    g_array_free (ids, TRUE);
    g_free (s);
    g_free (d);
}

/* Quita el registro y avisa. */
static void
rec_remove (Rec *r, AttemptEvent ev)
{
    gchar *s = g_strdup (r->ssid);
    gchar *d = g_strdup (r->device_path);
    g_hash_table_remove (T.recs, r->key);   /* libera r */
    emit (s, d, ev);
    g_free (s);
    g_free (d);
}

/* Deshace lo que se le cambió al perfil para este intento. */
static void
rec_undo (Rec *r)
{
    if (r->undo_profile && r->undo_password)
        nm_set_profile_password (T.conn, r->undo_profile, r->undo_password);
    if (r->undo_profile && r->undo_unhide)
        nm_set_profile_hidden (T.conn, r->undo_profile, FALSE);
    if (r->delete_new && r->new_profile)
        nm_delete_profile (T.conn, r->new_profile);
    rec_forget_undo (r);
}

/* Motivo de la conexión activa (NMActiveConnectionStateReason) pasado al
 * código del adaptador que entiende failure_text. Se usa solo si el
 * adaptador no informó uno propio, que es más preciso. */
static guint32
reason_from_active (guint32 r)
{
    switch (r) {
    case 9:  return 7;                  /* NO_SECRETS: clave mala o cancelada */
    case 5:  return 5;                  /* IP_CONFIG_INVALID */
    case 6:  return NM_FAIL_TOO_LONG;   /* CONNECT_TIMEOUT */
    default: return NM_FAIL_UNKNOWN;
    }
}

static void
rec_fail (Rec *r, guint32 reason)
{
    rec_stop_watching (r);
    rec_undo (r);
    r->state  = ATTEMPT_FAILED;
    r->reason = reason ? reason : NM_FAIL_UNKNOWN;
    emit (r->ssid, r->device_path, ATTEMPT_EVENT_FAILED);
}

static gboolean
settle_done_cb (gpointer data)
{
    Rec *r = rec_by_serial (GPOINTER_TO_UINT (data));
    if (!r) return G_SOURCE_REMOVE;
    r->timer_id = 0;
    rec_stop_watching (r);
    rec_remove (r, ATTEMPT_EVENT_CLEARED);
    return G_SOURCE_REMOVE;
}

static void
rec_connected (Rec *r)
{
    if (r->timer_id) {
        g_source_remove (r->timer_id);
        r->timer_id = 0;
    }
    /* La clave nueva sirvió: ya no hay nada que deshacer. */
    rec_forget_undo (r);
    r->state  = ATTEMPT_CONNECTED;
    r->reason = 0;
    /* Se sigue escuchando la conexión activa un rato (watch_id sigue). */
    r->timer_id = g_timeout_add_seconds (SETTLE_S, settle_done_cb,
                                         GUINT_TO_POINTER (r->serial));
    emit (r->ssid, r->device_path, ATTEMPT_EVENT_CONNECTED);
}

/* El adaptador falló: ¿informó por qué? Si no, el motivo de la conexión. */
static guint32
best_reason (Rec *r, guint32 active_reason)
{
    guint32 dev = nm_device_last_failure (r->device_path, r->started);
    return dev ? dev : reason_from_active (active_reason);
}

/* ---------- conexión activa: avisos y respuesta al pedido ---------- */

static void
on_active_state (const gchar *active_path, guint32 state, guint32 reason,
                 gpointer data)
{
    (void) active_path;
    Rec *r = rec_by_serial (GPOINTER_TO_UINT (data));
    if (!r) return;

    if (state == NM_ACTIVE_STATE_ACTIVATED && r->state == ATTEMPT_CONNECTING)
        rec_connected (r);
    else if (state == NM_ACTIVE_STATE_DEACTIVATED &&
             (r->state == ATTEMPT_CONNECTING || r->state == ATTEMPT_CONNECTED))
        rec_fail (r, best_reason (r, reason));
}

static void
on_activate_reply (const gchar *active_path, const gchar *new_profile,
                   const gchar *error, gpointer data)
{
    Rec *r = rec_by_serial (GPOINTER_TO_UINT (data));
    if (!r || r->state != ATTEMPT_CONNECTING)
        return;   /* intento reemplazado o cancelado mientras tanto */

    if (new_profile && !r->new_profile)
        r->new_profile = g_strdup (new_profile);

    if (!active_path) {
        /* NM rechazó el pedido: no hay cambio de estado que lo avise. */
        g_debug ("attempts: pedido rechazado: %s", error ? error : "?");
        rec_fail (r, best_reason (r, 0));
        return;
    }

    r->active_path = g_strdup (active_path);
    r->watch_id    = nm_watch_active_connection (T.conn, active_path,
                                                 on_active_state,
                                                 GUINT_TO_POINTER (r->serial));

    /* Puede haber cambiado de estado antes de empezar a escuchar. */
    guint32 st = nm_get_active_state (T.conn, active_path);
    if (st == NM_ACTIVE_STATE_ACTIVATED) {
        rec_connected (r);
    } else if (st == NM_ACTIVE_STATE_DEACTIVATED ||
               st == NM_ACTIVE_STATE_UNKNOWN) {
        /* Ya terminó (o el objeto ya no existe): decide el adaptador. */
        if (nm_get_device_state (T.conn, r->device_path) == DEV_STATE_ACTIVATED &&
            !nm_device_last_failure (r->device_path, r->started))
            rec_connected (r);
        else
            rec_fail (r, best_reason (r, 0));
    }
}

static gboolean
safety_cb (gpointer data)
{
    Rec *r = rec_by_serial (GPOINTER_TO_UINT (data));
    if (!r) return G_SOURCE_REMOVE;
    /* Esperando que el usuario escriba la contraseña (diálogo del agente):
     * no es una demora de NM, se le da otra vuelta. */
    if (nm_get_device_state (T.conn, r->device_path) == DEV_STATE_NEED_AUTH)
        return G_SOURCE_CONTINUE;
    r->timer_id = 0;
    rec_fail (r, NM_FAIL_TOO_LONG);
    return G_SOURCE_REMOVE;
}

/* ---------- desconexión ---------- */

static gboolean
disconnect_timeout_cb (gpointer data)
{
    Rec *r = rec_by_serial (GPOINTER_TO_UINT (data));
    if (!r) return G_SOURCE_REMOVE;
    r->timer_id = 0;
    rec_remove (r, ATTEMPT_EVENT_DISCONNECTED);
    return G_SOURCE_REMOVE;
}

/* Revisa las desconexiones pedidas (al llegar cualquier aviso de NM). */
static gboolean
eval_idle_cb (gpointer data)
{
    (void) data;
    T.eval_idle_id = 0;
    if (!T.recs) return G_SOURCE_REMOVE;

    GSList        *done = NULL;
    GHashTableIter it;
    gpointer       v;
    g_hash_table_iter_init (&it, T.recs);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
        Rec *r = v;
        if (r->state != ATTEMPT_DISCONNECTING) continue;
        guint32 st = nm_get_device_state (T.conn, r->device_path);
        if (st > 0 && st <= DEV_STATE_DISCONNECTED)
            done = g_slist_prepend (done, GUINT_TO_POINTER (r->serial));
    }
    for (GSList *l = done; l; l = l->next) {
        Rec *r = rec_by_serial (GPOINTER_TO_UINT (l->data));
        if (r) rec_remove (r, ATTEMPT_EVENT_DISCONNECTED);
    }
    g_slist_free (done);
    return G_SOURCE_REMOVE;
}

static void
on_nm_signal (gpointer data)
{
    (void) data;
    if (!T.eval_idle_id)
        T.eval_idle_id = g_idle_add (eval_idle_cb, NULL);
}

/* Un adaptador hace una sola cosa a la vez: al empezar algo nuevo en él se
 * abandonan su conexión o desconexión en curso (NM cancela la anterior).
 * Los fallos de otras redes se conservan. */
static void
cancel_device_work (const gchar *device_path)
{
    GSList        *gone = NULL;
    GHashTableIter it;
    gpointer       v;
    g_hash_table_iter_init (&it, T.recs);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
        Rec *r = v;
        if (g_strcmp0 (r->device_path, device_path) == 0 &&
            (r->state == ATTEMPT_CONNECTING || r->state == ATTEMPT_DISCONNECTING ||
             r->state == ATTEMPT_CONNECTED))
            gone = g_slist_prepend (gone, GUINT_TO_POINTER (r->serial));
    }
    for (GSList *l = gone; l; l = l->next) {
        Rec *r = rec_by_serial (GPOINTER_TO_UINT (l->data));
        if (!r) continue;
        rec_stop_watching (r);
        rec_undo (r);
        rec_remove (r, ATTEMPT_EVENT_CLEARED);
    }
    g_slist_free (gone);
}

static Rec *
rec_new (const gchar *ssid, const gchar *device_path, AttemptState state)
{
    Rec *r = g_new0 (Rec, 1);
    r->ssid        = g_strdup (ssid ? ssid : "");
    r->device_path = g_strdup (device_path);
    r->key         = make_ssid_dev_key (r->ssid, device_path);
    r->state       = state;
    r->serial      = ++T.next_serial;
    r->started     = g_get_monotonic_time ();
    /* Mismo par red|adaptador: el registro viejo (un fallo anterior) se
     * reemplaza. */
    g_hash_table_replace (T.recs, g_strdup (r->key), r);
    return r;
}

/* ---------- API ---------- */

void
attempts_acquire (GDBusConnection *conn)
{
    if (T.refs++ > 0)
        return;
    T.conn = conn ? g_object_ref (conn) : NULL;
    T.recs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, rec_free);
    if (T.conn)
        T.signal_ids = nm_subscribe_signals (T.conn, on_nm_signal, NULL);
}

void
attempts_release (void)
{
    if (T.refs <= 0 || --T.refs > 0)
        return;
    if (T.eval_idle_id) {
        g_source_remove (T.eval_idle_id);
        T.eval_idle_id = 0;
    }
    if (T.signal_ids) {
        nm_unsubscribe_signals (T.conn, T.signal_ids);
        T.signal_ids = NULL;
    }
    g_clear_pointer (&T.recs, g_hash_table_destroy);
    g_list_free_full (T.listeners, g_free);
    T.listeners = NULL;
    g_clear_object (&T.conn);
}

void
attempts_connect (const AttemptRequest *req)
{
    if (!T.conn || !T.recs || !req || !req->device_path)
        return;

    cancel_device_work (req->device_path);

    Rec *r = rec_new (req->ssid, req->device_path, ATTEMPT_CONNECTING);
    r->from_hidden = req->from_hidden;
    if (req->profile_path) {
        r->undo_profile  = g_strdup (req->profile_path);
        r->undo_password = g_strdup (req->restore_password);
        r->undo_unhide   = req->restore_unhide;
    }
    r->delete_new = req->delete_new_profile;
    r->timer_id   = g_timeout_add_seconds (SAFETY_S, safety_cb,
                                           GUINT_TO_POINTER (r->serial));
    guint serial = r->serial;
    emit (r->ssid, r->device_path, ATTEMPT_EVENT_STARTED);

    /* La respuesta puede llegar en el acto (pedido inválido): por eso el
     * registro ya está anotado antes de pedir. */
    if (req->profile_path)
        nm_activate_profile_async (T.conn, req->profile_path, req->device_path,
                                   req->ap_path, on_activate_reply,
                                   GUINT_TO_POINTER (serial));
    else
        nm_add_and_activate_connection_async (T.conn, req->device_path,
                                              req->ap_path, req->ssid,
                                              req->password, req->key_mgmt,
                                              req->autoconnect, req->hidden,
                                              on_activate_reply,
                                              GUINT_TO_POINTER (serial));
}

void
attempts_disconnect (const gchar *ssid, const gchar *device_path)
{
    if (!T.conn || !T.recs || !device_path)
        return;
    cancel_device_work (device_path);
    Rec *r = rec_new (ssid, device_path, ATTEMPT_DISCONNECTING);
    r->timer_id = g_timeout_add_seconds (DISCONNECT_S, disconnect_timeout_cb,
                                         GUINT_TO_POINTER (r->serial));
    emit (r->ssid, r->device_path, ATTEMPT_EVENT_STARTED);
    nm_disconnect_device_async (T.conn, device_path);
}

gboolean
attempts_lookup (const gchar *ssid, const gchar *device_path, Attempt *out)
{
    if (!T.recs || !ssid || !device_path)
        return FALSE;
    gchar *key = make_ssid_dev_key (ssid, device_path);
    Rec   *r   = g_hash_table_lookup (T.recs, key);
    g_free (key);
    if (!r)
        return FALSE;
    if (out) {
        out->ssid        = r->ssid;
        out->device_path = r->device_path;
        out->state       = r->state;
        out->reason      = r->reason;
        out->from_hidden = r->from_hidden;
    }
    return TRUE;
}

gboolean
attempts_device_busy (const gchar *device_path)
{
    if (!T.recs || !device_path) return FALSE;
    GHashTableIter it;
    gpointer       v;
    g_hash_table_iter_init (&it, T.recs);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
        Rec *r = v;
        if (r->state == ATTEMPT_CONNECTING &&
            g_strcmp0 (r->device_path, device_path) == 0)
            return TRUE;
    }
    return FALSE;
}

const gchar *
attempts_connecting_ssid (void)
{
    if (!T.recs) return NULL;
    GHashTableIter it;
    gpointer       v;
    g_hash_table_iter_init (&it, T.recs);
    while (g_hash_table_iter_next (&it, NULL, &v))
        if (((Rec *) v)->state == ATTEMPT_CONNECTING)
            return ((Rec *) v)->ssid;
    return NULL;
}

gboolean
attempts_any_connecting (void)
{
    return attempts_connecting_ssid () != NULL;
}

void
attempts_clear (const gchar *ssid, const gchar *device_path)
{
    if (!T.recs || !ssid || !device_path) return;
    gchar *key = make_ssid_dev_key (ssid, device_path);
    Rec   *r   = g_hash_table_lookup (T.recs, key);
    g_free (key);
    /* Un intento en curso no se borra así: termina solo. */
    if (r && (r->state == ATTEMPT_FAILED || r->state == ATTEMPT_CONNECTED))
        rec_remove (r, ATTEMPT_EVENT_CLEARED);
}

void
attempts_clear_ssid (const gchar *ssid)
{
    if (!T.recs || !ssid) return;
    GSList        *gone = NULL;
    GHashTableIter it;
    gpointer       v;
    g_hash_table_iter_init (&it, T.recs);
    while (g_hash_table_iter_next (&it, NULL, &v))
        if (g_strcmp0 (((Rec *) v)->ssid, ssid) == 0)
            gone = g_slist_prepend (gone, GUINT_TO_POINTER (((Rec *) v)->serial));
    for (GSList *l = gone; l; l = l->next) {
        Rec *r = rec_by_serial (GPOINTER_TO_UINT (l->data));
        if (!r) continue;
        /* Se olvidó la red: sus perfiles ya no existen, nada que deshacer. */
        rec_stop_watching (r);
        rec_forget_undo (r);
        rec_remove (r, ATTEMPT_EVENT_CLEARED);
    }
    g_slist_free (gone);
}

guint
attempts_add_listener (AttemptsListener cb, gpointer user_data)
{
    Listener *ls = g_new0 (Listener, 1);
    ls->id        = ++T.next_listener_id;
    ls->cb        = cb;
    ls->user_data = user_data;
    T.listeners   = g_list_append (T.listeners, ls);
    return ls->id;
}

void
attempts_remove_listener (guint id)
{
    for (GList *l = T.listeners; l; l = l->next) {
        Listener *ls = l->data;
        if (ls->id == id) {
            T.listeners = g_list_delete_link (T.listeners, l);
            g_free (ls);
            return;
        }
    }
}

/* Motivos de NM (NMDeviceStateReason) que interesan para explicar un fallo. */
const gchar *
failure_text (guint32 reason)
{
    switch (reason) {
    case 7:                         /* NO_SECRETS: clave mala o cancelada */
        return _("Incorrect password");
    case 8:                         /* SUPPLICANT_DISCONNECT */
    case 9:                         /* SUPPLICANT_CONFIG_FAILED */
        return _("The network rejected the connection");
    case 10:                        /* SUPPLICANT_FAILED */
    case 11:                        /* SUPPLICANT_TIMEOUT */
        return _("The network did not respond");
    case 53:                        /* SSID_NOT_FOUND */
        return _("Network not found");
    case 5:                         /* IP_CONFIG_UNAVAILABLE */
    case 6:                         /* IP_CONFIG_EXPIRED */
    case 15:                        /* DHCP_START_FAILED */
    case 16:                        /* DHCP_ERROR */
    case 17:                        /* DHCP_FAILED */
        return _("Did not get an address from the network");
    case NM_FAIL_TOO_LONG:
        return _("The connection took too long");
    default:
        return _("Connection failed");
    }
}
