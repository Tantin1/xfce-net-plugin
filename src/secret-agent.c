/* secret-agent.c — agente de secretos de NetworkManager (ver secret-agent.h).
 *
 * Cómo funciona:
 *   1. Publicamos en el bus del sistema un objeto en la ruta fija
 *      /org/freedesktop/NetworkManager/SecretAgent con la interfaz
 *      org.freedesktop.NetworkManager.SecretAgent (cuatro métodos que
 *      NetworkManager llama: GetSecrets, CancelGetSecrets, SaveSecrets y
 *      DeleteSecrets).
 *   2. Nos registramos en el AgentManager de NetworkManager. Si
 *      NetworkManager se reinicia, nos volvemos a registrar solos (vigilamos
 *      su nombre en el bus).
 *   3. Cuando NM necesita una contraseña llama a GetSecrets. Si es de Wi-Fi
 *      personal, mostramos un diálogo y la respuesta sale recién cuando el
 *      usuario aprieta Conectar o Cancelar (la llamada queda "abierta").
 *      Si no es un pedido que sepamos atender, contestamos NoSecrets ("no
 *      tengo"), y NM prueba con el siguiente agente.
 *
 * Seguridad: solo se atienden llamadas que vienen de NetworkManager (se
 * compara el remitente con el dueño actual de su nombre en el bus). */

#include "secret-agent.h"
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <string.h>

#define NM_BUS_NAME            "org.freedesktop.NetworkManager"
#define NM_AGENT_MANAGER_PATH  "/org/freedesktop/NetworkManager/AgentManager"
#define NM_AGENT_MANAGER_IFACE "org.freedesktop.NetworkManager.AgentManager"
#define NM_SECRET_AGENT_PATH   "/org/freedesktop/NetworkManager/SecretAgent"
#define NM_SECRET_AGENT_IFACE  "org.freedesktop.NetworkManager.SecretAgent"

/* Identificador con el que nos presentamos ante NM. Si otra instancia del
 * plugin (otro proceso del mismo usuario) ya está registrada con este
 * nombre, NM rechaza la segunda: así nunca salen dos diálogos iguales. */
#define AGENT_IDENTIFIER       "org.xfce.netplugin"

/* Errores que NM entiende como respuesta a GetSecrets. */
#define ERR_NO_SECRETS         NM_SECRET_AGENT_IFACE ".NoSecrets"
#define ERR_USER_CANCELED      NM_SECRET_AGENT_IFACE ".UserCanceled"
#define ERR_AGENT_CANCELED     NM_SECRET_AGENT_IFACE ".AgentCanceled"
#define ERR_PERMISSION_DENIED  NM_SECRET_AGENT_IFACE ".PermissionDenied"

/* Banderas de GetSecrets (NMSecretAgentGetSecretsFlags). */
#define GET_SECRETS_ALLOW_INTERACTION 0x1   /* se puede preguntar al usuario */
#define GET_SECRETS_REQUEST_NEW       0x2   /* la anterior fue rechazada */

#define WIFI_SECURITY_SETTING "802-11-wireless-security"

static const gchar agent_xml[] =
    "<node>"
    "  <interface name='" NM_SECRET_AGENT_IFACE "'>"
    "    <method name='GetSecrets'>"
    "      <arg name='connection'      type='a{sa{sv}}' direction='in'/>"
    "      <arg name='connection_path' type='o'         direction='in'/>"
    "      <arg name='setting_name'    type='s'         direction='in'/>"
    "      <arg name='hints'           type='as'        direction='in'/>"
    "      <arg name='flags'           type='u'         direction='in'/>"
    "      <arg name='secrets'         type='a{sa{sv}}' direction='out'/>"
    "    </method>"
    "    <method name='CancelGetSecrets'>"
    "      <arg name='connection_path' type='o' direction='in'/>"
    "      <arg name='setting_name'    type='s' direction='in'/>"
    "    </method>"
    "    <method name='SaveSecrets'>"
    "      <arg name='connection'      type='a{sa{sv}}' direction='in'/>"
    "      <arg name='connection_path' type='o'         direction='in'/>"
    "    </method>"
    "    <method name='DeleteSecrets'>"
    "      <arg name='connection'      type='a{sa{sv}}' direction='in'/>"
    "      <arg name='connection_path' type='o'         direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

