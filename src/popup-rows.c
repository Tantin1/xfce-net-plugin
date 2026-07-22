/* popup-rows.c — construcción de la fila de red (make_ap_row), su área
 * expandible y todos los callbacks de interacción con una fila: conectar,
 * desconectar, olvidar, reintentar, QR, detalles, foco y realce.
 * Separado de popup.c sin cambios de lógica. */

#include "popup-private.h"
#ifdef HAVE_LIBQRENCODE
#include <qrencode.h>
#endif

#ifdef HAVE_LIBQRENCODE
/* Escapa los caracteres especiales del formato WIFI: (\ ; , : ") para que
 * un SSID o una contraseña con ';' o ':' no rompan el QR generado. */
static gchar *
qr_escape (const gchar *s)
{
    GString *out = g_string_new (NULL);
    for (const gchar *p = s; *p; p++) {
        if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"')
            g_string_append_c (out, '\\');
        g_string_append_c (out, *p);
    }
    return g_string_free (out, FALSE);
}
#endif /* HAVE_LIBQRENCODE */


/* ---------- ícono decorativo QR para botón (Cairo) ---------- */

static gboolean
draw_qr_button_icon (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    (void) user_data;
    gint w = gtk_widget_get_allocated_width  (widget);
    gint h = gtk_widget_get_allocated_height (widget);
    gint s = MIN (w, h);
    gint x0 = (w - s) / 2;
    gint y0 = (h - s) / 2;

    /* Color del texto del tema */
    GtkStyleContext *ctx = gtk_widget_get_style_context (widget);
    GdkRGBA color;
    gtk_style_context_get_color (ctx, gtk_style_context_get_state (ctx), &color);

    gdouble u = s / 7.0;  /* unidad base */

    /* Surface intermedio con alpha para que CAIRO_OPERATOR_CLEAR funcione
     * correctamente independientemente de si el surface del widget tiene alpha. */
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo_t         *c    = cairo_create (surf);

#define DRAW_FINDER(cx, cy)     cairo_set_source_rgba (c, color.red, color.green, color.blue, color.alpha);     cairo_rectangle (c, (cx), (cy), u*3, u*3); cairo_fill (c);     cairo_set_operator (c, CAIRO_OPERATOR_CLEAR);     cairo_rectangle (c, (cx)+u*0.5, (cy)+u*0.5, u*2, u*2); cairo_fill (c);     cairo_set_operator (c, CAIRO_OPERATOR_OVER);     cairo_set_source_rgba (c, color.red, color.green, color.blue, color.alpha);     cairo_rectangle (c, (cx)+u, (cy)+u, u, u); cairo_fill (c);

    DRAW_FINDER (x0,         y0)          /* esquina superior izquierda */
    DRAW_FINDER (x0+s-u*3,   y0)          /* esquina superior derecha   */
    DRAW_FINDER (x0,         y0+s-u*3)    /* esquina inferior izquierda */

#undef DRAW_FINDER

    /* Algunos módulos de datos en la zona central */
    cairo_set_source_rgba (c, color.red, color.green, color.blue, color.alpha);
    gdouble pts[][2] = {
        {3.5, 1.5}, {4.5, 2.5}, {3.5, 3.5}, {5.0, 3.5},
        {3.5, 4.5}, {4.5, 5.0}, {5.5, 4.0}, {4.0, 4.0}
    };
    for (gsize i = 0; i < G_N_ELEMENTS (pts); i++)
        cairo_rectangle (c, x0 + pts[i][0]*u, y0 + pts[i][1]*u, u*0.8, u*0.8);
    cairo_fill (c);

    cairo_destroy (c);

    /* Composite el surface sobre el context del widget */
    cairo_set_source_surface (cr, surf, 0, 0);
    cairo_paint (cr);
    cairo_surface_destroy (surf);

    return FALSE;
}

/* ---------- dibujar matriz QR real con Cairo ---------- */

#ifdef HAVE_LIBQRENCODE
static gboolean
draw_qr_matrix (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    QRcode *qr = user_data;
    if (!qr) return FALSE;

    gint w = gtk_widget_get_allocated_width  (widget);
    gint h = gtk_widget_get_allocated_height (widget);
    gint n = qr->width;
    gdouble cell = MIN (w, h) / (gdouble)(n + 4); /* margen de 2 celdas */
    gdouble ox   = (w - cell * n) / 2.0;
    gdouble oy   = (h - cell * n) / 2.0;

    /* Fondo blanco */
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);

    /* Módulos */
    cairo_set_source_rgb (cr, 0, 0, 0);
    for (gint row = 0; row < n; row++) {
        for (gint col = 0; col < n; col++) {
            if (qr->data[row * n + col] & 1) {
                cairo_rectangle (cr,
                    ox + col * cell,
                    oy + row * cell,
                    cell, cell);
                cairo_fill (cr);
            }
        }
    }
    return FALSE;
}
#endif /* HAVE_LIBQRENCODE */

/* ---------- callback botón "Mostrar QR" ---------- */

static void
on_qr_clicked (GtkWidget *btn, gpointer rd_ptr)
{
#ifndef HAVE_LIBQRENCODE
    (void) btn; (void) rd_ptr;
    return;
#else
    RowData   *rd  = rd_ptr;
    GtkWidget *da  = rd->qr_drawing_area;
    if (!da) return;

    if (gtk_widget_get_visible (da)) {
        gtk_widget_hide (da);
        {
            GtkWidget *lbl = g_object_get_data (G_OBJECT (btn), "qr-label");
            if (lbl) gtk_label_set_text (GTK_LABEL (lbl), _("Show QR"));
        }
        return;
    }

    /* Cerrar panel de detalles si estaba abierto */
    if (rd->details_box && gtk_widget_get_visible (rd->details_box)) {
        gtk_widget_hide (rd->details_box);
        if (rd->details_arrow)
            gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                          "pan-down-symbolic", GTK_ICON_SIZE_MENU);
    }

    /* Obtener contraseña guardada */
    gchar *password = nm_get_saved_password (rd->popup->conn, rd->ssid);
    if (!password) {
        {
            GtkWidget *lbl = g_object_get_data (G_OBJECT (btn), "qr-label");
            if (lbl) gtk_label_set_text (GTK_LABEL (lbl), _("No saved password"));
        }
        return;
    }

    /* Construir cadena WIFI: escapando los caracteres especiales del formato.
     * Sin el escape, un SSID o una clave con ';' o ':' rompían el QR. */
    gchar *ssid_esc = qr_escape (rd->ssid);
    gchar *pass_esc = qr_escape (password);
    secure_wipe_free (password);
    gchar *wifi_str = g_strdup_printf ("WIFI:T:WPA;S:%s;P:%s;;", ssid_esc, pass_esc);
    g_free (ssid_esc);
    secure_wipe_free (pass_esc);

    QRcode *qr = QRcode_encodeString (wifi_str, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    secure_wipe_free (wifi_str);

    if (!qr) {
        {
            GtkWidget *lbl = g_object_get_data (G_OBJECT (btn), "qr-label");
            if (lbl) gtk_label_set_text (GTK_LABEL (lbl), _("QR error"));
        }
        return;
    }

    /* Desconectar handler anterior por nombre de señal + callback,
     * ignorando user_data (el QRcode* anterior puede diferir). */
    g_signal_handlers_disconnect_matched (da,
        G_SIGNAL_MATCH_FUNC,
        0, 0, NULL, draw_qr_matrix, NULL);
    QRcode *old_qr = g_object_get_data (G_OBJECT (da), "qrcode");
    if (old_qr) QRcode_free (old_qr);
    g_object_set_data (G_OBJECT (da), "qrcode", qr);

    g_signal_connect (da, "draw", G_CALLBACK (draw_qr_matrix), qr);
    gtk_widget_show (da);
    gtk_widget_queue_draw (da);
    {
        GtkWidget *lbl = g_object_get_data (G_OBJECT (btn), "qr-label");
        if (lbl) gtk_label_set_text (GTK_LABEL (lbl), _("Hide QR"));
    }
#endif /* HAVE_LIBQRENCODE */
}


