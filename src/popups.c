/* History and Downloads popups: popovers that drop down from the title bar. */
#include "tuxbrowser.h"
#include <string.h>

/* One per window and kind, created on first use; freed with its popover. */
typedef struct {
    BrowserWindow *bw;
    GtkWidget *popover;
    /* history */
    GtkWidget *hi_search, *hi_view;
    GtkTreeStore *hi_store;
    gboolean hi_dirty;
    guint hi_timer;
    /* downloads */
    GtkWidget *tr_list;
} Popup;

enum { COL_ICON, COL_TITLE, COL_URL, COL_TIP, COL_PTR, COL_WHEN, N_COLS };
#define STORE_TYPES G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_STRING

/* ---------------------------------------------------------------- */
/* helpers                                                           */

static GtkWidget *small_button(const char *icon, const char *tip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    return b;
}

/* Popup header: title on the left, the popup's buttons on the right. */
static GtkWidget *popup_header(const char *title, GtkWidget **toolbar)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "popup-header");
    GtkWidget *l = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "popup-title");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 2);
    *toolbar = box;
    return box;
}

static void add_text_column(GtkWidget *tv, const char *title, int col, gboolean expand, PangoEllipsizeMode el)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", el, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_column_set_resizable(c, TRUE);
    if (expand) gtk_tree_view_column_set_min_width(c, 160);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
}

static GtkWidget *tree_view(GtkTreeModel *model, gboolean headers)
{
    GtkWidget *tv = gtk_tree_view_new_with_model(model);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), headers);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tv), FALSE);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(tv), COL_TIP);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, ri, FALSE);
    gtk_tree_view_column_add_attribute(col, ri, "icon-name", COL_ICON);
    GtkCellRenderer *rt = gtk_cell_renderer_text_new();
    g_object_set(rt, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(col, rt, TRUE);
    gtk_tree_view_column_add_attribute(col, rt, "text", COL_TITLE);
    gtk_tree_view_column_set_title(col, "Title");
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    gtk_style_context_add_class(gtk_widget_get_style_context(tv), "popup-tree");
    return tv;
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

static char *selected_url(GtkWidget *tv)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    char *url = NULL;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), &m, &it))
        gtk_tree_model_get(m, &it, COL_URL, &url, -1);
    return url;
}

static void open_url(Popup *p, const char *url, int how)   /* 0 current, 1 new tab, 2 background */
{
    if (!url || !*url) return;
    Tab *t = browser_window_current_tab(p->bw);
    if (how == 0 && t) {
        tab_load(t, url);
        gtk_popover_popdown(GTK_POPOVER(p->popover));   /* done: show the page */
    } else {
        browser_window_open(p->bw, url, how == 2);
    }
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *c, Popup *p)
{
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(m, &it, path)) return;
    char *url = NULL;
    gtk_tree_model_get(m, &it, COL_URL, &url, -1);
    if (url && *url) open_url(p, url, 0);
    else if (gtk_tree_view_row_expanded(tv, path)) gtk_tree_view_collapse_row(tv, path);
    else gtk_tree_view_expand_row(tv, path, FALSE);
    g_free(url);
}

static gboolean on_tree_press(GtkWidget *w, GdkEventButton *ev, Popup *p)
{
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 2) return FALSE;
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), ev->x, ev->y, &path, NULL, NULL, NULL)) {
        GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(w));
        GtkTreeIter it;
        char *url = NULL;
        if (gtk_tree_model_get_iter(m, &it, path)) gtk_tree_model_get(m, &it, COL_URL, &url, -1);
        open_url(p, url, 2);
        g_free(url);
        gtk_tree_path_free(path);
        return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- */
/* history                                                           */

