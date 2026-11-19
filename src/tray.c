/* Tray icon (XEmbed system tray): a round violet badge with a white bell; while there are
 * unread notifications it blinks between violet and raspberry.
 * Always 22 px. Left click shows or hides the browser, right click opens a small menu. */
#include "tuxbrowser.h"
#include <math.h>

/* badge colours (0xRRGGBB) */
#define IDLE_COLOR    0x7E5BD6   /* violet */
#define UNREAD_COLOR  0xE0457B   /* raspberry */
#define BLINK_MS      500

static GtkStatusIcon *icon;
static GtkWidget *menu, *menu_show, *menu_close_to_tray;
static GdkPixbuf *pix_idle, *pix_unread;
#define ICON_SIZE 22   /* px, fixed */
static guint blink_id;
static gboolean blink_on;

/* ---------------------------------------------------------------- */
/* drawing                                                           */

static void to_rgb(guint32 rgb, double k, double *r, double *g, double *b)
{
    *r = ((rgb >> 16) & 0xff) / 255.0;
    *g = ((rgb >> 8) & 0xff) / 255.0;
    *b = (rgb & 0xff) / 255.0;
    if (k > 0) {
        *r += (1.0 - *r) * k; *g += (1.0 - *g) * k; *b += (1.0 - *b) * k;
    } else {
        *r *= 1.0 + k; *g *= 1.0 + k; *b *= 1.0 + k;
    }
}

static void add_stop(cairo_pattern_t *p, double off, guint32 rgb, double k)
{
    double r, g, b;
    to_rgb(rgb, k, &r, &g, &b);
    cairo_pattern_add_color_stop_rgb(p, off, r, g, b);
}

/* Round badge with a 1 px margin and a soft vertical gradient, a white bell inside. */
static GdkPixbuf *draw_badge(int size, guint32 color)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(s);
    double c = size / 2.0, rad = (size - 2.0) / 2.0;

    cairo_pattern_t *grad = cairo_pattern_create_linear(0, c - rad, 0, c + rad);
    add_stop(grad, 0.0, color, 0.12);
    add_stop(grad, 1.0, color, -0.12);
    cairo_arc(cr, c, c, rad, 0, 2 * G_PI);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    /* bell: dome and flared skirt, the knob on top and the clapper below */
    double u = rad * 2 / 22.0 * 0.82;   /* designed on a 22 px badge, scaled to ~60% of its width */
    double top = c - 6.0 * u, rim = c + 4.5 * u;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, c - 7.0 * u, rim);
    cairo_curve_to(cr, c - 5.0 * u, rim - 1.5 * u, c - 5.0 * u, c - 0.5 * u, c - 5.0 * u, c - 1.0 * u);
    cairo_curve_to(cr, c - 5.0 * u, top, c + 5.0 * u, top, c + 5.0 * u, c - 1.0 * u);
    cairo_curve_to(cr, c + 5.0 * u, c - 0.5 * u, c + 5.0 * u, rim - 1.5 * u, c + 7.0 * u, rim);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_arc(cr, c, top + 0.2 * u, 1.3 * u, 0, 2 * G_PI);         /* knob */
    cairo_fill(cr);
    cairo_arc(cr, c, rim + 1.4 * u, 2.0 * u, 0, G_PI);              /* clapper */
    cairo_fill(cr);

    cairo_destroy(cr);
    GdkPixbuf *p = gdk_pixbuf_get_from_surface(s, 0, 0, size, size);
    cairo_surface_destroy(s);
    return p;
}

static void icons_load(void)
{
    g_clear_object(&pix_idle);
    g_clear_object(&pix_unread);
    pix_idle = draw_badge(ICON_SIZE, IDLE_COLOR);
    pix_unread = draw_badge(ICON_SIZE, UNREAD_COLOR);
}

/* ---------------------------------------------------------------- */
/* state                                                             */

static gboolean any_window_active(void)
{
    for (GList *l = browser_windows(); l; l = l->next) {
        GtkWindow *w = GTK_WINDOW(((BrowserWindow *)l->data)->window);
        if (gtk_widget_get_visible(GTK_WIDGET(w)) && gtk_window_is_active(w)) return TRUE;
    }
    return FALSE;
}