void
reopen_expand_for_ssid (NetPopup *popup, const gchar *ssid, const gchar *device_path)
{
    if (!popup || !ssid) return;

    GList *sections = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *s = sections; s; s = s->next) {
        if (s->data == popup->vpn_section) continue;
        if (!GTK_IS_CONTAINER (s->data))    continue;

        GList *rows = gtk_container_get_children (GTK_CONTAINER (s->data));
        for (GList *r = rows; r; r = r->next) {
            if (!GTK_IS_EVENT_BOX (r->data)) continue;
            const gchar *row_ssid = g_object_get_data (G_OBJECT (r->data), "ssid");
            if (!row_ssid || g_strcmp0 (row_ssid, ssid) != 0) continue;
            const gchar *row_dev = g_object_get_data (G_OBJECT (r->data), "device-path");
            if (device_path && (!row_dev || g_strcmp0 (row_dev, device_path) != 0)) continue;

            GList *inner = gtk_container_get_children (GTK_CONTAINER (r->data));
            if (inner && inner->data) {
                RowData *rd = g_object_get_data (G_OBJECT (inner->data), "row-data");
                if (rd && rd->expand_box) {
                    if (g_object_get_data (G_OBJECT (rd->expand_box),
                                           "passive-connecting")) {
                        if (rd->retry_btn)
                            gtk_widget_set_sensitive (rd->retry_btn, FALSE);
                        if (rd->try_other_btn)
                            gtk_widget_set_sensitive (rd->try_other_btn, FALSE);
                    }
                    gtk_widget_set_no_show_all (rd->expand_box, FALSE);
                    gtk_widget_show_all (rd->expand_box);
                    gtk_widget_set_no_show_all (rd->expand_box, TRUE);
                    if (rd->state_a_box && rd->state_b_box) {
                        gtk_widget_show (rd->state_a_box);
                        gtk_widget_hide (rd->state_b_box);
                    }
                    popup->current_expand_box = rd->expand_box;
                }
            }
            g_list_free (inner);
            g_list_free (rows);
            goto done;
        }
        g_list_free (rows);
    }
done:
    g_list_free (sections);
}

static void
row_data_free (RowData *rd)
{
    g_free (rd->ssid);
    g_free (rd->ap_path);
    g_free (rd->device_path);
    g_free (rd);
}

static gboolean
scroll_to_expand_idle (gpointer user_data)
{
    NetPopup      *popup   = user_data;
    GtkWidget     *expand  = popup->current_expand_box;
    GtkWidget     *scroll  = popup->scroll;
    GtkWidget     *content = gtk_bin_get_child (GTK_BIN (scroll));
    GtkAdjustment *adj     = gtk_scrolled_window_get_vadjustment (
                                 GTK_SCROLLED_WINDOW (scroll));

    if (!expand || !gtk_widget_get_visible (expand))
        return G_SOURCE_REMOVE;

    /* El contenedor desplazable envuelve el contenido en un GtkViewport.
     * El ajustador trabaja con coordenadas relativas al hijo del GtkViewport. */
    if (GTK_IS_VIEWPORT (content))
        content = gtk_bin_get_child (GTK_BIN (content));
    if (!content)
        return G_SOURCE_REMOVE;

    gint expand_y = 0;
    if (!gtk_widget_translate_coordinates (expand, content, 0, 0, NULL, &expand_y))
        return G_SOURCE_REMOVE;

    GtkAllocation alloc;
    gtk_widget_get_allocation (expand, &alloc);

    gdouble cur    = gtk_adjustment_get_value (adj);
    gdouble page   = gtk_adjustment_get_page_size (adj);
    gdouble top    = (gdouble) expand_y;
    gdouble bottom = top + (gdouble) alloc.height;

    /* Mover lo mínimo necesario para que el expand sea completamente visible. */
    if (bottom > cur + page)
        gtk_adjustment_set_value (adj, MIN (bottom - page,
                                            gtk_adjustment_get_upper (adj) - page));
    else if (top < cur)
        gtk_adjustment_set_value (adj, top);

    return G_SOURCE_REMOVE;
}

static void
scroll_to_expand_on_alloc (GtkWidget *expand, GdkRectangle *alloc, gpointer user_data)
{
    (void) alloc;
    g_signal_handlers_disconnect_by_func (expand, scroll_to_expand_on_alloc, user_data);
    g_idle_add (scroll_to_expand_idle, user_data);
}

/* Saca el realce "abierta" (.net-row-open) de TODAS las filas. Se usa cuando
 * el expand se cierra desde lugares que no conocen la fila concreta (confirmar
 * conexión, timeout, olvidar). Recorre las secciones del content_box igual que
 * reopen_expand_for_ssid. */
void
clear_open_highlight (NetPopup *popup)
{
    if (!popup || !popup->content_box) return;
    GList *sections = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *s = sections; s; s = s->next) {
        if (!GTK_IS_CONTAINER (s->data)) continue;
        GList *rows = gtk_container_get_children (GTK_CONTAINER (s->data));
        for (GList *r = rows; r; r = r->next) {
            if (!GTK_IS_EVENT_BOX (r->data)) continue;
            gtk_style_context_remove_class (
                gtk_widget_get_style_context (GTK_WIDGET (r->data)),
                "net-row-open");
        }
        g_list_free (rows);
    }
    g_list_free (sections);
}

/* Al abrir un expand, mover el foco al primer widget enfocable de adentro
 * (típicamente el campo de contraseña o el primer botón). Se difiere con
 * g_idle_add porque el expand recién se muestra y el foco no agarra bien
 * en el mismo instante. Devuelve el GtkWidget* del expand por user_data. */
static gboolean
focus_into_expand_idle (gpointer user_data)
{
    GtkWidget *expand_box = user_data;
    if (GTK_IS_WIDGET (expand_box) && gtk_widget_get_visible (expand_box))
        gtk_widget_child_focus (expand_box, GTK_DIR_TAB_FORWARD);
    return G_SOURCE_REMOVE;
}

/* Abre/cierra el expand de una fila. Compartido entre el click de mouse
 * (on_row_clicked) y el teclado (on_row_key_press), para que ambos hagan
 * exactamente lo mismo. */
static void
row_toggle_expand (RowData *rd)
{
    gboolean ya_abierto = (rd->popup->current_expand_box == rd->expand_box);

    /* Sacar el realce "abierta" de cualquier fila que lo tuviera. */
    clear_open_highlight (rd->popup);

    if (rd->popup->current_expand_box) {
        gtk_widget_hide (rd->popup->current_expand_box);
        rd->popup->current_expand_box = NULL;
    }

    if (!ya_abierto) {
        /* Si el expand tiene estados A/B y estamos abriendo desde cero,
         * asegurar que el estado visible es A (regla: cerrar sin accionar
         * vuelve al primer estado). */
        if (rd->state_a_box && rd->state_b_box) {
            gtk_widget_show (rd->state_a_box);
            gtk_widget_hide (rd->state_b_box);
            if (rd->pass_entry)
                gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
        }
        gtk_widget_show (rd->expand_box);
        rd->popup->current_expand_box = rd->expand_box;
        /* Marcar esta fila como abierta (realce visible con mouse y teclado). */
        if (rd->row_box)
            gtk_style_context_add_class (
                gtk_widget_get_style_context (rd->row_box), "net-row-open");
        /* Desconectar cualquier conexión previa antes de reconectar,
         * para que al reabrir el mismo expand también se dispare. */
        g_signal_handlers_disconnect_by_func (rd->expand_box,
                                              scroll_to_expand_on_alloc,
                                              rd->popup);
        g_signal_connect (rd->expand_box, "size-allocate",
                          G_CALLBACK (scroll_to_expand_on_alloc), rd->popup);
        /* Bajar el foco al primer control del expand para poder navegarlo
         * con Tab/flechas (si no, el foco se queda en la fila y Enter cierra). */
        g_idle_add (focus_into_expand_idle, rd->expand_box);
    }
}

static void
on_row_clicked (GtkWidget *event_box, GdkEventButton *event, gpointer rd_ptr)
{
    (void) event_box;
    (void) event;
    row_toggle_expand ((RowData *) rd_ptr);
}

/* Permite abrir la fila con el teclado: Enter o Espacio cuando la fila
 * (event_box) tiene el foco. Devolver TRUE consume la tecla.
 *
 * IMPORTANTE: el expand vive DENTRO de este event_box, así que sus botones
 * (Conectar, Olvidar, etc.) son descendientes. Sin el chequeo de abajo, este
 * manejador se tragaría el Enter dirigido a esos botones y dispararía el
 * toggle (cerrando el expand) en vez de accionar el botón. Por eso solo
 * actuamos si el foco está exactamente en el event_box de la fila. */