static void hi_rebuild(Popup *p)
{
    p->hi_dirty = FALSE;
    gtk_tree_store_clear(p->hi_store);
    const char *q = gtk_entry_get_text(GTK_ENTRY(p->hi_search));
    GPtrArray *items = history_query(q, 1500);

    GDateTime *now = g_date_time_new_now_local();
    GDateTime *today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
                                             g_date_time_get_day_of_month(now), 0, 0, 0);
    gint64 t0 = g_date_time_to_unix(today);
    gint64 bounds[] = { t0, t0 - 86400, t0 - 7 * 86400, t0 - 31 * 86400, G_MININT64 };
    const char *names[] = { "Today", "Yesterday", "Earlier this week", "Earlier this month", "Older" };
    GtkTreeIter groups[5];
    gboolean made[5] = { FALSE };

    for (guint i = 0; i < items->len; i++) {
        HistoryItem *h = items->pdata[i];
        int g = 0;
        while (g < 4 && h->last_visit < bounds[g]) g++;
        if (!made[g]) {
            gtk_tree_store_insert_with_values(p->hi_store, &groups[g], NULL, -1, COL_ICON, "folder-symbolic",
                                              COL_TITLE, names[g], -1);
            made[g] = TRUE;
        }
        GDateTime *dt = g_date_time_new_from_unix_local(h->last_visit);
        char *when = g_date_time_format(dt, g < 2 ? "%H:%M" : "%Y-%m-%d %H:%M");
        g_date_time_unref(dt);
        char *tip = g_markup_printf_escaped("<b>%s</b>\n%s\n<small>%s · %d visit%s</small>",
                                            h->title && *h->title ? h->title : h->url, h->url, when,
                                            h->visits, h->visits == 1 ? "" : "s");
        gtk_tree_store_insert_with_values(p->hi_store, NULL, &groups[g], -1, COL_ICON, "text-html-symbolic",
                                          COL_TITLE, h->title && *h->title ? h->title : h->url,
                                          COL_URL, h->url, COL_TIP, tip, COL_WHEN, when, -1);
        g_free(tip);
        g_free(when);
    }
    g_ptr_array_unref(items);
    g_date_time_unref(today);
    g_date_time_unref(now);

    /* expand Today (and everything when searching) */
    if (*q) {
        gtk_tree_view_expand_all(GTK_TREE_VIEW(p->hi_view));
    } else if (made[0]) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(p->hi_store), &groups[0]);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(p->hi_view), path, FALSE);
        gtk_tree_path_free(path);
    }
}

static gboolean hi_timeout(gpointer data)
{
    Popup *p = data;
    p->hi_timer = 0;
    if (gtk_widget_get_mapped(p->hi_view)) hi_rebuild(p);
    else p->hi_dirty = TRUE;
    return G_SOURCE_REMOVE;
}

static void hi_changed(gpointer data)
{
    Popup *p = data;
    if (!p->hi_timer) p->hi_timer = g_timeout_add(800, hi_timeout, p);
}

static void hi_map(GtkWidget *w, Popup *p) { if (p->hi_dirty) hi_rebuild(p); }
static void hi_search_changed(GtkSearchEntry *e, Popup *p) { hi_rebuild(p); }

static void hi_delete(GtkButton *b, Popup *p)
{
    char *url = selected_url(p->hi_view);
    if (url && *url) history_delete(url);
    g_free(url);
}

static void hi_clear(GtkButton *b, Popup *p)
{
    gtk_popover_popdown(GTK_POPOVER(p->popover));   /* the dialog can't sit on top of a modal popover */
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(p->bw->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, "Delete all history?");
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) history_delete(NULL);
    gtk_widget_destroy(d);
}

static gboolean hi_key(GtkWidget *w, GdkEventKey *ev, Popup *p)
{
    if (ev->keyval == GDK_KEY_Delete) { hi_delete(NULL, p); return TRUE; }
    return FALSE;
}