static gboolean any_window_visible(void)
{
    for (GList *l = browser_windows(); l; l = l->next)
        if (gtk_widget_get_visible(GTK_WIDGET(((BrowserWindow *)l->data)->window))) return TRUE;
    return FALSE;
}

/* only touch the icon when something changes: each update makes the tray redo its layout */
static GdkPixbuf *shown_pix;
static char *shown_tip;
static gboolean sized;   /* the tray has told us its size: from then on the image shows at full size */

static void set_pix(GdkPixbuf *p)
{
    if (p == shown_pix) return;
    shown_pix = p;
    gtk_status_icon_set_from_pixbuf(icon, p);
}

static gboolean blink_cb(gpointer data)
{
    blink_on = !blink_on;
    set_pix(blink_on ? pix_unread : pix_idle);
    return G_SOURCE_CONTINUE;
}

static void blink_stop(void)
{
    if (blink_id) g_source_remove(blink_id);
    blink_id = 0;
}

/* Steady violet with nothing new; while there is, it blinks violet / raspberry, or stays
 * raspberry while you are in the browser (or blinking is turned off). */
static void tray_apply(void)
{
    if (!icon || !sized) return;
    guint unread = notices_unread();
    if (!unread) {
        blink_stop();
        set_pix(pix_idle);
    } else if (pref_get_bool("tray_blink") && !any_window_active()) {
        if (!blink_id) {
            blink_on = TRUE;
            set_pix(pix_unread);
            blink_id = g_timeout_add(BLINK_MS, blink_cb, NULL);
        }
    } else {
        blink_stop();
        set_pix(pix_unread);
    }
    char *tip = unread ? g_strdup_printf(APP_NAME "\n%u new notification%s", unread, unread == 1 ? "" : "s")
                       : g_strdup_printf(APP_NAME "\n%s", notices->len ? "No new notifications" : "No notifications");
    if (g_strcmp0(tip, shown_tip)) {
        gtk_status_icon_set_tooltip_text(icon, tip);
        g_free(shown_tip);
        shown_tip = tip;
    } else {
        g_free(tip);
    }
}

static void on_notices(gpointer data) { tray_apply(); }

static void on_window_active(GObject *w, GParamSpec *ps, gpointer data) { tray_apply(); }

/* each browser window reports focus changes, so blinking stops while you are in it */
void tray_window_added(BrowserWindow *bw)
{
    g_signal_connect(bw->window, "notify::is-active", G_CALLBACK(on_window_active), NULL);
}

/* ---------------------------------------------------------------- */
/* showing and hiding the browser                                    */

void tray_show_browser(void)
{
    GList *ws = browser_windows();
    if (!ws) {
        GtkApplication *app = GTK_APPLICATION(g_application_get_default());
        BrowserWindow *bw = browser_window_new(app);
        browser_window_open(bw, START_URI, FALSE);
        return;
    }
    /* the list is most recently focused first: raise that one last so it ends up on top */
    GList *rev = g_list_reverse(g_list_copy(ws));
    for (GList *l = rev; l; l = l->next) browser_window_show(l->data);
    g_list_free(rev);
}

void tray_hide_browser(void)
{
    session_save();
    for (GList *l = browser_windows(); l; l = l->next)
        gtk_widget_hide(GTK_WIDGET(((BrowserWindow *)l->data)->window));
    tray_apply();
}

gboolean tray_is_active(void) { return icon && gtk_status_icon_is_embedded(icon); }

/* the popover opens once the window is back on screen, not while it is being mapped */
static gboolean open_list(gpointer data)
{
    BrowserWindow *bw = browser_window_get_active();
    if (bw && gtk_widget_get_mapped(GTK_WIDGET(bw->window))) notifications_popup_show(bw);
    return G_SOURCE_REMOVE;
}

static void show_with_list(void)
{
    tray_show_browser();
    g_timeout_add(150, open_list, NULL);
}

/* Left click: with news, show the browser and the list; otherwise show or hide it. */
static void on_activate(GtkStatusIcon *i, gpointer data)
{
    if (notices_unread()) {
        show_with_list();
    } else if (any_window_active()) {
        tray_hide_browser();
    } else {
        tray_show_browser();
    }
}

static void m_show(GtkMenuItem *it, gpointer d)
{
    if (any_window_visible()) tray_hide_browser();
    else tray_show_browser();
}

