/* Browser window: title bar with address bar, vertical tab/bookmark bars, status label. */
#include "tuxbrowser.h"
#include <string.h>

static GList *windows;          /* BrowserWindow*, most recently focused first */
static gboolean quitting;
static gboolean restoring;   /* append tabs in order while restoring a session */
static GtkWidget *build_downloads_button(BrowserWindow *bw);
static GtkWidget *build_notify_button(BrowserWindow *bw);

GList *browser_windows(void) { return windows; }

BrowserWindow *browser_window_get_active(void)
{
    return windows ? windows->data : NULL;
}

Tab *browser_window_current_tab(BrowserWindow *bw)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(bw->notebook));
    if (n < 0) return NULL;
    GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(bw->notebook), n);
    return child ? g_object_get_data(G_OBJECT(child), "tab") : NULL;
}

GList *browser_window_tabs_in_order(BrowserWindow *bw)
{
    GList *out = NULL;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(bw->notebook));
    for (int i = 0; i < n; i++) {
        Tab *t = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(GTK_NOTEBOOK(bw->notebook), i)), "tab");
        if (t) out = g_list_append(out, t);
    }
    return out;
}

/* ---------------------------------------------------------------- */
/* address parsing                                                   */

static gboolean looks_like_host(const char *s)
{
    if (strchr(s, ' ')) return FALSE;
    if (g_str_has_prefix(s, "localhost")) return TRUE;
    const char *slash = strchr(s, '/');
    char *host = slash ? g_strndup(s, slash - s) : g_strdup(s);
    char *colon = strrchr(host, ':');
    if (colon) *colon = 0;
    gboolean ok = strchr(host, '.') && host[0] != '.' && host[strlen(host) - 1] != '.';
    if (ok) {
        const char *tld = strrchr(host, '.') + 1;
        gboolean alpha = TRUE, digits = TRUE;
        for (const char *p = tld; *p; p++) {
            if (!g_ascii_isalpha(*p) && *p != '-') alpha = FALSE;
            if (!g_ascii_isdigit(*p)) digits = FALSE;
        }
        ok = (alpha && strlen(tld) >= 2) || digits;   /* domain or IPv4 */
    }
    g_free(host);
    return ok;
}

/* Turns whatever the user typed into a URL (or a search). Returns NULL for empty input. */
char *browser_resolve_input(const char *text)
{
    char *s = g_strstrip(g_strdup(text ? text : ""));
    char *res = NULL;
    if (!*s) {
        g_free(s);
        return NULL;
    }
    /* search keywords: "g cats", "w Linux" */
    const char *sp = strchr(s, ' ');
    if (sp) {
        char *kw = g_strndup(s, sp - s);
        for (guint i = 0; i < profile->engines->len; i++) {
            SearchEngine *e = profile->engines->pdata[i];
            if (*e->keyword && g_str_equal(kw, e->keyword)) {
                res = search_url_for(e, g_strstrip((char *)sp + 1));
                break;
            }
        }
        g_free(kw);
        if (res) { g_free(s); return res; }
    }
    if (g_str_has_prefix(s, "/") || g_str_has_prefix(s, "~/")) {
        char *p = s[0] == '~' ? g_build_filename(g_get_home_dir(), s + 2, NULL) : g_strdup(s);
        res = g_filename_to_uri(p, NULL, NULL);
        g_free(p);
    } else if (g_str_has_prefix(s, "tuxbrowser:") || g_str_has_prefix(s, "gbrowser:")) {
        res = g_strdup(START_URI);
    } else if (g_uri_peek_scheme(s) && (strstr(s, "://") || g_str_has_prefix(s, "about:") ||
               g_str_has_prefix(s, "data:") || g_str_has_prefix(s, "javascript:") ||
               g_str_has_prefix(s, "mailto:") || g_str_has_prefix(s, "file:"))) {
        res = g_strdup(s);
    } else if (looks_like_host(s)) {
        res = g_strconcat(g_str_has_prefix(s, "localhost") || g_ascii_isdigit(s[0]) ? "http://" : "https://", s, NULL);
    } else {
        SearchEngine *e = search_engine_default();
        res = e ? search_url_for(e, s) : g_strdup(s);
    }
    g_free(s);
    return res;
}

/* ---------------------------------------------------------------- */
/* status                                                            */

/* There is no status bar: link targets and short messages show in a small label floating
 * over the bottom-left corner of the page, only while there is something to show.
 * Loading progress is shown in the address bar. */
static void status_show(BrowserWindow *bw, const char *text)
{
    gboolean on = text && *text;
    if (on && g_strcmp0(text, gtk_label_get_text(GTK_LABEL(bw->status_label))))
        gtk_label_set_text(GTK_LABEL(bw->status_label), text);
    if (on != gtk_widget_get_visible(bw->status_float)) gtk_widget_set_visible(bw->status_float, on);
}

static void show_default_status(BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    status_show(bw, t && t->hover_uri ? t->hover_uri : bw->temp_status);
}

void browser_window_set_status(BrowserWindow *bw, const char *text)
{
    if (text && *text) status_show(bw, text);
    else show_default_status(bw);
}

static gboolean clear_flash(gpointer data)
{
    BrowserWindow *bw = data;
    bw->status_timeout_id = 0;
    g_clear_pointer(&bw->temp_status, g_free);
    show_default_status(bw);
    return G_SOURCE_REMOVE;
}

void browser_window_flash_status(BrowserWindow *bw, const char *text)
{
    g_free(bw->temp_status);
    bw->temp_status = g_strdup(text);
    if (bw->status_timeout_id) g_source_remove(bw->status_timeout_id);
    bw->status_timeout_id = g_timeout_add_seconds(3, clear_flash, bw);
    status_show(bw, text);
}

/* ---------------------------------------------------------------- */
/* chrome update                                                     */

static void set_address_text(BrowserWindow *bw, const char *text)
{
    bw->setting_address = TRUE;
    gtk_entry_set_text(GTK_ENTRY(bw->address), text ? text : "");
    bw->setting_address = FALSE;
}

/* Popup menus are destroyed once closed. GTK 3 emits "deactivate" before it activates
 * the chosen item, so destroying the menu right away would drop the click (and free
 * data the item's handler uses): wait until the main loop is idle. */
static gboolean destroy_idle(gpointer w)
{
    gtk_widget_destroy(w);
    g_object_unref(w);
    return G_SOURCE_REMOVE;
}

static void menu_destroy_later(GtkWidget *menu)
{
    g_idle_add(destroy_idle, g_object_ref(menu));
}

/* ---------------------------------------------------------------- */
/* vertical tab bar (overlays the page from the left)                */

static gboolean vtabs_shown(BrowserWindow *bw)
{
    return bw->vtabs_revealer && gtk_revealer_get_reveal_child(GTK_REVEALER(bw->vtabs_revealer));
}

static void vtab_close_clicked(GtkButton *b, BrowserWindow *bw)
{
    Tab *t = g_object_get_data(G_OBJECT(b), "tab");
    if (g_list_find(bw->tabs, t)) browser_window_close_tab(bw, t);
}

/* Favicon strip: a narrow column on the left that is always there while the tab bar
 * is collapsed, so favicons (which some sites change to get attention) stay visible.
 * The expanded tab bar slides in over it. */

#define VSTRIP_ICON 24   /* favicon size in logical pixels */
#define VSTRIP_WIDTH 34  /* icon + 3 px current-tab marker + padding + border */

static gboolean vstrip_pressed(GtkWidget *w, GdkEventButton *ev, BrowserWindow *bw)
{
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    Tab *t = g_object_get_data(G_OBJECT(w), "tab");
    if (!t || !g_list_find(bw->tabs, t)) return FALSE;
    if (ev->button == 2) browser_window_close_tab(bw, t);
    else if (ev->button == 3) browser_window_tab_context_menu(bw, t, ev);
    else return FALSE;
    return TRUE;
}

static void vstrip_clicked(GtkButton *b, BrowserWindow *bw)
{
    Tab *t = g_object_get_data(G_OBJECT(b), "tab");
    if (g_list_find(bw->tabs, t)) browser_window_select_tab(bw, t);
}

static void vstrip_scroll_to(GtkWidget *item, GdkRectangle *a, BrowserWindow *bw)
{
    /* keep the current tab in view (once, after the strip has been laid out) */
    g_signal_handlers_disconnect_by_func(item, vstrip_scroll_to, bw);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(bw->vstrip_scroll));
    double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    if (a->y < v) gtk_adjustment_set_value(adj, a->y);
    else if (a->y + a->height > v + page) gtk_adjustment_set_value(adj, a->y + a->height - page);
}

/* What a strip button shows; it is only redrawn when this changes. */
enum { VS_FAVICON, VS_SPINNER, VS_START, VS_PAGE };

static GtkWidget *vstrip_new_item(BrowserWindow *bw, Tab *t)
{
    GtkWidget *item = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(item), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(item, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(item), "vstrip-tab");
    g_object_set_data(G_OBJECT(item), "tab", t);
    g_object_set_data(G_OBJECT(item), "kind", GINT_TO_POINTER(-1));
    g_signal_connect(item, "clicked", G_CALLBACK(vstrip_clicked), bw);
    g_signal_connect(item, "button-press-event", G_CALLBACK(vstrip_pressed), bw);
    return item;
}

/* Brings one button up to date, touching only what changed (a changed child or tooltip
 * makes GTK re-lay out the strip, so unchanged ones are left alone). */
static void vstrip_update_item(GtkWidget *item, Tab *t, gboolean current, int scale)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(item);
    if (current != gtk_style_context_has_class(sc, "current")) {
        if (current) gtk_style_context_add_class(sc, "current");
        else gtk_style_context_remove_class(sc, "current");
    }

    const char *title = tab_get_title(t), *uri = tab_get_uri(t);
    char *tip = *uri && !tab_is_start_page(t) && g_strcmp0(title, uri)
              ? g_markup_printf_escaped("<b>%s</b>\n%s", title, uri)
              : g_markup_escape_text(title, -1);
    if (g_strcmp0(tip, g_object_get_data(G_OBJECT(item), "tip"))) {
        gtk_widget_set_tooltip_markup(item, tip);
        g_object_set_data_full(G_OBJECT(item), "tip", tip, g_free);
    } else {
        g_free(tip);
    }

    cairo_surface_t *fav = tab_is_start_page(t) ? NULL : webkit_web_view_get_favicon(t->view);
    int kind = tab_is_start_page(t) ? VS_START
             : fav ? VS_FAVICON   /* the favicon stays visible even while the page loads */
             : webkit_web_view_is_loading(t->view) ? VS_SPINNER : VS_PAGE;
    if (kind == GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "kind"))
        && (kind != VS_FAVICON || fav == g_object_get_data(G_OBJECT(item), "fav")))
        return;
    g_object_set_data(G_OBJECT(item), "kind", GINT_TO_POINTER(kind));
    /* hold a reference so the address can't be reused by the next favicon */
    g_object_set_data_full(G_OBJECT(item), "fav", fav ? cairo_surface_reference(fav) : NULL,
                           (GDestroyNotify)cairo_surface_destroy);

    GtkWidget *icon = NULL;
    cairo_surface_t *s = kind == VS_FAVICON ? favicon_surface_for_view(t->view, VSTRIP_ICON, scale) : NULL;
    if (s) {
        icon = gtk_image_new_from_surface(s);
        cairo_surface_destroy(s);
    } else if (kind == VS_SPINNER) {
        icon = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(icon));
    } else {
        icon = gtk_image_new_from_icon_name(kind == VS_START ? "emblem-documents-symbolic" : "text-html-symbolic",
                                            GTK_ICON_SIZE_LARGE_TOOLBAR);
        /* the empty-tab page is drawn smaller than favicons, in the same 24 px slot */
        gtk_image_set_pixel_size(GTK_IMAGE(icon), kind == VS_START ? 16 : VSTRIP_ICON);
    }
    gtk_widget_set_size_request(icon, VSTRIP_ICON, VSTRIP_ICON);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(item));
    if (old) gtk_widget_destroy(old);
    gtk_container_add(GTK_CONTAINER(item), icon);
    gtk_widget_show(icon);
}

static void vstrip_rebuild(BrowserWindow *bw)
{
    if (!bw->vstrip_box || !gtk_widget_get_visible(bw->vstrip)) return;   /* hidden in full screen */
    int scale = gtk_widget_get_scale_factor(bw->vstrip_box);
    GList *tabs = browser_window_tabs_in_order(bw);
    Tab *cur = browser_window_current_tab(bw);
    GList *items = gtk_container_get_children(GTK_CONTAINER(bw->vstrip_box));

    /* reuse the buttons while the tabs and their order are unchanged */
    gboolean same = g_list_length(items) == g_list_length(tabs);
    for (GList *i = items, *t = tabs; same && i; i = i->next, t = t->next)
        same = g_object_get_data(G_OBJECT(i->data), "tab") == t->data;
    if (!same) {
        for (GList *i = items; i; i = i->next) gtk_widget_destroy(i->data);
        g_list_free(items);
        items = NULL;
        for (GList *t = tabs; t; t = t->next) {
            GtkWidget *item = vstrip_new_item(bw, t->data);
            gtk_box_pack_start(GTK_BOX(bw->vstrip_box), item, FALSE, FALSE, 0);
            gtk_widget_show(item);
            items = g_list_append(items, item);
        }
    }

    GtkWidget *cur_item = NULL;
    for (GList *i = items, *t = tabs; i && t; i = i->next, t = t->next) {
        vstrip_update_item(i->data, t->data, t->data == cur, scale);
        if (t->data == cur) cur_item = i->data;
    }
    if (cur_item && cur_item != bw->vstrip_current) {
        /* the current tab changed: scroll it into view once it has been laid out */
        g_signal_connect(cur_item, "size-allocate", G_CALLBACK(vstrip_scroll_to), bw);
        gtk_widget_queue_allocate(cur_item);
    }
    bw->vstrip_current = cur_item;
    g_list_free(items);
    g_list_free(tabs);
}

static gboolean vstrip_refresh_idle(gpointer data)
{
    BrowserWindow *bw = data;
    bw->vstrip_refresh_id = 0;
    vstrip_rebuild(bw);
    return G_SOURCE_REMOVE;
}

/* favicon changes are shown promptly, without waiting for the slower tab bar refresh */
void browser_window_tab_icon_changed(BrowserWindow *bw, Tab *tab)
{
    if (!bw->vstrip_refresh_id) bw->vstrip_refresh_id = g_timeout_add(50, vstrip_refresh_idle, bw);
    browser_window_tab_changed(bw, tab);
}

static GtkWidget *build_vstrip(BrowserWindow *bw)
{
    bw->vstrip = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(bw->vstrip, "vstrip");
    bw->vstrip_scroll = gtk_scrolled_window_new(NULL, NULL);
    /* scrolls with the wheel but shows no scrollbar, which would eat the narrow width */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bw->vstrip_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
    /* A fixed width: whatever the buttons show, the strip never resizes the page next to
     * it (every page resize makes WebKit block the UI until the page has repainted). */
    gtk_widget_set_size_request(bw->vstrip, VSTRIP_WIDTH, -1);
    gtk_widget_set_vexpand(bw->vstrip_scroll, TRUE);
    bw->vstrip_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(bw->vstrip_scroll), bw->vstrip_box);
    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(add), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(add, FALSE);
    gtk_widget_set_tooltip_text(add, "New tab (Ctrl+T)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(add), "win.new-tab");
    gtk_style_context_add_class(gtk_widget_get_style_context(add), "vstrip-add");
    gtk_box_pack_start(GTK_BOX(bw->vstrip), add, FALSE, FALSE, 0);   /* on top, under the tab bar toggle */
    gtk_box_pack_start(GTK_BOX(bw->vstrip), bw->vstrip_scroll, TRUE, TRUE, 0);
    return bw->vstrip;
}

