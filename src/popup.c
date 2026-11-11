/* SPDX-FileCopyrightText: 2026 Tantin1 <juanperez33840@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* popup.c — núcleo del popup: ventana (crear/mostrar/ocultar/destruir),
 * posicionamiento y grab, helpers compartidos, diálogos (red oculta) y
 * manejadores de la ventana. Los intentos de conexión los sigue attempts.c.
 * La fila de red vive en popup-rows.c; las secciones y el refresco, en
 * popup-sections.c; lo compartido se declara en popup-private.h. */

#include "popup-private.h"
#include <string.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

typedef struct {
    NetPopup  *popup;
    GtkWidget *button;
} GrabData;

/* ---------- marco "menu" y popover de medición ----------
 *
 * Los temas de GTK 3 escriben sus reglas para el COMPONENTE (menu { … },
 * popover.background { … }), no para una clase: una caja común con la clase
 * .menu o .popover no las recibe. Por eso dos subclases mínimas de GtkBox,
 * cada una con su nombre de componente.
 *
 * Nov-2026: los COLORES salen del menú del tema, igual que los plugins nativos
 * de Xfce (pulseaudio, notifyd), que son GtkMenu. En la mayoría de los temas
 * menú y popover coinciden, pero en Greybird-dark el popover es gris claro
 * (#676d6e, poco contraste) y el menú oscuro (#303133). Todo el contenido va
 * ADENTRO del marco de menú, en los tres estilos de ventana.
 *
 * El RADIO de las esquinas, en cambio, se toma de los popovers (los menús
 * casi siempre tienen 0): de un popover que nunca se muestra, solo para leer
 * el estilo del tema. */
typedef GtkBox      NetMenuFrame;
typedef GtkBoxClass NetMenuFrameClass;
G_DEFINE_TYPE (NetMenuFrame, net_menu_frame, GTK_TYPE_BOX)

static void
net_menu_frame_class_init (NetMenuFrameClass *klass)
{
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "menu");
}

static void
net_menu_frame_init (NetMenuFrame *self)
{
    (void) self;
}

typedef GtkBox      NetPopupFrame;
typedef GtkBoxClass NetPopupFrameClass;
G_DEFINE_TYPE (NetPopupFrame, net_popup_frame, GTK_TYPE_BOX)

static void
net_popup_frame_class_init (NetPopupFrameClass *klass)
{
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "popover");
}

static void
net_popup_frame_init (NetPopupFrame *self)
{
    (void) self;
}

/* Esquinas redondeadas del marco de menú, con el radio de los popovers del
 * tema. Solo con la ventana transparente: opaca, las esquinas se pintarían
 * con el fondo de la ventana y se verían cuadradas. radius <= 0: rectas. */
static void
popup_set_round (NetPopup *popup, gint radius)
{
    static GtkCssProvider *provider = NULL;   /* uno por proceso: mismo tema */
    GtkStyleContext *fctx = gtk_widget_get_style_context (popup->frame);

    if (radius <= 0) {
        gtk_style_context_remove_class (fctx, "net-round");
        return;
    }
    if (!provider) {
        provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_screen (gtk_widget_get_screen (popup->window),
            GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gchar *css = g_strdup_printf ("menu.net-popup-frame.net-round { border-radius: %dpx; }",
                                  radius);
    gtk_css_provider_load_from_data (provider, css, -1, NULL);
    g_free (css);
    gtk_style_context_add_class (fctx, "net-round");
}

/* Prototipos locales de este módulo. */
static gboolean on_button_press (GtkWidget *widget, GdkEventButton *event, NetPopup *popup);
static gboolean on_focus_out    (GtkWidget *widget, GdkEventFocus *ev, NetPopup *popup);
static gboolean popup_is_wayland (GtkWidget *w);

/* Libera una cadena sensible (contraseña) sobreescribiéndola primero con
 * ceros, para que el texto no quede flotando en la memoria liberada.
 * El puntero volátil evita que el compilador elimine el borrado. */
void
secure_wipe_free (gchar *s)
{
    if (!s) return;
    volatile gchar *p = s;
    while (*p) { *p = '\0'; p++; }
    g_free (s);
}

/* ---------- grab diferido al mapear la ventana ---------- */

static gboolean
on_window_mapped (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void) event;
    GrabData  *gd   = user_data;
    NetPopup  *popup = gd->popup;
    GdkDisplay *display = gtk_widget_get_display (widget);
    GdkSeat    *seat    = gdk_display_get_default_seat (display);

    /* Desconectarse inmediatamente — solo ejecutar una vez por apertura. */
    g_signal_handlers_disconnect_by_func (widget, on_window_mapped, gd);

    GdkGrabStatus grab_status = gdk_seat_grab (
            seat,
            gtk_widget_get_window (widget),
            GDK_SEAT_CAPABILITY_ALL,
            TRUE, NULL, NULL, NULL, NULL);

    popup->grab_active = (grab_status == GDK_GRAB_SUCCESS);
    if (grab_status == GDK_GRAB_SUCCESS) {
        popup->press_handler =
            g_signal_connect (popup->window, "button-press-event",
                              G_CALLBACK (on_button_press), popup);
    } else {
        popup->press_handler =
            g_signal_connect (popup->window, "focus-out-event",
                              G_CALLBACK (on_focus_out), popup);
    }

    g_free (gd);
    return FALSE; /* no consumir el evento */
}

/* ---------- menú contextual: devolver la captura al cerrarse ----------
 *
 * Solo puede haber una captura del mouse a la vez: el menú contextual se la
 * lleva al abrirse y la suelta al cerrarse, y el popup se queda sin ella
 * (dejaría de cerrarse al clickear afuera). Por eso, al cerrarse el menú se
 * vuelve a pedir. Se hace diferido (idle) porque GTK emite "deactivate"
 * ANTES de soltar su captura y ANTES de ejecutar la opción elegida; así la
 * opción corre con los datos de la fila todavía vivos y recién después se
 * destruye el menú y se refrescan las filas. */

static gboolean
menu_closed_idle (gpointer user_data)
{
    NetPopup *popup = user_data;
    popup->menu_idle_id = 0;

    if (popup->ctx_menu) {
        gtk_widget_destroy (popup->ctx_menu);
        popup->ctx_menu = NULL;
    }

    if (popup->grab_active && gtk_widget_get_visible (popup->window)) {
        GdkSeat *seat = gdk_display_get_default_seat (
                            gtk_widget_get_display (popup->window));
        if (gdk_seat_grab (seat, gtk_widget_get_window (popup->window),
                           GDK_SEAT_CAPABILITY_ALL, TRUE,
                           NULL, NULL, NULL, NULL) != GDK_GRAB_SUCCESS)
            g_warning ("xfce-net-plugin: no se pudo recuperar la captura "
                       "del mouse tras cerrar el menú contextual");
    }

    /* El refresco estuvo congelado mientras el menú estaba abierto. */
    schedule_refresh_ui (popup);
    return G_SOURCE_REMOVE;
}

void
popup_menu_closed (NetPopup *popup)
{
    if (!popup->menu_idle_id)
        popup->menu_idle_id = g_idle_add (menu_closed_idle, popup);
}

/* ---------- posicionamiento ---------- */

/* Dónde va el popup a lo largo del panel (eje X en un panel horizontal,
 * eje Y en uno vertical). Regla:
 *   1. centrado sobre el plugin;
 *   2. si no entra, se corre hasta quedar dentro de la pantalla;
 *   3. "imán": si contra un borde queda un hueco más chico que medio popup,
 *      se pega a ese borde.
 * Así, con el plugin cerca de un extremo el popup se pega a ESE extremo, y
 * con el plugin hacia el centro queda centrado sobre él; nunca quedan huecos
 * chicos que se vean descuidados. start/len: tramo disponible; btn/btn_len:
 * el botón; size: tamaño del popup. Devuelve el comienzo del popup. */
static gint
place_along_panel (gint start, gint len, gint btn, gint btn_len, gint size)
{
    gint end  = start + len;
    gint half = size / 2;
    gint p    = btn + btn_len / 2 - half;

    if (p + size > end) p = end - size;
    if (p < start)      p = start;

    /* Imán: si quedan huecos chicos en los dos lados, gana el más cercano. */
    gint gap_start = p - start;
    gint gap_end   = end - (p + size);
    if (gap_start < half && gap_start <= gap_end)
        p = start;
    else if (gap_end < half)
        p = end - size;
    return MAX (p, start);
}

static void
position_popup (NetPopup *popup, GtkWidget *button, GtkArrowType arrow)
{
    GdkWindow      *gdk_win;
    GdkMonitor     *monitor;
    GdkRectangle    workarea;
    GtkRequisition  preferred;
    gint bx, by, bw, bh, pw, ph, x, y;

    gdk_win = gtk_widget_get_window (button);
    gdk_window_get_origin (gdk_win, &bx, &by);
    /* gdk_window_get_origin da el origen de la ventana que CONTIENE al botón;
     * si el botón no tiene ventana propia, hay que sumarle su lugar en ella. */
    if (!gtk_widget_get_has_window (button)) {
        GtkAllocation a;
        gtk_widget_get_allocation (button, &a);
        bx += a.x;
        by += a.y;
    }
    bw = gtk_widget_get_allocated_width  (button);
    bh = gtk_widget_get_allocated_height (button);
    gint g = popup->float_gap;   /* 0 si el popup va pegado al panel */

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);
    pw = preferred.width;
    ph = preferred.height;
    popup->placed_w     = pw;
    popup->placed_h     = ph;
    popup->placed_arrow = arrow;

    monitor = gdk_display_get_monitor_at_point (
                  gtk_widget_get_display (button), bx + bw / 2, by + bh / 2);
    gdk_monitor_get_workarea (monitor, &workarea);

    /* Flotante: separado g px del panel Y de los bordes de la pantalla
     * (en todas las direcciones o en ninguna: un hueco solo contra el borde,
     * con la ventana pegada al panel, parece un error de cálculo). */
    if (arrow == GTK_ARROW_LEFT || arrow == GTK_ARROW_RIGHT) {
        /* Panel vertical: al costado del panel, ubicado a lo alto. */
        y = place_along_panel (workarea.y + g, workarea.height - 2 * g, by, bh, ph);
        if (arrow == GTK_ARROW_RIGHT) {          /* panel a la izquierda */
            x = bx + bw + g;
            if (x + pw > workarea.x + workarea.width)
                x = bx - pw - g;
        } else {                                 /* panel a la derecha */
            x = bx - pw - g;
            if (x < workarea.x)
                x = bx + bw + g;
        }
    } else {
        /* Panel horizontal: encima o debajo del panel, ubicado a lo ancho. */
        x = place_along_panel (workarea.x + g, workarea.width - 2 * g, bx, bw, pw);
        if (arrow == GTK_ARROW_UP) {             /* panel abajo */
            y = by - ph - g;
            if (y < workarea.y)
                y = by + bh + g;
        } else {                                 /* panel arriba */
            y = by + bh + g;
            if (y + ph > workarea.y + workarea.height)
                y = by - ph - g;
        }
    }

    gtk_window_move (GTK_WINDOW (popup->window), x, y);
}