static gboolean
on_row_key_press (GtkWidget *event_box, GdkEventKey *event, gpointer rd_ptr)
{
    if (!gtk_widget_is_focus (event_box))
        return FALSE;   /* el foco está en un botón del expand: dejar pasar la tecla */
    if (event->keyval == GDK_KEY_Return ||
        event->keyval == GDK_KEY_KP_Enter ||
        event->keyval == GDK_KEY_space) {
        row_toggle_expand ((RowData *) rd_ptr);
        return TRUE;
    }
    return FALSE;
}

static void
on_disconnect_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;

    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Disconnecting…"));

    OpInProgress *op = g_new0 (OpInProgress, 1);
    op->kind        = OP_DISCONNECT;
    op->ssid        = g_strdup (rd->ssid);
    op->device_path = g_strdup (rd->device_path);
    op->expand_box  = rd->expand_box;
    op->action_btn  = btn;
    op->popup       = rd->popup;
    op->timeout_id  = g_timeout_add (OP_TIMEOUT_MS, op_timeout_cb, op);
    g_hash_table_replace (rd->popup->ops_in_progress,
                          g_strdup (rd->device_path), op);

    /* Limpiar intento pendiente: la desconexión es voluntaria, no un fallo. */
    if (rd->popup->pending_attempts && rd->ssid && rd->device_path) {
        gchar *attempt_key = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_remove (rd->popup->pending_attempts, attempt_key);
        g_free (attempt_key);
    }

    nm_disconnect_device_async (rd->popup->conn, rd->device_path);
}

static void
on_forget_response (GtkDialog *dialog, gint response, gpointer rd_ptr)
{
    RowData *rd = rd_ptr;
    gtk_widget_destroy (GTK_WIDGET (dialog));
    if (response != GTK_RESPONSE_YES)
        return;
    nm_forget_connection (rd->popup->conn, rd->ssid);
    /* Limpiar marca de fallo: la red ya no está guardada. */
    {
        gchar *fk = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_remove (rd->popup->failed_ssids, fk);
        g_free (fk);
    }
    /* Limpiar intento pendiente: la red fue olvidada explícitamente. */
    {
        gchar *attempt_key = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_remove (rd->popup->pending_attempts, attempt_key);
        g_free (attempt_key);
    }

    /* Cerrar el expand para que update_devices_section pueda reconstruir las
     * filas (regla 1A bloquea la reconstrucción mientras hay expand abierto).
     * NM no emite señal de device al borrar un perfil de conexión, así que
     * forzamos el refresh nosotros. */
    if (rd->expand_box) {
        gtk_widget_hide (rd->expand_box);
        if (rd->popup->current_expand_box == rd->expand_box)
            rd->popup->current_expand_box = NULL;
    }
    schedule_refresh_ui (rd->popup);
}

static void
on_forget_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (!rd->confirm_box) return;
    if (rd->action_row)        gtk_widget_hide (rd->action_row);
    if (rd->autoconnect_check) gtk_widget_hide (rd->autoconnect_check);
    if (rd->qr_btn)            gtk_widget_hide (rd->qr_btn);
    if (rd->qr_drawing_area)   gtk_widget_hide (rd->qr_drawing_area);
    if (rd->details_btn)       gtk_widget_hide (rd->details_btn);
    if (rd->details_box)       gtk_widget_hide (rd->details_box);
    gtk_widget_show (rd->confirm_box);
}

static void
on_forget_cancel_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->confirm_box)       gtk_widget_hide (rd->confirm_box);
    if (rd->action_row)        gtk_widget_show (rd->action_row);
    if (rd->autoconnect_check) gtk_widget_show (rd->autoconnect_check);
    if (rd->qr_btn)            gtk_widget_show (rd->qr_btn);
    if (rd->details_btn)       gtk_widget_show (rd->details_btn);
}

static void
on_forget_confirm_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->active)
        nm_disconnect_device_async (rd->popup->conn, rd->device_path);
    nm_forget_connection (rd->popup->conn, rd->ssid);
    {
        gchar *fk = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_remove (rd->popup->failed_ssids, fk);
        g_free (fk);
    }
    {
        gchar *attempt_key = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_remove (rd->popup->pending_attempts, attempt_key);
        g_free (attempt_key);
    }
    if (rd->expand_box) {
        gtk_widget_hide (rd->expand_box);
        if (rd->popup->current_expand_box == rd->expand_box)
            rd->popup->current_expand_box = NULL;
    }
    schedule_refresh_ui (rd->popup);
}

static void
on_autoconnect_toggled (GtkToggleButton *btn, gpointer user_data)
{
    (void) user_data;
    const gchar     *ssid = g_object_get_data (G_OBJECT (btn), "ssid");
    GDBusConnection *conn = g_object_get_data (G_OBJECT (btn), "conn");
    if (!ssid || !conn) return;
    gboolean active = gtk_toggle_button_get_active (btn);
    nm_set_autoconnect_by_ssid (conn, ssid, active);
}

static void
do_connect (RowData *rd)
{
    gchar       *saved_pw    = NULL;
    const gchar *password    = NULL;
    gboolean     autoconnect = TRUE;

    if (rd->pass_entry)
        password = gtk_entry_get_text (GTK_ENTRY (rd->pass_entry));
    if (!password || !*password)
        saved_pw = nm_get_saved_password (rd->popup->conn, rd->ssid);

    if (rd->autoconnect_check)
        autoconnect = gtk_toggle_button_get_active (
                          GTK_TOGGLE_BUTTON (rd->autoconnect_check));

    /* Ocultar label de error previo, si existía. */
    GtkWidget *prev_err = g_object_get_data (G_OBJECT (rd->expand_box), "error-label");
    if (prev_err)
        gtk_widget_hide (prev_err);

    /* Decidir cuál es el botón "principal" según el estado visible del expand.
     * Si estamos en estado A (state_a_box visible), el botón principal es
     * "Reintentar con la contraseña guardada"; si estamos en estado B (o en
     * el expand simple sin fallo), es el "Conectar" normal. */
    GtkWidget   *primary_btn  = rd->action_btn;
    const gchar *primary_text = NULL;
    if (rd->state_a_box && gtk_widget_get_visible (rd->state_a_box)) {
        primary_btn  = rd->retry_btn;
        primary_text = _("Retry with saved password");
    } else if (rd->connect_btn_b && rd->state_b_box &&
               gtk_widget_get_visible (rd->state_b_box)) {
        primary_btn  = rd->connect_btn_b;
        primary_text = _("Connect");
    } else {
        primary_text = _("Connect");
    }

    /* UI inmediata: botón a "Conectando…", deshabilitado. Entry también. */
    gtk_widget_set_sensitive (primary_btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (primary_btn), _("Connecting…"));
    if (rd->pass_entry)
        gtk_widget_set_sensitive (rd->pass_entry, FALSE);

    /* Deshabilitar los otros botones del expand para evitar acciones cruzadas. */
    GSList *extras = NULL;
    if (rd->try_other_btn && gtk_widget_get_sensitive (rd->try_other_btn)) {
        gtk_widget_set_sensitive (rd->try_other_btn, FALSE);
        extras = g_slist_prepend (extras, rd->try_other_btn);
    }
    if (rd->back_btn && gtk_widget_get_sensitive (rd->back_btn)) {
        gtk_widget_set_sensitive (rd->back_btn, FALSE);
        extras = g_slist_prepend (extras, rd->back_btn);
    }
    if (rd->forget_btn && gtk_widget_get_sensitive (rd->forget_btn)) {
        gtk_widget_set_sensitive (rd->forget_btn, FALSE);
        extras = g_slist_prepend (extras, rd->forget_btn);
    }

    /* Registrar operación en curso. */
    OpInProgress *op = g_new0 (OpInProgress, 1);
    op->kind        = OP_CONNECT;
    op->ssid        = g_strdup (rd->ssid);
    op->device_path = g_strdup (rd->device_path);
    op->expand_box  = rd->expand_box;
    op->action_btn  = primary_btn;
    op->action_label = g_strdup (primary_text);
    op->pass_entry  = rd->pass_entry;
    op->extra_disabled = extras;
    op->popup       = rd->popup;
    op->timeout_id  = g_timeout_add (OP_TIMEOUT_MS, op_timeout_cb, op);
    g_hash_table_replace (rd->popup->ops_in_progress,
                          g_strdup (rd->device_path), op);

    /* Registrar intento pendiente con timestamp. Si el usuario cierra el popup
     * antes de que la op se resuelva, al reabrir lo procesamos para decidir
     * si marcar como fallido o no según el estado real de la conexión. */
    {
        gint64 *ts = g_new (gint64, 1);
        *ts = g_get_monotonic_time ();
        gchar *attempt_key = g_strdup_printf ("%s|%s", rd->ssid, rd->device_path);
        g_hash_table_replace (rd->popup->pending_attempts,
                              attempt_key, ts);
    }

    /* Activar spinner en el botón del panel mientras conecta. */
    if (rd->popup->plugin_ref)
        net_plugin_set_connecting (rd->popup->plugin_ref, TRUE);

    /* Si hay perfil guardado y no se ingresó password, usar ActivateConnection.
     * Si no, usar AddAndActivate. AddAndActivate crea un perfil nuevo en cada
     * llamada — si ya había uno guardado para este SSID, lo borramos antes para
     * evitar acumular perfiles duplicados tras varios intentos con clave mala. */
    if (saved_pw && (!password || !*password)) {
        /* Migración automática: si el perfil viejo está bindeado a un adapter
         * específico (campo interface-name fijado), se lo sacamos para que sirva
         * a cualquier wlanX. También borra duplicados si quedaron de antes. */
        nm_strip_interface_name (rd->popup->conn, rd->ssid);
        /* Aplicar el valor del checkbox antes de activar. */
        nm_set_autoconnect_by_ssid (rd->popup->conn, rd->ssid, autoconnect);
        nm_activate_connection_async (rd->popup->conn, rd->device_path,
                                      rd->ap_path, rd->ssid);
    } else {
        if (password && *password)
            nm_forget_connection (rd->popup->conn, rd->ssid);
        nm_add_and_activate_connection_async (rd->popup->conn, rd->device_path,
                                              rd->ap_path, rd->ssid,
                                              saved_pw ? saved_pw : password,
                                              rd->key_mgmt,
                                              autoconnect);
    }

    secure_wipe_free (saved_pw);
}