static void vtabs_rebuild(BrowserWindow *bw)
{
    vstrip_rebuild(bw);
    /* don't rebuild under an active drag: it would destroy the dragged row */
    if (!vtabs_shown(bw) || bw->vtabs_dragging) return;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(bw->vtabs_scroll));
    double scroll = gtk_adjustment_get_value(adj);

    GList *rows = gtk_container_get_children(GTK_CONTAINER(bw->vtabs_list));
    for (GList *l = rows; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(rows);

    GList *tabs = browser_window_tabs_in_order(bw);
    Tab *cur = browser_window_current_tab(bw);
    GtkWidget *cur_row = NULL;
    for (GList *l = tabs; l; l = l->next) {
        Tab *t = l->data;
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "tab", t);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *icon;
        if (webkit_web_view_is_loading(t->view) && !tab_is_start_page(t)) {
            icon = gtk_spinner_new();
            gtk_spinner_start(GTK_SPINNER(icon));
            gtk_widget_set_size_request(icon, 16, 16);
        } else {
            GdkPixbuf *pb = tab_is_start_page(t) ? NULL : favicon_for_view(t->view, 16);
            if (pb) {
                icon = gtk_image_new_from_pixbuf(pb);
                g_object_unref(pb);
            } else {
                icon = gtk_image_new_from_icon_name(tab_is_start_page(t) ? "emblem-documents-symbolic" : "text-html-symbolic",
                                                    GTK_ICON_SIZE_MENU);
            }
        }
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        GtkWidget *label = gtk_label_new(tab_get_title(t));
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
        gtk_widget_set_focus_on_click(close, FALSE);
        gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(close, "Close tab");
        gtk_style_context_add_class(gtk_widget_get_style_context(close), "tab-close");
        g_object_set_data(G_OBJECT(close), "tab", t);
        g_signal_connect(close, "clicked", G_CALLBACK(vtab_close_clicked), bw);
        gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(box), close, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), box);
        gtk_widget_set_tooltip_text(row, *tab_get_uri(t) && !tab_is_start_page(t) ? tab_get_uri(t) : tab_get_title(t));
        gtk_list_box_insert(GTK_LIST_BOX(bw->vtabs_list), row, -1);
        if (t == cur) cur_row = row;
    }
    gtk_widget_show_all(bw->vtabs_list);
    if (cur_row) gtk_list_box_select_row(GTK_LIST_BOX(bw->vtabs_list), GTK_LIST_BOX_ROW(cur_row));

    char *title = g_strdup_printf("Tabs (%u)", g_list_length(tabs));
    gtk_label_set_text(GTK_LABEL(bw->vtabs_title), title);
    g_free(title);
    g_list_free(tabs);
    gtk_adjustment_set_value(adj, scroll);
}

static void vtabs_row_activated(GtkListBox *lb, GtkListBoxRow *row, BrowserWindow *bw)
{
    Tab *t = g_object_get_data(G_OBJECT(row), "tab");
    if (g_list_find(bw->tabs, t)) browser_window_select_tab(bw, t);
}

static gboolean vtabs_pressed(GtkWidget *w, GdkEventButton *ev, BrowserWindow *bw)
{
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(w), (int)ev->y);
    Tab *t = row ? g_object_get_data(G_OBJECT(row), "tab") : NULL;
    if (ev->button == 1) {
        /* remember what a possible drag will move */
        bw->vtabs_drag_tab = t;
        bw->vtabs_press_x = ev->x;
        bw->vtabs_press_y = ev->y;
        return FALSE;
    }
    if (ev->button != 2 && ev->button != 3) return FALSE;
    if (!t || !g_list_find(bw->tabs, t)) return FALSE;
    if (ev->button == 2) browser_window_close_tab(bw, t);
    else browser_window_tab_context_menu(bw, t, ev);
    return TRUE;
}

static void vtabs_set_shown(BrowserWindow *bw, gboolean on)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(bw->vtabs_revealer), on);
    vtabs_rebuild(bw);
}

/* drag and drop reordering of the vertical tab bar */

static const GtkTargetEntry vtab_targets[] = { { "TUXBROWSER_TAB", GTK_TARGET_SAME_WIDGET, 0 } };

static GtkListBoxRow *vtabs_row_for_tab(BrowserWindow *bw, Tab *t)
{
    GtkListBoxRow *found = NULL;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(bw->vtabs_list));
    for (GList *l = rows; l && !found; l = l->next)
        if (g_object_get_data(G_OBJECT(l->data), "tab") == t) found = l->data;
    g_list_free(rows);
    return found;
}

static void vtabs_drag_begin(GtkWidget *w, GdkDragContext *ctx, BrowserWindow *bw)
{
    bw->vtabs_dragging = TRUE;
    GtkListBoxRow *row = bw->vtabs_drag_tab ? vtabs_row_for_tab(bw, bw->vtabs_drag_tab) : NULL;
    if (!row) return;
    /* use a picture of the row as the drag icon */
    GtkAllocation a;
    gtk_widget_get_allocation(GTK_WIDGET(row), &a);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, a.width, a.height);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    gtk_widget_draw(GTK_WIDGET(row), cr);
    cairo_set_source_rgb(cr, 0.31, 0.50, 0.78);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, 1, 1, a.width - 2, a.height - 2);
    cairo_stroke(cr);
    cairo_destroy(cr);
    cairo_surface_set_device_offset(s, -bw->vtabs_press_x, -(bw->vtabs_press_y - a.y));
    gtk_drag_set_icon_surface(ctx, s);
    cairo_surface_destroy(s);
}

static void vtabs_drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *data,
                                guint info, guint time, BrowserWindow *bw)
{
    gtk_selection_data_set(data, gtk_selection_data_get_target(data), 8, (const guchar *)"tab", 3);
}

static gboolean vtabs_drag_motion(GtkWidget *w, GdkDragContext *ctx, int x, int y, guint time, BrowserWindow *bw)
{
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(w), y);
    if (row) gtk_list_box_drag_highlight_row(GTK_LIST_BOX(w), row);
    else gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(w));
    gdk_drag_status(ctx, bw->vtabs_drag_tab ? GDK_ACTION_MOVE : 0, time);
    return TRUE;
}

static void vtabs_drag_leave(GtkWidget *w, GdkDragContext *ctx, guint time, BrowserWindow *bw)
{
    gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(w));
}

static void vtabs_drag_received(GtkWidget *w, GdkDragContext *ctx, int x, int y, GtkSelectionData *data,
                                guint info, guint time, BrowserWindow *bw)
{
    Tab *src = bw->vtabs_drag_tab;
    if (!src || !g_list_find(bw->tabs, src)) return;
    GtkNotebook *nb = GTK_NOTEBOOK(bw->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    int from = gtk_notebook_page_num(nb, src->nb_child);
    int to = n - 1;                               /* dropped below the last row: move to the end */
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(w), y);
    Tab *target = row ? g_object_get_data(G_OBJECT(row), "tab") : NULL;
    if (target && g_list_find(bw->tabs, target)) {
        GtkAllocation a;
        gtk_widget_get_allocation(GTK_WIDGET(row), &a);
        gboolean after = y > a.y + a.height / 2;  /* lower half: drop after the row */
        to = gtk_notebook_page_num(nb, target->nb_child) + (after ? 1 : 0);
        if (from < to) to--;
    }
    if (to != from) gtk_notebook_reorder_child(nb, src->nb_child, to);
}

static void vtabs_drag_end(GtkWidget *w, GdkDragContext *ctx, BrowserWindow *bw)
{
    bw->vtabs_dragging = FALSE;
    bw->vtabs_drag_tab = NULL;
    gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(w));
    vtabs_rebuild(bw);
}

static void vtabs_dnd_setup(BrowserWindow *bw)
{
    gtk_drag_source_set(bw->vtabs_list, GDK_BUTTON1_MASK, vtab_targets, G_N_ELEMENTS(vtab_targets), GDK_ACTION_MOVE);
    gtk_drag_dest_set(bw->vtabs_list, GTK_DEST_DEFAULT_DROP, vtab_targets, G_N_ELEMENTS(vtab_targets), GDK_ACTION_MOVE);
    g_signal_connect(bw->vtabs_list, "drag-begin", G_CALLBACK(vtabs_drag_begin), bw);
    g_signal_connect(bw->vtabs_list, "drag-data-get", G_CALLBACK(vtabs_drag_data_get), bw);
    g_signal_connect(bw->vtabs_list, "drag-motion", G_CALLBACK(vtabs_drag_motion), bw);
    g_signal_connect(bw->vtabs_list, "drag-leave", G_CALLBACK(vtabs_drag_leave), bw);
    g_signal_connect(bw->vtabs_list, "drag-data-received", G_CALLBACK(vtabs_drag_received), bw);
    g_signal_connect(bw->vtabs_list, "drag-end", G_CALLBACK(vtabs_drag_end), bw);
}

/* ---------------------------------------------------------------- */
/* vertical bookmarks bar (overlays the page from the right)         */

enum { VB_ICON, VB_TITLE, VB_URL, VB_TIP, VB_N };

static void vbm_rebuild(BrowserWindow *bw)
{
    if (!bw->vbm_store) return;
    GtkTreeModel *model = GTK_TREE_MODEL(bw->vbm_store);
    /* remember which folders are open so a rebuild (e.g. after a drop) keeps them open */
    GHashTable *open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(model, &it); ok; ok = gtk_tree_model_iter_next(model, &it)) {
        GtkTreePath *p = gtk_tree_model_get_path(model, &it);
        if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(bw->vbm_view), p)) {
            char *t = NULL;
            gtk_tree_model_get(model, &it, VB_TITLE, &t, -1);
            if (t) g_hash_table_add(open, t);
        }
        gtk_tree_path_free(p);
    }
    gtk_tree_store_clear(bw->vbm_store);
    const char *q = gtk_entry_get_text(GTK_ENTRY(bw->vbm_search));
    char *fq = *q ? g_utf8_casefold(q, -1) : NULL;
    /* reordering a filtered list would lose the hidden bookmarks, so only allow it unfiltered */
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(bw->vbm_view), fq == NULL);
    GHashTable *folders = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)gtk_tree_iter_free);
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        if (fq) {
            char *a = g_utf8_casefold(b->title, -1), *u = g_utf8_casefold(b->url, -1);
            gboolean hit = strstr(a, fq) || strstr(u, fq);
            g_free(a); g_free(u);
            if (!hit) continue;
        }
        GtkTreeIter *parent = NULL;
        if (b->folder && *b->folder) {
            parent = g_hash_table_lookup(folders, b->folder);
            if (!parent) {
                GtkTreeIter fit;
                gtk_tree_store_insert_with_values(bw->vbm_store, &fit, NULL, -1, VB_ICON, "folder-symbolic",
                                                  VB_TITLE, b->folder, -1);
                parent = gtk_tree_iter_copy(&fit);
                g_hash_table_insert(folders, b->folder, parent);
            }
        }
        char *tip = g_markup_printf_escaped("<b>%s</b>\n%s", b->title, b->url);
        gtk_tree_store_insert_with_values(bw->vbm_store, NULL, parent, -1, VB_ICON, "text-html-symbolic",
                                          VB_TITLE, b->title, VB_URL, b->url, VB_TIP, tip, -1);
        g_free(tip);
    }
    g_hash_table_destroy(folders);
    if (fq) {
        gtk_tree_view_expand_all(GTK_TREE_VIEW(bw->vbm_view));
    } else {
        for (gboolean ok = gtk_tree_model_get_iter_first(model, &it); ok; ok = gtk_tree_model_iter_next(model, &it)) {
            char *t = NULL, *u = NULL;
            gtk_tree_model_get(model, &it, VB_TITLE, &t, VB_URL, &u, -1);
            if (!u && t && g_hash_table_contains(open, t)) {
                GtkTreePath *p = gtk_tree_model_get_path(model, &it);
                gtk_tree_view_expand_row(GTK_TREE_VIEW(bw->vbm_view), p, FALSE);
                gtk_tree_path_free(p);
            }
            g_free(t); g_free(u);
        }
    }
    g_hash_table_destroy(open);
    g_free(fq);
    char *title = g_strdup_printf("Bookmarks (%u)", profile->bookmarks->len);
    gtk_label_set_text(GTK_LABEL(bw->vbm_title), title);
    g_free(title);
}

static char *vbm_url_at(BrowserWindow *bw, GtkTreePath *path)
{
    GtkTreeIter it;
    char *url = NULL;
    if (path && gtk_tree_model_get_iter(GTK_TREE_MODEL(bw->vbm_store), &it, path))
        gtk_tree_model_get(GTK_TREE_MODEL(bw->vbm_store), &it, VB_URL, &url, -1);
    return url;
}

static char *vbm_selected_url(BrowserWindow *bw)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    char *url = NULL;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(bw->vbm_view)), &m, &it))
        gtk_tree_model_get(m, &it, VB_URL, &url, -1);
    return url;
}

static void vbm_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *c, BrowserWindow *bw)
{
    char *url = vbm_url_at(bw, path);
    if (url && *url) {
        Tab *t = browser_window_current_tab(bw);
        if (t) tab_load(t, url);
        else browser_window_open(bw, url, FALSE);
    } else if (gtk_tree_view_row_expanded(tv, path)) {
        gtk_tree_view_collapse_row(tv, path);
    } else {
        gtk_tree_view_expand_row(tv, path, FALSE);
    }
    g_free(url);
}

static gboolean vbm_pressed(GtkWidget *w, GdkEventButton *ev, BrowserWindow *bw)
{
    if (ev->type != GDK_BUTTON_PRESS || (ev->button != 2 && ev->button != 3)) return FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), ev->x, ev->y, &path, NULL, NULL, NULL)) return FALSE;
    char *url = vbm_url_at(bw, path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
    gtk_tree_path_free(path);
    if (!url || !*url) { g_free(url); return FALSE; }
    if (ev->button == 2) {
        browser_window_open(bw, url, TRUE);
    } else {
        GMenu *m = g_menu_new();
        GMenu *s1 = g_menu_new(), *s2 = g_menu_new();
        GMenuItem *it;
        struct { const char *label, *action; GMenu *sec; } items[] = {
            { "_Open", "win.open-url", s1 }, { "Open in New _Tab", "win.open-tab", s1 },
            { "Open in _Background Tab", "win.open-background-tab", s1 },
            { "Open in New _Window", "win.open-window", s1 },
            { "_Properties...", "win.bookmark-edit", s2 }, { "_Delete", "win.bookmark-delete", s2 },
        };
        for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
            it = g_menu_item_new(items[i].label, NULL);
            g_menu_item_set_action_and_target_value(it, items[i].action, g_variant_new_string(url));
            g_menu_append_item(items[i].sec, it);
            g_object_unref(it);
        }
        g_menu_append_section(m, NULL, G_MENU_MODEL(s1));
        g_menu_append_section(m, NULL, G_MENU_MODEL(s2));
        GtkWidget *menu = gtk_menu_new_from_model(G_MENU_MODEL(m));
        g_object_unref(s1); g_object_unref(s2); g_object_unref(m);
        gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(bw->window), NULL);
        g_signal_connect(menu, "deactivate", G_CALLBACK(menu_destroy_later), NULL);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    }
    g_free(url);
    return TRUE;
}

static void vbm_edit_url(BrowserWindow *bw, const char *url)
{
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        if (g_strcmp0(b->url, url) == 0) {
            bookmark_edit_dialog(GTK_WINDOW(bw->window), b->url, b->title, FALSE);
            return;
        }
    }
}

static void vbm_add_clicked(GtkButton *b, BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    if (t && !tab_is_start_page(t))
        bookmark_edit_dialog(GTK_WINDOW(bw->window), tab_get_uri(t), tab_get_title(t), !bookmark_exists(tab_get_uri(t)));
}

static void vbm_edit_clicked(GtkButton *b, BrowserWindow *bw)
{
    char *url = vbm_selected_url(bw);
    if (url && *url) vbm_edit_url(bw, url);
    g_free(url);
}

static void vbm_delete_clicked(GtkButton *b, BrowserWindow *bw)
{
    char *url = vbm_selected_url(bw);
    if (url && *url) bookmark_remove(url);
    g_free(url);
}

static gboolean vbm_key(GtkWidget *w, GdkEventKey *ev, BrowserWindow *bw)
{
    if (ev->keyval == GDK_KEY_Delete) { vbm_delete_clicked(NULL, bw); return TRUE; }
    if (ev->keyval == GDK_KEY_F2) { vbm_edit_clicked(NULL, bw); return TRUE; }
    return FALSE;
}