struct _NetSecretAgent {
    GDBusConnection        *conn;          /* prestada */
    GDBusNodeInfo          *node_info;
    guint                   object_id;     /* objeto publicado en el bus */
    guint                   watch_id;      /* vigilancia del nombre de NM */
    gchar                  *nm_owner;      /* dueño actual del nombre de NM */
    gboolean                registered;
    GList                  *requests;      /* SecretRequest* abiertos */
    NetSecretAgentPromptCb  prompt_cb;
    gpointer                prompt_data;
};

/* Un pedido de contraseña abierto (un diálogo en pantalla). */
typedef struct {
    NetSecretAgent        *agent;
    GDBusMethodInvocation *invocation;    /* NULL una vez contestado */
    gchar                 *conn_path;
    gchar                 *setting_name;
    gchar                 *field;         /* "psk" o "wep-key0" */
    gchar                 *key_mgmt;
    GtkWidget             *dialog;
    GtkWidget             *entry;
} SecretRequest;

/* ---------- utilidades ---------- */

static void
wipe_string (gchar *s)
{
    if (!s) return;
    volatile gchar *p = s;
    while (*p) { *p = '\0'; p++; }
}

/* Contesta la llamada abierta con un error (una sola vez). */
static void
request_reply_error (SecretRequest *req, const gchar *error_name,
                     const gchar *message)
{
    if (!req->invocation)
        return;
    g_dbus_method_invocation_return_dbus_error (req->invocation,
                                                error_name, message);
    req->invocation = NULL;
}

/* Se dispara al destruirse el diálogo, sea por la razón que sea. */
static void
on_request_dialog_destroy (GtkWidget *dialog, SecretRequest *req)
{
    (void) dialog;
    /* Si nadie contestó todavía (ventana cerrada con la X), es una
     * cancelación del usuario. */
    request_reply_error (req, ERR_USER_CANCELED, "Diálogo cerrado");
    req->agent->requests = g_list_remove (req->agent->requests, req);
    g_free (req->conn_path);
    g_free (req->setting_name);
    g_free (req->field);
    g_free (req->key_mgmt);
    g_free (req);
}

/* Cierra un pedido contestando antes con el error indicado. */
static void
request_close (SecretRequest *req, const gchar *error_name, const gchar *message)
{
    request_reply_error (req, error_name, message);
    gtk_widget_destroy (req->dialog);     /* libera req */
}

/* ¿La contraseña tipeada tiene un formato que NM va a aceptar? */
static gboolean
secret_is_valid (const SecretRequest *req, const gchar *text)
{
    gsize len = text ? strlen (text) : 0;
    if (len == 0)
        return FALSE;
    if (g_strcmp0 (req->key_mgmt, "wpa-psk") == 0) {
        /* WPA personal: frase de 8 a 63 caracteres, o clave de 64 dígitos
         * hexadecimales. */
        if (len >= 8 && len <= 63)
            return TRUE;
        if (len == 64) {
            for (gsize i = 0; i < len; i++)
                if (!g_ascii_isxdigit (text[i]))
                    return FALSE;
            return TRUE;
        }
        return FALSE;
    }
    /* WPA3 (sae) y WEP: cualquier largo no vacío; NM valida el resto. */
    return TRUE;
}

static void
on_secret_entry_changed (GtkEditable *editable, SecretRequest *req)
{
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (editable));
    gtk_dialog_set_response_sensitive (GTK_DIALOG (req->dialog),
                                       GTK_RESPONSE_OK,
                                       secret_is_valid (req, text));
}