/* GTK no actualiza el estilo de una ventana OCULTA: después de cambiar el
 * tema, la primera apertura se mide con el tamaño del tema anterior y la
 * ventana queda mal ubicada (muy arriba del panel, o encima de él). Cuando
 * aparece, GTK le da el tamaño real: si no coincide con el usado para
 * ubicarla, se la vuelve a ubicar. En el uso normal coinciden y no hace nada.
 * Sirve también si el contenido cambia de alto con la ventana abierta. */
static void
on_popup_size_allocate (GtkWidget *win, GtkAllocation *alloc, NetPopup *popup)
{
    (void) alloc;
    if (!gtk_widget_get_visible (win) || !popup->button || popup_is_wayland (win))
        return;
    gint w, h;
    gtk_window_get_size (GTK_WINDOW (win), &w, &h);
    if (w != popup->placed_w || h != popup->placed_h)
        position_popup (popup, popup->button, popup->placed_arrow);
}

/* GTK no actualiza el estilo de los widgets de una ventana OCULTA (lo hace
 * al mostrarla). Medida así, la primera apertura tras cambiar el tema usa el
 * tamaño del tema anterior y queda corrida: en X11 lo corrige
 * on_popup_size_allocate con un reacomodo; en Wayland la ubicación ya quedó
 * pedida al compositor. Los widgets NUEVOS sí nacen con el tema actual, así
 * que con la ventana cerrada se marca para rearmar el contenido al abrir. */
static void
on_theme_changed (GObject *settings, GParamSpec *pspec, NetPopup *popup)
{
    (void) settings; (void) pspec;
    if (!gtk_widget_get_visible (popup->window))
        popup->theme_dirty = TRUE;
}

/* ---------- Wayland: popup pegado al botón ----------
 *
 * En Wayland una ventana común no puede elegir dónde aparece: la ubica el
 * compositor (por eso el popup salía centrado). Lo que sí se puede es
 * declararla "popup hija" del panel (transient_for + tipo POPUP_MENU) y
 * pedirle al compositor que la acomode junto a un rectángulo (el botón) con
 * gdk_window_move_to_rect. El compositor la corre o la da vuelta sola si no
 * entra en la pantalla. Es el mismo mecanismo que usan los menús del panel.
 *
 * La captura (grab) tiene que pedirse ANTES de mostrar la ventana: así el
 * compositor le da el teclado (búsqueda, contraseña) y la cierra solo al
 * clickear afuera. Por eso se usa la función "prepare" de gdk_seat_grab,
 * que muestra la ventana dentro del grab (igual que GtkMenu).
 * El camino X11 no cambia. */

/* Mensajes de diagnóstico de Wayland. No se ven salvo que se lance el panel
 * con G_MESSAGES_DEBUG=xfce-net-plugin (o =all). */
#define WL_DEBUG(...) g_log ("xfce-net-plugin", G_LOG_LEVEL_DEBUG, __VA_ARGS__)

static gboolean
popup_is_wayland (GtkWidget *w)
{
#ifdef GDK_WINDOWING_WAYLAND
    return GDK_IS_WAYLAND_DISPLAY (gtk_widget_get_display (w));
#else
    (void) w;
    return FALSE;
#endif
}

static void
wl_prepare_show (GdkSeat *seat, GdkWindow *window, gpointer data)
{
    (void) seat; (void) window;
    gtk_widget_show_all (GTK_WIDGET (data));
}

/* Clic con la captura interna de GTK activa: todos los clics del programa
 * (también los del panel y del botón del plugin) llegan primero acá. Si el
 * clic cayó fuera del popup, se cierra y el clic se consume: así tocar el
 * botón del plugin cierra el popup sin volver a abrirlo. Hace falta porque
 * el compositor no cierra el popup cuando el clic es en otra ventana del
 * mismo programa (el panel). Lo mismo hacen los menús de GTK. */
static gboolean
wl_on_button_press (GtkWidget *w, GdkEventButton *ev, NetPopup *popup)
{
    (void) w;
    GdkWindow *own = gtk_widget_get_window (popup->window);

    /* Durante la captura, Wayland entrega los clics del panel como si fueran
     * en la ventana del popup, pero con coordenadas fuera de su área. Por eso
     * no alcanza con mirar la ventana: hay que pasar el punto a coordenadas
     * del popup y ver si cae dentro. */
    gboolean   inside = FALSE;
    gdouble    x = ev->x, y = ev->y;
    GdkWindow *win = ev->window;
    while (win && win != own) {
        gdk_window_coords_to_parent (win, x, y, &x, &y);
        win = gdk_window_get_effective_parent (win);
    }
    if (win == own)
        inside = x >= 0 && y >= 0 &&
                 x < gtk_widget_get_allocated_width  (popup->window) &&
                 y < gtk_widget_get_allocated_height (popup->window);

    WL_DEBUG ("wayland: clic %s del popup en (%.0f, %.0f) (menú abierto: %s)",
              inside ? "dentro" : "fuera", x, y,
              (popup->ctx_menu || popup->menu_idle_id) ? "sí" : "no");
    if (inside)
        return GDK_EVENT_PROPAGATE;     /* clic dentro del popup */
    if (popup->ctx_menu || popup->menu_idle_id)
        return GDK_EVENT_PROPAGATE;
    popup_hide (popup);
    return GDK_EVENT_STOP;
}

/* El compositor cerró el popup por su cuenta (clic afuera, otra ventana
 * tomó el foco): GDK oculta la ventana pero GTK todavía la cree visible.
 * Hay que pasar por popup_hide para que quede todo en orden. */
static gboolean
wl_on_popup_gone (GtkWidget *w, GdkEvent *ev, NetPopup *popup)
{
    (void) w;
    WL_DEBUG ("wayland: el popup recibió %s",
              ev->type == GDK_GRAB_BROKEN ? "grab-broken" : "unmap");
    /* Abrir el menú contextual le saca el grab al popup: no es cerrar. */
    if (popup->ctx_menu || popup->menu_idle_id)
        return FALSE;
    if (gtk_widget_get_visible (popup->window))
        popup_hide (popup);
    return FALSE;
}

static void
popup_show_wayland (NetPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel (button);
    GdkWindow *gdk_win;

    if (!g_object_get_data (G_OBJECT (popup->window), "wl-handlers")) {
        g_signal_connect (popup->window, "unmap-event",
                          G_CALLBACK (wl_on_popup_gone), popup);
        g_signal_connect (popup->window, "grab-broken-event",
                          G_CALLBACK (wl_on_popup_gone), popup);
        g_object_set_data (G_OBJECT (popup->window), "wl-handlers",
                           GINT_TO_POINTER (1));
    }

    if (GTK_IS_WINDOW (toplevel))
        gtk_window_set_transient_for (GTK_WINDOW (popup->window),
                                      GTK_WINDOW (toplevel));
    /* Mostrar el CONTENIDO (no la ventana) antes de medir: si no, la primera
     * vez el alto da 0 y la ventana queda mal ubicada. */
    gtk_widget_show_all (popup->frame);
    gtk_widget_realize (popup->window);
    gdk_win = gtk_widget_get_window (popup->window);

    /* Rectángulo del botón, en coordenadas de la ventana del panel. */
    GdkRectangle rect = { 0, 0, 1, 1 };
    gtk_widget_translate_coordinates (button, toplevel, 0, 0, &rect.x, &rect.y);
    rect.width  = MAX (gtk_widget_get_allocated_width  (button), 1);
    rect.height = MAX (gtk_widget_get_allocated_height (button), 1);

    /* Hacia dónde abrir según dónde esté el panel. Igual que en X11: el
     * popup arranca alineado con el botón y, si no entra, se CORRE a lo
     * largo del panel hasta tocar el borde de la pantalla. Solo se "da
     * vuelta" en el otro eje (abrir hacia el otro lado del panel). Pedir
     * FLIP en los dos ejes lo espejaba en vez de correrlo y quedaba un
     * espacio contra el borde. */
    /* Flotante: el despegue del panel va en los desplazamientos (dx, dy);
     * si el compositor da vuelta la ventana, los invierte él solo. */
    gint          g           = popup->float_gap;
    gint          dx = 0, dy  = g;                         /* panel arriba */
    GdkGravity    rect_anchor = GDK_GRAVITY_SOUTH_WEST;   /* panel arriba */
    GdkGravity    win_anchor  = GDK_GRAVITY_NORTH_WEST;
    GdkAnchorHints hints      = GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X;
    switch (xfce_panel_plugin_arrow_type (plugin)) {
    case GTK_ARROW_UP:      /* panel abajo: abrir hacia arriba */
        rect_anchor = GDK_GRAVITY_NORTH_WEST;
        win_anchor  = GDK_GRAVITY_SOUTH_WEST;
        dy          = -g;
        break;
    case GTK_ARROW_RIGHT:   /* panel a la izquierda */
        rect_anchor = GDK_GRAVITY_NORTH_EAST;
        win_anchor  = GDK_GRAVITY_NORTH_WEST;
        hints       = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_SLIDE_Y;
        dx = g; dy = 0;
        break;
    case GTK_ARROW_LEFT:    /* panel a la derecha */
        rect_anchor = GDK_GRAVITY_NORTH_WEST;
        win_anchor  = GDK_GRAVITY_NORTH_EAST;
        hints       = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_SLIDE_Y;
        dx = -g; dy = 0;
        break;
    default:
        break;
    }
    /* Misma regla que en X11 (place_along_panel). En Wayland el plugin no
     * sabe dónde está el panel en la pantalla (el compositor no lo informa),
     * así que se usa el propio panel como referencia: exacto cuando el panel
     * ocupa todo el ancho (o alto) de la pantalla, aproximado si es corto.
     * Se le pide al compositor que ponga el comienzo del popup en el punto
     * calculado; SLIDE corrige si igual no entrara. */
    {
        GtkRequisition pref;
        gtk_widget_get_preferred_size (popup->window, NULL, &pref);
        if (hints & GDK_ANCHOR_SLIDE_X) {        /* panel horizontal */
            rect.x = place_along_panel (g, gtk_widget_get_allocated_width (toplevel) - 2 * g,
                                        rect.x, rect.width, pref.width);
            rect.width = 1;
        } else {                                  /* panel vertical */
            rect.y = place_along_panel (g, gtk_widget_get_allocated_height (toplevel) - 2 * g,
                                        rect.y, rect.height, pref.height);
            rect.height = 1;
        }
        WL_DEBUG ("wayland: panel %dx%d, popup %dx%d, comienzo en (%d, %d)",
                  gtk_widget_get_allocated_width (toplevel),
                  gtk_widget_get_allocated_height (toplevel),
                  pref.width, pref.height, rect.x, rect.y);
    }

    gdk_window_move_to_rect (gdk_win, &rect, rect_anchor, win_anchor,
                             hints, dx, dy);

    GdkSeat *seat = gdk_display_get_default_seat (gtk_widget_get_display (popup->window));
    GdkGrabStatus st = gdk_seat_grab (seat, gdk_win, GDK_SEAT_CAPABILITY_ALL,
                                      TRUE, NULL, NULL,
                                      wl_prepare_show, popup->window);
    popup->grab_active = (st == GDK_GRAB_SUCCESS);
    WL_DEBUG ("wayland: panel=%s, captura=%d (0 = conseguida), visible=%s",
              G_OBJECT_TYPE_NAME (toplevel), (gint) st,
              gtk_widget_get_visible (popup->window) ? "sí" : "no");
    if (popup->grab_active) {
        /* Captura interna de GTK (ver wl_on_button_press). El popup va al
         * mismo grupo de ventanas que el panel para que abarque sus clics. */
        if (GTK_IS_WINDOW (toplevel))
            gtk_window_group_add_window (gtk_window_get_group (GTK_WINDOW (toplevel)),
                                         GTK_WINDOW (popup->window));
        gtk_grab_add (popup->window);
        WL_DEBUG ("wayland: captura interna de GTK activa: %s",
                  gtk_widget_has_grab (popup->window) ? "sí" : "no");
        popup->press_handler =
            g_signal_connect (popup->window, "button-press-event",
                              G_CALLBACK (wl_on_button_press), popup);
    } else {
        /* Sin grab: mostrar igual y cerrar al perder el foco. */
        gtk_widget_show_all (popup->window);
        popup->press_handler =
            g_signal_connect (popup->window, "focus-out-event",
                              G_CALLBACK (on_focus_out), popup);
    }
}