static void vbm_search_changed(GtkSearchEntry *e, BrowserWindow *bw) { vbm_rebuild(bw); }

/* After a drag in the bookmarks tree, read the new order and folders back from the tree
 * and store them. A bookmark's folder is the folder row it ends up under; a bookmark dropped
 * "into" another bookmark joins that bookmark's folder. */
static void vbm_collect(GtkTreeModel *m, GtkTreeIter *parent, const char *folder,
                        GPtrArray *urls, GPtrArray *folders)
{
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_iter_children(m, &it, parent); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        char *url = NULL, *title = NULL;
        gtk_tree_model_get(m, &it, VB_URL, &url, VB_TITLE, &title, -1);
        if (url && *url) {
            g_ptr_array_add(urls, url);
            g_ptr_array_add(folders, g_strdup(folder));
            url = NULL;
            vbm_collect(m, &it, folder, urls, folders);
        } else {
            vbm_collect(m, &it, title ? title : "", urls, folders);
        }
        g_free(url);
        g_free(title);
    }
}

static gboolean vbm_sync_idle(gpointer data)
{
    BrowserWindow *bw = data;
    bw->vbm_sync_id = 0;
    GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *folders = g_ptr_array_new_with_free_func(g_free);
    vbm_collect(GTK_TREE_MODEL(bw->vbm_store), NULL, "", urls, folders);

    GPtrArray *order = g_ptr_array_new();
    gboolean ok = urls->len == profile->bookmarks->len, changed = FALSE;
    for (guint i = 0; ok && i < urls->len; i++) {
        Bookmark *found = NULL;
        for (guint j = 0; j < profile->bookmarks->len && !found; j++) {
            Bookmark *b = profile->bookmarks->pdata[j];
            if (g_str_equal(b->url, urls->pdata[i])) found = b;
        }
        if (!found) { ok = FALSE; break; }
        if (found != profile->bookmarks->pdata[i] || g_strcmp0(found->folder, folders->pdata[i]) != 0) changed = TRUE;
        g_ptr_array_add(order, found);
    }
    if (ok && changed) {
        for (guint i = 0; i < order->len; i++) {
            Bookmark *b = order->pdata[i];
            g_free(b->folder);
            b->folder = g_strdup(folders->pdata[i]);
            profile->bookmarks->pdata[i] = b;   /* same set of pointers, new order */
        }
        bookmarks_save();                       /* notifies every window, which rebuilds */
    } else {
        vbm_rebuild(bw);                        /* drop cancelled or nothing changed: tidy the tree */
    }
    g_ptr_array_free(order, TRUE);
    g_ptr_array_unref(urls);
    g_ptr_array_unref(folders);
    return G_SOURCE_REMOVE;
}

static void vbm_drag_end(GtkWidget *w, GdkDragContext *ctx, BrowserWindow *bw)
{
    /* the tree has finished moving the row (insert + delete); sync once things settle */
    if (!bw->vbm_sync_id) bw->vbm_sync_id = g_idle_add(vbm_sync_idle, bw);
}

static GtkWidget *header_button(const char *icon, const char *tip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    return b;
}

static GtkWidget *build_vbookmarks(BrowserWindow *bw)
{
    bw->vbm_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(bw->vbm_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(bw->vbm_revealer), 150);
    gtk_widget_set_halign(bw->vbm_revealer, GTK_ALIGN_END);
    gtk_widget_set_valign(bw->vbm_revealer, GTK_ALIGN_FILL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(box, "vbookmarks");
    gtk_widget_set_size_request(box, 270, -1);

    /* header: title + all bookmark management buttons */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(hdr), "vbar-header");
    bw->vbm_title = gtk_label_new("Bookmarks");
    gtk_label_set_xalign(GTK_LABEL(bw->vbm_title), 0);
    gtk_box_pack_start(GTK_BOX(hdr), bw->vbm_title, TRUE, TRUE, 0);
    GtkWidget *add = header_button("list-add-symbolic", "Bookmark current page (Ctrl+D)");
    GtkWidget *edit = header_button("document-edit-symbolic", "Properties (F2)");
    GtkWidget *del = header_button("edit-delete-symbolic", "Delete (Del)");
    g_signal_connect(add, "clicked", G_CALLBACK(vbm_add_clicked), bw);
    g_signal_connect(edit, "clicked", G_CALLBACK(vbm_edit_clicked), bw);
    g_signal_connect(del, "clicked", G_CALLBACK(vbm_delete_clicked), bw);
    gtk_box_pack_end(GTK_BOX(hdr), del, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), edit, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);

    bw->vbm_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(bw->vbm_search), "Quick find");
    g_signal_connect(bw->vbm_search, "search-changed", G_CALLBACK(vbm_search_changed), bw);
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(sbox), "vbar-search");
    gtk_box_pack_start(GTK_BOX(sbox), bw->vbm_search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sbox, FALSE, FALSE, 0);

    bw->vbm_store = gtk_tree_store_new(VB_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    bw->vbm_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(bw->vbm_store));
    g_object_unref(bw->vbm_store);   /* owned by the view */
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(bw->vbm_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(bw->vbm_view), FALSE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(bw->vbm_view), TRUE);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(bw->vbm_view), VB_TIP);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, ri, FALSE);
    gtk_tree_view_column_add_attribute(col, ri, "icon-name", VB_ICON);
    GtkCellRenderer *rt = gtk_cell_renderer_text_new();
    g_object_set(rt, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 3, NULL);
    gtk_tree_view_column_pack_start(col, rt, TRUE);
    gtk_tree_view_column_add_attribute(col, rt, "text", VB_TITLE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(bw->vbm_view), col);
    gtk_widget_add_events(bw->vbm_view, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(bw->vbm_view, "row-activated", G_CALLBACK(vbm_activated), bw);
    g_signal_connect(bw->vbm_view, "button-press-event", G_CALLBACK(vbm_pressed), bw);
    g_signal_connect(bw->vbm_view, "key-press-event", G_CALLBACK(vbm_key), bw);
    g_signal_connect(bw->vbm_view, "drag-end", G_CALLBACK(vbm_drag_end), bw);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), bw->vbm_view);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(bw->vbm_revealer), box);
    vbm_rebuild(bw);
    return bw->vbm_revealer;
}

static gboolean refresh_tabs_idle(gpointer data)
{
    BrowserWindow *bw = data;
    bw->tabs_refresh_id = 0;
    vtabs_rebuild(bw);
    return G_SOURCE_REMOVE;
}

static void entry_set_icon(GtkWidget *entry, GtkEntryIconPosition pos, const char *icon, const char *tip)
{
    if (g_strcmp0(icon, gtk_entry_get_icon_name(GTK_ENTRY(entry), pos)))
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), pos, icon);
    char *old = gtk_entry_get_icon_tooltip_text(GTK_ENTRY(entry), pos);
    if (g_strcmp0(old, tip)) gtk_entry_set_icon_tooltip_text(GTK_ENTRY(entry), pos, tip);
    g_free(old);
}

void browser_window_update_chrome(BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    if (!t) return;
    WebKitWebView *v = t->view;
    const char *uri = tab_get_uri(t);
    gboolean sd = tab_is_start_page(t);
    gboolean loading = webkit_web_view_is_loading(v);

    gtk_widget_set_sensitive(bw->back_btn, webkit_web_view_can_go_back(v));
    gtk_widget_set_sensitive(bw->fwd_btn, webkit_web_view_can_go_forward(v));
    /* This runs on every load-progress step, so widgets are only touched when their
     * state really changes: each change makes GTK re-lay out the title bar. */
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(bw->reload_btn), "loading")) != loading + 1) {
        g_object_set_data(G_OBJECT(bw->reload_btn), "loading", GINT_TO_POINTER(loading + 1));
        gtk_button_set_image(GTK_BUTTON(bw->reload_btn), gtk_image_new_from_icon_name(
            loading ? "process-stop-symbolic" : "view-refresh-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR));
        gtk_widget_set_tooltip_text(bw->reload_btn, loading ? "Stop (Esc)" : "Reload (F5)");
    }

    if (!gtk_widget_has_focus(bw->address) || !t->typed)
        set_address_text(bw, t->typed ? t->typed : sd ? "" : uri);
    gtk_entry_set_progress_fraction(GTK_ENTRY(bw->address), loading && !sd && t->progress < 1.0 ? t->progress : 0.0);

    const char *icon = "text-html-symbolic";
    const char *tip = "Web page";
    if (sd) {
        icon = "emblem-documents-symbolic"; tip = "Start page";
    } else if (g_str_has_prefix(uri, "https://")) {
        GTlsCertificate *cert = NULL;
        GTlsCertificateFlags errs = 0;
        if (webkit_web_view_get_tls_info(v, &cert, &errs) && errs) {
            icon = "dialog-warning-symbolic"; tip = "Secure connection with certificate problems";
        } else {
            icon = "channel-secure-symbolic"; tip = "Secure connection";
        }
    } else if (g_str_has_prefix(uri, "http://")) {
        icon = "channel-insecure-symbolic"; tip = "Connection is not secure";
    } else if (g_str_has_prefix(uri, "file:")) {
        icon = "folder-symbolic"; tip = "Local file";
    }
    entry_set_icon(bw->address, GTK_ENTRY_ICON_PRIMARY, icon, tip);
    gboolean marked = !sd && bookmark_exists(uri);
    entry_set_icon(bw->address, GTK_ENTRY_ICON_SECONDARY, sd ? NULL : marked ? "starred-symbolic" : "non-starred-symbolic",
                   marked ? "Remove bookmark" : "Bookmark this page (Ctrl+D)");

    bw->updating_zoom = TRUE;
    double z = webkit_web_view_get_zoom_level(v) * 100.0;
    gtk_range_set_value(GTK_RANGE(bw->zoom_scale), z);
    char *zl = g_strdup_printf("%d%%", (int)(z + 0.5));
    gtk_label_set_text(GTK_LABEL(bw->zoom_label), zl);
    /* the toolbar button shows the zoom only when it isn't 100% */
    gtk_label_set_text(GTK_LABEL(bw->zoom_btn_label), (int)(z + 0.5) == 100 ? "" : zl);
    gtk_widget_set_visible(bw->zoom_btn_label, (int)(z + 0.5) != 100);
    char *ztip = g_strdup_printf("Zoom: %s (Ctrl+wheel, Ctrl +/-/0)", zl);
    gtk_widget_set_tooltip_text(bw->zoom_btn, ztip);
    g_free(ztip);
    g_free(zl);
    bw->updating_zoom = FALSE;

    char *title = g_strdup_printf("%s - %s", tab_get_title(t), APP_NAME);
    if (g_strcmp0(title, gtk_window_get_title(GTK_WINDOW(bw->window))))
        gtk_window_set_title(GTK_WINDOW(bw->window), title);   /* each call is a round to the window manager */
    g_free(title);
    show_default_status(bw);
}

void browser_window_tab_changed(BrowserWindow *bw, Tab *tab)
{
    if (browser_window_current_tab(bw) == tab)
        browser_window_update_chrome(bw);
    if (!bw->tabs_refresh_id)
        bw->tabs_refresh_id = g_timeout_add(300, refresh_tabs_idle, bw);
}

/* ---------------------------------------------------------------- */
/* tabs                                                              */

static Tab *add_tab_full(BrowserWindow *bw, WebKitWebView *related, gboolean background, gboolean start)
{
    Tab *tab = tab_new_full(bw, related, start);
    bw->tabs = g_list_append(bw->tabs, tab);
    gtk_container_add(GTK_CONTAINER(bw->content_stack), GTK_WIDGET(tab->view));

    int pos = -1;
    if (!restoring && pref_get_bool("new_tab_next")) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(bw->notebook));
        if (cur >= 0) pos = cur + 1;
    }
    int idx = gtk_notebook_insert_page(GTK_NOTEBOOK(bw->notebook), tab->nb_child, tab->label_box, pos);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(bw->notebook), tab->nb_child, TRUE);
    if (!background || gtk_notebook_get_n_pages(GTK_NOTEBOOK(bw->notebook)) == 1)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(bw->notebook), idx);
    browser_window_tab_changed(bw, tab);
    return tab;
}

Tab *browser_window_add_tab(BrowserWindow *bw, WebKitWebView *related, gboolean background)
{
    return add_tab_full(bw, related, background, FALSE);
}

Tab *browser_window_open(BrowserWindow *bw, const char *uri, gboolean background)
{
    /* gbrowser://start and gbrowser://speeddial: saved sessions from before the rename */
    if (!uri || !*uri || g_str_has_prefix(uri, "gbrowser:")) uri = START_URI;
    gboolean sd = g_str_has_prefix(uri, START_URI);
    /* Always try the ready spare tab: its web process is already running, so the page
     * starts loading at once instead of waiting ~0.1-0.6 s for a new process. */
    Tab *tab = add_tab_full(bw, NULL, background, TRUE);
    const char *have = webkit_web_view_get_uri(tab->view);
    gboolean adopted = have && g_str_has_prefix(have, START_URI);
    if (sd) {
        if (!adopted) tab_load(tab, uri);        /* spare already shows the start page */
    } else if (adopted) {
        tab_load_replace(tab, uri);             /* keep the start page out of Back history */
    } else {
        tab_load(tab, uri);
    }
    if (browser_window_current_tab(bw) == tab) {
        if (tab_is_start_page(tab)) start_page_focus(tab);   /* type straight into its search box */
        else gtk_widget_grab_focus(GTK_WIDGET(tab->view));
    }
    return tab;
}

void browser_window_close_tab(BrowserWindow *bw, Tab *tab)
{
    if (!g_list_find(bw->tabs, tab)) return;
    closed_tab_push(tab_get_uri(tab), webkit_web_view_get_title(tab->view));

    if (g_list_length(bw->tabs) == 1) {
        if (pref_get_bool("close_last_tab_closes_window")) {
            gtk_window_close(GTK_WINDOW(bw->window));
        } else {
            tab_load(tab, START_URI);   /* keep the window open with the start page */
            start_page_focus(tab);
        }
        return;
    }
    bw->tabs = g_list_remove(bw->tabs, tab);
    int n = gtk_notebook_page_num(GTK_NOTEBOOK(bw->notebook), tab->nb_child);
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(bw->notebook));
    if (n == cur && n > 0 && n == gtk_notebook_get_n_pages(GTK_NOTEBOOK(bw->notebook)) - 1)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(bw->notebook), n - 1);
    gtk_notebook_remove_page(GTK_NOTEBOOK(bw->notebook), n);
    gtk_container_remove(GTK_CONTAINER(bw->content_stack), GTK_WIDGET(tab->view));
    tab_free(tab);
    browser_window_update_chrome(bw);
    vtabs_rebuild(bw);
}

void browser_window_select_tab(BrowserWindow *bw, Tab *tab)
{
    int n = gtk_notebook_page_num(GTK_NOTEBOOK(bw->notebook), tab->nb_child);
    if (n >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(bw->notebook), n);
}

void browser_window_cycle_tab(BrowserWindow *bw, int dir)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(bw->notebook));
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(bw->notebook));
    if (n > 1) gtk_notebook_set_current_page(GTK_NOTEBOOK(bw->notebook), (cur + dir + n) % n);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *child, guint n, BrowserWindow *bw)
{
    Tab *t = g_object_get_data(G_OBJECT(child), "tab");
    if (!t) return;
    gtk_stack_set_visible_child(GTK_STACK(bw->content_stack), GTK_WIDGET(t->view));
    browser_window_update_chrome(bw);
    if (gtk_window_is_active(GTK_WINDOW(bw->window))) notices_seen(t->view);
    if (tab_is_start_page(t)) start_page_focus(t);
    else if (!*tab_get_uri(t)) gtk_widget_grab_focus(bw->address);
    else gtk_widget_grab_focus(GTK_WIDGET(t->view));
    if (!bw->tabs_refresh_id)
        bw->tabs_refresh_id = g_timeout_add(100, refresh_tabs_idle, bw);
}

