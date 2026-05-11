#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "nm-dbus.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *top_box;
    GtkWidget *content_box;
    GtkWidget *scroll;
    gulong     press_handler;
    GtkWidget *current_expand_box;
    gboolean   scanning;
    gint       popup_width;
    gint       popup_height;
    GtkWidget *button;
    GSList    *device_switches;
} NetPopup;

/* Crea un widget con ícono de señal Wi-Fi. Si secure=TRUE superpone un candado.
 * icon_size: tamaño en píxeles (ej. 16 para popup, icon_size para panel). */
GtkWidget *make_signal_icon (gint strength, gboolean secure, gint icon_size);

NetPopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void      popup_show    (NetPopup *popup, XfcePanelPlugin *plugin,
                         GtkWidget *button, GSList *devices,
                         GDBusConnection *conn,
                         gint popup_width, gint popup_height);
void      popup_hide    (NetPopup *popup);
void      popup_destroy (NetPopup *popup);

#endif /* POPUP_H */