static void m_notifications(GtkMenuItem *it, gpointer d) { show_with_list(); }

static void m_new_window(GtkMenuItem *it, gpointer d)
{
    BrowserWindow *bw = browser_window_new(GTK_APPLICATION(g_application_get_default()));
    browser_window_open(bw, START_URI, FALSE);
    gtk_window_present(GTK_WINDOW(bw->window));
}

static void m_clear(GtkMenuItem *it, gpointer d) { notices_clear(); }

static void m_close_to_tray(GtkCheckMenuItem *it, gpointer d)
{
    pref_set_bool("close_to_tray", gtk_check_menu_item_get_active(it));
    prefs_save();
}

static void m_quit(GtkMenuItem *it, gpointer d) { browsers_quit(); }

static GtkWidget *menu_item(const char *label, GCallback cb)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(it, "activate", cb, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static void build_menu(void)
{
    menu = gtk_menu_new();
    menu_show = menu_item("_Show Tux Browser", G_CALLBACK(m_show));
    menu_item("_Notifications", G_CALLBACK(m_notifications));
    menu_item("_Dismiss All Notifications", G_CALLBACK(m_clear));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item("New _Window", G_CALLBACK(m_new_window));
    menu_close_to_tray = gtk_check_menu_item_new_with_mnemonic("_Keep Running in Tray When Closed");
    g_signal_connect(menu_close_to_tray, "toggled", G_CALLBACK(m_close_to_tray), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_close_to_tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item("_Quit", G_CALLBACK(m_quit));
    gtk_widget_show_all(menu);
}

static void on_popup_menu(GtkStatusIcon *i, guint button, guint time, gpointer data)
{
    gtk_menu_item_set_label(GTK_MENU_ITEM(menu_show), any_window_visible() ? "_Hide Tux Browser" : "_Show Tux Browser");
    g_signal_handlers_block_by_func(menu_close_to_tray, m_close_to_tray, NULL);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_close_to_tray), pref_get_bool("close_to_tray"));
    g_signal_handlers_unblock_by_func(menu_close_to_tray, m_close_to_tray, NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, i, button, time);
}

/* The image is always drawn at ICON_SIZE. GTK would scale an image given before the icon
 * is embedded down to a guessed size, so the icon stays empty until the tray reports its
 * size; otherwise it starts out smaller and grows. */
static gboolean size_idle(gpointer data)
{
    if (!icon) return G_SOURCE_REMOVE;
    sized = TRUE;
    shown_pix = NULL;   /* set it again even if unchanged */
    blink_stop();
    tray_apply();
    return G_SOURCE_REMOVE;
}

static gboolean on_size_changed(GtkStatusIcon *i, int size, gpointer data)
{
    g_idle_add(size_idle, NULL);
    return TRUE;
}

/* Creates or removes the icon to match the "tray_icon" preference. */
void tray_update(void)
{
    gboolean want = pref_get_bool("tray_icon");
    if (want && !icon) {
        icons_load();
        /* the name is needed: without it the Xfce tray never embeds the icon. Once it is
         * embedded GTK 3 sometimes logs a harmless gdk_window_thaw_toplevel_updates
         * critical; a bare GtkStatusIcon does the same. */
        icon = gtk_status_icon_new();
        gtk_status_icon_set_title(icon, APP_NAME);
        gtk_status_icon_set_name(icon, "tuxbrowser");
        g_signal_connect(icon, "activate", G_CALLBACK(on_activate), NULL);
        g_signal_connect(icon, "popup-menu", G_CALLBACK(on_popup_menu), NULL);
        g_signal_connect(icon, "size-changed", G_CALLBACK(on_size_changed), NULL);
        if (!menu) build_menu();
        profile_connect(EV_NOTIFICATIONS, on_notices, icon);
        tray_apply();
        gtk_status_icon_set_visible(icon, TRUE);
    } else if (!want && icon) {
        blink_stop();
        profile_disconnect_data(icon);
        g_clear_object(&icon);
        shown_pix = NULL;
        sized = FALSE;
        g_clear_pointer(&shown_tip, g_free);
        if (!any_window_visible()) tray_show_browser();   /* nothing left to bring it back with */
    } else {
        blink_stop();   /* "tray_blink" may have changed */
        tray_apply();
    }
}