static gboolean on_tabbar_press(GtkWidget *w, GdkEventButton *ev, BrowserWindow *bw)
{
    /* double click on empty tab bar = maximize / restore the window */
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        GtkWindow *win = GTK_WINDOW(bw->window);
        if (gtk_window_is_maximized(win)) gtk_window_unmaximize(win);
        else gtk_window_maximize(win);
        return TRUE;
    }
    /* middle click on empty tab bar = new tab */
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
        browser_window_open(bw, START_URI, FALSE);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_tabbar_scroll(GtkWidget *w, GdkEventScroll *ev, BrowserWindow *bw)
{
    if (ev->direction == GDK_SCROLL_UP || (ev->direction == GDK_SCROLL_SMOOTH && ev->delta_y < 0))
        browser_window_cycle_tab(bw, -1);
    else if (ev->direction == GDK_SCROLL_DOWN || (ev->direction == GDK_SCROLL_SMOOTH && ev->delta_y > 0))
        browser_window_cycle_tab(bw, 1);
    return TRUE;
}

/* tab context menu */
typedef struct { BrowserWindow *bw; Tab *tab; } TabCtx;

/* the tab may have been closed while its menu was open */
#define TABCTX_CHECK(c) if (!g_list_find(browser_windows(), (c)->bw) || !g_list_find((c)->bw->tabs, (c)->tab)) return

static void tabctx_close(GtkMenuItem *i, TabCtx *c) { TABCTX_CHECK(c); browser_window_close_tab(c->bw, c->tab); }
static void tabctx_reload(GtkMenuItem *i, TabCtx *c) { TABCTX_CHECK(c); webkit_web_view_reload(c->tab->view); }
static void tabctx_dup(GtkMenuItem *i, TabCtx *c) { TABCTX_CHECK(c); browser_window_open(c->bw, tab_get_uri(c->tab), FALSE); }
static void tabctx_bookmark(GtkMenuItem *i, TabCtx *c)
{
    TABCTX_CHECK(c);
    bookmark_edit_dialog(GTK_WINDOW(c->bw->window), tab_get_uri(c->tab), tab_get_title(c->tab), TRUE);
}
static void tabctx_others(GtkMenuItem *i, TabCtx *c)
{
    TABCTX_CHECK(c);
    GList *all = g_list_copy(c->bw->tabs);
    for (GList *l = all; l; l = l->next)
        if (l->data != c->tab) browser_window_close_tab(c->bw, l->data);
    g_list_free(all);
}
static void tabctx_reload_all(GtkMenuItem *i, TabCtx *c)
{
    if (!g_list_find(browser_windows(), c->bw)) return;
    for (GList *l = c->bw->tabs; l; l = l->next) webkit_web_view_reload(((Tab *)l->data)->view);
}

void browser_window_tab_context_menu(BrowserWindow *bw, Tab *tab, GdkEventButton *ev)
{
    TabCtx *c = g_new0(TabCtx, 1);
    c->bw = bw;
    c->tab = tab;
    GtkWidget *m = gtk_menu_new();
    struct { const char *label; GCallback cb; } items[] = {
        { "_New Tab", NULL },
        { "_Reload", G_CALLBACK(tabctx_reload) },
        { "Reload _All Tabs", G_CALLBACK(tabctx_reload_all) },
        { "_Duplicate Tab", G_CALLBACK(tabctx_dup) },
        { "_Bookmark Page...", G_CALLBACK(tabctx_bookmark) },
        { NULL, NULL },
        { "Close _Other Tabs", G_CALLBACK(tabctx_others) },
        { "_Close Tab", G_CALLBACK(tabctx_close) },
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *mi;
        if (!items[i].label) {
            mi = gtk_separator_menu_item_new();
        } else {
            mi = gtk_menu_item_new_with_mnemonic(items[i].label);
            if (items[i].cb) g_signal_connect(mi, "activate", items[i].cb, c);
            else gtk_actionable_set_action_name(GTK_ACTIONABLE(mi), "win.new-tab");
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    }
    g_object_set_data_full(G_OBJECT(m), "ctx", c, g_free);
    gtk_menu_attach_to_widget(GTK_MENU(m), GTK_WIDGET(bw->window), NULL);
    g_signal_connect(m, "deactivate", G_CALLBACK(menu_destroy_later), NULL);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)ev);
}

/* ---------------------------------------------------------------- */
/* fullscreen                                                        */

static void apply_bars(BrowserWindow *bw)
{
    gboolean fs = bw->fullscreen;
    gtk_widget_set_visible(bw->notebook, FALSE);   /* tab model only, never shown */
    gtk_widget_set_visible(bw->toolbar, !fs);
    if (bw->vstrip) {
        gtk_widget_set_visible(bw->vstrip, !fs);
        vstrip_rebuild(bw);   /* it is not kept up to date while hidden */
    }
}

void browser_window_fullscreen(BrowserWindow *bw, gboolean on)
{
    if (bw->fullscreen == on) return;
    bw->fullscreen = on;
    if (on) gtk_window_fullscreen(GTK_WINDOW(bw->window));
    else gtk_window_unfullscreen(GTK_WINDOW(bw->window));
    apply_bars(bw);
}

/* ---------------------------------------------------------------- */
/* menus                                                             */

static char *menu_label(const char *s)
{
    /* escape mnemonic underscores and keep menus a sane width */
    GString *o = g_string_new(NULL);
    int n = 0;
    for (const char *p = s; *p && n < 60; p = g_utf8_next_char(p), n++) {
        if (*p == '_') g_string_append(o, "__");
        else g_string_append_unichar(o, g_utf8_get_char(p));
    }
    if (n >= 60) g_string_append(o, "…");
    return g_string_free(o, FALSE);
}

static void on_bookmarks_changed(gpointer data)
{
    BrowserWindow *bw = data;
    vbm_rebuild(bw);
    browser_window_update_chrome(bw);
}

static void rebuild_trash(BrowserWindow *bw)
{
    g_menu_remove_all(bw->trash_menu);
    GMenu *sec = g_menu_new();
    for (guint i = 0; i < profile->closed_tabs->len; i++) {
        Dial *d = profile->closed_tabs->pdata[i];
        char *l = menu_label(d->title);
        GMenuItem *it = g_menu_item_new(l, NULL);
        g_menu_item_set_action_and_target_value(it, "win.restore-closed", g_variant_new_int32(i));
        g_menu_append_item(sec, it);
        g_object_unref(it);
        g_free(l);
    }
    if (!profile->closed_tabs->len)
        g_menu_append(sec, "No closed tabs", "win.nothing");
    g_menu_append_section(bw->trash_menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
    if (profile->closed_tabs->len) {
        GMenu *s2 = g_menu_new();
        g_menu_append(s2, "Empty Trash", "win.empty-trash");
        g_menu_append_section(bw->trash_menu, NULL, G_MENU_MODEL(s2));
        g_object_unref(s2);
    }
}

static void on_closed_changed(gpointer data) { rebuild_trash(data); }


static void on_settings_changed(gpointer data)
{
    BrowserWindow *bw = data;
    SearchEngine *e = search_engine_default();
    if (e) {
        char *ph = g_strdup_printf("Search with %s or enter address", e->name);
        gtk_entry_set_placeholder_text(GTK_ENTRY(bw->address), ph);
        g_free(ph);
    }
    apply_bars(bw);
}

/* The main menu holds only what has no button elsewhere: tabs are managed in the tab bar
 * (and its right-click menu), bookmarks in the bookmarks bar, zoom in the zoom button,
 * downloads in the downloads button, deleting private data in Preferences. */
static GMenuModel *build_main_menu(BrowserWindow *bw)
{
    GMenu *root = g_menu_new();
    GMenu *s;
#define SEC() do { s = g_menu_new(); g_menu_append_section(root, NULL, G_MENU_MODEL(s)); g_object_unref(s); } while (0)
    SEC();
    g_menu_append(s, "New _Window", "win.new-window");
    SEC();
    g_menu_append(s, "_History", "win.show-history");
    SEC();
    g_menu_append(s, "_Find...", "win.find");
    g_menu_append(s, "_Print...", "win.print");
    g_menu_append(s, "_Save Page As...", "win.save-page");
    g_menu_append(s, "_Open File...", "win.open-file");
    g_menu_append(s, "F_ull Screen", "win.fullscreen");
    g_menu_append(s, "_Developer Tools", "win.inspector");
    SEC();
    g_menu_append(s, "_Light Theme", "app.theme::light");
    g_menu_append(s, "Dar_k Theme", "app.theme::dark");
    SEC();
    g_menu_append(s, "_Preferences...", "win.preferences");
    SEC();
    g_menu_append(s, "_Keyboard Shortcuts", "win.shortcuts");
    g_menu_append(s, "_About " APP_NAME, "win.about");
#undef SEC
    return G_MENU_MODEL(root);
}

/* ---------------------------------------------------------------- */
/* actions                                                           */

#define BW ((BrowserWindow *)data)
#define CUR (browser_window_current_tab(BW))

static void a_new_tab(GSimpleAction *a, GVariant *p, gpointer data) { browser_window_open(BW, START_URI, FALSE); }
static void a_new_window(GSimpleAction *a, GVariant *p, gpointer data)
{
    BrowserWindow *nw = browser_window_new(gtk_window_get_application(GTK_WINDOW(BW->window)));
    browser_window_open(nw, START_URI, FALSE);
    gtk_window_present(GTK_WINDOW(nw->window));
}
static void a_close_tab(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) browser_window_close_tab(BW, CUR); }
static void a_close_window(GSimpleAction *a, GVariant *p, gpointer data) { gtk_window_close(GTK_WINDOW(BW->window)); }

static void restore_closed(BrowserWindow *bw, guint i)
{
    if (i >= profile->closed_tabs->len) return;
    Dial *d = g_ptr_array_steal_index(profile->closed_tabs, i);
    browser_window_open(bw, d->url, FALSE);
    g_free(d->url); g_free(d->title); g_free(d);
    profile_emit(EV_CLOSED_TABS);
}
static void a_reopen_tab(GSimpleAction *a, GVariant *p, gpointer data) { restore_closed(BW, 0); }
static void a_restore_closed(GSimpleAction *a, GVariant *p, gpointer data) { restore_closed(BW, g_variant_get_int32(p)); }
static void a_empty_trash(GSimpleAction *a, GVariant *p, gpointer data)
{
    g_ptr_array_set_size(profile->closed_tabs, 0);
    profile_emit(EV_CLOSED_TABS);
}
static void a_nothing(GSimpleAction *a, GVariant *p, gpointer data) {}

static void a_next_tab(GSimpleAction *a, GVariant *p, gpointer data) { browser_window_cycle_tab(BW, 1); }
static void a_prev_tab(GSimpleAction *a, GVariant *p, gpointer data) { browser_window_cycle_tab(BW, -1); }
static void a_select_tab(GSimpleAction *a, GVariant *p, gpointer data)
{
    int i = g_variant_get_int32(p);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(BW->notebook));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(BW->notebook), i < 0 || i >= n ? n - 1 : i);
}
static void a_duplicate(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) browser_window_open(BW, tab_get_uri(CUR), FALSE); }
static void a_open_location(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (BW->fullscreen) browser_window_fullscreen(BW, FALSE);
    gtk_widget_grab_focus(BW->address);
}
static void a_focus_search(GSimpleAction *a, GVariant *p, gpointer data)
{
    /* the address bar is also the search field */
    gtk_widget_grab_focus(BW->address);
    gtk_editable_select_region(GTK_EDITABLE(BW->address), 0, -1);
}
static void a_back(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) webkit_web_view_go_back(CUR->view); }
static void a_forward(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) webkit_web_view_go_forward(CUR->view); }
static void a_reload(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) webkit_web_view_reload(CUR->view); }
static void a_reload_hard(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) webkit_web_view_reload_bypass_cache(CUR->view); }
static void a_reload_all(GSimpleAction *a, GVariant *p, gpointer data)
{
    for (GList *l = BW->tabs; l; l = l->next) webkit_web_view_reload(((Tab *)l->data)->view);
}
static void a_stop(GSimpleAction *a, GVariant *p, gpointer data) { if (CUR) webkit_web_view_stop_loading(CUR->view); }
static void a_go_up(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (!CUR) return;
    char *u = g_strdup(tab_get_uri(CUR));
    char *q = strpbrk(u, "?#");
    if (q) *q = 0;
    char *scheme_end = strstr(u, "://");
    gsize l = strlen(u);
    if (l && u[l - 1] == '/') u[--l] = 0;
    char *slash = strrchr(u, '/');
    if (scheme_end && slash > scheme_end + 2) {
        slash[1] = 0;
        tab_load(CUR, u);
    }
    g_free(u);
}

static void a_bookmark(GSimpleAction *a, GVariant *p, gpointer data)
{
    Tab *t = CUR;
    if (!t || tab_is_start_page(t)) return;
    bookmark_edit_dialog(GTK_WINDOW(BW->window), tab_get_uri(t), tab_get_title(t), !bookmark_exists(tab_get_uri(t)));
}
static void a_bookmark_link(GSimpleAction *a, GVariant *p, gpointer data)
{
    const char *u = g_variant_get_string(p, NULL);
    bookmark_edit_dialog(GTK_WINDOW(BW->window), u, u, TRUE);
}
static void a_open_url(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (CUR) tab_load(CUR, g_variant_get_string(p, NULL));
}
static void a_open_tab(GSimpleAction *a, GVariant *p, gpointer data) { browser_window_open(BW, g_variant_get_string(p, NULL), FALSE); }
static void a_open_bg_tab(GSimpleAction *a, GVariant *p, gpointer data) { browser_window_open(BW, g_variant_get_string(p, NULL), TRUE); }
static void a_open_window(GSimpleAction *a, GVariant *p, gpointer data)
{
    BrowserWindow *nw = browser_window_new(gtk_window_get_application(GTK_WINDOW(BW->window)));
    browser_window_open(nw, g_variant_get_string(p, NULL), FALSE);
    gtk_window_present(GTK_WINDOW(nw->window));
}

/* find in page */
static void find_run(BrowserWindow *bw, gboolean backwards)
{
    Tab *t = browser_window_current_tab(bw);
    if (!t) return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(t->view);
    const char *text = gtk_entry_get_text(GTK_ENTRY(bw->find_entry));
    if (!*text) {
        webkit_find_controller_search_finish(fc);
        gtk_label_set_text(GTK_LABEL(bw->find_label), "");
        return;
    }
    guint32 opts = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bw->find_case))) opts |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
    if (backwards) opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;
    if (g_strcmp0(webkit_find_controller_get_search_text(fc), text) == 0 &&
        webkit_find_controller_get_options(fc) == opts) {
        if (backwards) webkit_find_controller_search_previous(fc);
        else webkit_find_controller_search_next(fc);
    } else {
        webkit_find_controller_search(fc, text, opts, G_MAXUINT);
        webkit_find_controller_count_matches(fc, text, opts, G_MAXUINT);
    }
}
static void a_find(GSimpleAction *a, GVariant *p, gpointer data)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(BW->find_revealer), TRUE);
    gtk_widget_grab_focus(BW->find_entry);
    if (*gtk_entry_get_text(GTK_ENTRY(BW->find_entry))) find_run(BW, FALSE);
}
static void a_find_next(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (!gtk_revealer_get_reveal_child(GTK_REVEALER(BW->find_revealer))) { a_find(a, p, data); return; }
    find_run(BW, FALSE);
}
static void a_find_prev(GSimpleAction *a, GVariant *p, gpointer data) { find_run(BW, TRUE); }
static void find_close(BrowserWindow *bw)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(bw->find_revealer), FALSE);
    Tab *t = browser_window_current_tab(bw);
    if (t) {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(t->view));
        gtk_widget_grab_focus(GTK_WIDGET(t->view));
    }
}