/* ---------- SSID de la conexión primaria ----------
 * get_primary_ssid (nombre de la red principal) vive ahora en nm-dbus.c,
 * donde puede leer de la instantánea del bus sin llamadas sueltas. Sigue
 * declarada en popup-private.h. */

/* ---------- estado del adaptador ---------- */

/* ¿Conectado de verdad? (estado 100 = activado) */
gboolean
device_is_activated (GDBusConnection *conn, const gchar *device_path)
{
    return nm_get_device_state (conn, device_path) == 100;
}

/* ---------- verificación robusta de existencia de íconos del tema ----------
 *
 * gtk_icon_theme_has_icon devuelve TRUE en cuanto el nombre está declarado
 * en el index.theme del tema, sin comprobar que el archivo realmente exista.
 * Hay temas (notablemente RedmondX y RedmondX-Light) que declaran nombres
 * como nm-secure-lock pero el archivo en disco es un enlace simbólico roto.
 *
 * Esta función pide a GTK que resuelva el ícono al tamaño deseado, obtiene
 * la ruta del archivo final, y verifica con g_file_test que ese archivo
 * exista de verdad. Solo si las tres cosas (lookup OK + filename OK +
 * archivo presente) se cumplen, devuelve TRUE. */
static gboolean
theme_icon_exists_real (const gchar *name, gint size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    GtkIconInfo  *info  = gtk_icon_theme_lookup_icon (theme, name, size, 0);
    if (!info)
        return FALSE;

    const gchar *filename = gtk_icon_info_get_filename (info);
    gboolean ok = (filename != NULL) && g_file_test (filename, G_FILE_TEST_EXISTS);
    g_object_unref (info);
    return ok;
}

/* ---------- ícono de candado superpuesto (helper reutilizable) ----------
 *
 * Devuelve un GtkImage listo para superponer como overlay encima de otro
 * widget. Prueba en orden:
 *   1) nm-vpn-active-lock (existe en muchos temas que soportan nm-applet).
 *   2) nm-secure-lock     (el nombre histórico de nm-applet).
 *   3) /org/xfce/net-plugin/icons/nm-secure-lock.svg (recurso embebido en
 *      el propio .so del plugin, garantiza que siempre se vea algo).
 *
 * Usa theme_icon_exists_real para evitar caer en enlaces rotos. */
GtkWidget *
make_lock_overlay_image (gint icon_size)
{
    GtkWidget *lock_img = NULL;

    if (theme_icon_exists_real ("nm-vpn-active-lock", icon_size)) {
        lock_img = gtk_image_new_from_icon_name ("nm-vpn-active-lock",
                                                 GTK_ICON_SIZE_MENU);
    } else if (theme_icon_exists_real ("nm-secure-lock", icon_size)) {
        lock_img = gtk_image_new_from_icon_name ("nm-secure-lock",
                                                 GTK_ICON_SIZE_MENU);
    } else {
        /* Último recurso: SVG embebido en el binario del plugin. */
        lock_img = gtk_image_new_from_resource (
            "/org/xfce/net-plugin/icons/nm-secure-lock.svg");
    }

    gtk_image_set_pixel_size (GTK_IMAGE (lock_img), icon_size);
    gtk_widget_set_halign (lock_img, GTK_ALIGN_FILL);
    gtk_widget_set_valign (lock_img, GTK_ALIGN_FILL);
    return lock_img;
}

/* ---------- orden de los íconos (estándar o nm-applet) ---------- */

static gboolean prefer_nm_icons = FALSE;

void
popup_set_prefer_nm_icons (gboolean prefer)
{
    prefer_nm_icons = prefer;
}

/* GTK recorre el tema activo (y los que hereda) probando los dos nombres
 * antes de caer al tema genérico de reserva (hicolor). Así, aunque se
 * prefiera nm-applet, si el tema no tiene ese ícono pero sí el estándar, se
 * usa el estándar del tema y no la versión genérica de hicolor. */
GIcon *
net_themed_icon_new (const gchar *freedesktop_name, const gchar *nm_name)
{
    const gchar *names[3];
    names[0] = prefer_nm_icons ? nm_name : freedesktop_name;
    names[1] = prefer_nm_icons ? freedesktop_name : nm_name;
    names[2] = NULL;
    return g_themed_icon_new_from_names ((gchar **) names, -1);
}

/* ---------- ícono de señal Wi-Fi (usado también por plugin.c) ----------
 *
 * Comportamiento:
 *   - Red abierta:  ícono de señal normal (nm-signal-* con fallback freedesktop).
 *   - Red segura:   ícono de señal normal + candado superpuesto via
 *                   make_lock_overlay_image (que tiene su propio fallback).
 *
 * Las variantes integradas nm-signal-{nivel}-secure se descartan porque,
 * cuando el tema activo no las provee, GTK cae a hicolor — y la versión de
 * hicolor (instalada con network-manager-applet) es genérica y en algunos
 * temas (notablemente Papirus en 16px) se ve mal. Usar siempre el candado
 * superpuesto garantiza un resultado predecible en todos los temas. */

GtkWidget *
make_signal_icon (gint strength, gboolean secure, gint icon_size)
{
    const gchar *nm_level;
    const gchar *fd_level;

    if      (strength >= 80) { nm_level = "100"; fd_level = "excellent"; }
    else if (strength >= 55) { nm_level = "75";  fd_level = "good";      }
    else if (strength >= 30) { nm_level = "50";  fd_level = "ok";        }
    else                     { nm_level = "25";  fd_level = "weak";      }

    gchar nm_name[64], fd_name[64];
    g_snprintf (nm_name, sizeof nm_name,
                "nm-signal-%s", nm_level);
    g_snprintf (fd_name, sizeof fd_name,
                "network-wireless-signal-%s-symbolic", fd_level);

    /* Ícono de señal: estándar o nm-signal-*, según la preferencia. */
    GIcon     *signal_gicon = net_themed_icon_new (fd_name, nm_name);
    GtkWidget *signal_img   = gtk_image_new_from_gicon (signal_gicon, GTK_ICON_SIZE_MENU);
    g_object_unref (signal_gicon);
    gtk_image_set_pixel_size (GTK_IMAGE (signal_img), icon_size);

    if (!secure)
        return signal_img;

    /* Red segura: superponemos el candado encima del ícono de señal. */
    GtkWidget *overlay  = gtk_overlay_new ();
    GtkWidget *lock_img = make_lock_overlay_image (icon_size);

    gtk_container_add       (GTK_CONTAINER (overlay), signal_img);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), lock_img);
    gtk_widget_set_size_request (overlay, icon_size, icon_size);
    gtk_widget_show_all (overlay);

    return overlay;
}

/* ---------- callback del switch Wi-Fi global ---------- */

gboolean
on_wifi_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    NetPopup *popup = user_data;
    nm_set_wifi_enabled (popup->conn, state);
    for (GSList *l = popup->device_switches; l; l = l->next)
        gtk_widget_set_sensitive (GTK_WIDGET (l->data), state);
    return FALSE;
}

/* ---------- callback del switch por adaptador ---------- */


void
device_switch_data_free (DeviceSwitchData *d)
{
    g_free (d->device_path);
    g_free (d->iface);
    g_free (d);
}

gboolean
on_device_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void) sw;
    DeviceSwitchData *d = user_data;
    nm_set_device_enabled_async (d->conn, d->device_path, state);
    if (d->popup && d->popup->plugin_ref && d->iface)
        net_plugin_device_toggled (d->popup->plugin_ref, d->iface, state);
    return FALSE;
}

/* ---------- Actualizar (botón) ---------- */

static gboolean
on_refresh_reenable (gpointer popup_ptr)
{
    NetPopup *popup = popup_ptr;
    popup->scanning = FALSE;
    popup->scan_timeout_id = 0;
    if (popup->refresh_button && GTK_IS_WIDGET (popup->refresh_button)) {
        gtk_widget_set_sensitive (popup->refresh_button, TRUE);
        if (popup->refresh_label)
            gtk_label_set_text (GTK_LABEL (popup->refresh_label), _("Refresh"));
    }
    return G_SOURCE_REMOVE;
}

/* ---------- portal cautivo ---------- */