/* Handler de "Reintentar con la contraseña guardada": vacía el entry de
 * contraseña para forzar el uso de la guardada, después llama a do_connect. */
static void
on_retry_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->pass_entry)
        gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
    do_connect (rd);
}

/* Handler de "Probar otra contraseña": pasa del estado A al B (oculta los
 * botones grandes, muestra el entry y los botones Volver/Conectar). */
static void
on_try_other_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->state_a_box) gtk_widget_hide (rd->state_a_box);
    if (rd->state_b_box) {
        /* show_all para que los hijos del state_b_box (entry, botones Volver/Conectar)
         * salgan visibles. set_no_show_all en el contenedor padre evita que un
         * gtk_widget_show_all externo lo reabra, pero acá lo forzamos a mano. */
        gtk_widget_set_no_show_all (rd->state_b_box, FALSE);
        gtk_widget_show_all (rd->state_b_box);
        gtk_widget_set_no_show_all (rd->state_b_box, TRUE);
    }
    if (rd->pass_entry) {
        gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
        gtk_widget_grab_focus (rd->pass_entry);
    }
    /* Conectar arranca deshabilitado: se habilita cuando el entry tenga texto. */
    if (rd->connect_btn_b)
        gtk_widget_set_sensitive (rd->connect_btn_b, FALSE);
}

/* Handler de "Volver": pasa del estado B al A. */
static void
on_back_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->state_b_box) gtk_widget_hide (rd->state_b_box);
    if (rd->state_a_box) gtk_widget_show (rd->state_a_box);
    if (rd->pass_entry)
        gtk_entry_set_text (GTK_ENTRY (rd->pass_entry), "");
}

/* Habilita el botón Conectar (estado B) solo si el entry tiene texto. */
static void
on_pass_entry_b_changed (GtkEditable *editable, gpointer rd_ptr)
{
    RowData     *rd   = rd_ptr;
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (editable));
    if (rd->connect_btn_b)
        gtk_widget_set_sensitive (rd->connect_btn_b, text && *text);
}

static void
on_connect_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    do_connect ((RowData *) rd_ptr);
}

static gboolean
on_pass_entry_key_press (GtkWidget *widget, GdkEventKey *event, gpointer rd_ptr)
{
    (void) widget;
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        do_connect ((RowData *) rd_ptr);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void
on_eye_pressed (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->pass_entry)
        gtk_entry_set_visibility (GTK_ENTRY (rd->pass_entry), TRUE);
}

static void
on_eye_released (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData *rd = rd_ptr;
    if (rd->pass_entry)
        gtk_entry_set_visibility (GTK_ENTRY (rd->pass_entry), FALSE);
}

/* Agrega fila "Label: valor" al panel de detalles. Omite si valor es NULL o vacío. */
static void
details_add_row (GtkWidget *box, const gchar *label, const gchar *value)
{
    if (!value || !*value) return;
    GtkWidget *row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl  = gtk_label_new (NULL);
    gchar *markup   = g_markup_printf_escaped ("<b>%s</b>", label);
    gtk_label_set_markup (GTK_LABEL (lbl), markup);
    g_free (markup);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
    gtk_widget_set_size_request (lbl, 80, -1);
    GtkWidget *val  = gtk_label_new (value);
    gtk_label_set_xalign (GTK_LABEL (val), 0.0);
    gtk_label_set_selectable (GTK_LABEL (val), TRUE);
    gtk_label_set_line_wrap (GTK_LABEL (val), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (val), 24);
    gtk_box_pack_start (GTK_BOX (row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), val, TRUE,  TRUE,  0);
    gtk_widget_show_all (row);
    gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);
}

/* ---------- callback botón "Detalles" ---------- */

static void
on_details_clicked (GtkWidget *btn, gpointer rd_ptr)
{
    (void) btn;
    RowData   *rd = rd_ptr;
    GtkWidget *db = rd->details_box;
    if (!db) return;

    if (!gtk_widget_get_visible (db)) {
        /* Cerrar QR si estaba abierto */
        if (rd->qr_drawing_area && gtk_widget_get_visible (rd->qr_drawing_area)) {
            gtk_widget_hide (rd->qr_drawing_area);
            if (rd->qr_btn) {
                GtkWidget *lbl = g_object_get_data (G_OBJECT (rd->qr_btn), "qr-label");
                if (lbl) gtk_label_set_text (GTK_LABEL (lbl), _("Show QR"));
            }
        }
        if (rd->details_arrow)
            gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                          "pan-up-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_show (db);
    } else {
        if (rd->details_arrow)
            gtk_image_set_from_icon_name (GTK_IMAGE (rd->details_arrow),
                                          "pan-down-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_hide (db);
    }
}

/* ---------- construcción de una fila de red ---------- */

/* "Huella" del estado de una fila: condensa todo lo que afecta su CONTENIDO,
 * menos la intensidad de señal (que se actualiza en el lugar). Si la huella
 * no cambió entre refrescos, la fila existente se reutiliza tal cual; si
 * cambió, se recrea solo esa fila. Tiene que reflejar exactamente las
 * decisiones que toma make_ap_row al construir: activo (con la corrección
 * por intento pendiente), conectando, guardada, con fallo previo, y las
 * banderas de seguridad (de las que derivan empresarial y wpa-psk/sae). */
gchar *
row_fingerprint (NetPopup *popup, NmAccessPoint *ap, const gchar *device_path,
                 GHashTable *saved_ssids)
{
    gboolean ap_active     = ap->active;
    gboolean is_connecting = FALSE;

    if (popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS) {
                /* Igual que en make_ap_row: con intento pendiente no se le
                 * cree a NM el "activo", y la fila muestra "Conectando…". */
                ap_active     = FALSE;
                is_connecting = TRUE;
            }
        }
    }

    gboolean saved = (saved_ssids &&
                      g_hash_table_contains (saved_ssids, ap->ssid));

    gchar   *fk = g_strdup_printf ("%s|%s", ap->ssid,
                                   device_path ? device_path : "");
    gboolean had_failure = saved && ap->secure &&
                           g_hash_table_contains (popup->failed_ssids, fk);
    g_free (fk);

    return g_strdup_printf ("%d|%d|%d|%d|%d|%u",
                            ap_active, is_connecting, saved, had_failure,
                            ap->secure, ap->wpa_flags | ap->rsn_flags);
}