static void set_zoom(BrowserWindow *bw, double z)
{
    Tab *t = browser_window_current_tab(bw);
    if (t) webkit_web_view_set_zoom_level(t->view, CLAMP(z, 0.2, 5.0));
}
static const double zoom_steps[] = { 0.2, 0.3, 0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.2, 1.5, 1.7, 2.0, 2.5, 3.0, 4.0, 5.0 };
static void a_zoom_in(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (!CUR) return;
    double z = webkit_web_view_get_zoom_level(CUR->view);
    for (guint i = 0; i < G_N_ELEMENTS(zoom_steps); i++)
        if (zoom_steps[i] > z + 0.001) { set_zoom(BW, zoom_steps[i]); return; }
}
static void a_zoom_out(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (!CUR) return;
    double z = webkit_web_view_get_zoom_level(CUR->view);
    for (int i = G_N_ELEMENTS(zoom_steps) - 1; i >= 0; i--)
        if (zoom_steps[i] < z - 0.001) { set_zoom(BW, zoom_steps[i]); return; }
}
static void a_zoom_reset(GSimpleAction *a, GVariant *p, gpointer data) { set_zoom(BW, pref_get_int("default_zoom") / 100.0); }
static void a_fullscreen(GSimpleAction *a, GVariant *p, gpointer data) { browser_window_fullscreen(BW, !BW->fullscreen); }
static void s_vbookmarks(GSimpleAction *a, GVariant *state, gpointer data)
{
    /* per window, closed by default, not remembered across restarts */
    g_simple_action_set_state(a, state);
    gtk_revealer_set_reveal_child(GTK_REVEALER(BW->vbm_revealer), g_variant_get_boolean(state));
}
static void a_bookmark_edit(GSimpleAction *a, GVariant *p, gpointer data) { vbm_edit_url(BW, g_variant_get_string(p, NULL)); }
static void a_bookmark_delete(GSimpleAction *a, GVariant *p, gpointer data) { bookmark_remove(g_variant_get_string(p, NULL)); }
static void s_vtabs(GSimpleAction *a, GVariant *state, gpointer data)
{
    /* per window, closed by default, not remembered across restarts */
    g_simple_action_set_state(a, state);
    vtabs_set_shown(BW, g_variant_get_boolean(state));
}

static void source_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    char *uri = data;
    gsize len = 0;
    guchar *bytes = webkit_web_resource_get_data_finish(WEBKIT_WEB_RESOURCE(src), res, &len, NULL);
    BrowserWindow *bw = browser_window_get_active();
    if (bytes && bw) {
        char *text = g_utf8_make_valid((char *)bytes, len);
        char *esc = g_markup_escape_text(text, -1);
        char *etitle = g_markup_escape_text(uri, -1);
        char *html = g_strdup_printf("<!DOCTYPE html><html><head><meta charset='utf-8'><title>Source: %s</title>"
            "<style>body{margin:0;background:#fff}pre{font:12px/1.45 'DejaVu Sans Mono',monospace;white-space:pre-wrap;"
            "word-wrap:break-word;padding:10px 14px;margin:0;counter-reset:l}</style></head><body><pre>%s</pre></body></html>",
            etitle, esc);
        Tab *t = browser_window_add_tab(bw, NULL, FALSE);
        webkit_web_view_load_html(t->view, html, NULL);
        g_free(html); g_free(esc); g_free(etitle); g_free(text);
    }
    g_free(bytes);
    g_free(uri);
}
static void a_view_source(GSimpleAction *a, GVariant *p, gpointer data)
{
    Tab *t = CUR;
    if (!t) return;
    WebKitWebResource *r = webkit_web_view_get_main_resource(t->view);
    if (r) webkit_web_resource_get_data(r, NULL, source_ready, g_strdup(tab_get_uri(t)));
}
static void a_inspector(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (CUR) webkit_web_inspector_show(webkit_web_view_get_inspector(CUR->view));
}
static void a_print(GSimpleAction *a, GVariant *p, gpointer data)
{
    if (!CUR) return;
    WebKitPrintOperation *op = webkit_print_operation_new(CUR->view);
    webkit_print_operation_run_dialog(op, GTK_WINDOW(BW->window));
    g_object_unref(op);
}
static void save_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    if (!webkit_web_view_save_to_file_finish(WEBKIT_WEB_VIEW(src), res, &err)) {
        g_warning("Save failed: %s", err->message);
        g_error_free(err);
    }
}
static void a_save_page(GSimpleAction *a, GVariant *p, gpointer data)
{
    Tab *t = CUR;
    if (!t || tab_is_start_page(t)) return;
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Save Page As", GTK_WINDOW(BW->window),
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    char *dir = pref_get_string("download_dir");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), dir);
    g_free(dir);
    char *name = g_strdup_printf("%s.mhtml", tab_get_title(t));
    g_strdelimit(name, "/\\:*?\"<>|", '_');
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), name);
    g_free(name);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(fc));
        webkit_web_view_save_to_file(t->view, f, WEBKIT_SAVE_MODE_MHTML, NULL, save_done, NULL);
        g_object_unref(f);
    }
    g_object_unref(fc);
}
static void a_open_file(GSimpleAction *a, GVariant *p, gpointer data)
{
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Open File", GTK_WINDOW(BW->window),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(fc));
        if (CUR && (tab_is_start_page(CUR) || !*tab_get_uri(CUR))) tab_load(CUR, uri);
        else browser_window_open(BW, uri, FALSE);
        g_free(uri);
    }
    g_object_unref(fc);
}
static void a_preferences(GSimpleAction *a, GVariant *p, gpointer data) { prefs_dialog_show(GTK_WINDOW(BW->window)); }
static void a_private_data(GSimpleAction *a, GVariant *p, gpointer data) { private_data_dialog_show(GTK_WINDOW(BW->window)); }
static void a_show_history(GSimpleAction *a, GVariant *p, gpointer data)   { history_popup_toggle(BW); }
static void a_block_element(GSimpleAction *a, GVariant *p, gpointer data)  { if (CUR) adblock_pick_element(CUR->view); }
static void a_adblock_site(GSimpleAction *a, GVariant *p, gpointer data)
{
    Tab *t = CUR;
    if (!t) return;
    gboolean allow = !adblock_site_is_allowed(tab_get_uri(t));
    adblock_set_site_allowed(tab_get_uri(t), allow);
    char *site = adblock_site_for_uri(tab_get_uri(t));
    char *msg = g_strdup_printf(allow ? "Ads allowed on %s" : "Ads blocked on %s", site ? site : "this site");
    browser_window_flash_status(BW, msg);
    g_free(msg);
    g_free(site);
    adblock_page_changed(t->ucm, tab_get_uri(t));   /* before the reload starts */
    webkit_web_view_reload(t->view);
}
static void a_show_downloads(GSimpleAction *a, GVariant *p, gpointer data) { downloads_popup_toggle(BW); }
static void a_show_notifications(GSimpleAction *a, GVariant *p, gpointer data) { notifications_popup_toggle(BW); }


typedef struct { BrowserWindow *bw; } SelCtx;
static void selection_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    SelCtx *c = data;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, NULL);
    if (v && jsc_value_is_string(v) && g_list_find(windows, c->bw)) {
        char *text = jsc_value_to_string(v);
        g_strstrip(text);
        if (*text) {
            SearchEngine *e = search_engine_default();
            if (e) {
                char *u = search_url_for(e, text);
                browser_window_open(c->bw, u, FALSE);
                g_free(u);
            }
        }
        g_free(text);
    }
    if (v) g_object_unref(v);
    g_free(c);
}
static void get_selection(BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    if (!t) return;
    SelCtx *c = g_new0(SelCtx, 1);
    c->bw = bw;
    webkit_web_view_evaluate_javascript(t->view, "window.getSelection().toString()", -1,
                                        NULL, NULL, NULL, selection_ready, c);
}
static void a_search_selection(GSimpleAction *a, GVariant *p, gpointer data) { get_selection(BW); }

static void a_about(GSimpleAction *a, GVariant *p, gpointer data)
{
    char *comments = g_strdup_printf("A fast, lightweight web browser.\n"
                                     "Powered by WebKitGTK %u.%u.%u",
                                     webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    gtk_show_about_dialog(GTK_WINDOW(BW->window), "program-name", APP_NAME, "version", APP_VERSION,
                          "comments", comments, "logo-icon-name", "web-browser",
                          "license-type", GTK_LICENSE_MIT_X11, NULL);
    g_free(comments);
}

static void a_shortcuts(GSimpleAction *a, GVariant *p, gpointer data)
{
    static const char *rows[][2] = {
        { "Ctrl+T", "New tab" }, { "Ctrl+W, Ctrl+F4", "Close tab" }, { "Ctrl+Shift+T", "Reopen closed tab" },
        { "Ctrl+Tab, Ctrl+PgDn", "Next tab" }, { "Ctrl+Shift+Tab, Ctrl+PgUp", "Previous tab" },
        { "Alt+1 … Alt+9", "Switch to tab" },
        { "Ctrl+N", "New window" }, { "F8, Ctrl+L, Alt+D", "Go to address bar" }, { "Ctrl+K, Ctrl+E", "Search (focus address bar)" },
        { "Shift+Enter", "Open address in new tab" }, { "Ctrl+Shift+V", "Paste and go (address bar)" }, { "Alt+Left / Alt+Right", "Back / Forward" },
        { "F5, Ctrl+R", "Reload" }, { "Ctrl+F5", "Reload without cache" },
        { "Ctrl+D", "Bookmark page" }, { "F9", "Show/hide vertical tab bar" }, { "F7, Ctrl+Shift+B", "Show/hide vertical bookmarks bar" },
        { "Ctrl+Shift+H, Ctrl+H", "History" }, { "Ctrl+J", "Downloads" }, { "Ctrl+Shift+M", "Notifications" },
        { "Ctrl+F", "Find in page" }, { "F3 / Shift+F3", "Find next / previous" },
        { "Ctrl++ / Ctrl+- / Ctrl+0", "Zoom in / out / reset" }, { "F11", "Full screen" },
        { "Ctrl+U", "View source" }, { "Ctrl+Shift+I, F12", "Developer tools" },
        { "Ctrl+P", "Print" }, { "Ctrl+S", "Save page" }, { "Ctrl+O", "Open file" },
        { "Ctrl+F12, Alt+P", "Preferences" }, { "Ctrl+Shift+Del", "Delete private data" }, { "Ctrl+Q", "Exit" },
        { "", "" },
        { "Right-drag left / right", "Gesture: back / forward" }, { "Right-drag up", "Gesture: stop" },
        { "Right-drag down", "Gesture: new tab" }, { "Right-drag up, down", "Gesture: reload" },
        { "Right-drag down, right", "Gesture: close tab" }, { "Right-drag down, up", "Gesture: duplicate tab" },
        { "Right-drag up, left", "Gesture: parent directory" }, { "Hold right + wheel", "Cycle tabs" },
        { "Hold right + click left", "Back (rocker)" }, { "Middle / Ctrl+click link", "Open in background tab" },
    };
    GtkWidget *d = gtk_dialog_new_with_buttons("Keyboard Shortcuts", GTK_WINDOW(BW->window),
        GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 460, 560);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw, TRUE);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    g_object_set(grid, "margin", 12, NULL);
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        GtkWidget *k = gtk_label_new(NULL);
        char *m = g_markup_printf_escaped("<b>%s</b>", rows[i][0]);
        gtk_label_set_markup(GTK_LABEL(k), m);
        g_free(m);
        gtk_label_set_xalign(GTK_LABEL(k), 0);
        GtkWidget *v = gtk_label_new(rows[i][1]);
        gtk_label_set_xalign(GTK_LABEL(v), 0);
        gtk_grid_attach(GTK_GRID(grid), k, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, 1, i, 1, 1);
    }
    gtk_container_add(GTK_CONTAINER(sw), grid);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), sw, TRUE, TRUE, 0);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
}

#undef CUR
#undef BW

static const GActionEntry win_actions[] = {
    { "new-tab", a_new_tab }, { "new-window", a_new_window }, { "close-tab", a_close_tab },
    { "close-window", a_close_window }, { "reopen-tab", a_reopen_tab },
    { "restore-closed", a_restore_closed, "i" }, { "empty-trash", a_empty_trash }, { "nothing", a_nothing },
    { "next-tab", a_next_tab }, { "prev-tab", a_prev_tab }, { "select-tab", a_select_tab, "i" },
    { "duplicate-tab", a_duplicate }, { "open-location", a_open_location }, { "focus-search", a_focus_search },
    { "back", a_back }, { "forward", a_forward }, { "reload", a_reload }, { "reload-hard", a_reload_hard },
    { "reload-all", a_reload_all }, { "stop", a_stop }, { "go-up", a_go_up },
    { "bookmark", a_bookmark }, { "bookmark-link", a_bookmark_link, "s" },
    { "open-url", a_open_url, "s" }, { "open-tab", a_open_tab, "s" },
    { "open-background-tab", a_open_bg_tab, "s" }, { "open-window", a_open_window, "s" },
    { "find", a_find }, { "find-next", a_find_next }, { "find-prev", a_find_prev },
    { "zoom-in", a_zoom_in }, { "zoom-out", a_zoom_out }, { "zoom-reset", a_zoom_reset },
    { "fullscreen", a_fullscreen }, { "view-source", a_view_source }, { "inspector", a_inspector },
    { "print", a_print }, { "save-page", a_save_page }, { "open-file", a_open_file },
    { "preferences", a_preferences }, { "private-data", a_private_data },
    { "show-history", a_show_history }, { "show-downloads", a_show_downloads },
    { "show-notifications", a_show_notifications },
    { "block-element", a_block_element }, { "adblock-site", a_adblock_site },
    { "toggle-vtabs", NULL, NULL, "false", s_vtabs },
    { "toggle-vbookmarks", NULL, NULL, "false", s_vbookmarks },
    { "bookmark-edit", a_bookmark_edit, "s" }, { "bookmark-delete", a_bookmark_delete, "s" },
    { "search-selection", a_search_selection },
    { "about", a_about }, { "shortcuts", a_shortcuts },
};

/* ---------------------------------------------------------------- */
/* address & search entries                                          */

static void submit_address(BrowserWindow *bw, const char *text, gboolean new_tab)
{
    char *url = browser_resolve_input(text);
    if (!url) return;
    Tab *t = browser_window_current_tab(bw);
    if (new_tab || !t) t = browser_window_open(bw, url, FALSE);
    else tab_load(t, url);
    gtk_widget_grab_focus(GTK_WIDGET(t->view));
    g_free(url);
}

static void on_address_activate(GtkEntry *e, BrowserWindow *bw)
{
    GdkModifierType mods = 0;
    gtk_get_current_event_state(&mods);
    submit_address(bw, gtk_entry_get_text(e), (mods & (GDK_SHIFT_MASK | GDK_MOD1_MASK)) != 0);
}

static void on_address_changed(GtkEditable *e, BrowserWindow *bw)
{
    if (bw->setting_address) return;
    Tab *t = browser_window_current_tab(bw);
    if (t) {
        g_free(t->typed);
        t->typed = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    }
    /* refill completion model */
    gtk_list_store_clear(bw->completion_store);
    const char *text = gtk_entry_get_text(GTK_ENTRY(e));
    if (!*text) return;
    char *low = g_utf8_casefold(text, -1);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    int count = 0;

    /* top row(s): what Enter will do, i.e. search or go to the address */
    char *stripped = g_strstrip(g_strdup(text));
    SearchEngine *kw_engine = NULL;                 /* "w linux" style keyword search? */
    const char *sp = strchr(stripped, ' ');
    if (sp) {
        char *kw = g_strndup(stripped, sp - stripped);
        for (guint i = 0; i < profile->engines->len && !kw_engine; i++) {
            SearchEngine *se = profile->engines->pdata[i];
            if (*se->keyword && g_str_equal(kw, se->keyword)) kw_engine = se;
        }
        g_free(kw);
    }
    char *target = browser_resolve_input(stripped);
    SearchEngine *def = search_engine_default();
    gboolean is_address = target && !kw_engine && (looks_like_host(stripped) || g_uri_peek_scheme(stripped)
                          || stripped[0] == '/' || g_str_has_prefix(stripped, "~/"));
    if (target && is_address) {
        char *m = g_markup_printf_escaped("<b>%s</b>  <span foreground='#5b6672'><small>Go to address</small></span>", target);
        gtk_list_store_insert_with_values(bw->completion_store, NULL, -1, 0, target, 1, m, -1);
        g_free(m);
        if (def) {   /* still offer a search for the same text */
            char *su = search_url_for(def, stripped);
            m = g_markup_printf_escaped("%s  <span foreground='#5b6672'><small>Search with %s</small></span>", stripped, def->name);
            gtk_list_store_insert_with_values(bw->completion_store, NULL, -1, 0, su, 1, m, -1);
            g_free(m);
            g_free(su);
        }
    } else if (target) {
        SearchEngine *se = kw_engine ? kw_engine : def;
        const char *terms = kw_engine ? sp + 1 : stripped;
        char *m = g_markup_printf_escaped("<b>%s</b>  <span foreground='#5b6672'><small>Search with %s</small></span>",
                                          terms, se ? se->name : "search");
        gtk_list_store_insert_with_values(bw->completion_store, NULL, -1, 0, target, 1, m, -1);
        g_free(m);
    }
    g_free(target);
    g_free(stripped);

    for (guint i = 0; i < profile->bookmarks->len && count < 4; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        char *u = g_utf8_casefold(b->url, -1), *ti = g_utf8_casefold(b->title, -1);
        if (strstr(u, low) || strstr(ti, low)) {
            char *m = g_markup_printf_escaped("★ %s  <span foreground='#3465a4'><small>%s</small></span>", b->title, b->url);
            gtk_list_store_insert_with_values(bw->completion_store, NULL, -1, 0, b->url, 1, m, -1);
            g_hash_table_add(seen, b->url);
            g_free(m);
            count++;
        }
        g_free(u); g_free(ti);
    }
    GPtrArray *h = history_complete(text, 10);
    for (guint i = 0; i < h->len && count < 10; i++) {
        HistoryItem *it = h->pdata[i];
        if (g_hash_table_contains(seen, it->url)) continue;
        char *m = g_markup_printf_escaped("%s  <span foreground='#3465a4'><small>%s</small></span>",
                                          it->title && *it->title ? it->title : it->url, it->url);
        gtk_list_store_insert_with_values(bw->completion_store, NULL, -1, 0, it->url, 1, m, -1);
        g_free(m);
        count++;
    }
    g_ptr_array_unref(h);
    g_hash_table_destroy(seen);
    g_free(low);
}