void
on_portal_signin_clicked (GtkWidget *btn, NetPopup *popup)
{
    (void) btn;
    /* Cualquier dirección http (sin s) sirve: el portal intercepta la
     * primera página que se pide y muestra su formulario. Se prefiere la que
     * usa NM para comprobar; si no hay, una pensada para esto. */
    gchar *uri = nm_get_connectivity_check_uri (popup->conn);
    if (!uri)
        uri = g_strdup ("http://neverssl.com/");

    popup_hide (popup);     /* el popup tiene capturado el mouse */

    GError *err = NULL;
    if (!gtk_show_uri_on_window (NULL, uri, GDK_CURRENT_TIME, &err)) {
        g_warning ("xfce-net-plugin: no se pudo abrir el navegador: %s",
                   err->message);
        g_error_free (err);
    }
    g_free (uri);
}

void
on_refresh_clicked (GtkWidget *btn, NetPopup *popup)
{
    if (popup->scanning) return;

    popup->scanning = TRUE;
    gtk_widget_set_sensitive (btn, FALSE);
    if (popup->refresh_label)
        gtk_label_set_text (GTK_LABEL (popup->refresh_label), _("Updating…"));
    popup->scan_timeout_id =
        g_timeout_add (SCAN_COOLDOWN_MS, on_refresh_reenable, popup);

    GSList *devs = nm_get_wifi_devices (popup->conn);
    for (GSList *l = devs; l; l = l->next) {
        NmDevice *dev = l->data;
        nm_request_scan (popup->conn, dev->object_path);
    }
    nm_device_list_free (devs);

    /* Volver a comprobar la salida a Internet (por si ya se inició sesión
     * en el portal, o recién apareció). */
    nm_check_connectivity_async (popup->conn);
}

/* ---------- diálogo "Conectar a red oculta" ----------
 *
 * El diálogo queda abierto mientras se intenta la conexión: muestra
 * "Conectando…", se cierra solo si conecta, y si falla muestra el motivo y
 * deja corregir los datos. El intento lo sigue el seguidor común
 * (attempts.c), igual que los de las filas; el diálogo solo escucha sus
 * avisos. Regla: un perfil que ya existía nunca se borra; si el intento
 * falla, el seguidor le devuelve lo que se le cambió (clave, marca de
 * oculta) y borra el perfil que se haya creado para el intento. */

typedef struct {
    GDBusConnection *conn;
    GtkWidget       *dialog;
    GtkWidget       *ssid_entry;
    GtkWidget       *sec_combo;
    GtkWidget       *pass_entry;
    GtkWidget       *status_label;
    gchar           *ssid;          /* red del intento en curso, o NULL */
    gchar           *device_path;   /* adaptador del intento en curso */
    guint            listener;      /* avisos del seguidor */
} HiddenData;

void
on_eye_clicked (GtkWidget *btn, GtkEntry *entry)
{
    (void) btn;
    gtk_entry_set_visibility (entry, !gtk_entry_get_visibility (entry));
}

static void
hidden_set_busy (HiddenData *hd, gboolean busy)
{
    gtk_widget_set_sensitive (hd->ssid_entry, !busy);
    gtk_widget_set_sensitive (hd->sec_combo,  !busy);
    gtk_widget_set_sensitive (hd->pass_entry, !busy);
    gtk_dialog_set_response_sensitive (GTK_DIALOG (hd->dialog),
                                       GTK_RESPONSE_OK, !busy);
}

/* Muestra el motivo del fallo y deja corregir los datos. */
static void
hidden_show_failure (HiddenData *hd, const gchar *text)
{
    g_clear_pointer (&hd->ssid, g_free);
    g_clear_pointer (&hd->device_path, g_free);
    GtkStyleContext *ctx = gtk_widget_get_style_context (hd->status_label);
    gtk_style_context_remove_class (ctx, "dim-label");
    gtk_style_context_add_class    (ctx, "error");
    gtk_label_set_text (GTK_LABEL (hd->status_label), text);
    gtk_widget_show (hd->status_label);
    hidden_set_busy (hd, FALSE);
}

/* Al destruirse el diálogo (Cancelar, cerrar ventana o éxito). Un intento
 * ya pedido sigue su curso en NM y el seguidor lo sigue igual. */
static void
on_hidden_destroy (GtkWidget *dialog, HiddenData *hd)
{
    (void) dialog;
    attempts_remove_listener (hd->listener);
    g_free (hd->ssid);
    g_free (hd->device_path);
    g_free (hd);
}

static void
on_hidden_attempt (const gchar *ssid, const gchar *device_path,
                   AttemptEvent event, gpointer user_data)
{
    HiddenData *hd = user_data;
    if (!hd->ssid || g_strcmp0 (ssid, hd->ssid) != 0 ||
        g_strcmp0 (device_path, hd->device_path) != 0)
        return;

    Attempt a;
    switch (event) {
    case ATTEMPT_EVENT_CONNECTED:
        gtk_widget_destroy (hd->dialog);      /* libera hd vía on_hidden_destroy */
        break;
    case ATTEMPT_EVENT_FAILED:
        hidden_show_failure (hd, attempts_lookup (ssid, device_path, &a)
                                 ? failure_text (a.reason)
                                 : _("Last connection attempt failed"));
        break;
    case ATTEMPT_EVENT_CLEARED:
        /* Otro pedido en el mismo adaptador lo reemplazó. */
        hidden_show_failure (hd, _("Last connection attempt failed"));
        break;
    default:
        break;
    }
}

/* Elige el adaptador: el primero que esté encendido; si ninguno, el primero. */
static gchar *
pick_wifi_device (GDBusConnection *conn)
{
    gchar  *chosen = NULL;
    GSList *devs   = nm_get_wifi_devices (conn);
    for (GSList *l = devs; l && !chosen; l = l->next) {
        NmDevice *dev = l->data;
        if (nm_get_device_enabled (conn, dev->object_path))
            chosen = g_strdup (dev->object_path);
    }
    if (!chosen && devs)
        chosen = g_strdup (((NmDevice *) devs->data)->object_path);
    nm_device_list_free (devs);
    return chosen;
}

static void
on_hidden_response (GtkDialog *dialog, gint response, HiddenData *hd)
{
    if (response != GTK_RESPONSE_OK) {
        gtk_widget_destroy (GTK_WIDGET (dialog));
        return;
    }
    if (hd->ssid)
        return;   /* ya hay un intento en curso */

    const gchar *ssid     = gtk_entry_get_text (GTK_ENTRY (hd->ssid_entry));
    const gchar *password = gtk_entry_get_text (GTK_ENTRY (hd->pass_entry));
    gboolean     secure   = gtk_combo_box_get_active (
                                GTK_COMBO_BOX (hd->sec_combo)) == 1;

    if (!ssid || !*ssid) {
        gtk_widget_grab_focus (hd->ssid_entry);
        return;
    }
    if (secure && (!password || !*password)) {
        gtk_widget_grab_focus (hd->pass_entry);
        return;
    }

    gchar *device_path = pick_wifi_device (hd->conn);
    if (!device_path)
        return;

    /* Igual que en la fila: si ya hay un perfil de esa red que este
     * adaptador pueda usar y con la seguridad elegida, se reutiliza (solo se
     * le cambia la clave y se lo marca como oculta). Si no, se crea uno. */
    guint32  rsn    = secure ? (NM_AP_SEC_KEY_MGMT_PSK | NM_AP_SEC_KEY_MGMT_SAE) : 0;
    gchar   *reuse  = nm_find_profile_for_ap (hd->conn, ssid, 0, rsn, device_path);
    gchar   *old_pw = NULL;
    gboolean set_pw = FALSE, set_hidden = FALSE;
    if (reuse && !nm_profile_fits_ap (hd->conn, reuse, 0, rsn))
        g_clear_pointer (&reuse, g_free);
    if (reuse && secure) {
        old_pw = nm_get_profile_password (hd->conn, reuse);
        if (nm_set_profile_password (hd->conn, reuse, password))
            set_pw = TRUE;
        else
            g_clear_pointer (&reuse, g_free);   /* no se pudo: crear uno */
    }
    if (reuse && !nm_get_profile_hidden (hd->conn, reuse) &&
        nm_set_profile_hidden (hd->conn, reuse, TRUE))
        set_hidden = TRUE;

    GtkStyleContext *ctx = gtk_widget_get_style_context (hd->status_label);
    gtk_style_context_remove_class (ctx, "error");
    gtk_style_context_add_class    (ctx, "dim-label");
    gtk_label_set_text (GTK_LABEL (hd->status_label), _("Connecting…"));
    gtk_widget_show (hd->status_label);
    hidden_set_busy (hd, TRUE);

    /* Anotar el intento ANTES de pedirlo: si NM lo rechaza en el acto, el
     * aviso llega durante attempts_connect. */
    hd->ssid        = g_strdup (ssid);
    hd->device_path = device_path;

    AttemptRequest req = { 0 };
    req.ssid               = ssid;
    req.device_path        = device_path;
    req.profile_path       = reuse;
    req.password           = secure ? password : NULL;
    req.autoconnect        = TRUE;
    req.hidden             = TRUE;
    req.from_hidden        = TRUE;
    req.restore_password   = set_pw ? old_pw : NULL;
    req.restore_unhide     = set_hidden;
    req.delete_new_profile = TRUE;   /* un perfil oculto fallido reintentaría para siempre */
    attempts_connect (&req);

    secure_wipe_free (old_pw);
    g_free (reuse);
}