static GtkWidget *build_history(Popup *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), popup_header("History", &hdr), FALSE, FALSE, 0);
    GtkWidget *del = small_button("edit-delete-symbolic", "Delete selected (Del)");
    GtkWidget *clr = small_button("edit-clear-all-symbolic", "Delete all history");
    g_signal_connect(del, "clicked", G_CALLBACK(hi_delete), p);
    g_signal_connect(clr, "clicked", G_CALLBACK(hi_clear), p);
    gtk_box_pack_end(GTK_BOX(hdr), clr, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), del, FALSE, FALSE, 0);
    p->hi_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->hi_search), "Search history");
    g_signal_connect(p->hi_search, "search-changed", G_CALLBACK(hi_search_changed), p);
    gtk_box_pack_start(GTK_BOX(box), p->hi_search, FALSE, FALSE, 0);

    p->hi_store = gtk_tree_store_new(N_COLS, STORE_TYPES);
    p->hi_view = tree_view(GTK_TREE_MODEL(p->hi_store), FALSE);
    add_text_column(p->hi_view, "Visited", COL_WHEN, FALSE, PANGO_ELLIPSIZE_NONE);
    g_signal_connect(p->hi_view, "row-activated", G_CALLBACK(on_row_activated), p);
    g_signal_connect(p->hi_view, "button-press-event", G_CALLBACK(on_tree_press), p);
    g_signal_connect(p->hi_view, "key-press-event", G_CALLBACK(hi_key), p);
    g_signal_connect(p->hi_view, "map", G_CALLBACK(hi_map), p);
    gtk_box_pack_start(GTK_BOX(box), scrolled(p->hi_view), TRUE, TRUE, 0);
    p->hi_dirty = TRUE;
    profile_connect(EV_HISTORY, hi_changed, p);
    return box;
}

/* ---------------------------------------------------------------- */
/* downloads                                                         */

static gboolean transfer_alive(Transfer *t)
{
    for (guint i = 0; i < transfers->len; i++)
        if (transfers->pdata[i] == t) return TRUE;
    return FALSE;
}

static void tr_open(GtkButton *b, gpointer data)
{
    Transfer *t = g_object_get_data(G_OBJECT(b), "transfer");
    if (!transfer_alive(t)) return;
    if (t->state == 0) transfer_cancel(t);
    else transfer_open(t, GPOINTER_TO_INT(data));
}

static void tr_row_update(GtkWidget *row, Transfer *t)
{
    GtkWidget *bar = g_object_get_data(G_OBJECT(row), "bar");
    GtkWidget *name = g_object_get_data(G_OBJECT(row), "name");
    GtkWidget *info = g_object_get_data(G_OBJECT(row), "info");
    GtkWidget *act = g_object_get_data(G_OBJECT(row), "action");
    GtkWidget *folder = g_object_get_data(G_OBJECT(row), "folder");

    gtk_label_set_text(GTK_LABEL(name), t->name ? t->name : "");
    char *rec = format_size(t->received), *tot = t->total ? format_size(t->total) : NULL;
    char *s;
    switch (t->state) {
    case 0: {
        double secs = (g_get_real_time() - t->started) / (double)G_USEC_PER_SEC;
        char *speed = format_size(secs > 0.5 ? (guint64)(t->received / secs) : 0);
        s = tot ? g_strdup_printf("%s of %s  (%s/s)", rec, tot, speed) : g_strdup_printf("%s  (%s/s)", rec, speed);
        g_free(speed);
        if (t->total) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), (double)t->received / t->total);
        else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(bar));
        break;
    }
    case 1:  s = g_strdup_printf("Done — %s", tot ? tot : rec);
             gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), 1.0); break;
    case 2:  s = g_strdup_printf("Failed: %s", t->error ? t->error : "error"); break;
    default: s = g_strdup("Stopped"); break;
    }
    gtk_label_set_text(GTK_LABEL(info), s);
    gtk_widget_set_tooltip_text(row, t->dest ? t->dest : t->name);
    gtk_widget_set_visible(bar, t->state == 0);
    gtk_button_set_image(GTK_BUTTON(act), gtk_image_new_from_icon_name(
        t->state == 0 ? "process-stop-symbolic" : "document-open-symbolic", GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(act, t->state == 0 ? "Stop" : "Open");
    gtk_widget_set_sensitive(act, t->state <= 1);
    gtk_widget_set_visible(folder, t->state == 1);
    g_free(s); g_free(rec); g_free(tot);
}

static GtkWidget *tr_row_new(Transfer *t)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_object_set(row, "margin", 4, NULL);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name("text-x-generic-symbolic", GTK_ICON_SIZE_DND), FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    GtkWidget *bar = gtk_progress_bar_new();
    GtkWidget *info = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(info), 0);
    gtk_label_set_ellipsize(GTK_LABEL(info), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(info), "dim-label");
    gtk_box_pack_start(GTK_BOX(col), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
    GtkWidget *act = small_button("process-stop-symbolic", "Stop");
    GtkWidget *folder = small_button("folder-open-symbolic", "Show in folder");
    g_object_set_data(G_OBJECT(act), "transfer", t);
    g_object_set_data(G_OBJECT(folder), "transfer", t);
    g_signal_connect(act, "clicked", G_CALLBACK(tr_open), GINT_TO_POINTER(0));
    g_signal_connect(folder, "clicked", G_CALLBACK(tr_open), GINT_TO_POINTER(1));
    gtk_widget_set_valign(act, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(folder, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(row), folder, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), act, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(row), "bar", bar);
    g_object_set_data(G_OBJECT(row), "name", name);
    g_object_set_data(G_OBJECT(row), "info", info);
    g_object_set_data(G_OBJECT(row), "action", act);
    g_object_set_data(G_OBJECT(row), "folder", folder);
    g_object_set_data(G_OBJECT(row), "transfer", t);
    gtk_widget_show_all(row);
    gtk_widget_set_no_show_all(bar, TRUE);
    gtk_widget_set_no_show_all(folder, TRUE);
    tr_row_update(row, t);
    return row;
}