GtkWidget *
make_ap_row (NmAccessPoint *ap, NetPopup *popup, const gchar *device_path,
             GHashTable *saved_ssids)
{
    GtkWidget *outer, *event_box, *row, *ssid_label, *signal_icon;
    GtkWidget *expand_box, *action_btn, *action_row;
    GtkWidget *forget_btn = NULL;
    GtkWidget *pass_entry = NULL;
    GtkWidget *autoconnect_check_widget = NULL;
    GDBusConnection *conn = popup->conn;

    /* Tipo de seguridad del AP, derivado de las banderas:
     *  - empresarial (802.1X sin clave compartida): no se puede conectar con
     *    contraseña simple, hay que derivar a la configuración avanzada;
     *  - WPA3 puro (SAE sin PSK): el perfil debe crearse con "sae";
     *  - resto: clave compartida clásica ("wpa-psk", cubre WPA2 y mixto). */
    guint    sec_flags     = ap->wpa_flags | ap->rsn_flags;
    gboolean is_enterprise = ap->secure &&
                             (sec_flags & NM_AP_SEC_KEY_MGMT_802_1X) &&
                             !(sec_flags & (NM_AP_SEC_KEY_MGMT_PSK |
                                            NM_AP_SEC_KEY_MGMT_SAE));
    const gchar *ap_key_mgmt = NULL;
    if (ap->secure) {
        if ((ap->rsn_flags & NM_AP_SEC_KEY_MGMT_SAE) &&
            !(sec_flags & NM_AP_SEC_KEY_MGMT_PSK))
            ap_key_mgmt = "sae";
        else
            ap_key_mgmt = "wpa-psk";
    }

    /* Si hay un intento de conexión pendiente para este SSID y aún estamos
     * dentro de la ventana del timeout (OP_TIMEOUT_MS), NO confiar en lo que
     * dice NM sobre ap->active: NM puede reportar "activado" varios segundos
     * antes de que el AP rechace la auth con clave mala. Forzamos active=FALSE
     * para que la fila no muestre "Conectado" falsamente. */
    gboolean ap_active = ap->active;
    if (ap_active && popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS)
                ap_active = FALSE;
        }
    }

    outer     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    event_box = gtk_event_box_new ();
    gtk_event_box_set_above_child (GTK_EVENT_BOX (event_box), FALSE);
    /* Enfocable con Tab y navegable con teclado (accesibilidad). */
    gtk_widget_set_can_focus (event_box, TRUE);
    /* Realce visual del foco: fondo sutil derivado del color del tema.
     * Los estilos .net-row / .net-row-open viven en el proveedor CSS global
     * registrado una sola vez en popup_create. Antes se creaba un proveedor
     * nuevo POR FILA en cada refresco: cientos de objetos al pedo. */
    gtk_style_context_add_class (gtk_widget_get_style_context (event_box),
                                 "net-row");
    gtk_container_add (GTK_CONTAINER (event_box), outer);

    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start  (row, 12);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 8);
    gtk_widget_set_margin_bottom (row, 8);

    signal_icon = make_signal_icon (ap->strength, ap->secure, 22);
    gtk_box_pack_start (GTK_BOX (row), signal_icon, FALSE, FALSE, 0);
    /* Referencias para el refresco por diferencia: permiten actualizar el
     * ícono de señal en el lugar sin recrear la fila. */
    g_object_set_data (G_OBJECT (event_box), "signal-icon", signal_icon);
    g_object_set_data (G_OBJECT (event_box), "row-strength",
                       GINT_TO_POINTER (ap->strength));

    ssid_label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (ssid_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (ssid_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (ssid_label), 24);

    {
        const gchar *band;
        if (ap->frequency >= 5925)      band = "6G";
        else if (ap->frequency >= 5000) band = "5G";
        else                            band = "2.4G";

        /* Banda guardada en la fila: junto con el SSID forma la clave con la
         * que el refresco por diferencia identifica filas entre refrescos. */
        g_object_set_data_full (G_OBJECT (event_box), "band",
                                g_strdup (band), g_free);

        /* is_connecting: hay un intento de conexión a este SSID iniciado en otra
     * apertura del popup, y aún estamos dentro de la ventana de 20s. */
    gboolean is_connecting = FALSE;
    if (!ap_active && popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS)
                is_connecting = TRUE;
        }
    }

    if (ap_active) {
            gchar *markup = g_markup_printf_escaped (
                "<b>%s</b>  <small><span alpha='60%%'>%s</span></small>",
                ap->ssid, band);
            gtk_label_set_markup (GTK_LABEL (ssid_label), markup);
            g_free (markup);
            GtkWidget *check = gtk_image_new_from_icon_name (
                                   "emblem-default-symbolic", GTK_ICON_SIZE_MENU);
            GtkWidget *connected_right = gtk_label_new (_("Connected"));
            gtk_style_context_add_class (gtk_widget_get_style_context (connected_right),
                                         "dim-label");
            gtk_label_set_ellipsize (GTK_LABEL (connected_right), PANGO_ELLIPSIZE_NONE);
            gtk_widget_set_halign (connected_right, GTK_ALIGN_END);
            gtk_widget_set_hexpand (connected_right, FALSE);
            gtk_box_pack_end (GTK_BOX (row), check, FALSE, FALSE, 0);
            gtk_box_pack_end (GTK_BOX (row), connected_right, FALSE, FALSE, 4);
        } else if (is_connecting) {
            gchar *markup = g_markup_printf_escaped (
                "%s  <small><span alpha='60%%'>%s</span></small>",
                ap->ssid, band);
            gtk_label_set_markup (GTK_LABEL (ssid_label), markup);
            g_free (markup);
            GtkWidget *connecting_right = gtk_label_new (_("Connecting…"));
            gtk_style_context_add_class (gtk_widget_get_style_context (connecting_right),
                                         "dim-label");
            gtk_label_set_ellipsize (GTK_LABEL (connecting_right), PANGO_ELLIPSIZE_NONE);
            gtk_widget_set_halign (connecting_right, GTK_ALIGN_END);
            gtk_widget_set_hexpand (connecting_right, FALSE);
            gtk_box_pack_end (GTK_BOX (row), connecting_right, FALSE, FALSE, 4);
        } else {
            gchar *markup = g_markup_printf_escaped (
                "%s  <small><span alpha='60%%'>%s</span></small>",
                ap->ssid, band);
            gtk_label_set_markup (GTK_LABEL (ssid_label), markup);
            g_free (markup);
        }
    }

    gtk_box_pack_start (GTK_BOX (row), ssid_label, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (outer), row, FALSE, FALSE, 0);

    /* ---- área expandible ---- */
    expand_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start  (expand_box, 12);
    gtk_widget_set_margin_end    (expand_box, 12);
    gtk_widget_set_margin_top    (expand_box, 4);
    gtk_widget_set_margin_bottom (expand_box, 8);
    

    /* is_connecting: hay un intento de conexión a este SSID iniciado en otra
     * apertura del popup, y aún estamos dentro de la ventana de 20s. El expand
     * muestra solo un label "Conectando…", sin botones, para no confundir al
     * usuario con un "Conectar" cuando ya hay un intento en curso. */
    gboolean is_connecting = FALSE;
    if (!ap_active && popup->pending_attempts && ap->ssid && device_path) {
        gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
        gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
        g_free (attempt_key);
        if (ts) {
            gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
            if (age_ms < OP_TIMEOUT_MS)
                is_connecting = TRUE;
        }
    }

    if (ap_active) {
        action_btn = gtk_button_new_with_label (_("Disconnect"));
        /* Label de señal para red activa */
        {
            gchar *sig_text = g_strdup_printf (_("Signal %d%%"), ap->strength);
            GtkWidget *sig_label = gtk_label_new (sig_text);
            g_free (sig_text);
            gtk_style_context_add_class (gtk_widget_get_style_context (sig_label), "dim-label");
            gtk_label_set_xalign (GTK_LABEL (sig_label), 0.0);
            gtk_box_pack_start (GTK_BOX (expand_box), sig_label, FALSE, FALSE, 0);
            gtk_widget_show (sig_label);
        }
    } else if (is_connecting) {
        GtkWidget *connecting_label = gtk_label_new (_("Connecting…"));
        gtk_label_set_xalign (GTK_LABEL (connecting_label), 0.0);
        gtk_box_pack_start (GTK_BOX (expand_box), connecting_label, FALSE, FALSE, 0);
        gtk_widget_show (connecting_label);
        /* Botón "Connect" creado oculto: no rompe la lógica de action_row. */
        action_btn = gtk_button_new_with_label (_("Connect"));
        gtk_widget_set_no_show_all (action_btn, TRUE);
        /* Marca: este expand es pasivo (solo muestra "Conectando…"), no hay
         * interacción del usuario que proteger. update_devices_section puede
         * reconstruirlo libremente y reabrirlo después. */
        g_object_set_data (G_OBJECT (expand_box), "passive-connecting",
                           GINT_TO_POINTER (1));
        /* Agendar refresh para cuando expire la ventana de pending_attempts.
         * Si NM no emitió ninguna señal hasta entonces, queremos transicionar
         * el expand a estado A sin esperar a que el usuario interactúe. */
        {
            /* La tabla está indexada por "ssid|adaptador", no por SSID solo:
             * la búsqueda anterior con el SSID pelado fallaba siempre y el
             * temporizador nunca se agendaba. */
            gchar  *attempt_key = g_strdup_printf ("%s|%s", ap->ssid, device_path);
            gint64 *ts = g_hash_table_lookup (popup->pending_attempts, attempt_key);
            g_free (attempt_key);
            if (ts) {
                gint64 age_ms = (g_get_monotonic_time () - *ts) / 1000;
                gint64 remaining = OP_TIMEOUT_MS - age_ms;
                if (remaining < 100) remaining = 100;
                g_timeout_add ((guint) remaining, deferred_refresh_cb, popup);
            }
        }
    } else {
        /* Set precalculado: antes se enumeraban TODOS los perfiles guardados
         * una vez por fila (nm_has_saved_connection). */
        gboolean saved = (saved_ssids &&
                          g_hash_table_contains (saved_ssids, ap->ssid));
        gchar   *fk_row = g_strdup_printf ("%s|%s", ap->ssid, device_path ? device_path : "");
        gboolean had_failure = saved && ap->secure &&
            g_hash_table_contains (popup->failed_ssids, fk_row);
        g_free (fk_row);

        /* Label de información: señal siempre, + "Red guardada" si aplica. */
        {
            gchar *info_text;
            if (saved)
                info_text = g_strdup_printf (_("Saved network · Signal %d%%"), ap->strength);
            else
                info_text = g_strdup_printf (_("Signal %d%%"), ap->strength);
            GtkWidget *info_label = gtk_label_new (info_text);
            g_free (info_text);
            gtk_style_context_add_class (gtk_widget_get_style_context (info_label), "dim-label");
            gtk_label_set_xalign (GTK_LABEL (info_label), 0.0);
            gtk_box_pack_start (GTK_BOX (expand_box), info_label, FALSE, FALSE, 0);
            gtk_widget_show (info_label);
            /* Para que el refresco por diferencia actualice el "Señal NN%"
             * sin recrear la fila. */
            g_object_set_data (G_OBJECT (event_box), "info-label", info_label);
            g_object_set_data (G_OBJECT (event_box), "info-saved",
                               GINT_TO_POINTER (saved));
        }

        if (saved) {
            if (had_failure) {
                /* Mensaje "El último intento falló al conectar", visible siempre. */
                GtkWidget *fail_label = gtk_label_new (
                    _("Last connection attempt failed"));
                gtk_style_context_add_class (
                    gtk_widget_get_style_context (fail_label), "error");
                gtk_label_set_xalign (GTK_LABEL (fail_label), 0.0);
                gtk_box_pack_start (GTK_BOX (expand_box), fail_label,
                                    FALSE, FALSE, 0);
                gtk_widget_show (fail_label);
            }

            GtkWidget *autoconnect_check_saved =
                gtk_check_button_new_with_label (_("Connect automatically"));
            /* Leer el valor real del perfil guardado en NM */
            gboolean cur_ac = nm_get_autoconnect_by_ssid (popup->conn, ap->ssid);
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (autoconnect_check_saved), cur_ac);
            g_object_set_data_full (G_OBJECT (autoconnect_check_saved), "ssid",
                                    g_strdup (ap->ssid), g_free);
            g_object_set_data (G_OBJECT (autoconnect_check_saved), "conn", popup->conn);
            g_signal_connect (autoconnect_check_saved, "toggled",
                              G_CALLBACK (on_autoconnect_toggled), NULL);
            gtk_box_pack_start (GTK_BOX (expand_box), autoconnect_check_saved, FALSE, FALSE, 0);
            gtk_widget_show (autoconnect_check_saved);
            autoconnect_check_widget = autoconnect_check_saved;

            forget_btn = gtk_button_new_with_label (_("Forget"));
            gtk_widget_set_halign (forget_btn, GTK_ALIGN_START);
        }
        if (ap->secure && !saved && is_enterprise) {
            /* Red empresarial (802.1X): pedirle al usuario una clave
             * compartida fallaría siempre. Avisar y derivar al gestor
             * avanzado, que sí maneja usuario/certificados. */
            GtkWidget *ent_label = gtk_label_new (
                _("Enterprise network (802.1X) — use Advanced settings"));
            gtk_style_context_add_class (
                gtk_widget_get_style_context (ent_label), "dim-label");
            gtk_label_set_xalign (GTK_LABEL (ent_label), 0.0);
            gtk_label_set_line_wrap (GTK_LABEL (ent_label), TRUE);
            gtk_box_pack_start (GTK_BOX (expand_box), ent_label, FALSE, FALSE, 0);
            gtk_widget_show (ent_label);
        } else if (ap->secure && !saved) {
            GtkWidget *pass_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

            pass_entry = gtk_entry_new ();
            gtk_entry_set_placeholder_text (GTK_ENTRY (pass_entry), _("Password"));
            gtk_entry_set_visibility       (GTK_ENTRY (pass_entry), FALSE);
            gtk_box_pack_start (GTK_BOX (pass_row), pass_entry, TRUE, TRUE, 0);

            GtkWidget *eye_btn  = gtk_button_new ();
            GtkWidget *eye_icon = gtk_image_new_from_icon_name (
                                      "view-reveal-symbolic", GTK_ICON_SIZE_MENU);
            gtk_button_set_image  (GTK_BUTTON (eye_btn), eye_icon);
            gtk_button_set_relief (GTK_BUTTON (eye_btn), GTK_RELIEF_NONE);
            gtk_box_pack_start (GTK_BOX (pass_row), eye_btn, FALSE, FALSE, 0);

            gtk_box_pack_start (GTK_BOX (expand_box), pass_row, FALSE, FALSE, 0);
            gtk_widget_show_all (pass_row);

            GtkWidget *autoconnect_check =
                gtk_check_button_new_with_label (_("Connect automatically"));
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (autoconnect_check), TRUE);
            gtk_box_pack_start (GTK_BOX (expand_box), autoconnect_check, FALSE, FALSE, 0);
            gtk_widget_show (autoconnect_check);
            autoconnect_check_widget = autoconnect_check;
        }

        /* action_btn de uso general: "Connect" simple para red guardada sin
         * fallo o para red nueva. En el caso "red guardada con fallo" no lo
         * usamos como botón principal (creamos retry_btn / connect_btn_b
         * abajo), pero lo dejamos creado y oculto para no romper la lógica
         * existente de action_row / RowData. */
        action_btn = gtk_button_new_with_label (_("Connect"));
        if (had_failure)
            gtk_widget_set_no_show_all (action_btn, TRUE);
        /* Empresarial sin perfil: el Conectar simple no puede funcionar. */
        if (is_enterprise && !saved)
            gtk_widget_set_sensitive (action_btn, FALSE);
    }

    gtk_widget_set_halign (action_btn, GTK_ALIGN_END);

    /* Botón Olvidar para red activa (controlado por opción show_forget_active). */
    GtkWidget *active_forget_btn = NULL;
    if (ap_active && popup->show_forget_active) {
        active_forget_btn = gtk_button_new_with_label (_("Forget"));
        gtk_widget_set_halign (active_forget_btn, GTK_ALIGN_START);
    }

    action_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    /* Olvidar a la izquierda, acción principal a la derecha */
    if (!ap_active && forget_btn) {
        gtk_box_pack_start (GTK_BOX (action_row), forget_btn, FALSE, FALSE, 0);
        gtk_widget_show (forget_btn);
    }
    if (ap_active && active_forget_btn) {
        gtk_box_pack_start (GTK_BOX (action_row), active_forget_btn, FALSE, FALSE, 0);
        gtk_widget_show (active_forget_btn);
    }
    gtk_box_pack_end (GTK_BOX (action_row), action_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (expand_box), action_row, FALSE, FALSE, 0);

    /* Botón "Mostrar QR" y área de dibujo — solo para redes activas y seguras. */
    GtkWidget *qr_btn          = NULL;
    GtkWidget *qr_drawing_area = NULL;
    GtkWidget *details_btn     = NULL;
    GtkWidget *details_box     = NULL;
    GtkWidget *details_arrow   = NULL;

    /* Fila secundaria: [QR] [Detalles ▼] */
    {
        GtkWidget *sec_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign (sec_row, GTK_ALIGN_START);

        /* Botón QR — solo redes activas y seguras */
        if (ap_active && ap->secure) {
            qr_btn = gtk_button_new ();
            GtkWidget *qr_box  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget *qr_da   = gtk_drawing_area_new ();
            gtk_widget_set_size_request (qr_da, 16, 16);
            gtk_widget_set_app_paintable (qr_da, TRUE);
            g_signal_connect (qr_da, "draw", G_CALLBACK (draw_qr_button_icon), NULL);
            GtkWidget *qr_lbl  = gtk_label_new (_("Show QR"));
            gtk_box_pack_start (GTK_BOX (qr_box), qr_da,  FALSE, FALSE, 0);
            gtk_box_pack_start (GTK_BOX (qr_box), qr_lbl, FALSE, FALSE, 0);
            g_object_set_data (G_OBJECT (qr_btn), "qr-label", qr_lbl);
            gtk_widget_show_all (qr_box);
            gtk_container_add (GTK_CONTAINER (qr_btn), qr_box);
            gtk_button_set_relief (GTK_BUTTON (qr_btn), GTK_RELIEF_NONE);
            gtk_box_pack_start (GTK_BOX (sec_row), qr_btn, FALSE, FALSE, 0);
            gtk_widget_show (qr_btn);
        }

        /* Botón Detalles — todas las redes */
        details_btn   = gtk_button_new ();
        details_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic",
                                                       GTK_ICON_SIZE_MENU);
        GtkWidget *det_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *det_lbl = gtk_label_new (_("Details"));
        gtk_box_pack_start (GTK_BOX (det_box), det_lbl,       FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (det_box), details_arrow, FALSE, FALSE, 0);
        gtk_widget_show_all (det_box);
        gtk_container_add (GTK_CONTAINER (details_btn), det_box);
        gtk_button_set_relief (GTK_BUTTON (details_btn), GTK_RELIEF_NONE);
        gtk_box_pack_start (GTK_BOX (sec_row), details_btn, FALSE, FALSE, 0);
        gtk_widget_show (details_btn);

        gtk_box_pack_start (GTK_BOX (expand_box), sec_row, FALSE, FALSE, 0);
        gtk_widget_show (sec_row);
    }

    /* Área de dibujo del QR */
    if (ap_active && ap->secure) {
        qr_drawing_area = gtk_drawing_area_new ();
        gtk_widget_set_size_request (qr_drawing_area, 180, 180);
        gtk_widget_set_halign (qr_drawing_area, GTK_ALIGN_CENTER);
        gtk_box_pack_start (GTK_BOX (expand_box), qr_drawing_area, FALSE, FALSE, 4);
        gtk_widget_hide (qr_drawing_area);
        gtk_widget_set_no_show_all (qr_drawing_area, TRUE);
    }

    /* Panel de detalles */
    {
        details_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start  (details_box, 4);
        gtk_widget_set_margin_bottom (details_box, 4);

        if (ap_active) {
            NmConnectionDetails *det = nm_get_connection_details (conn, device_path);
            if (det) {
                gchar *ip_str = NULL;
                if (det->ip4_address && det->ip4_prefix)
                    ip_str = g_strdup_printf ("%s/%s", det->ip4_address, det->ip4_prefix);
                else if (det->ip4_address)
                    ip_str = g_strdup (det->ip4_address);
                details_add_row (details_box, _("IP:"),      ip_str);
                details_add_row (details_box, _("Gateway:"), det->ip4_gateway);
                details_add_row (details_box, _("DNS:"),     det->ip4_dns);
                details_add_row (details_box, _("IPv6:"),    det->ip6_address);
                g_free (ip_str);
                nm_connection_details_free (det);
            }
        }

        /* Datos del AP: seguridad, frecuencia, canal, BSSID */
        {
            const gchar *sec_str = _("Open");
            if      (is_enterprise)          sec_str = "Enterprise (802.1X)";
            else if ((ap->rsn_flags & NM_AP_SEC_KEY_MGMT_SAE) &&
                     !(sec_flags & NM_AP_SEC_KEY_MGMT_PSK))
                                             sec_str = "WPA3";
            else if (ap->rsn_flags)          sec_str = "WPA2/WPA3";
            else if (ap->wpa_flags)          sec_str = "WPA";
            else if (ap->secure)             sec_str = _("Encrypted");
            details_add_row (details_box, _("Security:"), sec_str);

            if (ap->frequency > 0) {
                gchar *freq_str = g_strdup_printf ("%u MHz", ap->frequency);
                details_add_row (details_box, _("Frequency:"), freq_str);
                g_free (freq_str);

                guint ch = 0;
                if      (ap->frequency >= 5925) ch = (ap->frequency - 5950) / 5;
                else if (ap->frequency >= 5000) ch = (ap->frequency - 5000) / 5;
                else if (ap->frequency >= 2412) ch = (ap->frequency - 2407) / 5;
                if (ch > 0) {
                    gchar *ch_str = g_strdup_printf ("%u", ch);
                    details_add_row (details_box, _("Channel:"), ch_str);
                    g_free (ch_str);
                }
            }

            details_add_row (details_box, _("BSSID:"), ap->bssid);
        }

        gtk_box_pack_start (GTK_BOX (expand_box), details_box, FALSE, FALSE, 0);
        gtk_widget_hide (details_box);
        gtk_widget_set_no_show_all (details_box, TRUE);
    }

    /* Contenedor de confirmación "¿Eliminar red guardada?" — oculto por defecto.
     * Las señales se conectan más abajo, cuando rd ya existe. */
    GtkWidget *confirm_box    = NULL;
    GtkWidget *confirm_cancel = NULL;
    GtkWidget *confirm_ok     = NULL;
    if (forget_btn || active_forget_btn) {
        confirm_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        gchar     *confirm_text  = g_strdup_printf (_("Forget network \"%s\"?"), ap->ssid);
        GtkWidget *confirm_label = gtk_label_new (confirm_text);
        g_free (confirm_text);
        gtk_label_set_xalign (GTK_LABEL (confirm_label), 0.0);
        gtk_box_pack_start (GTK_BOX (confirm_box), confirm_label, FALSE, FALSE, 0);

        GtkWidget *confirm_btns_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign (confirm_btns_row, GTK_ALIGN_END);

        confirm_cancel = gtk_button_new_with_label (_("Cancel"));
        confirm_ok     = gtk_button_new_with_label (_("Confirm"));
        gtk_box_pack_start (GTK_BOX (confirm_btns_row), confirm_cancel, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (confirm_btns_row), confirm_ok,     FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (confirm_box), confirm_btns_row,    FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (expand_box), confirm_box, FALSE, FALSE, 0);
        gtk_widget_show_all (confirm_box);
        gtk_widget_hide (confirm_box);
        gtk_widget_set_no_show_all (confirm_box, TRUE);
    }

    /* Si action_btn tiene no_show_all (caso red guardada con fallo, donde el
     * botón principal pasa a ser retry_btn / connect_btn_b), no lo mostramos.
     * Sin esto el "Connect" residual queda visible en el estado A. */
    if (!gtk_widget_get_no_show_all (action_btn))
        gtk_widget_show (action_btn);
    gtk_widget_show (action_row);

    gtk_widget_set_no_show_all (expand_box, TRUE);
    gtk_widget_hide (expand_box);
    gtk_box_pack_start (GTK_BOX (outer), expand_box, FALSE, FALSE, 0);

    /* ---- RowData ---- */
    RowData *rd     = g_new0 (RowData, 1);
    rd->popup       = popup;
    rd->expand_box  = expand_box;
    rd->action_btn  = action_btn;
    rd->ssid        = g_strdup (ap->ssid);
    rd->ap_path     = g_strdup (ap->object_path);
    rd->secure      = ap->secure;
    rd->active      = ap_active;
    rd->saved       = ap_active ? FALSE
                      : (saved_ssids &&
                         g_hash_table_contains (saved_ssids, ap->ssid));
    rd->key_mgmt    = ap_key_mgmt;
    rd->device_path = g_strdup (device_path);
    rd->pass_entry        = pass_entry;
    rd->autoconnect_check = autoconnect_check_widget;
    rd->forget_btn        = forget_btn;
    rd->confirm_box       = confirm_box;
    rd->action_row        = action_row;
    rd->qr_drawing_area   = qr_drawing_area;
    rd->qr_btn            = qr_btn;
    rd->active_forget_btn = active_forget_btn;
    rd->details_btn       = details_btn;
    rd->details_box       = details_box;
    rd->details_arrow     = details_arrow;
    if (confirm_cancel)
        g_signal_connect (confirm_cancel, "clicked",
                          G_CALLBACK (on_forget_cancel_clicked),  rd);
    if (confirm_ok)
        g_signal_connect (confirm_ok, "clicked",
                          G_CALLBACK (on_forget_confirm_clicked), rd);

    /* Si es red guardada con fallo previo, construir los dos sub-bloques
     * (estado A = botones de reintentar/probar otra; estado B = entry de
     * reescribir contraseña + Volver/Conectar). Se intercalan después del
     * autoconnect_check y antes del action_row (que solo tiene "Olvidar"). */
    gchar   *fk_exp      = g_strdup_printf ("%s|%s", ap->ssid, device_path ? device_path : "");
    gboolean had_fail_exp = g_hash_table_contains (popup->failed_ssids, fk_exp);
    g_free (fk_exp);
    if (!ap_active && rd->saved && ap->secure && had_fail_exp) {

        /* ---- Estado A ---- */
        rd->state_a_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        rd->retry_btn = gtk_button_new_with_label (
            _("Retry with saved password"));
        gtk_widget_set_hexpand (rd->retry_btn, FALSE);
        gtk_widget_set_halign  (rd->retry_btn, GTK_ALIGN_FILL);
        gtk_box_pack_start (GTK_BOX (rd->state_a_box), rd->retry_btn,
                            TRUE, TRUE, 0);

        rd->try_other_btn = gtk_button_new_with_label (
            _("Try a different password"));
        gtk_widget_set_hexpand (rd->try_other_btn, FALSE);
        gtk_widget_set_halign  (rd->try_other_btn, GTK_ALIGN_FILL);
        gtk_box_pack_start (GTK_BOX (rd->state_a_box), rd->try_other_btn,
                            TRUE, TRUE, 0);

        /* Insertar state_a_box justo antes del action_row. */
        gtk_box_pack_start (GTK_BOX (expand_box), rd->state_a_box,
                            FALSE, FALSE, 0);
        /* Si hay un intento de conexión en curso (conectando B), deshabilitar
         * los botones antes de mostrarlos para evitar el destello habilitado. */
        if (is_connecting) {
            gtk_widget_set_sensitive (rd->retry_btn,     FALSE);
            gtk_widget_set_sensitive (rd->try_other_btn, FALSE);
        }
        gtk_widget_show_all (rd->state_a_box);

        /* ---- Estado B (oculto inicialmente) ---- */
        rd->state_b_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        GtkWidget *pass_row_b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        pass_entry = gtk_entry_new ();
        gtk_entry_set_placeholder_text (GTK_ENTRY (pass_entry),
                                        _("Rewrite password"));
        gtk_entry_set_visibility       (GTK_ENTRY (pass_entry), FALSE);
        gtk_box_pack_start (GTK_BOX (pass_row_b), pass_entry, TRUE, TRUE, 0);

        GtkWidget *eye_btn_b  = gtk_button_new ();
        GtkWidget *eye_icon_b = gtk_image_new_from_icon_name (
                                    "view-reveal-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_image  (GTK_BUTTON (eye_btn_b), eye_icon_b);
        gtk_button_set_relief (GTK_BUTTON (eye_btn_b), GTK_RELIEF_NONE);
        gtk_box_pack_start (GTK_BOX (pass_row_b), eye_btn_b, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (rd->state_b_box), pass_row_b,
                            FALSE, FALSE, 0);

        GtkWidget *btns_row_b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign (btns_row_b, GTK_ALIGN_END);

        rd->back_btn = gtk_button_new_with_label (_("Back"));
        gtk_box_pack_start (GTK_BOX (btns_row_b), rd->back_btn,
                            FALSE, FALSE, 0);

        rd->connect_btn_b = gtk_button_new_with_label (_("Connect"));
        gtk_widget_set_sensitive (rd->connect_btn_b, FALSE);
        gtk_box_pack_start (GTK_BOX (btns_row_b), rd->connect_btn_b,
                            FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (rd->state_b_box), btns_row_b,
                            FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (expand_box), rd->state_b_box,
                            FALSE, FALSE, 0);

        gtk_widget_set_no_show_all (rd->state_b_box, TRUE);
        gtk_widget_hide (rd->state_b_box);

        /* Reordenar para que action_row (con el botón Olvidar) quede al final. */
        gtk_box_reorder_child (GTK_BOX (expand_box), action_row, -1);

        /* Apuntamos el pass_entry de RowData al de B (necesario para do_connect). */
        rd->pass_entry = pass_entry;

        /* Conectar señales propias de A y B. */
        g_signal_connect (rd->retry_btn, "clicked",
                          G_CALLBACK (on_retry_clicked), rd);
        g_signal_connect (rd->try_other_btn, "clicked",
                          G_CALLBACK (on_try_other_clicked), rd);
        g_signal_connect (rd->back_btn, "clicked",
                          G_CALLBACK (on_back_clicked), rd);
        g_signal_connect (rd->connect_btn_b, "clicked",
                          G_CALLBACK (on_connect_clicked), rd);
        g_signal_connect (pass_entry, "changed",
                          G_CALLBACK (on_pass_entry_b_changed), rd);
        g_signal_connect_swapped (eye_btn_b, "clicked",
                                  G_CALLBACK (on_eye_clicked), pass_entry);
    }

    g_object_set_data_full (G_OBJECT (outer), "row-data", rd,
                            (GDestroyNotify) row_data_free);
    rd->row_box = event_box;
    /* Marca el SSID y device_path en el event_box para identificar la fila desde afuera. */
    g_object_set_data_full (G_OBJECT (event_box), "ssid",
                            g_strdup (ap->ssid), g_free);
    g_object_set_data_full (G_OBJECT (event_box), "device-path",
                            g_strdup (device_path), g_free);
    /* Huella de estado: el refresco por diferencia la compara para decidir
     * si la fila se puede reutilizar o hay que recrearla. */
    g_object_set_data_full (G_OBJECT (event_box), "row-fp",
                            row_fingerprint (popup, ap, device_path, saved_ssids),
                            g_free);

    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_row_clicked), rd);
    g_signal_connect (event_box, "key-press-event",
                      G_CALLBACK (on_row_key_press), rd);

    if (ap_active) {
        g_signal_connect (action_btn, "clicked",
                          G_CALLBACK (on_disconnect_clicked), rd);
        if (qr_btn)
            g_signal_connect (qr_btn, "clicked",
                              G_CALLBACK (on_qr_clicked), rd);
        if (active_forget_btn)
            g_signal_connect (active_forget_btn, "clicked",
                              G_CALLBACK (on_forget_clicked), rd);
    } else {
        g_signal_connect (action_btn, "clicked",
                          G_CALLBACK (on_connect_clicked), rd);

        if (forget_btn)
            g_signal_connect (forget_btn, "clicked",
                              G_CALLBACK (on_forget_clicked), rd);

        if (pass_entry) {
            g_signal_connect (pass_entry, "key-press-event",
                              G_CALLBACK (on_pass_entry_key_press), rd);

            GtkWidget *pass_row = gtk_widget_get_parent (pass_entry);
            GList     *kids     = gtk_container_get_children (GTK_CONTAINER (pass_row));
            for (GList *k = kids; k; k = k->next) {
                if (GTK_IS_BUTTON (k->data)) {
                    g_signal_connect (k->data, "pressed",
                                      G_CALLBACK (on_eye_pressed), rd);
                    g_signal_connect (k->data, "released",
                                      G_CALLBACK (on_eye_released), rd);
                    break;
                }
            }
            g_list_free (kids);
        }
    }

    if (details_btn)
        g_signal_connect (details_btn, "clicked",
                          G_CALLBACK (on_details_clicked), rd);

    return event_box;
}