void
on_hidden_network_clicked (GtkWidget *btn, NetPopup *popup)
{
    (void) btn;
    GDBusConnection *conn = popup->conn;
    if (!conn) return;

    /* Wayland: el popup tiene la captura del teclado; con él abierto el
     * diálogo no recibe lo que se escribe. Se cierra antes, igual que hace
     * el agente de contraseñas. */
    gboolean wayland = popup_is_wayland (popup->window);
    if (wayland)
        popup_hide (popup);

    HiddenData *hd = g_new0 (HiddenData, 1);
    hd->conn     = conn;
    hd->listener = attempts_add_listener (on_hidden_attempt, hd);

    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Connect to hidden network"),
        NULL,
        0,
        _("Cancel"), GTK_RESPONSE_CANCEL,
        _("Connect"), GTK_RESPONSE_OK,
        NULL);
    hd->dialog = dialog;
    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

    GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (area), 12);

    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
    gtk_box_pack_start (GTK_BOX (area), grid, TRUE, TRUE, 0);

    GtkWidget *ssid_label = gtk_label_new (_("Network name:"));
    gtk_label_set_xalign (GTK_LABEL (ssid_label), 0.0);
    hd->ssid_entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (hd->ssid_entry), TRUE);
    gtk_grid_attach (GTK_GRID (grid), ssid_label,     0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), hd->ssid_entry, 1, 0, 1, 1);

    GtkWidget *sec_label = gtk_label_new (_("Security:"));
    gtk_label_set_xalign (GTK_LABEL (sec_label), 0.0);
    hd->sec_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (hd->sec_combo), _("None"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (hd->sec_combo), "WPA2/WPA3");
    gtk_combo_box_set_active (GTK_COMBO_BOX (hd->sec_combo), 1);
    gtk_grid_attach (GTK_GRID (grid), sec_label,     0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), hd->sec_combo, 1, 1, 1, 1);

    GtkWidget *pass_label = gtk_label_new (_("Password:"));
    gtk_label_set_xalign (GTK_LABEL (pass_label), 0.0);
    GtkWidget *pass_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    hd->pass_entry = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (hd->pass_entry), FALSE);
    gtk_entry_set_activates_default (GTK_ENTRY (hd->pass_entry), TRUE);
    GtkWidget *eye_btn  = gtk_button_new ();
    GtkWidget *eye_icon = gtk_image_new_from_icon_name ("view-reveal-symbolic",
                                                         GTK_ICON_SIZE_MENU);
    gtk_button_set_image  (GTK_BUTTON (eye_btn), eye_icon);
    gtk_button_set_relief (GTK_BUTTON (eye_btn), GTK_RELIEF_NONE);
    g_signal_connect (eye_btn, "clicked", G_CALLBACK (on_eye_clicked), hd->pass_entry);
    gtk_box_pack_start (GTK_BOX (pass_box), hd->pass_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (pass_box), eye_btn,        FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), pass_label, 0, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), pass_box,   1, 2, 1, 1);

    /* Línea de estado: "Conectando…" o el error. Oculta hasta el primer intento. */
    hd->status_label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (hd->status_label), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (hd->status_label),
                                 "dim-label");
    gtk_grid_attach (GTK_GRID (grid), hd->status_label, 0, 3, 2, 1);

    g_signal_connect (dialog, "response", G_CALLBACK (on_hidden_response), hd);
    g_signal_connect (dialog, "destroy",  G_CALLBACK (on_hidden_destroy),  hd);
    gtk_widget_show_all (dialog);
    gtk_widget_hide (hd->status_label);
    if (wayland) {
        gtk_window_present (GTK_WINDOW (dialog));
        gtk_widget_grab_focus (hd->ssid_entry);
    }
}

void
on_advanced_clicked (GtkWidget *btn, gpointer user_data)
{
    (void) btn; (void) user_data;

    if (g_find_program_in_path ("nm-connection-editor")) {
        g_spawn_command_line_async ("nm-connection-editor", NULL);
        return;
    }
    if (g_find_program_in_path ("cmst")) {
        g_spawn_command_line_async ("cmst", NULL);
        return;
    }
    if (g_find_program_in_path ("connman-gtk")) {
        g_spawn_command_line_async ("connman-gtk", NULL);
        return;
    }
    if (g_find_program_in_path ("nmtui")) {
        /* Abrir nmtui en una terminal. Primero la terminal preferida de Xfce
         * (exo-open respeta la que eligió el usuario); después las comunes.
         * Antes era siempre xterm, que casi nadie tiene en Xfce. */
        static const gchar *terms[][2] = {
            { "exo-open",           "exo-open --launch TerminalEmulator nmtui" },
            { "xfce4-terminal",     "xfce4-terminal -e nmtui" },
            { "x-terminal-emulator","x-terminal-emulator -e nmtui" },
            { "xterm",              "xterm -e nmtui" },
        };
        for (gsize i = 0; i < G_N_ELEMENTS (terms); i++) {
            gchar *found = g_find_program_in_path (terms[i][0]);
            if (found) {
                g_free (found);
                g_spawn_command_line_async (terms[i][1], NULL);
                return;
            }
        }
        g_warning ("xfce-net-plugin: nmtui está instalado pero no se encontró "
                   "ninguna terminal para abrirlo");
        return;
    }
    g_warning ("xfce-net-plugin: no se encontró ningún gestor de red avanzado "
               "(nm-connection-editor, cmst, connman-gtk o nmtui)");
}

/* ---------- búsqueda de redes ----------
 *
 * La franja de búsqueda está oculta. Aparece al empezar a escribir con el
 * foco en la lista (o en cualquier lugar que no sea un campo de texto) o con
 * Ctrl+F. Escape la borra y la oculta; un segundo Escape cierra el popup.
 * El filtrado en sí vive en popup-sections.c (apply_search_filter). */

static gboolean
search_is_open (NetPopup *popup)
{
    return popup->search_bar && gtk_widget_get_visible (popup->search_bar);
}

/* El popup no se puede agrandar al mostrar la franja: crecería hacia el
 * panel y lo taparía. Por eso a la lista se le resta lo que ocupa la franja
 * (incluido el aviso "No hay redes que coincidan" cuando está visible). */
void
search_sync_height (NetPopup *popup)
{
    if (!popup->scroll || popup->popup_height <= 0) return;
    gint used = 0;
    if (search_is_open (popup))
        gtk_widget_get_preferred_height (popup->search_bar, NULL, &used);

    /* El borde y el relleno del popover del tema se suman al contenido:
     * se descuentan para que la ventana mida lo configurado. */
    gint fw = 0, fh = 0;
    if (popup->frame) {
        GtkStyleContext *ctx = gtk_widget_get_style_context (popup->frame);
        GtkStateFlags    st  = gtk_style_context_get_state (ctx);
        GtkBorder b, p;
        gtk_style_context_get_border  (ctx, st, &b);
        gtk_style_context_get_padding (ctx, st, &p);
        fw = b.left + b.right + p.left + p.right;
        fh = b.top + b.bottom + p.top + p.bottom;
    }
    gtk_widget_set_size_request (popup->scroll,
                                 MAX (popup->popup_width - fw, 1),
                                 MAX (popup->popup_height - used - fh, 100));
}

static void
search_open (NetPopup *popup)
{
    if (!popup->search_bar) return;
    gtk_widget_show (popup->search_bar);
    search_sync_height (popup);
    /* Sin seleccionar el texto que ya hubiera: la letra nueva va al final. */
    gtk_entry_grab_focus_without_selecting (GTK_ENTRY (popup->search_entry));
}

/* Borra la búsqueda y oculta la franja. Si el foco estaba en el campo, lo
 * baja a la lista para poder seguir con el teclado. */
static void
search_close (NetPopup *popup)
{
    if (!popup->search_bar) return;
    gboolean had_focus = gtk_widget_has_focus (popup->search_entry);
    gtk_entry_set_text (GTK_ENTRY (popup->search_entry), "");
    gtk_widget_hide (popup->search_bar);
    search_sync_height (popup);
    if (had_focus && gtk_widget_get_visible (popup->window))
        gtk_widget_child_focus (popup->scroll, GTK_DIR_TAB_FORWARD);
}

void
on_search_button_clicked (GtkWidget *btn, NetPopup *popup)
{
    (void) btn;
    if (search_is_open (popup))
        search_close (popup);
    else
        search_open (popup);
}

static void
on_search_changed (GtkEditable *editable, NetPopup *popup)
{
    (void) editable;
    apply_search_filter (popup);
    /* Volver arriba: las coincidencias quedan a la vista. */
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (
                             GTK_SCROLLED_WINDOW (popup->scroll));
    gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));
}

/* Teclas de la búsqueda. Devuelve TRUE si la tecla fue consumida. */
static gboolean
search_handle_key (NetPopup *popup, GdkEventKey *event)
{
    if (!popup->search_entry)
        return FALSE;

    GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (popup->window));
    guint      mods  = event->state & gtk_accelerator_get_default_mod_mask ();

    /* Ctrl+F: abrir la búsqueda (o volver al campo si ya estaba abierta). */
    if ((mods & GDK_CONTROL_MASK) &&
        (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
        search_open (popup);
        return TRUE;
    }

    /* Escape con la búsqueda abierta: borrarla y ocultarla (no cierra). */
    if (event->keyval == GDK_KEY_Escape && search_is_open (popup)) {
        search_close (popup);
        return TRUE;
    }

    /* Desde el campo, Enter o flecha abajo bajan a la primera red visible. */
    if (focus == popup->search_entry &&
        (event->keyval == GDK_KEY_Down   || event->keyval == GDK_KEY_KP_Down ||
         event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)) {
        gtk_widget_child_focus (popup->scroll, GTK_DIR_TAB_FORWARD);
        return TRUE;
    }

    /* Una letra escrita fuera de cualquier campo de texto (por ejemplo, con
     * el foco en una fila) arranca la búsqueda. El espacio no cuenta: sobre
     * una fila abre el expand. Dentro del campo de contraseña tampoco: ahí
     * las letras son de la contraseña. */
    if (mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK))
        return FALSE;
    if (focus && GTK_IS_EDITABLE (focus))
        return FALSE;

    gunichar uc = gdk_keyval_to_unicode (event->keyval);
    if (!uc || uc == ' ' || !g_unichar_isprint (uc))
        return FALSE;

    search_open (popup);
    gchar buf[8];
    gint  len = g_unichar_to_utf8 (uc, buf);
    buf[len]  = '\0';
    GtkEditable *ed = GTK_EDITABLE (popup->search_entry);
    gint pos = (gint) gtk_entry_get_text_length (GTK_ENTRY (popup->search_entry));
    gtk_editable_insert_text (ed, buf, len, &pos);
    gtk_editable_set_position (ed, pos);
    return TRUE;
}