static gboolean completion_match(GtkEntryCompletion *c, const char *key, GtkTreeIter *it, gpointer d) { return TRUE; }

static gboolean on_completion_selected(GtkEntryCompletion *c, GtkTreeModel *m, GtkTreeIter *it, BrowserWindow *bw)
{
    char *url = NULL;
    gtk_tree_model_get(m, it, 0, &url, -1);
    if (url) submit_address(bw, url, FALSE);
    g_free(url);
    return TRUE;
}

/* ---- paste and go ---- */


/* whitespace runs become one space; line breaks are dropped (join) or become spaces */
static char *squeeze(const char *raw, gboolean join_lines)
{
    GString *o = g_string_new(NULL);
    gboolean space = FALSE;
    for (const char *p = raw; *p; p++) {
        if (join_lines && (*p == '\r' || *p == '\n')) continue;
        if (g_ascii_isspace(*p)) { space = TRUE; continue; }
        if (space && o->len) g_string_append_c(o, ' ');
        space = FALSE;
        g_string_append_c(o, *p);
    }
    if (!o->len) { g_string_free(o, TRUE); return NULL; }
    return g_string_free(o, FALSE);
}

static gboolean input_is_address(const char *s);

/* clipboard text as one line, NULL if there is none. An address copied across two
 * lines is joined back together; other text keeps its words apart. */
static char *one_line(const char *raw)
{
    char *joined = squeeze(raw, TRUE);
    if (joined && input_is_address(joined)) return joined;
    g_free(joined);
    return squeeze(raw, FALSE);
}

static char *clipboard_line(GtkWidget *w)
{
    char *raw = gtk_clipboard_wait_for_text(gtk_widget_get_clipboard(w, GDK_SELECTION_CLIPBOARD));
    char *line = raw ? one_line(raw) : NULL;
    g_free(raw);
    return line;
}

/* whether Enter on this text goes to an address (rather than searching) */
static gboolean input_is_address(const char *s)
{
    const char *sp = strchr(s, ' ');
    if (sp) {   /* "w linux": a search keyword */
        char *kw = g_strndup(s, sp - s);
        gboolean keyword = FALSE;
        for (guint i = 0; i < profile->engines->len && !keyword; i++) {
            SearchEngine *se = profile->engines->pdata[i];
            keyword = *se->keyword && g_str_equal(kw, se->keyword);
        }
        g_free(kw);
        if (keyword) return FALSE;
    }
    return looks_like_host(s) || (g_uri_peek_scheme(s) && !sp) || s[0] == '/' || g_str_has_prefix(s, "~/");
}

static void paste_and_go(BrowserWindow *bw)
{
    char *text = clipboard_line(bw->address);
    if (!text) return;
    submit_address(bw, text, FALSE);
    g_free(text);
}

static void on_paste_go_item(GtkMenuItem *mi, BrowserWindow *bw) { paste_and_go(bw); }

/* address bar right-click menu: "Paste and Go" / "Paste and Search" below "Paste" */
static void on_address_popup(GtkEntry *e, GtkWidget *menu, BrowserWindow *bw)
{
    if (!GTK_IS_MENU(menu)) return;   /* touch selection popovers */
    char *text = clipboard_line(GTK_WIDGET(e));
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(
        text && !input_is_address(text) ? "Paste and _Search" : "Paste and _Go");
    gtk_widget_set_sensitive(mi, text != NULL);
    g_signal_connect(mi, "activate", G_CALLBACK(on_paste_go_item), bw);
    g_free(text);

    int pos = 3, i = 0;   /* GTK's order: Cut, Copy, Paste, ... */
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    const char *paste = g_dgettext("gtk30", "_Paste");
    for (GList *l = items; l; l = l->next, i++)
        if (GTK_IS_MENU_ITEM(l->data) && g_strcmp0(gtk_menu_item_get_label(l->data), paste) == 0) pos = i + 1;
    g_list_free(items);
    gtk_menu_shell_insert(GTK_MENU_SHELL(menu), mi, pos);
    gtk_widget_show(mi);
}

static gboolean on_address_key(GtkWidget *w, GdkEventKey *ev, BrowserWindow *bw)
{
    /* Ctrl+Shift+V: paste and go (only here; in pages it pastes without formatting) */
    if ((ev->keyval == GDK_KEY_V || ev->keyval == GDK_KEY_v)
        && (ev->state & gtk_accelerator_get_default_mod_mask()) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        paste_and_go(bw);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Escape) {
        Tab *t = browser_window_current_tab(bw);
        if (t) {
            g_clear_pointer(&t->typed, g_free);
            browser_window_update_chrome(bw);
            if (tab_is_start_page(t)) start_page_focus(t);
            else gtk_widget_grab_focus(GTK_WIDGET(t->view));
        }
        return TRUE;
    }
    return FALSE;
}

static gboolean select_all_idle(gpointer data)
{
    GtkWidget *e = data;
    if (gtk_widget_has_focus(e)) gtk_editable_select_region(GTK_EDITABLE(e), 0, -1);
    g_object_unref(e);
    return G_SOURCE_REMOVE;
}

static gboolean on_entry_focus_in(GtkWidget *w, GdkEvent *ev, BrowserWindow *bw)
{
    g_idle_add(select_all_idle, g_object_ref(w));
    return FALSE;
}

static void on_address_icon(GtkEntry *e, GtkEntryIconPosition pos, GdkEvent *ev, BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    if (!t || tab_is_start_page(t)) return;
    if (pos == GTK_ENTRY_ICON_SECONDARY) {
        if (bookmark_exists(tab_get_uri(t))) {
            bookmark_remove(tab_get_uri(t));
            browser_window_flash_status(bw, "Bookmark removed");
        } else {
            bookmark_add(tab_get_uri(t), tab_get_title(t), "");
            browser_window_flash_status(bw, "Bookmark added");
        }
        return;
    }
    /* primary: site info */
    const char *uri = tab_get_uri(t);
    GTlsCertificate *cert = NULL;
    GTlsCertificateFlags errs = 0;
    GString *msg = g_string_new(NULL);
    if (webkit_web_view_get_tls_info(t->view, &cert, &errs) && cert) {
        char *subj = NULL, *issuer = NULL;
        GDateTime *exp = NULL;
        g_object_get(cert, "subject-name", &subj, "issuer-name", &issuer, "not-valid-after", &exp, NULL);
        char *exps = exp ? g_date_time_format(exp, "%Y-%m-%d") : g_strdup("?");
        g_string_append_printf(msg, "Connection is encrypted%s.\n\nSubject: %s\nIssuer: %s\nValid until: %s",
                               errs ? ", but the certificate has problems" : "",
                               subj ? subj : "?", issuer ? issuer : "?", exps);
        g_free(subj); g_free(issuer); g_free(exps);
        if (exp) g_date_time_unref(exp);
    } else if (g_str_has_prefix(uri, "http://")) {
        g_string_append(msg, "This connection is not encrypted. Information you send could be read by others.");
    } else {
        g_string_append(msg, uri);
    }
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(bw->window), GTK_DIALOG_DESTROY_WITH_PARENT,
        errs || !cert ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", tab_get_title(t));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", msg->str);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
    g_string_free(msg, TRUE);
}

/* ---------------------------------------------------------------- */
/* find bar & status bar callbacks                                   */

static void on_find_changed(GtkEditable *e, BrowserWindow *bw) { find_run(bw, FALSE); }
static void on_find_activate(GtkEntry *e, BrowserWindow *bw)
{
    GdkModifierType mods = 0;
    gtk_get_current_event_state(&mods);
    find_run(bw, (mods & GDK_SHIFT_MASK) != 0);
}
static gboolean on_find_key(GtkWidget *w, GdkEventKey *ev, BrowserWindow *bw)
{
    if (ev->keyval == GDK_KEY_Escape) { find_close(bw); return TRUE; }
    return FALSE;
}
static void find_prev_clicked(GtkButton *b, BrowserWindow *bw) { find_run(bw, TRUE); }
static void find_next_clicked(GtkButton *b, BrowserWindow *bw) { find_run(bw, FALSE); }
static void find_close_clicked(GtkButton *b, BrowserWindow *bw) { find_close(bw); }
static void find_case_toggled(GtkToggleButton *b, BrowserWindow *bw) { find_run(bw, FALSE); }

static void on_counted_matches(WebKitFindController *fc, guint n, BrowserWindow *bw)
{
    char *s = n ? g_strdup_printf("%u match%s", n, n == 1 ? "" : "es") : g_strdup("No matches");
    gtk_label_set_text(GTK_LABEL(bw->find_label), s);
    g_free(s);
}
static void on_find_failed(WebKitFindController *fc, BrowserWindow *bw)
{
    gtk_label_set_text(GTK_LABEL(bw->find_label), "No matches");
}

static void on_zoom_scale(GtkRange *r, BrowserWindow *bw)
{
    if (bw->updating_zoom) return;
    double v = gtk_range_get_value(r);
    if (v > 95 && v < 105) v = 100;   /* snap to 100% */
    set_zoom(bw, v / 100.0);
}


/* The zoom button shows a magnifier: icon themes (elementary, current Adwaita) draw
 * zoom-in as a boxed "+", which looks like "new tab". */
static GtkWidget *zoom_icon(void)
{
    return gtk_image_new_from_icon_name("edit-find-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
}

/* zoom button: [−] [slider] [+] and the percentage, which resets when clicked */
static GtkWidget *build_zoom_button(BrowserWindow *bw)
{
    bw->zoom_btn = gtk_menu_button_new();
    gtk_widget_set_name(bw->zoom_btn, "zoom-button");
    gtk_button_set_relief(GTK_BUTTON(bw->zoom_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(bw->zoom_btn, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(bw->zoom_btn), "nav-button");
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(bb), zoom_icon(), FALSE, FALSE, 0);
    bw->zoom_btn_label = gtk_label_new("");
    gtk_widget_set_no_show_all(bw->zoom_btn_label, TRUE);
    gtk_box_pack_start(GTK_BOX(bb), bw->zoom_btn_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bw->zoom_btn), bb);

    GtkWidget *pop = gtk_popover_new(bw->zoom_btn);
    gtk_widget_set_name(pop, "zoom-popover");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_set(row, "margin", 8, NULL);
    GtkWidget *out = gtk_button_new_from_icon_name("zoom-out-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(out), "win.zoom-out");
    gtk_widget_set_tooltip_text(out, "Zoom out (Ctrl -)");
    bw->zoom_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20, 300, 10);
    gtk_scale_set_draw_value(GTK_SCALE(bw->zoom_scale), FALSE);
    gtk_scale_add_mark(GTK_SCALE(bw->zoom_scale), 100, GTK_POS_BOTTOM, NULL);
    gtk_widget_set_size_request(bw->zoom_scale, 180, -1);
    gtk_widget_set_can_focus(bw->zoom_scale, FALSE);
    g_signal_connect(bw->zoom_scale, "value-changed", G_CALLBACK(on_zoom_scale), bw);
    GtkWidget *in = gtk_button_new_from_icon_name("zoom-in-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(in), "win.zoom-in");
    gtk_widget_set_tooltip_text(in, "Zoom in (Ctrl +)");
    GtkWidget *reset = gtk_button_new();
    bw->zoom_label = gtk_label_new("100%");
    gtk_label_set_width_chars(GTK_LABEL(bw->zoom_label), 5);
    gtk_container_add(GTK_CONTAINER(reset), bw->zoom_label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(reset), "win.zoom-reset");
    gtk_widget_set_tooltip_text(reset, "Reset zoom (Ctrl 0)");
    gtk_box_pack_start(GTK_BOX(row), out, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), bw->zoom_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), in, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), reset, FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(pop), row);
    gtk_widget_show_all(row);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(bw->zoom_btn), pop);
    return bw->zoom_btn;
}

/* ---------------------------------------------------------------- */
/* window lifetime                                                   */

static gboolean on_focus_in(GtkWidget *w, GdkEvent *ev, BrowserWindow *bw)
{
    windows = g_list_remove(windows, bw);
    windows = g_list_prepend(windows, bw);
    Tab *t = browser_window_current_tab(bw);
    if (t) notices_seen(t->view);
    return FALSE;
}