static void
on_secret_eye_clicked (GtkButton *btn, GtkEntry *entry)
{
    (void) btn;
    gtk_entry_set_visibility (entry, !gtk_entry_get_visibility (entry));
}

static void
on_request_response (GtkDialog *dialog, gint response, SecretRequest *req)
{
    (void) dialog;

    if (response != GTK_RESPONSE_OK) {
        request_close (req, ERR_USER_CANCELED, "Cancelado por el usuario");
        return;
    }

    const gchar *text = gtk_entry_get_text (GTK_ENTRY (req->entry));
    if (!secret_is_valid (req, text))
        return;   /* el botón ya debería estar deshabilitado */

    gchar *secret = g_strdup (text);

    /* Respuesta: { "802-11-wireless-security": { "<campo>": "<clave>" } } */
    GVariantBuilder setting;
    g_variant_builder_init (&setting, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&setting, "{sv}", req->field,
                           g_variant_new_string (secret));

    GVariantBuilder all;
    g_variant_builder_init (&all, G_VARIANT_TYPE ("a{sa{sv}}"));
    g_variant_builder_add (&all, "{sa{sv}}", req->setting_name, &setting);

    if (req->invocation) {
        g_dbus_method_invocation_return_value (req->invocation,
            g_variant_new ("(a{sa{sv}})", &all));
        req->invocation = NULL;
    } else {
        g_variant_builder_clear (&all);
    }

    wipe_string (secret);
    g_free (secret);
    /* Borrar también el texto del campo antes de destruirlo. */
    gtk_entry_set_text (GTK_ENTRY (req->entry), "");
    gtk_widget_destroy (req->dialog);     /* libera req */
}

/* ---------- diálogo ---------- */

static void
request_show_dialog (SecretRequest *req, const gchar *network, gboolean retry)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Network authentication required"),
        NULL, 0,
        _("Cancel"),  GTK_RESPONSE_CANCEL,
        _("Connect"), GTK_RESPONSE_OK,
        NULL);
    req->dialog = dialog;
    gtk_window_set_position    (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above  (GTK_WINDOW (dialog), TRUE);
    gtk_window_set_resizable   (GTK_WINDOW (dialog), FALSE);
    gtk_window_set_icon_name   (GTK_WINDOW (dialog), "network-wireless");
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_dialog_set_response_sensitive (GTK_DIALOG (dialog), GTK_RESPONSE_OK, FALSE);

    GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (area), 12);
    gtk_box_set_spacing (GTK_BOX (area), 8);

    gchar *msg = g_strdup_printf (
        _("A password is required to connect to the Wi-Fi network \"%s\"."),
        network);
    GtkWidget *msg_label = gtk_label_new (msg);
    g_free (msg);
    gtk_label_set_xalign (GTK_LABEL (msg_label), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (msg_label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (msg_label), 40);
    gtk_box_pack_start (GTK_BOX (area), msg_label, FALSE, FALSE, 0);

    if (retry) {
        GtkWidget *retry_label =
            gtk_label_new (_("The previous password was not accepted."));
        gtk_label_set_xalign (GTK_LABEL (retry_label), 0.0);
        gtk_style_context_add_class (gtk_widget_get_style_context (retry_label),
                                     "error");
        gtk_box_pack_start (GTK_BOX (area), retry_label, FALSE, FALSE, 0);
    }

    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *pass_label = gtk_label_new (_("Password:"));
    gtk_box_pack_start (GTK_BOX (row), pass_label, FALSE, FALSE, 0);

    req->entry = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (req->entry), FALSE);
    gtk_entry_set_activates_default (GTK_ENTRY (req->entry), TRUE);
    gtk_entry_set_input_purpose (GTK_ENTRY (req->entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_box_pack_start (GTK_BOX (row), req->entry, TRUE, TRUE, 0);

    GtkWidget *eye_btn  = gtk_button_new ();
    GtkWidget *eye_icon = gtk_image_new_from_icon_name ("view-reveal-symbolic",
                                                         GTK_ICON_SIZE_MENU);
    gtk_button_set_image  (GTK_BUTTON (eye_btn), eye_icon);
    gtk_button_set_relief (GTK_BUTTON (eye_btn), GTK_RELIEF_NONE);
    g_signal_connect (eye_btn, "clicked",
                      G_CALLBACK (on_secret_eye_clicked), req->entry);
    gtk_box_pack_start (GTK_BOX (row), eye_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (area), row, FALSE, FALSE, 0);

    g_signal_connect (req->entry, "changed",
                      G_CALLBACK (on_secret_entry_changed), req);
    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_request_response), req);
    g_signal_connect (dialog, "destroy",
                      G_CALLBACK (on_request_dialog_destroy), req);

    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));
    gtk_widget_grab_focus (req->entry);
}