/* ---------- event handlers de la ventana ---------- */

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, NetPopup *popup)
{
    (void) widget;
    if (search_handle_key (popup, event))
        return GDK_EVENT_STOP;

    if (event->keyval == GDK_KEY_Escape) {
        popup_hide (popup);
        return GDK_EVENT_STOP;
    }

    /* Navegación con teclado dentro del popup.
     *
     * La ventana del popup es de tipo "menú emergente": el gestor de ventanas
     * no la trata como ventana con foco de teclado activo, así que GTK no
     * dispara solo su navegación con Tab. Las teclas SÍ llegan (Escape anda),
     * por eso movemos el foco nosotros con gtk_widget_child_focus, que recorre
     * los widgets enfocables (filas, botones del expand, switches, entry).
     *
     * Solo interceptamos Tab y Shift+Tab. Las flechas se dejan pasar a propósito
     * para no romper el movimiento del cursor dentro del campo de contraseña. */
    if (event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_KP_Tab) {
        gtk_widget_child_focus (popup->window, GTK_DIR_TAB_FORWARD);
        return GDK_EVENT_STOP;
    }
    if (event->keyval == GDK_KEY_ISO_Left_Tab) {   /* Shift+Tab */
        gtk_widget_child_focus (popup->window, GTK_DIR_TAB_BACKWARD);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

static gboolean
on_focus_out (GtkWidget *widget, GdkEventFocus *ev, NetPopup *popup)
{
    (void) widget; (void) ev;
    /* Abrir el menú contextual le quita el foco al popup: eso no es
     * "clickear afuera", no hay que cerrar. */
    if (popup->ctx_menu || popup->menu_idle_id)
        return FALSE;
    popup_hide (popup);
    return FALSE;
}

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *event, NetPopup *popup)
{
    gint x, y, w, h;
    (void) widget;

    gtk_window_get_position (GTK_WINDOW (popup->window), &x, &y);
    gtk_window_get_size     (GTK_WINDOW (popup->window), &w, &h);

    if (event->x_root < x || event->x_root > x + w ||
        event->y_root < y || event->y_root > y + h)
        popup_hide (popup);

    return GDK_EVENT_PROPAGATE;
}

/* ---------- barra de desplazamiento propia, encima de la lista ----------
 *
 * Historia: con barras clásicas (las que ocupan lugar), al aparecer la barra
 * la lista se angostaba y lo alineado a la derecha se corría (parches 02 y
 * 03: primero un margen, después la barra siempre presente e invisible).
 * Con eso nada se movía, pero las líneas separadoras y el resaltado de la
 * fila abierta terminaban antes del borde, donde empezaba la franja de la
 * barra.
 *
 * Ahora la lista ocupa TODO el ancho y la barra es una capa encima de ella
 * (GtkOverlay), del lado derecho: se muestra cuando hay algo que desplazar y
 * se oculta cuando no. Como está encima, aparecer o desaparecer no cambia el
 * ancho de nada. Para que nunca tape un botón o un texto, el contenido de
 * cada fila deja a la derecha su margen de siempre MÁS el ancho de la barra
 * (popup_end_margin); el fondo y las líneas sí llegan al borde.
 *
 * Con barras flotantes del sistema (se dibujan encima del contenido) se usa
 * la de GTK como siempre y la propia queda oculta; el margen extra es 0. */

/* ¿Las barras de esta lista son flotantes? Mismo criterio que usa GTK:
 * la propiedad de la lista, el ajuste del sistema y la variable de entorno. */
static gboolean
scroll_uses_overlay (GtkWidget *scroll)
{
    if (!gtk_scrolled_window_get_overlay_scrolling (GTK_SCROLLED_WINDOW (scroll)))
        return FALSE;
    if (g_strcmp0 (g_getenv ("GTK_OVERLAY_SCROLLING"), "0") == 0)
        return FALSE;
    GtkSettings *settings = gtk_widget_get_settings (scroll);
    gboolean     overlay  = TRUE;
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (settings),
                                      "gtk-overlay-scrolling"))
        g_object_get (settings, "gtk-overlay-scrolling", &overlay, NULL);
    return overlay;
}

/* Ancho de una barra vertical en el tema actual. GTK da 0 al medir un widget
 * oculto, así que no se puede medir la barra propia cuando no se ve: se mide
 * una barra de muestra que nunca se dibuja. Se rehace si cambia el tema. */
static gint
scrollbar_width (NetPopup *popup)
{
    GObject *win   = G_OBJECT (popup->window);
    gchar   *theme = NULL;
    g_object_get (gtk_widget_get_settings (popup->window),
                  "gtk-theme-name", &theme, NULL);

    GtkWidget *probe = g_object_get_data (win, "net-vbar-probe");
    if (!probe || g_strcmp0 (g_object_get_data (win, "net-vbar-theme"), theme) != 0) {
        probe = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, NULL);
        gtk_style_context_add_class (gtk_widget_get_style_context (probe),
                                     GTK_STYLE_CLASS_RIGHT);
        gtk_widget_show (probe);
        g_object_set_data_full (win, "net-vbar-probe",
                                g_object_ref_sink (probe), g_object_unref);
        g_object_set_data_full (win, "net-vbar-theme", g_strdup (theme), g_free);
    }
    g_free (theme);

    gint w = 0;
    gtk_widget_get_preferred_width (probe, &w, NULL);
    return w;
}

/* Margen derecho del contenido de una fila de la lista: el de siempre más
 * el ancho de la barra, que se dibuja encima (ver arriba). */
gint
popup_end_margin (NetPopup *popup)
{
    if (!popup->scroll || !popup->window || scroll_uses_overlay (popup->scroll))
        return POPUP_ROW_MARGIN;
    return POPUP_ROW_MARGIN + scrollbar_width (popup);
}

static void
scroll_bar_sync (NetPopup *popup)
{
    if (!popup->scroll)
        return;

    GtkScrolledWindow *sw      = GTK_SCROLLED_WINDOW (popup->scroll);
    gboolean           overlay = scroll_uses_overlay (popup->scroll);
    GtkWidget         *bar     = g_object_get_data (G_OBJECT (popup->scroll), "net-vbar");

    /* Barras clásicas: la lista se desplaza pero sin barra propia de GTK
     * (EXTERNAL); se ve la nuestra. Flotantes: la de GTK, como siempre. */
    GtkPolicyType want = overlay ? GTK_POLICY_AUTOMATIC : GTK_POLICY_EXTERNAL;
    GtkPolicyType hpol, vpol;
    gtk_scrolled_window_get_policy (sw, &hpol, &vpol);
    if (vpol != want)
        gtk_scrolled_window_set_policy (sw, GTK_POLICY_NEVER, want);

    /* ¿Hay algo que desplazar? (el contenido es más alto que lo visible) */
    GtkAdjustment *adj    = gtk_scrolled_window_get_vadjustment (sw);
    gboolean       needed = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_lower (adj)
                            > gtk_adjustment_get_page_size (adj);

    if (bar && gtk_widget_get_visible (bar) != (!overlay && needed))
        gtk_widget_set_visible (bar, !overlay && needed);
}

/* Diferido: el aviso de cambio llega en medio del acomodo de la ventana.
 * La demora no mueve nada: como mucho la barra aparece un instante tarde. */
static gboolean
scroll_reserve_idle (gpointer user_data)
{
    NetPopup *popup = user_data;
    popup->reserve_idle_id = 0;
    scroll_bar_sync (popup);
    return G_SOURCE_REMOVE;
}

/* Antes de dibujar la barra propia, se pinta debajo de ella el fondo del
 * popup (el del marco "menu"). Muchos temas dibujan la barra con fondo
 * transparente, y se veían a través de ella las líneas separadoras y el
 * resaltado de la fila abierta. Así, con barra, terminan donde empieza la
 * barra; sin barra siguen llegando al borde. No cambia ningún tamaño.
 * Se pinta el fondo del marco entero corrido a la posición de la barra (el
 * dibujo queda recortado a la barra): un degradado del tema queda alineado
 * con el resto del popup. */
static gboolean
on_vbar_draw (GtkWidget *bar, cairo_t *cr, NetPopup *popup)
{
    if (!popup->frame)
        return FALSE;
    gint x = 0, y = 0;
    if (!gtk_widget_translate_coordinates (bar, popup->frame, 0, 0, &x, &y))
        return FALSE;
    gtk_render_background (gtk_widget_get_style_context (popup->frame), cr,
                           -x, -y,
                           gtk_widget_get_allocated_width  (popup->frame),
                           gtk_widget_get_allocated_height (popup->frame));
    return FALSE;   /* después la barra se dibuja como siempre, encima */
}

/* Cambió el alto del contenido o el de la zona visible. */
static void
on_scroll_adj_changed (GtkAdjustment *adj, NetPopup *popup)
{
    (void) adj;
    if (!popup->reserve_idle_id)
        popup->reserve_idle_id = g_idle_add (scroll_reserve_idle, popup);
}

/* ---------- API publica ---------- */