static void tr_changed(gpointer data)
{
    Popup *p = data;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(p->tr_list));
    gboolean same = g_list_length(rows) == transfers->len;
    guint i = 0;
    for (GList *l = rows; same && l; l = l->next, i++) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(l->data));
        if (g_object_get_data(G_OBJECT(child), "transfer") != transfers->pdata[i]) same = FALSE;
    }
    if (same) {
        i = 0;
        for (GList *l = rows; l; l = l->next, i++)
            tr_row_update(gtk_bin_get_child(GTK_BIN(l->data)), transfers->pdata[i]);
    } else {
        for (GList *l = rows; l; l = l->next) gtk_widget_destroy(l->data);
        for (i = 0; i < transfers->len; i++)
            gtk_list_box_insert(GTK_LIST_BOX(p->tr_list), tr_row_new(transfers->pdata[i]), -1);
        gtk_widget_show_all(p->tr_list);
    }
    g_list_free(rows);
}

static void tr_row_activated(GtkListBox *lb, GtkListBoxRow *row, Popup *p)
{
    Transfer *t = g_object_get_data(G_OBJECT(gtk_bin_get_child(GTK_BIN(row))), "transfer");
    if (transfer_alive(t) && t->state == 1) transfer_open(t, FALSE);
}

static void tr_clear(GtkButton *b, Popup *p) { transfers_clear_finished(); }
static void tr_folder(GtkButton *b, Popup *p)
{
    char *dir = pref_get_string("download_dir");
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_free(dir);
}

static GtkWidget *build_downloads(Popup *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), popup_header("Downloads", &hdr), FALSE, FALSE, 0);
    GtkWidget *clr = small_button("edit-clear-all-symbolic", "Clean up finished downloads");
    GtkWidget *fol = small_button("folder-download-symbolic", "Open download folder");
    g_signal_connect(clr, "clicked", G_CALLBACK(tr_clear), p);
    g_signal_connect(fol, "clicked", G_CALLBACK(tr_folder), p);
    gtk_box_pack_end(GTK_BOX(hdr), clr, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), fol, FALSE, FALSE, 0);
    p->tr_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->tr_list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->tr_list), FALSE);
    GtkWidget *ph = gtk_label_new("No downloads");
    gtk_style_context_add_class(gtk_widget_get_style_context(ph), "dim-label");
    gtk_widget_show(ph);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(p->tr_list), ph);
    g_signal_connect(p->tr_list, "row-activated", G_CALLBACK(tr_row_activated), p);
    gtk_box_pack_start(GTK_BOX(box), scrolled(p->tr_list), TRUE, TRUE, 0);
    tr_changed(p);
    profile_connect(EV_DOWNLOADS, tr_changed, p);
    return box;
}