/* ---------- métodos del agente ---------- */

static SecretRequest *
find_request (NetSecretAgent *agent, const gchar *conn_path,
              const gchar *setting_name)
{
    for (GList *l = agent->requests; l; l = l->next) {
        SecretRequest *r = l->data;
        if (g_strcmp0 (r->conn_path, conn_path) == 0 &&
            g_strcmp0 (r->setting_name, setting_name) == 0)
            return r;
    }
    return NULL;
}

/* Lee un texto de una sección del perfil, o NULL. Liberar con g_free(). */
static gchar *
lookup_setting_string (GVariant *connection, const gchar *section,
                       const gchar *key)
{
    gchar    *result = NULL;
    GVariant *dict   = g_variant_lookup_value (connection, section,
                                               G_VARIANT_TYPE ("a{sv}"));
    if (dict) {
        const gchar *s = NULL;
        if (g_variant_lookup (dict, key, "&s", &s) && s)
            result = g_strdup (s);
        g_variant_unref (dict);
    }
    return result;
}

/* Nombre a mostrar: el nombre de la red (SSID); si no hay, el del perfil. */
static gchar *
network_display_name (GVariant *connection)
{
    gchar    *name = NULL;
    GVariant *wifi = g_variant_lookup_value (connection, "802-11-wireless",
                                             G_VARIANT_TYPE ("a{sv}"));
    if (wifi) {
        GVariant *ssid_v = g_variant_lookup_value (wifi, "ssid",
                                                   G_VARIANT_TYPE ("ay"));
        if (ssid_v) {
            gsize         len;
            const guchar *bytes = g_variant_get_fixed_array (ssid_v, &len, 1);
            if (len > 0) {
                gchar *raw = g_strndup ((const gchar *) bytes, len);
                name = g_utf8_make_valid (raw, -1);
                g_free (raw);
            }
            g_variant_unref (ssid_v);
        }
        g_variant_unref (wifi);
    }
    if (!name)
        name = lookup_setting_string (connection, "connection", "id");
    return name ? name : g_strdup ("?");
}