NetPopup *
popup_create (XfcePanelPlugin *plugin, GtkWidget *button)
{
    (void) plugin; (void) button;
    NetPopup  *popup = g_new0 (NetPopup, 1);
    GtkWidget *win, *outer_box, *scroll;

    /* Proveedor de estilos ÚNICO para todo el plugin (filas, separadores y
     * aspecto del popup), registrado a nivel de pantalla una sola vez.
     * Nov-2026: los colores salen del propio popover (currentColor = su
     * letra) y NO de @theme_bg_color, que es el fondo de VENTANA del tema:
     * dentro de un popover casi no se veía (Adwaita) o dejaba la letra
     * ilegible (Numix). La fila abierta lleva además una barra con el color
     * de selección del tema. */
    {
        static gboolean css_installed = FALSE;
        if (!css_installed) {
            GtkCssProvider *prov = gtk_css_provider_new ();
            GError         *css_err = NULL;
            gtk_css_provider_load_from_data (prov,
                ".net-row:focus { background-color: alpha(currentColor, 0.06); }"
                ".net-row-open, .net-row-open:focus {"
                "  background-color: alpha(currentColor, 0.12);"
                "  box-shadow: inset 3px 0 @theme_selected_bg_color; }"
                ".net-row-open:dir(rtl), .net-row-open:focus:dir(rtl) {"
                "  box-shadow: inset -3px 0 @theme_selected_bg_color; }"
                ".module-sep { background-color: alpha(currentColor, 0.3); background-image: none; min-height: 2px; }"
                /* Ventana transparente: se ven las esquinas redondeadas. */
                "window.net-popup-window.net-rounded { background-color: transparent; background-image: none; }"
                /* Marco de menú: sin margen ni sombra (quedaría cortada) y
                 * recto salvo con la clase net-round (popup_set_round). */
                "menu.net-popup-frame { margin: 0; box-shadow: none; border-radius: 0; }"
                /* Separadores: sin margen (quedaba una franja sin resaltar
                 * junto a la fila) y con color propio relativo a la letra
                 * del menú: muchos temas los pintan casi del color del fondo
                 * (Arc: blanco sobre blanco). background-image: none anula
                 * degradados de algunos temas. */
                "menu.net-popup-frame separator { margin: 0; min-height: 1px;"
                "  background-color: alpha(currentColor, 0.15); background-image: none; }",
                -1, &css_err);
            if (css_err) {
                g_warning ("xfce-net-plugin: CSS: %s", css_err->message);
                g_error_free (css_err);
            }
            gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                GTK_STYLE_PROVIDER (prov),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref (prov);
            css_installed = TRUE;
        }
    }

    win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated        (GTK_WINDOW (win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW (win), TRUE);
    gtk_window_set_skip_pager_hint  (GTK_WINDOW (win), TRUE);
    gtk_window_set_resizable        (GTK_WINDOW (win), FALSE);
    gtk_window_set_type_hint        (GTK_WINDOW (win),
                                     GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_keep_above       (GTK_WINDOW (win), TRUE);
    gtk_widget_set_size_request     (win, -1, -1);
    gtk_style_context_add_class (gtk_widget_get_style_context (win), "net-popup-window");

    /* ventana → marco "menu" → todo el contenido. SIN la clase
     * "background": en Greybird claro pisaba el blanco del menú (#fcfcfc)
     * con el gris de las ventanas (#cecece). */
    popup->frame = g_object_new (net_menu_frame_get_type (),
                                 "orientation", GTK_ORIENTATION_VERTICAL, NULL);
    gtk_style_context_add_class (gtk_widget_get_style_context (popup->frame),
                                 "net-popup-frame");
    gtk_container_add (GTK_CONTAINER (win), popup->frame);

    /* Popover que nunca se muestra: solo para leer el radio de los popovers
     * del tema. Sin padre; lo mantiene vivo esta referencia y se libera con
     * la ventana. Lleva "background" porque los temas escriben
     * popover.background { border-radius: … }. */
    {
        GtkWidget *probe = g_object_new (net_popup_frame_get_type (),
                                         "orientation", GTK_ORIENTATION_VERTICAL, NULL);
        gtk_style_context_add_class (gtk_widget_get_style_context (probe), "background");
        g_object_set_data_full (G_OBJECT (win), "net-popover-probe",
                                g_object_ref_sink (probe), g_object_unref);
    }

    outer_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (outer_box), "popup");
    gtk_box_pack_start (GTK_BOX (popup->frame), outer_box, TRUE, TRUE, 0);

    popup->top_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (outer_box), popup->top_box, FALSE, FALSE, 0);

    /* Franja de búsqueda: fija (no se desplaza con la lista) y oculta hasta
     * que se usa. no_show_all evita que el show_all de popup_show la muestre. */
    {
        popup->search_bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start  (popup->search_bar, 12);
        gtk_widget_set_margin_end    (popup->search_bar, 12);
        gtk_widget_set_margin_bottom (popup->search_bar, 8);

        popup->search_entry = gtk_search_entry_new ();
        gtk_entry_set_placeholder_text (GTK_ENTRY (popup->search_entry),
                                        _("Search networks"));
        g_signal_connect (popup->search_entry, "changed",
                          G_CALLBACK (on_search_changed), popup);
        gtk_box_pack_start (GTK_BOX (popup->search_bar), popup->search_entry,
                            FALSE, FALSE, 0);
        gtk_widget_show (popup->search_entry);

        popup->search_empty = gtk_label_new (_("No matching networks"));
        gtk_style_context_add_class (gtk_widget_get_style_context (popup->search_empty),
                                     "dim-label");
        gtk_label_set_xalign (GTK_LABEL (popup->search_empty), 0.0);
        gtk_box_pack_start (GTK_BOX (popup->search_bar), popup->search_empty,
                            FALSE, FALSE, 0);
        gtk_widget_set_no_show_all (popup->search_empty, TRUE);
        gtk_widget_hide (popup->search_empty);

        gtk_box_pack_start (GTK_BOX (outer_box), popup->search_bar, FALSE, FALSE, 0);
        gtk_widget_set_no_show_all (popup->search_bar, TRUE);
        gtk_widget_hide (popup->search_bar);
    }

    /* Lista desplazable dentro de una capa (GtkOverlay) que lleva encima la
     * barra propia, del lado derecho (ver popup_end_margin). */
    GtkWidget *list_overlay = gtk_overlay_new ();
    gtk_box_pack_start (GTK_BOX (outer_box), list_overlay, TRUE, TRUE, 0);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (list_overlay), scroll);
    popup->scroll = scroll;

    {
        GtkWidget *vbar = gtk_scrollbar_new (
            GTK_ORIENTATION_VERTICAL,
            gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroll)));
        /* Clase de posición, igual que la barra de una lista común: los
         * temas dibujan el borde del lado que corresponde. */
        gtk_style_context_add_class (gtk_widget_get_style_context (vbar),
            gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL
                ? GTK_STYLE_CLASS_LEFT : GTK_STYLE_CLASS_RIGHT);
        gtk_widget_set_halign (vbar, GTK_ALIGN_END);   /* END: a la izquierda en RTL */
        gtk_widget_set_valign (vbar, GTK_ALIGN_FILL);
        gtk_widget_set_can_focus (vbar, FALSE);
        g_signal_connect (vbar, "draw", G_CALLBACK (on_vbar_draw), popup);
        gtk_overlay_add_overlay (GTK_OVERLAY (list_overlay), vbar);
        /* La muestra o la oculta scroll_bar_sync, no el show_all de popup_show. */
        gtk_widget_set_no_show_all (vbar, TRUE);
        gtk_widget_hide (vbar);
        g_object_set_data (G_OBJECT (scroll), "net-vbar", vbar);
    }

    popup->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (scroll), popup->content_box);
    g_signal_connect (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroll)),
                      "changed", G_CALLBACK (on_scroll_adj_changed), popup);

    /* Zona fija inferior. OJO: pack_end apila de abajo hacia arriba: de
     * abajo hacia arriba quedan Avanzado y Red oculta. Sin línea propia
     * arriba de los botones (nov-2026): la línea de debajo de la última red
     * ya separa la lista de esta zona. */
    GtkWidget *adv_btn = gtk_button_new_with_label (_("Advanced settings…"));
    gtk_button_set_relief (GTK_BUTTON (adv_btn), GTK_RELIEF_NONE);
    gtk_widget_set_margin_start  (adv_btn, 8);
    gtk_widget_set_margin_end    (adv_btn, 8);
    gtk_widget_set_margin_top    (adv_btn, 4);
    gtk_widget_set_margin_bottom (adv_btn, 4);
    g_signal_connect (adv_btn, "clicked",
                      G_CALLBACK (on_advanced_clicked), NULL);
    gtk_box_pack_end (GTK_BOX (outer_box), adv_btn, FALSE, FALSE, 0);

    GtkWidget *hidden_btn = gtk_button_new_with_label (_("Connect to hidden network…"));
    gtk_button_set_relief (GTK_BUTTON (hidden_btn), GTK_RELIEF_NONE);
    gtk_widget_set_margin_start  (hidden_btn, 8);
    gtk_widget_set_margin_end    (hidden_btn, 8);
    gtk_widget_set_margin_top    (hidden_btn, 0);
    gtk_widget_set_margin_bottom (hidden_btn, 4);
    g_signal_connect (hidden_btn, "clicked",
                      G_CALLBACK (on_hidden_network_clicked), popup);
    gtk_box_pack_end (GTK_BOX (outer_box), hidden_btn, FALSE, FALSE, 0);

    g_signal_connect (win, "key-press-event", G_CALLBACK (on_key_press), popup);

    popup->window = win;
    g_signal_connect (win, "size-allocate",
                      G_CALLBACK (on_popup_size_allocate), popup);
    popup->theme_settings = gtk_settings_get_for_screen (gtk_widget_get_screen (win));
    popup->theme_handler  = g_signal_connect (popup->theme_settings,
                                              "notify::gtk-theme-name",
                                              G_CALLBACK (on_theme_changed), popup);
    /* Avisos del seguidor de intentos (lo pidió el plugin antes de crear
     * el popup): redibujar cuando un intento empieza, conecta o falla. */
    popup->attempts_listener = attempts_add_listener (on_attempt_event, popup);
    return popup;
}

/* Elige el aspecto según el estilo pedido y lo que permite el sistema. Se
 * llama al principio de cada apertura, con la ventana OCULTA: el visual solo
 * se puede cambiar con la ventana sin realizar. Reevaluarlo en cada apertura
 * cubre también un compositor que se prende o se apaga. */
static void
popup_apply_style (NetPopup *popup)
{
    GtkWidget       *win    = popup->window;
    GdkScreen       *screen = gtk_widget_get_screen (win);
    GdkVisual       *rgba   = gdk_screen_get_rgba_visual (screen);
    GtkStyleContext *wctx   = gtk_widget_get_style_context (win);

    /* Radio de los POPOVERS del tema, leído del popover oculto (nunca del
     * marco de menú: casi siempre da 0 y Automático no flotaría nunca). */
    gint       radius = 0;
    GtkWidget *probe  = g_object_get_data (G_OBJECT (win), "net-popover-probe");
    if (probe) {
        GtkStyleContext *pctx = gtk_widget_get_style_context (probe);
        gtk_style_context_get (pctx, gtk_style_context_get_state (pctx),
                               "border-radius", &radius, NULL);
    }

    /* Transparencia solo con compositor: sin él, las esquinas saldrían negras.
     * (En Wayland siempre hay: el compositor es labwc / xfwl4.) */
    gboolean composited = gdk_screen_is_composited (screen) && rgba != NULL;

    /* Automático: flota solo si se puede (compositor) y el tema redondea de
     * forma VISIBLE. Antes bastaba cualquier radio mayor que 0, y casi todos
     * los temas dan 2-5 px: nunca quedaba pegada al panel. */
    gboolean floating;
    switch (popup->popup_style) {
    case NET_POPUP_STYLE_FLOATING: floating = TRUE;  break;
    case NET_POPUP_STYLE_ATTACHED: floating = FALSE; break;
    default: floating = composited && radius >= POPUP_ROUND_MIN; break;
    }
    gboolean want = floating && composited;

    if (!popup->style_applied || want != popup->rgba_active) {
        if (gtk_widget_get_realized (win) && !gtk_widget_get_visible (win))
            gtk_widget_unrealize (win);
        if (!gtk_widget_get_realized (win)) {
            gtk_widget_set_visual (win, want ? rgba : gdk_screen_get_system_visual (screen));
            popup->rgba_active   = want;
            popup->style_applied = TRUE;
        }
        /* Si no se pudo (ventana visible), se aplica en la próxima apertura. */
    }

    if (popup->rgba_active)
        gtk_style_context_add_class    (wctx, "net-rounded");
    else
        gtk_style_context_remove_class (wctx, "net-rounded");

    /* Los colores son siempre los del menú; el estilo solo cambia la
     * separación y las esquinas (redondeadas solo con transparencia). */
    popup_set_round (popup, popup->rgba_active ? radius : 0);

    /* Separación del panel: flotante sí, pegada no. */
    popup->float_gap = floating ? POPUP_FLOAT_GAP : 0;
}