static gboolean save_size_idle(gpointer data)
{
    BrowserWindow *bw = data;
    bw->save_size_id = 0;
    GdkWindow *gw = gtk_widget_get_window(GTK_WIDGET(bw->window));
    if (!gw) return G_SOURCE_REMOVE;
    gboolean max = (gdk_window_get_state(gw) & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    pref_set_bool("window_maximized", max);
    if (!max && !bw->fullscreen) {
        int w, h;
        gtk_window_get_size(GTK_WINDOW(bw->window), &w, &h);
        pref_set_int("window_width", w);
        pref_set_int("window_height", h);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_configure(GtkWidget *w, GdkEvent *ev, BrowserWindow *bw)
{
    if (!bw->save_size_id) bw->save_size_id = g_timeout_add(500, save_size_idle, bw);
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *ev, BrowserWindow *bw)
{
    /* the window manager may take us out of fullscreen */
    if ((ev->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) && !(ev->new_window_state & GDK_WINDOW_STATE_FULLSCREEN)
        && bw->fullscreen) {
        bw->fullscreen = FALSE;
        apply_bars(bw);
    }
    return on_configure(w, (GdkEvent *)ev, bw);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, BrowserWindow *bw)
{
    /* closing the last window keeps the browser running in the tray, so pages can still
     * send notifications; Quit (Ctrl+Q or the tray menu) ends it */
    if (!quitting && pref_get_bool("close_to_tray") && tray_is_active()) {
        int visible = 0;
        for (GList *l = windows; l; l = l->next)
            if (gtk_widget_get_visible(GTK_WIDGET(((BrowserWindow *)l->data)->window))) visible++;
        if (visible <= 1) {
            tray_hide_browser();
            return TRUE;
        }
    }
    if (!quitting && g_list_length(windows) == 1)
        session_save();   /* last window: remember it for next start */
    return FALSE;
}

static void on_destroy(GtkWidget *w, BrowserWindow *bw)
{
    windows = g_list_remove(windows, bw);
    if (!quitting && windows) session_save();
    profile_disconnect_data(bw);
    if (bw->tabs_refresh_id) g_source_remove(bw->tabs_refresh_id);
    if (bw->vstrip_refresh_id) g_source_remove(bw->vstrip_refresh_id);
    if (bw->vbm_sync_id) g_source_remove(bw->vbm_sync_id);
    if (bw->status_timeout_id) g_source_remove(bw->status_timeout_id);
    if (bw->save_size_id) g_source_remove(bw->save_size_id);
    for (GList *l = bw->tabs; l; l = l->next) {
        Tab *t = l->data;
        /* the widgets are being destroyed with the window; just release our refs */
        tab_free(t);
    }
    g_list_free(bw->tabs);
    g_object_unref(bw->trash_menu);
    g_object_unref(bw->completion_store);
    g_free(bw->temp_status);
    g_free(bw);
}

gboolean browser_any_tab_loading(void)
{
    for (GList *w = windows; w; w = w->next)
        for (GList *t = ((BrowserWindow *)w->data)->tabs; t; t = t->next)
            if (webkit_web_view_is_loading(((Tab *)t->data)->view)) return TRUE;
    return FALSE;
}

void browsers_quit(void)
{
    session_save();
    quitting = TRUE;
    GList *copy = g_list_copy(windows);
    for (GList *l = copy; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(((BrowserWindow *)l->data)->window));
    g_list_free(copy);
}

/* ---------------------------------------------------------------- */
/* construction                                                      */

static GtkWidget *tool_button(const char *icon, const char *tip, const char *action)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_set_can_focus(b, FALSE);
    if (action) gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "nav-button");
    return b;
}

static void reload_clicked(GtkButton *b, BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    if (!t) return;
    if (webkit_web_view_is_loading(t->view)) webkit_web_view_stop_loading(t->view);
    else webkit_web_view_reload(t->view);
}

static gboolean nav_pressed(GtkWidget *w, GdkEventButton *ev, BrowserWindow *bw)
{
    /* middle click on back/forward/reload opens the result in a new tab */
    if (ev->button != 2) return FALSE;
    Tab *t = browser_window_current_tab(bw);
    if (!t) return TRUE;
    WebKitBackForwardList *bf = webkit_web_view_get_back_forward_list(t->view);
    WebKitBackForwardListItem *it = NULL;
    if (w == bw->back_btn) it = webkit_back_forward_list_get_back_item(bf);
    else if (w == bw->fwd_btn) it = webkit_back_forward_list_get_forward_item(bf);
    if (it) browser_window_open(bw, webkit_back_forward_list_item_get_uri(it), TRUE);
    else if (w == bw->reload_btn) browser_window_open(bw, tab_get_uri(t), TRUE);
    return TRUE;
}

static void history_item_activate(GtkMenuItem *mi, BrowserWindow *bw)
{
    Tab *t = browser_window_current_tab(bw);
    WebKitBackForwardListItem *it = g_object_get_data(G_OBJECT(mi), "item");
    if (t && it) webkit_web_view_go_to_back_forward_list_item(t->view, it);
}

static gboolean nav_right_click(GtkWidget *w, GdkEventButton *ev, BrowserWindow *bw)
{
    if (ev->button != 3) return nav_pressed(w, ev, bw);
    Tab *t = browser_window_current_tab(bw);
    if (!t) return TRUE;
    WebKitBackForwardList *bf = webkit_web_view_get_back_forward_list(t->view);
    GList *items = w == bw->back_btn ? webkit_back_forward_list_get_back_list_with_limit(bf, 15)
                                     : webkit_back_forward_list_get_forward_list_with_limit(bf, 15);
    if (!items) return TRUE;
    GtkWidget *m = gtk_menu_new();
    if (w == bw->back_btn) items = g_list_reverse(items);
    for (GList *l = items; l; l = l->next) {
        WebKitBackForwardListItem *it = l->data;
        const char *ti = webkit_back_forward_list_item_get_title(it);
        GtkWidget *mi = gtk_menu_item_new_with_label(ti && *ti ? ti : webkit_back_forward_list_item_get_uri(it));
        g_object_set_data_full(G_OBJECT(mi), "item", g_object_ref(it), g_object_unref);
        g_signal_connect(mi, "activate", G_CALLBACK(history_item_activate), bw);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    }
    g_list_free(items);
    gtk_menu_attach_to_widget(GTK_MENU(m), w, NULL);
    g_signal_connect(m, "deactivate", G_CALLBACK(menu_destroy_later), NULL);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_widget(GTK_MENU(m), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *)ev);
    return TRUE;
}

static void connect_find_controller(BrowserWindow *bw, Tab *t);

static void on_page_added(GtkNotebook *nb, GtkWidget *child, guint n, BrowserWindow *bw)
{
    Tab *t = g_object_get_data(G_OBJECT(child), "tab");
    if (t) connect_find_controller(bw, t);
}

static void connect_find_controller(BrowserWindow *bw, Tab *t)
{
    WebKitFindController *fc = webkit_web_view_get_find_controller(t->view);
    g_signal_connect(fc, "counted-matches", G_CALLBACK(on_counted_matches), bw);
    g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_find_failed), bw);
}

/* back/forward/reload/home, address entry with completion, search entry */
static void build_address_bar(BrowserWindow *bw)
{
    bw->back_btn = tool_button("go-previous-symbolic", "Back (Alt+Left)", "win.back");
    bw->fwd_btn = tool_button("go-next-symbolic", "Forward (Alt+Right)", "win.forward");
    bw->reload_btn = tool_button("view-refresh-symbolic", "Reload (F5)", NULL);
    g_signal_connect(bw->reload_btn, "clicked", G_CALLBACK(reload_clicked), bw);
    g_signal_connect(bw->back_btn, "button-press-event", G_CALLBACK(nav_right_click), bw);
    g_signal_connect(bw->fwd_btn, "button-press-event", G_CALLBACK(nav_right_click), bw);
    g_signal_connect(bw->reload_btn, "button-press-event", G_CALLBACK(nav_pressed), bw);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), bw->back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), bw->fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), bw->reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), build_zoom_button(bw), FALSE, FALSE, 0);

    bw->address = gtk_entry_new();
    gtk_widget_set_name(bw->address, "address");
    gtk_entry_set_placeholder_text(GTK_ENTRY(bw->address), "Enter address or search");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(bw->address), GTK_ENTRY_ICON_PRIMARY, "text-html-symbolic");
    gtk_entry_set_icon_activatable(GTK_ENTRY(bw->address), GTK_ENTRY_ICON_PRIMARY, TRUE);
    gtk_entry_set_input_purpose(GTK_ENTRY(bw->address), GTK_INPUT_PURPOSE_URL);
    gtk_widget_set_hexpand(bw->address, TRUE);
    g_signal_connect(bw->address, "activate", G_CALLBACK(on_address_activate), bw);
    g_signal_connect(bw->address, "populate-popup", G_CALLBACK(on_address_popup), bw);
    g_signal_connect(bw->address, "changed", G_CALLBACK(on_address_changed), bw);
    g_signal_connect(bw->address, "key-press-event", G_CALLBACK(on_address_key), bw);
    g_signal_connect(bw->address, "focus-in-event", G_CALLBACK(on_entry_focus_in), bw);
    g_signal_connect(bw->address, "icon-press", G_CALLBACK(on_address_icon), bw);

    bw->completion_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkEntryCompletion *comp = gtk_entry_completion_new();
    gtk_entry_completion_set_model(comp, GTK_TREE_MODEL(bw->completion_store));
    gtk_entry_completion_set_match_func(comp, completion_match, NULL, NULL);
    gtk_entry_completion_set_minimum_key_length(comp, 1);
    gtk_entry_completion_set_popup_set_width(comp, TRUE);
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 3, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(comp), cr, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(comp), cr, "markup", 1);
    g_signal_connect(comp, "match-selected", G_CALLBACK(on_completion_selected), bw);
    gtk_entry_set_completion(GTK_ENTRY(bw->address), comp);
    g_object_unref(comp);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), bw->address, TRUE, TRUE, 4);

}

/* Developer aid: TUXBROWSER_SCREENSHOT=file.png renders the first window (title bar
 * included) into a PNG six seconds after it opens. */
static gboolean self_screenshot(gpointer w)
{
    GtkWidget *win = w;
    int sc = gtk_widget_get_scale_factor(win);
    GtkAllocation a;
    gtk_widget_get_allocation(win, &a);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, a.width * sc, a.height * sc);
    cairo_surface_set_device_scale(s, sc, sc);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(win, cr);
    cairo_destroy(cr);
    cairo_surface_write_to_png(s, g_getenv("TUXBROWSER_SCREENSHOT"));
    cairo_surface_destroy(s);
    return G_SOURCE_REMOVE;
}

BrowserWindow *browser_window_new(GtkApplication *app)
{
    BrowserWindow *bw = g_new0(BrowserWindow, 1);
    bw->window = GTK_APPLICATION_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(bw->window), APP_NAME);
    gtk_window_set_icon_name(GTK_WINDOW(bw->window), "web-browser");
    gtk_window_set_default_size(GTK_WINDOW(bw->window), pref_get_int("window_width"), pref_get_int("window_height"));
    if (pref_get_bool("window_maximized")) gtk_window_maximize(GTK_WINDOW(bw->window));
    gtk_widget_set_name(GTK_WIDGET(bw->window), "tuxbrowser");
    g_action_map_add_action_entries(G_ACTION_MAP(bw->window), win_actions, G_N_ELEMENTS(win_actions), bw);

    bw->trash_menu = g_menu_new();

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(bw->window), vbox);

    /* ---- tab bar ---- */
    bw->notebook = gtk_notebook_new();
    gtk_widget_set_name(bw->notebook, "tabbar");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(bw->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(bw->notebook), FALSE);
    gtk_widget_set_vexpand(bw->notebook, FALSE);
    gtk_widget_add_events(bw->notebook, GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(bw->notebook, "switch-page", G_CALLBACK(on_switch_page), bw);
    g_signal_connect(bw->notebook, "page-added", G_CALLBACK(on_page_added), bw);
    g_signal_connect_swapped(bw->notebook, "page-reordered", G_CALLBACK(vtabs_rebuild), bw);
    g_signal_connect(bw->notebook, "button-press-event", G_CALLBACK(on_tabbar_press), bw);
    g_signal_connect(bw->notebook, "scroll-event", G_CALLBACK(on_tabbar_scroll), bw);

    bw->menu_button = gtk_menu_button_new();
    gtk_widget_set_name(bw->menu_button, "main-menu");
    gtk_menu_button_set_use_popover(GTK_MENU_BUTTON(bw->menu_button), FALSE);
    GMenuModel *om = build_main_menu(bw);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(bw->menu_button), om);
    g_object_unref(om);
    GtkWidget *burger = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(burger), 20);
    gtk_button_set_image(GTK_BUTTON(bw->menu_button), burger);
    gtk_button_set_relief(GTK_BUTTON(bw->menu_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(bw->menu_button, "Main menu");
    gtk_widget_set_focus_on_click(bw->menu_button, FALSE);
    gtk_widget_set_valign(bw->menu_button, GTK_ALIGN_CENTER);
    gtk_widget_show_all(bw->menu_button);

    bw->newtab_button = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_name(bw->newtab_button, "newtab-button");
    gtk_button_set_relief(GTK_BUTTON(bw->newtab_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(bw->newtab_button, "New tab (Ctrl+T)");
    gtk_widget_set_focus_on_click(bw->newtab_button, FALSE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(bw->newtab_button), "win.new-tab");
    bw->trash_button = gtk_menu_button_new();
    gtk_widget_set_name(bw->trash_button, "trash-button");
    gtk_menu_button_set_use_popover(GTK_MENU_BUTTON(bw->trash_button), FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(bw->trash_button), G_MENU_MODEL(bw->trash_menu));
    gtk_button_set_image(GTK_BUTTON(bw->trash_button), gtk_image_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(bw->trash_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(bw->trash_button, "Closed tabs");
    gtk_widget_set_focus_on_click(bw->trash_button, FALSE);
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(bw->trash_button), GTK_ARROW_DOWN);
    bw->vtabs_button = gtk_toggle_button_new();
    gtk_widget_set_name(bw->vtabs_button, "vtabs-button");
    const char *vt_icon = gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "sidebar-show-right-symbolic")
                          ? "sidebar-show-right-symbolic" : "view-list-symbolic";
    gtk_button_set_image(GTK_BUTTON(bw->vtabs_button), gtk_image_new_from_icon_name(vt_icon, GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(bw->vtabs_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(bw->vtabs_button, "Show/hide vertical tab bar (F9)");
    gtk_widget_set_focus_on_click(bw->vtabs_button, FALSE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(bw->vtabs_button), "win.toggle-vtabs");
    /* it sits in the window's top-left corner at full title bar height, as wide as the
     * favicon strip below it: easy to hit without looking, also at the screen corner */
    gtk_widget_set_valign(bw->vtabs_button, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(bw->vtabs_button, VSTRIP_WIDTH, -1);

    /* The notebook no longer shows tabs (the vertical tab bar does); it stays as the
     * invisible model that keeps tab order and the current tab. */
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(bw->notebook), FALSE);
    gtk_widget_set_no_show_all(bw->notebook, TRUE);
    gtk_widget_hide(bw->notebook);
    gtk_box_pack_start(GTK_BOX(vbox), bw->notebook, FALSE, FALSE, 0);

    /* ---- [tab bar toggle] | address bar: [Menu] [nav] [address] [bookmarks bar toggle] ---- */
    bw->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_name(bw->toolbar, "addressbar");
    gtk_box_pack_start(GTK_BOX(bw->toolbar), bw->menu_button, FALSE, FALSE, 0);
    bw->vbm_button = gtk_toggle_button_new();
    gtk_widget_set_name(bw->vbm_button, "vbm-button");
    gtk_button_set_image(GTK_BUTTON(bw->vbm_button), gtk_image_new_from_icon_name(
        gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "user-bookmarks-symbolic") ? "user-bookmarks-symbolic" : "starred-symbolic",
        GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(bw->vbm_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(bw->vbm_button, "Show/hide vertical bookmarks bar (F7)");
    gtk_widget_set_focus_on_click(bw->vbm_button, FALSE);
    gtk_widget_set_valign(bw->vbm_button, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(bw->vbm_button), "win.toggle-vbookmarks");
    build_address_bar(bw);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), build_notify_button(bw), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), build_downloads_button(bw), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->toolbar), bw->vbm_button, FALSE, FALSE, 0);
    /* the toggle is outside the padded address bar so it can reach the window edges */
    GtkWidget *toolrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(toolrow, "toolrow");
    gtk_box_pack_start(GTK_BOX(toolrow), bw->vtabs_button, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(bw->toolbar, TRUE);
    gtk_widget_set_valign(bw->toolbar, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(toolrow), bw->toolbar, TRUE, TRUE, 0);
    bw->toolbar = toolrow;   /* hidden as a whole in fullscreen */

    /* the address bar lives in the window title bar, next to the window buttons */
    GtkWidget *hb = gtk_header_bar_new();
    gtk_widget_set_name(hb, "tabtitlebar");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_has_subtitle(GTK_HEADER_BAR(hb), FALSE);
    /* keep the user's window buttons but drop the window icon and menu, so nothing
     * sits left of the tab bar toggle (window buttons placed on the left stay there) */
    char *layout = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-decoration-layout", &layout, NULL);
    if (layout && (strstr(layout, "icon") || strstr(layout, "menu"))) {
        GString *o = g_string_new(NULL);
        char **sides = g_strsplit(layout, ":", 2);
        for (int s = 0; sides[s]; s++) {
            if (s) g_string_append_c(o, ':');
            char **items = g_strsplit(sides[s], ",", -1);
            gboolean first = TRUE;
            for (int i = 0; items[i]; i++) {
                g_strstrip(items[i]);
                if (g_str_equal(items[i], "icon") || g_str_equal(items[i], "menu") || !*items[i]) continue;
                if (!first) g_string_append_c(o, ',');
                g_string_append(o, items[i]);
                first = FALSE;
            }
            g_strfreev(items);
        }
        g_strfreev(sides);
        gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb), o->str);
        g_string_free(o, TRUE);
    }
    g_free(layout);
    gtk_widget_set_hexpand(bw->toolbar, TRUE);
    gtk_widget_set_valign(bw->toolbar, GTK_ALIGN_FILL);
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hb), bw->toolbar);
    gtk_window_set_titlebar(GTK_WINDOW(bw->window), hb);
    gtk_widget_show_all(hb);

    /* ---- main area: the page ---- */
    bw->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    bw->content_stack = gtk_stack_new();
    gtk_widget_set_name(bw->content_stack, "content");
    gtk_widget_set_vexpand(bw->content_stack, TRUE);

    /* The page sits in an overlay; the vertical tab bar slides in on top of it from
     * the left (the bookmarks bar from the right), so opening one never resizes or
     * moves the web content. */
    GtkWidget *overlay = gtk_overlay_new();
    /* the favicon strip takes its own column next to the page (so it never hides page
     * content or its scrollbar); the expanded tab bar covers it */
    GtkWidget *page_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(page_row), build_vstrip(bw), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_row), bw->content_stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), page_row);
    bw->vtabs_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(bw->vtabs_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(bw->vtabs_revealer), 150);
    gtk_widget_set_halign(bw->vtabs_revealer, GTK_ALIGN_START);
    gtk_widget_set_valign(bw->vtabs_revealer, GTK_ALIGN_FILL);
    GtkWidget *vt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(vt, "vtabs");
    gtk_widget_set_size_request(vt, 270, -1);
    GtkWidget *vth = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(vth), "vtabs-header");
    bw->vtabs_title = gtk_label_new("Tabs");
    gtk_label_set_xalign(GTK_LABEL(bw->vtabs_title), 0);
    gtk_box_pack_start(GTK_BOX(vth), bw->vtabs_title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vt), vth, FALSE, FALSE, 0);
    /* tab management lives in the tab bar header; the title bar keeps only the toggle */
    gtk_box_pack_end(GTK_BOX(vth), bw->trash_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(vth), bw->newtab_button, FALSE, FALSE, 0);
    bw->vtabs_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(bw->vtabs_list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(bw->vtabs_list), TRUE);
    gtk_widget_add_events(bw->vtabs_list, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(bw->vtabs_list, "row-activated", G_CALLBACK(vtabs_row_activated), bw);
    g_signal_connect(bw->vtabs_list, "button-press-event", G_CALLBACK(vtabs_pressed), bw);
    vtabs_dnd_setup(bw);
    bw->vtabs_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bw->vtabs_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(bw->vtabs_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(bw->vtabs_scroll), bw->vtabs_list);
    gtk_box_pack_start(GTK_BOX(vt), bw->vtabs_scroll, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(bw->vtabs_revealer), vt);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bw->vtabs_revealer);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_vbookmarks(bw));
    bw->status_float = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(bw->status_float, "status-float");
    gtk_widget_set_halign(bw->status_float, GTK_ALIGN_START);
    gtk_widget_set_valign(bw->status_float, GTK_ALIGN_END);
    bw->status_label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(bw->status_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(bw->status_label), 90);
    gtk_label_set_xalign(GTK_LABEL(bw->status_label), 0);
    gtk_container_add(GTK_CONTAINER(bw->status_float), bw->status_label);
    gtk_widget_show(bw->status_label);
    gtk_widget_set_no_show_all(bw->status_float, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bw->status_float);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), bw->status_float, TRUE);   /* never blocks the page */
    gtk_box_pack_start(GTK_BOX(bw->content_box), overlay, TRUE, TRUE, 0);

    /* find bar */
    bw->find_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(bw->find_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    bw->findbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(bw->findbar, "findbar");
    gtk_box_pack_start(GTK_BOX(bw->findbar), gtk_label_new("Find:"), FALSE, FALSE, 4);
    bw->find_entry = gtk_search_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(bw->find_entry), 28);
    g_signal_connect(bw->find_entry, "changed", G_CALLBACK(on_find_changed), bw);
    g_signal_connect(bw->find_entry, "activate", G_CALLBACK(on_find_activate), bw);
    g_signal_connect(bw->find_entry, "key-press-event", G_CALLBACK(on_find_key), bw);
    gtk_box_pack_start(GTK_BOX(bw->findbar), bw->find_entry, FALSE, FALSE, 0);
    GtkWidget *fp = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *fn = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(fp, "Previous (Shift+F3)");
    gtk_widget_set_tooltip_text(fn, "Next (F3)");
    g_signal_connect(fp, "clicked", G_CALLBACK(find_prev_clicked), bw);
    g_signal_connect(fn, "clicked", G_CALLBACK(find_next_clicked), bw);
    gtk_box_pack_start(GTK_BOX(bw->findbar), fp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->findbar), fn, FALSE, FALSE, 0);
    bw->find_case = gtk_check_button_new_with_mnemonic("Match _case");
    g_signal_connect(bw->find_case, "toggled", G_CALLBACK(find_case_toggled), bw);
    gtk_box_pack_start(GTK_BOX(bw->findbar), bw->find_case, FALSE, FALSE, 6);
    bw->find_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bw->findbar), bw->find_label, FALSE, FALSE, 6);
    GtkWidget *fc = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(fc), GTK_RELIEF_NONE);
    g_signal_connect(fc, "clicked", G_CALLBACK(find_close_clicked), bw);
    gtk_box_pack_end(GTK_BOX(bw->findbar), fc, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bw->find_revealer), bw->findbar);
    gtk_box_pack_end(GTK_BOX(bw->content_box), bw->find_revealer, FALSE, FALSE, 0);
    gtk_widget_set_vexpand(bw->content_box, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), bw->content_box, TRUE, TRUE, 0);


    g_signal_connect(bw->window, "focus-in-event", G_CALLBACK(on_focus_in), bw);
    tray_window_added(bw);
    g_signal_connect(bw->window, "configure-event", G_CALLBACK(on_configure), bw);
    g_signal_connect(bw->window, "window-state-event", G_CALLBACK(on_window_state), bw);
    g_signal_connect(bw->window, "delete-event", G_CALLBACK(on_delete), bw);
    g_signal_connect(bw->window, "destroy", G_CALLBACK(on_destroy), bw);

    profile_connect(EV_BOOKMARKS, on_bookmarks_changed, bw);
    profile_connect(EV_CLOSED_TABS, on_closed_changed, bw);
    profile_connect(EV_SETTINGS, on_settings_changed, bw);

    windows = g_list_prepend(windows, bw);
    gtk_widget_show_all(vbox);
    rebuild_trash(bw);
    on_settings_changed(bw);
    gtk_widget_show(GTK_WIDGET(bw->window));
    if (g_getenv("TUXBROWSER_SCREENSHOT") && !g_object_get_data(G_OBJECT(app), "screenshot")) {
        g_object_set_data(G_OBJECT(app), "screenshot", bw);
        g_timeout_add_seconds(6, self_screenshot, bw->window);
    }
    return bw;
}