static void
handle_get_secrets (NetSecretAgent *agent, GVariant *params,
                    GDBusMethodInvocation *invocation)
{
    GVariant     *connection = NULL;
    const gchar  *conn_path  = NULL;
    const gchar  *setting    = NULL;
    const gchar **hints      = NULL;
    guint32       flags      = 0;

    g_variant_get (params, "(@a{sa{sv}}&o&s^a&su)",
                   &connection, &conn_path, &setting, &hints, &flags);
    g_free (hints);

    /* Sin permiso para preguntar al usuario: no tenemos secretos guardados. */
    if (!(flags & GET_SECRETS_ALLOW_INTERACTION) ||
        g_strcmp0 (setting, WIFI_SECURITY_SETTING) != 0) {
        g_dbus_method_invocation_return_dbus_error (invocation, ERR_NO_SECRETS,
            "Este agente solo atiende contraseñas de Wi-Fi personal");
        g_variant_unref (connection);
        return;
    }

    /* Qué campo pedir según el tipo de seguridad del perfil. */
    gchar       *key_mgmt = lookup_setting_string (connection,
                                                   WIFI_SECURITY_SETTING,
                                                   "key-mgmt");
    const gchar *field    = NULL;
    if (g_strcmp0 (key_mgmt, "wpa-psk") == 0 ||
        g_strcmp0 (key_mgmt, "sae") == 0)
        field = "psk";
    else if (g_strcmp0 (key_mgmt, "none") == 0)
        field = "wep-key0";             /* WEP estático */

    if (!field) {
        /* Empresarial (wpa-eap), WEP dinámico, etc.: que lo atienda otro. */
        g_dbus_method_invocation_return_dbus_error (invocation, ERR_NO_SECRETS,
            "Tipo de seguridad no soportado por este agente");
        g_free (key_mgmt);
        g_variant_unref (connection);
        return;
    }

    /* Si ya había un pedido igual abierto, el viejo se cancela. */
    SecretRequest *old = find_request (agent, conn_path, setting);
    if (old)
        request_close (old, ERR_AGENT_CANCELED, "Reemplazado por un pedido nuevo");

    SecretRequest *req = g_new0 (SecretRequest, 1);
    req->agent        = agent;
    req->invocation   = invocation;
    req->conn_path    = g_strdup (conn_path);
    req->setting_name = g_strdup (setting);
    req->field        = g_strdup (field);
    req->key_mgmt     = key_mgmt;
    agent->requests   = g_list_prepend (agent->requests, req);

    gchar *network = network_display_name (connection);
    g_variant_unref (connection);

    if (agent->prompt_cb)
        agent->prompt_cb (agent->prompt_data);

    request_show_dialog (req, network,
                         (flags & GET_SECRETS_REQUEST_NEW) != 0);
    g_free (network);
}

static void
handle_cancel_get_secrets (NetSecretAgent *agent, GVariant *params,
                           GDBusMethodInvocation *invocation)
{
    const gchar *conn_path = NULL;
    const gchar *setting   = NULL;
    g_variant_get (params, "(&o&s)", &conn_path, &setting);

    SecretRequest *req = find_request (agent, conn_path, setting);
    if (req)
        request_close (req, ERR_AGENT_CANCELED, "Cancelado por NetworkManager");

    g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
on_agent_method_call (GDBusConnection       *conn,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *method_name,
                      GVariant              *params,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
    (void) conn; (void) object_path; (void) interface_name;
    NetSecretAgent *agent = user_data;

    /* Solo NetworkManager puede pedirnos secretos. */
    if (!agent->nm_owner || g_strcmp0 (sender, agent->nm_owner) != 0) {
        g_dbus_method_invocation_return_dbus_error (invocation,
            ERR_PERMISSION_DENIED, "Solo NetworkManager puede usar este agente");
        return;
    }

    if (g_strcmp0 (method_name, "GetSecrets") == 0)
        handle_get_secrets (agent, params, invocation);
    else if (g_strcmp0 (method_name, "CancelGetSecrets") == 0)
        handle_cancel_get_secrets (agent, params, invocation);
    else
        /* SaveSecrets / DeleteSecrets: no guardamos nada, solo confirmamos. */
        g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable agent_vtable = {
    .method_call = on_agent_method_call,
};

/* ---------- registro en NetworkManager ---------- */

static void
on_register_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    /* user_data es el agente, pero puede haberse liberado si el usuario
     * apagó la opción mientras la llamada viajaba: por eso solo se usa el
     * resultado para el aviso y no se toca el agente. */
    (void) user_data;
    GError   *err    = NULL;
    GVariant *result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src),
                                                      res, &err);
    if (result) {
        g_variant_unref (result);
        g_message ("xfce-net-plugin: agente de secretos registrado");
    } else {
        /* Caso típico: otra instancia del plugin ya está registrada. */
        g_warning ("xfce-net-plugin: no se pudo registrar el agente de "
                   "secretos: %s", err->message);
        g_error_free (err);
    }
}