/* ---------- medición de la apertura ----------
 *
 * Mide cuánto tarda el popup desde el clic en el botón del panel (momento
 * que anota plugin.c en el dato "net-open-t0" de la ventana) hasta que la
 * ventana se dibuja por primera vez. Separa tres tramos:
 *   datos:   clic → estado de la red leído y filas actualizadas;
 *   mostrar: datos → fin de popup_show (ubicar y mostrar la ventana);
 *   dibujo:  fin de popup_show → primer dibujo en pantalla.
 * No cambia nada del funcionamiento. El mensaje solo se ve lanzando el
 * panel con G_MESSAGES_DEBUG=xfce-net-plugin. */

static gint64
open_mark_get (NetPopup *popup, const gchar *key)
{
    gint64 *t = g_object_get_data (G_OBJECT (popup->window), key);
    return t ? *t : 0;
}

static void
open_mark_set (NetPopup *popup, const gchar *key)
{
    gint64 *t = g_new (gint64, 1);
    *t = g_get_monotonic_time ();
    g_object_set_data_full (G_OBJECT (popup->window), key, t, g_free);
}

static gboolean
on_first_draw (GtkWidget *win, cairo_t *cr, NetPopup *popup)
{
    (void) cr;
    g_signal_handlers_disconnect_by_func (win, on_first_draw, popup);

    gint64 now   = g_get_monotonic_time ();
    gint64 t0    = open_mark_get (popup, "net-open-t0");
    gint64 tdata = open_mark_get (popup, "net-open-data");
    gint64 tshow = open_mark_get (popup, "net-open-shown");
    if (!t0 || !tdata || !tshow)
        return FALSE;

    g_log ("xfce-net-plugin", G_LOG_LEVEL_DEBUG,
           "MEDICION apertura=%s datos=%.1f mostrar=%.1f dibujo=%.1f total=%.1f ms",
           GPOINTER_TO_INT (g_object_get_data (G_OBJECT (win), "net-open-full"))
               ? "completa" : "normal",
           (tdata - t0)    / 1000.0,
           (tshow - tdata) / 1000.0,
           (now - tshow)   / 1000.0,
           (now - t0)      / 1000.0);

    /* Una medición por apertura. */
    g_object_set_data (G_OBJECT (win), "net-open-t0", NULL);
    return FALSE;   /* no interferir con el dibujo */
}

void
popup_show (NetPopup *popup, XfcePanelPlugin *plugin, GtkWidget *button,
            GDBusConnection *conn,
            gint popup_width, gint popup_height)
{
    /* Medición: si el clic no anotó su momento, se toma desde acá. */
    if (!open_mark_get (popup, "net-open-t0"))
        open_mark_set (popup, "net-open-t0");
    g_object_set_data (G_OBJECT (popup->window), "net-open-full",
                       GINT_TO_POINTER (!popup->ui_built || popup->theme_dirty));
    g_signal_handlers_disconnect_by_func (popup->window, on_first_draw, popup);
    g_signal_connect_after (popup->window, "draw",
                            G_CALLBACK (on_first_draw), popup);

    popup->conn         = conn;
    popup->popup_width  = popup_width;
    popup->popup_height = popup_height;
    popup_apply_style (popup);
    gtk_widget_set_size_request (popup->window, popup_width, -1);
    search_sync_height (popup);   /* lista = alto configurado − borde del tema */
    scroll_bar_sync (popup);      /* barra propia: tipo de barra del sistema */

    /* Los intentos de conexión los sigue el seguidor común, también con el
     * popup cerrado: al armar las filas ya se sabe cómo está cada red. */

    /* Construir UI si es la primera vez. */
    nm_cache_begin (conn);
    if (!popup->ui_built || popup->theme_dirty) {
        popup->theme_dirty = FALSE;
        rebuild_ui (popup);
    } else {
        /* Ya estaba construida; refrescar estado por las dudas. */
        update_top_status      (popup);
        update_eth_section     (popup);
        update_devices_section (popup);
        update_vpn_section     (popup);
    }

    /* Pedir escaneo al abrir. Va dentro de la instantánea: la lista de
     * adaptadores sale de memoria, sin llamadas al bus con espera (el pedido
     * en sí no espera respuesta). Solo a los adaptadores que pueden escanear
     * (estado 30 = desconectado o más): a uno "no disponible" (20) o no
     * administrado (10) NM le contesta con un error y deja un aviso. */
    {
        GSList *devs = nm_get_wifi_devices (conn);
        for (GSList *l = devs; l; l = l->next) {
            NmDevice *dev = l->data;
            if (nm_get_device_state (conn, dev->object_path) >= 30)
                nm_request_scan (conn, dev->object_path);
        }
        nm_device_list_free (devs);
    }
    nm_cache_end ();
    open_mark_set (popup, "net-open-data");   /* medición */

    /* Suscribirse a señales DBus de NM (las gestiona el popup mientras está abierto). */
    if (!popup->signal_ids)
        popup->signal_ids = nm_subscribe_signals (conn, on_nm_signal_popup, popup);

    popup->button = button;
    if (popup_is_wayland (popup->window)) {
        popup_show_wayland (popup, plugin, button);
    } else {
        /* Mostrar el CONTENIDO (no la ventana) antes de medir: si no, la
         * primera vez el alto da 0 y con el panel abajo queda mal ubicada. */
        gtk_widget_show_all (popup->frame);
        gtk_widget_realize (popup->window);
        position_popup (popup, button, xfce_panel_plugin_arrow_type (plugin));
        gtk_widget_show_all (popup->window);

        /* Diferir el grab a cuando la ventana esté realmente visible. */
        GrabData *gd = g_new0 (GrabData, 1);
        gd->popup  = popup;
        gd->button = button;
        g_signal_connect (popup->window, "map-event",
                          G_CALLBACK (on_window_mapped), gd);
    }

    open_mark_set (popup, "net-open-shown");   /* medición */
}

void
popup_hide (NetPopup *popup)
{
    GdkDisplay *display;
    GdkSeat    *seat;

    if (!gtk_widget_get_visible (popup->window))
        return;

    /* Menú contextual abierto o a medio cerrar: descartarlo. */
    if (popup->menu_idle_id) {
        g_source_remove (popup->menu_idle_id);
        popup->menu_idle_id = 0;
    }
    if (popup->ctx_menu) {
        gtk_widget_destroy (popup->ctx_menu);
        popup->ctx_menu = NULL;
    }

    display = gtk_widget_get_display (popup->window);
    seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);
    popup->grab_active = FALSE;
    /* Wayland: soltar la captura interna de GTK (en X11 no se usa). */
    if (gtk_widget_has_grab (popup->window))
        gtk_grab_remove (popup->window);

    if (popup->press_handler) {
        g_signal_handler_disconnect (popup->window, popup->press_handler);
        popup->press_handler = 0;
    }

    /* Desuscribirse de las señales DBus mientras está cerrado: el plugin
     * sigue suscripto por su cuenta para mantener el ícono del panel al día. */
    if (popup->signal_ids) {
        nm_unsubscribe_signals (popup->conn, popup->signal_ids);
        popup->signal_ids = NULL;
    }

    /* Cancelar cooldown del botón Actualizar. */
    if (popup->scan_timeout_id) {
        g_source_remove (popup->scan_timeout_id);
        popup->scan_timeout_id = 0;
    }
    if (popup->refresh_idle_id) {
        g_source_remove (popup->refresh_idle_id);
        popup->refresh_idle_id = 0;
    }
    popup->scanning = FALSE;

    /* Cerrar expand abierto para que al reabrir el popup esté limpio. */
    if (popup->current_expand_box) {
        gtk_widget_hide (popup->current_expand_box);
        popup->current_expand_box = NULL;
    }
    clear_open_highlight (popup);
    g_clear_pointer (&popup->reopen_ssid, g_free);
    g_clear_pointer (&popup->reopen_dev, g_free);

    /* La búsqueda no sobrevive al cierre: al reabrir se ven todas las redes. */
    search_close (popup);

    /* Wayland: mientras el popup tuvo la captura, el botón del panel no se
     * enteró de que el mouse se fue, y quedaba resaltado hasta volver a
     * pasar por encima. También quedaba visible el indicador de foco del
     * teclado (el mismo que aparece al tabear). Se limpian los dos. */
    if (popup->button && popup_is_wayland (popup->window)) {
        gtk_widget_unset_state_flags (popup->button,
                                      GTK_STATE_FLAG_PRELIGHT | GTK_STATE_FLAG_ACTIVE);
        GtkWidget *top = gtk_widget_get_toplevel (popup->button);
        if (GTK_IS_WINDOW (top))
            gtk_window_set_focus_visible (GTK_WINDOW (top), FALSE);
    }

    /* El bloqueo de señales que había acá no matcheaba ningún manejador:
     * los del botón están registrados con el complemento como dato, no con
     * el botón mismo, así que el block/unblock era código muerto. La
     * protección real contra la recursión vive en on_popup_hidden de
     * plugin.c. */
    gtk_widget_hide (popup->window);
}

void
popup_destroy (NetPopup *popup)
{
    if (!popup) return;
    popup_hide (popup);
    /* La configuración de GTK sobrevive al popup: soltar el aviso de tema. */
    if (popup->theme_handler)
        g_signal_handler_disconnect (popup->theme_settings, popup->theme_handler);
    popup->theme_handler = 0;
    /* popup_hide no hace nada si el popup ya estaba oculto, así que los
     * temporizadores se cancelan también acá: ninguno debe disparar con el
     * popup liberado (quitar el plugin del panel). */
    /* Soltar el aviso de la barra ANTES de destruir la ventana: al
     * destruirse podría avisar un cambio y agendar un reacomodo sobre el
     * popup ya liberado. */
    if (popup->scroll)
        g_signal_handlers_disconnect_by_func (
            gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (popup->scroll)),
            on_scroll_adj_changed, popup);
    attempts_remove_listener (popup->attempts_listener);
    popup->attempts_listener = 0;
    guint *timers[] = { &popup->scan_timeout_id, &popup->refresh_idle_id,
                        &popup->menu_idle_id, &popup->reserve_idle_id };
    for (gsize i = 0; i < G_N_ELEMENTS (timers); i++) {
        if (*timers[i]) {
            g_source_remove (*timers[i]);
            *timers[i] = 0;
        }
    }
    g_free (popup->reopen_ssid);
    g_free (popup->reopen_dev);
    gtk_widget_destroy (popup->window);
    g_free (popup);
}