/* ---------------------------------------------------------------- */
/* downloads button (next to the address bar)                       */

/* While something downloads, a ring around the icon fills up with the overall progress
 * (a turning arc when sizes are unknown); the tooltip has the count and percentage. */
static void downloads_button_update(gpointer data)
{
    BrowserWindow *bw = data;
    guint running = 0;
    guint64 got = 0, total = 0;
    gboolean sized = TRUE;
    for (guint i = 0; i < transfers->len; i++) {
        Transfer *t = transfers->pdata[i];
        if (t->state != 0) continue;
        running++;
        got += t->received;
        total += t->total;
        if (!t->total) sized = FALSE;
    }
    bw->downloads_running = running;
    bw->downloads_frac = running && sized && total ? (double)got / total : -1;
    bw->downloads_spin = (bw->downloads_spin + 1) % 8;   /* called every 0.5 s while downloading */
    GtkStyleContext *sc = gtk_widget_get_style_context(bw->downloads_btn);
    if (running) gtk_style_context_add_class(sc, "active");
    else gtk_style_context_remove_class(sc, "active");
    char *tip = !running ? g_strdup("Downloads (Ctrl+J)")
              : bw->downloads_frac >= 0 ? g_strdup_printf("Downloads: %u in progress, %d%% (Ctrl+J)", running,
                                                          (int)(100 * bw->downloads_frac))
              : g_strdup_printf("Downloads: %u in progress (Ctrl+J)", running);
    gtk_widget_set_tooltip_text(bw->downloads_btn, tip);
    g_free(tip);
    gtk_widget_queue_draw(bw->downloads_ring);
}

static gboolean draw_downloads_ring(GtkWidget *w, cairo_t *cr, BrowserWindow *bw)
{
    if (!bw->downloads_running) return FALSE;
    double cx = gtk_widget_get_allocated_width(w) / 2.0, cy = gtk_widget_get_allocated_height(w) / 2.0;
    double r = MIN(cx, cy) - 1.5;
    GdkRGBA fg;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), gtk_widget_get_state_flags(w), &fg);
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.22);   /* track */
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 1.0);   /* progress, from 12 o'clock */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    double start = -G_PI / 2, len = 2 * G_PI * bw->downloads_frac;
    if (bw->downloads_frac < 0) {                                 /* unknown size: a turning arc */
        start += bw->downloads_spin * G_PI / 4;
        len = G_PI / 2;
    }
    if (len > 0.01) {
        cairo_arc(cr, cx, cy, r, start, start + len);
        cairo_stroke(cr);
    }
    return FALSE;
}

static void downloads_clicked(GtkButton *b, BrowserWindow *bw) { downloads_popup_toggle(bw); }

static GtkWidget *build_downloads_button(BrowserWindow *bw)
{
    /* same size and alignment as the back/forward/reload buttons: a 24 px slot with the
     * icon in the middle, small enough for the ring to fit around it */
    bw->downloads_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(bw->downloads_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(bw->downloads_btn, FALSE);
    gtk_widget_set_can_focus(bw->downloads_btn, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(bw->downloads_btn), "nav-button");
    gtk_widget_set_name(bw->downloads_btn, "downloads-button");
    gtk_widget_set_valign(bw->downloads_btn, GTK_ALIGN_CENTER);
    GtkWidget *ov = gtk_overlay_new();
    GtkWidget *img = gtk_image_new_from_icon_name("folder-download-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 14);
    gtk_widget_set_size_request(img, 24, 24);
    gtk_container_add(GTK_CONTAINER(ov), img);
    bw->downloads_ring = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(bw->downloads_ring), "downloads-ring");
    g_signal_connect(bw->downloads_ring, "draw", G_CALLBACK(draw_downloads_ring), bw);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), bw->downloads_ring);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(ov), bw->downloads_ring, TRUE);
    gtk_container_add(GTK_CONTAINER(bw->downloads_btn), ov);
    g_signal_connect(bw->downloads_btn, "clicked", G_CALLBACK(downloads_clicked), bw);
    profile_connect(EV_DOWNLOADS, downloads_button_update, bw);
    downloads_button_update(bw);
    return bw->downloads_btn;
}

/* ---------------------------------------------------------------- */
/* notifications button (the bell, left of the downloads button)    */

/* Shown only while there are notifications; a soft red dot with the number of unread ones
 * sits on its top right corner. */
static void notify_button_update(gpointer data)
{
    BrowserWindow *bw = data;
    guint unread = notices_unread();
    gtk_widget_set_visible(bw->notify_btn, notices->len > 0);
    char *tip = unread ? g_strdup_printf("Notifications: %u new (Ctrl+Shift+M)", unread)
                       : g_strdup_printf("Notifications: %u (Ctrl+Shift+M)", notices->len);
    gtk_widget_set_tooltip_text(bw->notify_btn, tip);
    g_free(tip);
    gtk_widget_queue_draw(bw->notify_badge);
}

static gboolean draw_notify_badge(GtkWidget *w, cairo_t *cr, BrowserWindow *bw)
{
    guint unread = notices_unread();
    if (!unread) return FALSE;
    double W = gtk_widget_get_allocated_width(w);
    double r = 5.5, cx = W - r - 0.5, cy = r + 0.5;
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0xe5 / 255.0, 0x53 / 255.0, 0x4b / 255.0);   /* soft red, as in the tray */
    cairo_fill(cr);
    char num[8];
    g_snprintf(num, sizeof num, "%u", MIN(unread, 9u));
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 8.5);
    cairo_text_extents_t te;
    cairo_text_extents(cr, num, &te);
    cairo_move_to(cr, cx - te.x_bearing - te.width / 2, cy - te.y_bearing - te.height / 2);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_show_text(cr, num);
    return FALSE;
}

static void notify_clicked(GtkButton *b, BrowserWindow *bw) { notifications_popup_toggle(bw); }

static GtkWidget *build_notify_button(BrowserWindow *bw)
{
    bw->notify_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(bw->notify_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(bw->notify_btn, FALSE);
    gtk_widget_set_can_focus(bw->notify_btn, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(bw->notify_btn), "nav-button");
    gtk_widget_set_name(bw->notify_btn, "notify-button");
    gtk_widget_set_valign(bw->notify_btn, GTK_ALIGN_CENTER);
    GtkWidget *ov = gtk_overlay_new();
    const char *icon = gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "notification-symbolic")
                     ? "notification-symbolic" : "preferences-system-notifications-symbolic";
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 14);
    gtk_widget_set_size_request(img, 24, 24);
    gtk_container_add(GTK_CONTAINER(ov), img);
    bw->notify_badge = gtk_drawing_area_new();
    g_signal_connect(bw->notify_badge, "draw", G_CALLBACK(draw_notify_badge), bw);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), bw->notify_badge);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(ov), bw->notify_badge, TRUE);
    gtk_container_add(GTK_CONTAINER(bw->notify_btn), ov);
    gtk_widget_show_all(bw->notify_btn);
    gtk_widget_set_no_show_all(bw->notify_btn, TRUE);
    g_signal_connect(bw->notify_btn, "clicked", G_CALLBACK(notify_clicked), bw);
    profile_connect(EV_NOTIFICATIONS, notify_button_update, bw);
    notify_button_update(bw);
    return bw->notify_btn;
}

void browser_window_show(BrowserWindow *bw)
{
    GtkWindow *w = GTK_WINDOW(bw->window);
    gtk_widget_show(GTK_WIDGET(w));
    gtk_window_deiconify(w);
    gtk_window_present_with_time(w, gtk_get_current_event_time());
}

/* ---------------------------------------------------------------- */
/* session                                                           */

void session_save(void)
{
    GKeyFile *kf = g_key_file_new();
    int wi = 0;
    /* oldest window first so restore order matches */
    GList *rev = g_list_reverse(g_list_copy(windows));
    for (GList *l = rev; l; l = l->next, wi++) {
        BrowserWindow *bw = l->data;
        GList *tabs = browser_window_tabs_in_order(bw);
        GPtrArray *uris = g_ptr_array_new();
        Tab *cur = browser_window_current_tab(bw);
        int active = 0, i = 0;
        for (GList *t = tabs; t; t = t->next, i++) {
            const char *u = tab_get_uri(t->data);
            g_ptr_array_add(uris, (gpointer)(*u && !g_str_has_prefix(u, "about:") ? u : START_URI));
            if (t->data == cur) active = i;
        }
        char grp[32];
        g_snprintf(grp, sizeof grp, "Window%d", wi);
        g_key_file_set_string_list(kf, grp, "tabs", (const char *const *)uris->pdata, uris->len);
        g_key_file_set_integer(kf, grp, "active", active);
        g_ptr_array_free(uris, TRUE);
        g_list_free(tabs);
    }
    g_list_free(rev);
    char *path = g_build_filename(profile->data_dir, "session.ini", NULL);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

gboolean session_restore(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = g_build_filename(profile->data_dir, "session.ini", NULL);
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    gboolean any = FALSE;
    if (ok) {
        gsize ng = 0;
        char **groups = g_key_file_get_groups(kf, &ng);
        GtkApplication *app = GTK_APPLICATION(g_application_get_default());
        for (gsize g = 0; g < ng; g++) {
            gsize n = 0;
            char **tabs = g_key_file_get_string_list(kf, groups[g], "tabs", &n, NULL);
            if (!tabs || !n) { g_strfreev(tabs); continue; }
            int active = g_key_file_get_integer(kf, groups[g], "active", NULL);
            BrowserWindow *bw = browser_window_new(app);
            restoring = TRUE;
            for (gsize i = 0; i < n; i++)
                browser_window_open(bw, tabs[i], TRUE);
            restoring = FALSE;
            gtk_notebook_set_current_page(GTK_NOTEBOOK(bw->notebook), CLAMP(active, 0, (int)n - 1));
            g_strfreev(tabs);
            any = TRUE;
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    return any;
}
