/* secret-agent.h — agente de secretos: responde cuando NetworkManager
 * necesita pedirle una contraseña al usuario (perfil en "preguntar siempre",
 * clave del router que cambió y fue rechazada, etc.).
 *
 * Alcance actual (nivel 1): contraseñas de Wi-Fi (WPA/WPA2/WPA3 personal y
 * WEP). Cualquier otro pedido (redes empresariales, VPN) se contesta "no
 * tengo secretos", así NetworkManager se lo pasa al siguiente agente que
 * haya (por ejemplo nm-applet, si está corriendo).
 *
 * No guarda contraseñas: no usa llavero. Lo que el usuario tipea se le pasa
 * a NetworkManager y se borra de la memoria. */
#ifndef SECRET_AGENT_H
#define SECRET_AGENT_H

#include <gio/gio.h>

typedef struct _NetSecretAgent NetSecretAgent;

/* Se llama justo antes de mostrar el diálogo de contraseña. El panel la usa
 * para cerrar el popup: el popup tiene capturado el teclado y el diálogo no
 * podría recibir lo que se tipea. */
typedef void (*NetSecretAgentPromptCb) (gpointer user_data);

/* Crea el agente y lo registra en NetworkManager (y lo vuelve a registrar
 * solo si NetworkManager se reinicia). Devuelve NULL si no se pudo publicar
 * en el bus (por ejemplo, otro agente de este mismo proceso ya ocupa la
 * ruta). La conexión es prestada: no se libera acá. */
NetSecretAgent *net_secret_agent_new  (GDBusConnection        *conn,
                                       NetSecretAgentPromptCb  prompt_cb,
                                       gpointer                user_data);

/* Cancela los pedidos abiertos, se da de baja en NetworkManager y libera. */
void            net_secret_agent_free (NetSecretAgent *agent);

#endif /* SECRET_AGENT_H */