static void
on_nm_appeared (GDBusConnection *conn, const gchar *name,
                const gchar *name_owner, gpointer user_data)
{
    (void) name;
    NetSecretAgent *agent = user_data;
    g_free (agent->nm_owner);
    agent->nm_owner   = g_strdup (name_owner);
    agent->registered = TRUE;

    g_dbus_connection_call (conn, NM_BUS_NAME, NM_AGENT_MANAGER_PATH,
                            NM_AGENT_MANAGER_IFACE, "RegisterWithCapabilities",
                            g_variant_new ("(su)", AGENT_IDENTIFIER, 0u),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_register_done, NULL);
}

static void
on_nm_vanished (GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void) conn; (void) name;
    NetSecretAgent *agent = user_data;
    g_clear_pointer (&agent->nm_owner, g_free);
    agent->registered = FALSE;
    /* NM se fue: los pedidos abiertos ya no tienen a quién contestar. */
    while (agent->requests)
        request_close (agent->requests->data, ERR_AGENT_CANCELED,
                       "NetworkManager dejó el bus");
}

/* ---------- API pública ---------- */

NetSecretAgent *
net_secret_agent_new (GDBusConnection        *conn,
                      NetSecretAgentPromptCb  prompt_cb,
                      gpointer                user_data)
{
    if (!conn)
        return NULL;

    GError         *err   = NULL;
    NetSecretAgent *agent = g_new0 (NetSecretAgent, 1);
    agent->conn        = conn;
    agent->prompt_cb   = prompt_cb;
    agent->prompt_data = user_data;

    agent->node_info = g_dbus_node_info_new_for_xml (agent_xml, &err);
    if (!agent->node_info) {
        g_warning ("xfce-net-plugin: agente de secretos: %s", err->message);
        g_error_free (err);
        g_free (agent);
        return NULL;
    }

    agent->object_id = g_dbus_connection_register_object (
        conn, NM_SECRET_AGENT_PATH, agent->node_info->interfaces[0],
        &agent_vtable, agent, NULL, &err);
    if (!agent->object_id) {
        g_warning ("xfce-net-plugin: no se pudo publicar el agente de "
                   "secretos: %s", err->message);
        g_error_free (err);
        g_dbus_node_info_unref (agent->node_info);
        g_free (agent);
        return NULL;
    }

    /* on_nm_appeared se llama enseguida si NM ya está corriendo. */
    agent->watch_id = g_bus_watch_name_on_connection (
        conn, NM_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_nm_appeared, on_nm_vanished, agent, NULL);

    return agent;
}

void
net_secret_agent_free (NetSecretAgent *agent)
{
    if (!agent)
        return;

    /* Cerrar los diálogos abiertos avisándole a NM. */
    while (agent->requests)
        request_close (agent->requests->data, ERR_AGENT_CANCELED,
                       "El agente se apagó");

    if (agent->watch_id)
        g_bus_unwatch_name (agent->watch_id);

    /* Darse de baja (corto y sincrónico: es al apagar la opción o al quitar
     * el plugin). Si falla no importa: NM igual nos borra cuando nuestra
     * conexión al bus se cierra. */
    if (agent->registered) {
        GVariant *r = g_dbus_connection_call_sync (
            agent->conn, NM_BUS_NAME, NM_AGENT_MANAGER_PATH,
            NM_AGENT_MANAGER_IFACE, "Unregister", NULL, NULL,
            G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (r)
            g_variant_unref (r);
    }

    if (agent->object_id)
        g_dbus_connection_unregister_object (agent->conn, agent->object_id);
    g_dbus_node_info_unref (agent->node_info);
    g_free (agent->nm_owner);
    g_free (agent);
}