/* ---------------------------------------------------------------- */

static void popup_destroyed(GtkWidget *w, Popup *p)
{
    profile_disconnect_data(p);
    if (p->hi_timer) g_source_remove(p->hi_timer);
    if (p->hi_store) g_object_unref(p->hi_store);
    g_free(p);
}

static void popup_closed(GtkPopover *pop, Popup *p)
{
    /* hand the keyboard back to the page */
    Tab *t = browser_window_current_tab(p->bw);
    if (t) gtk_widget_grab_focus(GTK_WIDGET(t->view));
}

static Popup *popup_new(BrowserWindow *bw, gboolean history)
{
    Popup *p = g_new0(Popup, 1);
    p->bw = bw;
    p->popover = gtk_popover_new(bw->menu_button);
    gtk_widget_set_name(p->popover, history ? "history-popup" : "downloads-popup");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->popover), "tux-popup");
    GtkWidget *content = history ? build_history(p) : build_downloads(p);
    g_object_set(content, "margin", 8, NULL);
    gtk_widget_set_size_request(content, 560, history ? 520 : 360);
    gtk_container_add(GTK_CONTAINER(p->popover), content);
    gtk_widget_show_all(content);
    g_signal_connect(p->popover, "destroy", G_CALLBACK(popup_destroyed), p);
    g_signal_connect(p->popover, "closed", G_CALLBACK(popup_closed), p);
    return p;
}

/* Shows (or hides, if it is already up) one of the window's popups. History drops down
 * from the menu button, Downloads from its own button; in full screen, where the title bar
 * is hidden, both drop from the top of the page. */
static void popup_toggle(BrowserWindow *bw, gboolean history)
{
    const char *key = history ? "history-popup" : "downloads-popup";
    Popup *p = g_object_get_data(G_OBJECT(bw->window), key);
    if (!p) {
        p = popup_new(bw, history);
        g_object_set_data(G_OBJECT(bw->window), key, p);
    }
    if (gtk_widget_get_visible(p->popover)) {
        gtk_popover_popdown(GTK_POPOVER(p->popover));
        return;
    }
    GtkWidget *anchor = history ? bw->menu_button : bw->downloads_btn;
    if (gtk_widget_is_drawable(anchor)) {
        GdkRectangle r = { 0, 0, gtk_widget_get_allocated_width(anchor), gtk_widget_get_allocated_height(anchor) };
        gtk_popover_set_relative_to(GTK_POPOVER(p->popover), anchor);
        gtk_popover_set_pointing_to(GTK_POPOVER(p->popover), &r);
    } else {
        GdkRectangle r = { gtk_widget_get_allocated_width(bw->content_box) / 2, 0, 1, 1 };
        gtk_popover_set_relative_to(GTK_POPOVER(p->popover), bw->content_box);
        gtk_popover_set_pointing_to(GTK_POPOVER(p->popover), &r);
    }
    gtk_popover_set_position(GTK_POPOVER(p->popover), GTK_POS_BOTTOM);
    gtk_popover_popup(GTK_POPOVER(p->popover));
    if (history) gtk_widget_grab_focus(p->hi_search);
    else gtk_widget_grab_focus(p->tr_list);
}

void history_popup_toggle(BrowserWindow *bw)   { popup_toggle(bw, TRUE); }
void downloads_popup_toggle(BrowserWindow *bw) { popup_toggle(bw, FALSE); }
