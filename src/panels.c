/* Side panels: Bookmarks, Notes, Transfers, History, Windows. */
#include "tuxbrowser.h"
#include <string.h>

struct _Panels {
    BrowserWindow *bw;
    GHashTable *buttons;                 /* name -> selector button */
    /* bookmarks */
    GtkWidget *bm_search, *bm_view;
    GtkTreeStore *bm_store;
    /* history */
    GtkWidget *hi_search, *hi_view;
    GtkTreeStore *hi_store;
    gboolean hi_dirty;
    guint hi_timer;
    /* transfers */
    GtkWidget *tr_list, *tr_empty;
    guint tr_count;
    /* notes */
    GtkWidget *no_view, *no_text;
    GtkListStore *no_store;
    Note *no_current;
    gboolean no_loading, no_saving;
    guint no_timer;
    /* windows */
    GtkWidget *wi_view;
    GtkTreeStore *wi_store;
};

enum { COL_ICON, COL_TITLE, COL_URL, COL_TIP, COL_PTR, N_COLS };

static const struct { const char *name, *icon, *title; } panel_defs[] = {
    { "bookmarks", "user-bookmarks-symbolic",   "Bookmarks" },
    { "notes",     "accessories-text-editor-symbolic", "Notes" },
    { "transfers", "folder-download-symbolic",  "Transfers" },
    { "history",   "document-open-recent-symbolic", "History" },
    { "windows",   "view-paged-symbolic",       "Windows" },
};

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

static GtkWidget *panel_header(const char *title, GtkWidget **toolbar)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "panel-header");
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 6);
    *toolbar = box;
    return box;
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
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    gtk_style_context_add_class(gtk_widget_get_style_context(tv), "panel-tree");
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

static void open_url(Panels *p, const char *url, int how)   /* 0 current, 1 new tab, 2 background */
{
    if (!url || !*url) return;
    Tab *t = browser_window_current_tab(p->bw);
    if (how == 0 && t) tab_load(t, url);
    else browser_window_open(p->bw, url, how == 2);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *c, Panels *p)
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

static gboolean on_tree_press(GtkWidget *w, GdkEventButton *ev, Panels *p)
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
/* bookmarks                                                         */

static void bm_rebuild(Panels *p)
{
    gtk_tree_store_clear(p->bm_store);
    const char *q = gtk_entry_get_text(GTK_ENTRY(p->bm_search));
    char *fq = *q ? g_utf8_casefold(q, -1) : NULL;
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
                gtk_tree_store_insert_with_values(p->bm_store, &fit, NULL, -1, COL_ICON, "folder-symbolic",
                                                  COL_TITLE, b->folder, COL_URL, NULL, COL_TIP, NULL, -1);
                parent = gtk_tree_iter_copy(&fit);
                g_hash_table_insert(folders, b->folder, parent);
            }
        }
        char *tip = g_markup_printf_escaped("<b>%s</b>\n%s", b->title, b->url);
        gtk_tree_store_insert_with_values(p->bm_store, NULL, parent, -1, COL_ICON, "text-html-symbolic",
                                          COL_TITLE, b->title, COL_URL, b->url, COL_TIP, tip, -1);
        g_free(tip);
    }
    g_hash_table_destroy(folders);
    g_free(fq);
    if (fq || profile->bookmarks->len < 40) gtk_tree_view_expand_all(GTK_TREE_VIEW(p->bm_view));
}

static void bm_changed(gpointer data) { bm_rebuild(data); }
static void bm_search_changed(GtkSearchEntry *e, Panels *p) { bm_rebuild(p); }

static void bm_add(GtkButton *b, Panels *p)
{
    Tab *t = browser_window_current_tab(p->bw);
    if (t && !tab_is_start_page(t))
        bookmark_edit_dialog(GTK_WINDOW(p->bw->window), tab_get_uri(t), tab_get_title(t), TRUE);
}

static void bm_edit(GtkButton *b, Panels *p)
{
    char *url = selected_url(p->bm_view);
    if (url && *url) {
        for (guint i = 0; i < profile->bookmarks->len; i++) {
            Bookmark *bm = profile->bookmarks->pdata[i];
            if (g_str_equal(bm->url, url)) {
                bookmark_edit_dialog(GTK_WINDOW(p->bw->window), bm->url, bm->title, FALSE);
                break;
            }
        }
    }
    g_free(url);
}

static void bm_delete(GtkButton *b, Panels *p)
{
    char *url = selected_url(p->bm_view);
    if (url && *url) bookmark_remove(url);
    g_free(url);
}

static gboolean bm_key(GtkWidget *w, GdkEventKey *ev, Panels *p)
{
    if (ev->keyval == GDK_KEY_Delete) { bm_delete(NULL, p); return TRUE; }
    if (ev->keyval == GDK_KEY_F2) { bm_edit(NULL, p); return TRUE; }
    return FALSE;
}

static GtkWidget *build_bookmarks(Panels *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), panel_header("Bookmarks", &hdr), FALSE, FALSE, 0);
    GtkWidget *add = small_button("list-add-symbolic", "Bookmark current page");
    GtkWidget *edit = small_button("document-edit-symbolic", "Properties (F2)");
    GtkWidget *del = small_button("edit-delete-symbolic", "Delete (Del)");
    g_signal_connect(add, "clicked", G_CALLBACK(bm_add), p);
    g_signal_connect(edit, "clicked", G_CALLBACK(bm_edit), p);
    g_signal_connect(del, "clicked", G_CALLBACK(bm_delete), p);
    gtk_box_pack_end(GTK_BOX(hdr), del, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), edit, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), add, FALSE, FALSE, 0);

    p->bm_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->bm_search), "Quick find");
    g_signal_connect(p->bm_search, "search-changed", G_CALLBACK(bm_search_changed), p);
    gtk_box_pack_start(GTK_BOX(box), p->bm_search, FALSE, FALSE, 0);

    p->bm_store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
    p->bm_view = tree_view(GTK_TREE_MODEL(p->bm_store), FALSE);
    g_signal_connect(p->bm_view, "row-activated", G_CALLBACK(on_row_activated), p);
    g_signal_connect(p->bm_view, "button-press-event", G_CALLBACK(on_tree_press), p);
    g_signal_connect(p->bm_view, "key-press-event", G_CALLBACK(bm_key), p);
    gtk_box_pack_start(GTK_BOX(box), scrolled(p->bm_view), TRUE, TRUE, 0);
    bm_rebuild(p);
    profile_connect(EV_BOOKMARKS, bm_changed, p);
    return box;
}

/* ---------------------------------------------------------------- */
/* history                                                           */

static void hi_rebuild(Panels *p)
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
                                          COL_URL, h->url, COL_TIP, tip, -1);
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
    Panels *p = data;
    p->hi_timer = 0;
    if (gtk_widget_get_mapped(p->hi_view)) hi_rebuild(p);
    else p->hi_dirty = TRUE;
    return G_SOURCE_REMOVE;
}

static void hi_changed(gpointer data)
{
    Panels *p = data;
    if (!p->hi_timer) p->hi_timer = g_timeout_add(800, hi_timeout, p);
}

static void hi_map(GtkWidget *w, Panels *p) { if (p->hi_dirty) hi_rebuild(p); }
static void hi_search_changed(GtkSearchEntry *e, Panels *p) { hi_rebuild(p); }

static void hi_delete(GtkButton *b, Panels *p)
{
    char *url = selected_url(p->hi_view);
    if (url && *url) history_delete(url);
    g_free(url);
}

static void hi_clear(GtkButton *b, Panels *p)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(p->bw->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, "Delete all history?");
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) history_delete(NULL);
    gtk_widget_destroy(d);
}

static gboolean hi_key(GtkWidget *w, GdkEventKey *ev, Panels *p)
{
    if (ev->keyval == GDK_KEY_Delete) { hi_delete(NULL, p); return TRUE; }
    return FALSE;
}

static GtkWidget *build_history(Panels *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), panel_header("History", &hdr), FALSE, FALSE, 0);
    GtkWidget *del = small_button("edit-delete-symbolic", "Delete selected (Del)");
    GtkWidget *clr = small_button("edit-clear-all-symbolic", "Delete all history");
    g_signal_connect(del, "clicked", G_CALLBACK(hi_delete), p);
    g_signal_connect(clr, "clicked", G_CALLBACK(hi_clear), p);
    gtk_box_pack_end(GTK_BOX(hdr), clr, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), del, FALSE, FALSE, 0);

    p->hi_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->hi_search), "Quick find");
    g_signal_connect(p->hi_search, "search-changed", G_CALLBACK(hi_search_changed), p);
    gtk_box_pack_start(GTK_BOX(box), p->hi_search, FALSE, FALSE, 0);

    p->hi_store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
    p->hi_view = tree_view(GTK_TREE_MODEL(p->hi_store), FALSE);
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
/* transfers                                                         */

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
    Panels *p = data;
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

static void tr_row_activated(GtkListBox *lb, GtkListBoxRow *row, Panels *p)
{
    Transfer *t = g_object_get_data(G_OBJECT(gtk_bin_get_child(GTK_BIN(row))), "transfer");
    if (transfer_alive(t) && t->state == 1) transfer_open(t, FALSE);
}

static void tr_clear(GtkButton *b, Panels *p) { transfers_clear_finished(); }
static void tr_folder(GtkButton *b, Panels *p)
{
    char *dir = pref_get_string("download_dir");
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_free(dir);
}

static GtkWidget *build_transfers(Panels *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), panel_header("Transfers", &hdr), FALSE, FALSE, 0);
    GtkWidget *clr = small_button("edit-clear-all-symbolic", "Clean up finished transfers");
    GtkWidget *fol = small_button("folder-download-symbolic", "Open download folder");
    g_signal_connect(clr, "clicked", G_CALLBACK(tr_clear), p);
    g_signal_connect(fol, "clicked", G_CALLBACK(tr_folder), p);
    gtk_box_pack_end(GTK_BOX(hdr), clr, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), fol, FALSE, FALSE, 0);
    p->tr_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->tr_list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->tr_list), FALSE);
    GtkWidget *ph = gtk_label_new("No transfers");
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
/* notes                                                             */

static char *note_title(Note *n)
{
    const char *s = n->text;
    while (*s == '\n' || *s == ' ') s++;
    const char *nl = strchr(s, '\n');
    char *t = nl ? g_strndup(s, nl - s) : g_strdup(s);
    if (!*t) { g_free(t); t = g_strdup("(empty note)"); }
    return t;
}

static void no_rebuild(Panels *p)
{
    p->no_loading = TRUE;
    gtk_list_store_clear(p->no_store);
    GtkTreeIter sel_it;
    gboolean have_sel = FALSE;
    for (guint i = profile->notes->len; i > 0; i--) {   /* newest first */
        Note *n = profile->notes->pdata[i - 1];
        char *t = note_title(n);
        GDateTime *dt = g_date_time_new_from_unix_local(n->created);
        char *when = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        g_date_time_unref(dt);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(p->no_store, &it, -1, COL_ICON, "text-x-generic-symbolic",
                                          COL_TITLE, t, COL_URL, NULL, COL_TIP, when, COL_PTR, n, -1);
        if (n == p->no_current) { sel_it = it; have_sel = TRUE; }
        g_free(t);
        g_free(when);
    }
    if (have_sel) {
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->no_view)), &sel_it);
    } else {
        p->no_current = NULL;
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->no_text)), "", -1);
    }
    gtk_widget_set_sensitive(p->no_text, p->no_current != NULL);
    p->no_loading = FALSE;
}

static void no_changed(gpointer data)
{
    Panels *p = data;
    if (p->no_saving) return;   /* our own edit: list already updated */
    /* the current note may have been deleted elsewhere */
    gboolean alive = FALSE;
    for (guint i = 0; i < profile->notes->len; i++)
        if (profile->notes->pdata[i] == p->no_current) alive = TRUE;
    if (!alive) p->no_current = NULL;
    no_rebuild(p);
}

static void no_selection_changed(GtkTreeSelection *sel, Panels *p)
{
    if (p->no_loading) return;
    GtkTreeModel *m;
    GtkTreeIter it;
    Note *n = NULL;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) gtk_tree_model_get(m, &it, COL_PTR, &n, -1);
    p->no_current = n;
    p->no_loading = TRUE;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->no_text)), n ? n->text : "", -1);
    p->no_loading = FALSE;
    gtk_widget_set_sensitive(p->no_text, n != NULL);
}

static gboolean no_save_timeout(gpointer data)
{
    Panels *p = data;
    p->no_timer = 0;
    p->no_saving = TRUE;
    notes_save();
    p->no_saving = FALSE;
    return G_SOURCE_REMOVE;
}

static void no_text_changed(GtkTextBuffer *buf, Panels *p)
{
    if (p->no_loading || !p->no_current) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    g_free(p->no_current->text);
    p->no_current->text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    /* update the row title in place */
    GtkTreeModel *m;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->no_view)), &m, &it)) {
        char *t = note_title(p->no_current);
        gtk_list_store_set(p->no_store, &it, COL_TITLE, t, -1);
        g_free(t);
    }
    if (p->no_timer) g_source_remove(p->no_timer);
    p->no_timer = g_timeout_add(800, no_save_timeout, p);
}

void panels_note_selected(Panels *p, Note *n)
{
    p->no_current = n;
    no_rebuild(p);
    no_selection_changed(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->no_view)), p);
}

static void no_new(GtkButton *b, Panels *p)
{
    Note *n = note_add("");
    panels_note_selected(p, n);
    gtk_widget_grab_focus(p->no_text);
}

static void no_delete(GtkButton *b, Panels *p)
{
    if (!p->no_current) return;
    if (p->no_timer) { g_source_remove(p->no_timer); p->no_timer = 0; }
    g_ptr_array_remove(profile->notes, p->no_current);
    p->no_current = NULL;
    notes_save();
}

static void no_copy(GtkButton *b, Panels *p)
{
    if (!p->no_current) return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), p->no_current->text, -1);
    browser_window_flash_status(p->bw, "Note copied to clipboard");
}

static void no_open_link(GtkButton *b, Panels *p)
{
    if (!p->no_current) return;
    /* open the first URL found in the note */
    GRegex *re = g_regex_new("(https?|ftp|file)://[^\\s<>\"]+", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    if (g_regex_match(re, p->no_current->text, 0, &mi)) {
        char *u = g_match_info_fetch(mi, 0);
        browser_window_open(p->bw, u, FALSE);
        g_free(u);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
}

static GtkWidget *build_notes(Panels *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), panel_header("Notes", &hdr), FALSE, FALSE, 0);
    GtkWidget *nw = small_button("document-new-symbolic", "New note");
    GtkWidget *cp = small_button("edit-copy-symbolic", "Copy note");
    GtkWidget *op = small_button("web-browser-symbolic", "Open link in note");
    GtkWidget *de = small_button("edit-delete-symbolic", "Delete note");
    g_signal_connect(nw, "clicked", G_CALLBACK(no_new), p);
    g_signal_connect(cp, "clicked", G_CALLBACK(no_copy), p);
    g_signal_connect(op, "clicked", G_CALLBACK(no_open_link), p);
    g_signal_connect(de, "clicked", G_CALLBACK(no_delete), p);
    gtk_box_pack_end(GTK_BOX(hdr), de, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), op, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), cp, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), nw, FALSE, FALSE, 0);

    GtkWidget *vp = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    p->no_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
    p->no_view = tree_view(GTK_TREE_MODEL(p->no_store), FALSE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->no_view)), "changed",
                     G_CALLBACK(no_selection_changed), p);
    GtkWidget *sw1 = scrolled(p->no_view);
    gtk_widget_set_size_request(sw1, -1, 140);
    gtk_paned_pack1(GTK_PANED(vp), sw1, TRUE, FALSE);
    p->no_text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->no_text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(p->no_text), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(p->no_text), 4);
    g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->no_text)), "changed", G_CALLBACK(no_text_changed), p);
    GtkWidget *sw2 = scrolled(p->no_text);
    gtk_widget_set_size_request(sw2, -1, 120);
    gtk_paned_pack2(GTK_PANED(vp), sw2, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(box), vp, TRUE, TRUE, 0);
    no_rebuild(p);
    profile_connect(EV_NOTES, no_changed, p);
    return box;
}

/* ---------------------------------------------------------------- */
/* windows / tabs                                                    */

void panels_refresh_tabs(Panels *p)
{
    if (!p || !p->wi_view || !gtk_widget_get_mapped(p->wi_view)) return;
    gtk_tree_store_clear(p->wi_store);
    int wn = 1;
    GList *ws = g_list_reverse(g_list_copy(browser_windows()));
    for (GList *w = ws; w; w = w->next, wn++) {
        BrowserWindow *bw = w->data;
        GtkTreeIter wit;
        char *wt = g_strdup_printf("Window %d%s", wn, bw == p->bw ? " (this)" : "");
        gtk_tree_store_insert_with_values(p->wi_store, &wit, NULL, -1, COL_ICON, "window-new-symbolic",
                                          COL_TITLE, wt, COL_PTR, bw, -1);
        g_free(wt);
        GList *tabs = browser_window_tabs_in_order(bw);
        Tab *cur = browser_window_current_tab(bw);
        for (GList *t = tabs; t; t = t->next) {
            Tab *tab = t->data;
            gtk_tree_store_insert_with_values(p->wi_store, NULL, &wit, -1,
                COL_ICON, tab == cur ? "go-next-symbolic" : "text-html-symbolic",
                COL_TITLE, tab_get_title(tab), COL_TIP, tab_get_uri(tab), COL_PTR, tab, -1);
        }
        g_list_free(tabs);
    }
    g_list_free(ws);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(p->wi_view));
}

static void wi_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *c, Panels *p)
{
    GtkTreeIter it, parent;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    if (!gtk_tree_model_get_iter(m, &it, path)) return;
    if (!gtk_tree_model_iter_parent(m, &parent, &it)) return;
    gpointer tab = NULL, win = NULL;
    gtk_tree_model_get(m, &it, COL_PTR, &tab, -1);
    gtk_tree_model_get(m, &parent, COL_PTR, &win, -1);
    if (!g_list_find(browser_windows(), win)) return;
    BrowserWindow *bw = win;
    if (!g_list_find(bw->tabs, tab)) return;
    browser_window_select_tab(bw, tab);
    gtk_window_present(GTK_WINDOW(bw->window));
}

static void wi_map(GtkWidget *w, Panels *p) { panels_refresh_tabs(p); }

static GtkWidget *build_windows(Panels *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hdr;
    gtk_box_pack_start(GTK_BOX(box), panel_header("Windows", &hdr), FALSE, FALSE, 0);
    p->wi_store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
    p->wi_view = tree_view(GTK_TREE_MODEL(p->wi_store), FALSE);
    g_signal_connect(p->wi_view, "row-activated", G_CALLBACK(wi_activated), p);
    g_signal_connect(p->wi_view, "map", G_CALLBACK(wi_map), p);
    gtk_box_pack_start(GTK_BOX(box), scrolled(p->wi_view), TRUE, TRUE, 0);
    return box;
}

/* ---------------------------------------------------------------- */

static void selector_clicked(GtkButton *b, Panels *p)
{
    browser_window_show_panel(p->bw, g_object_get_data(G_OBJECT(b), "panel"), TRUE);
}

static void on_stack_child(GObject *o, GParamSpec *ps, Panels *p)
{
    const char *cur = gtk_stack_get_visible_child_name(GTK_STACK(p->bw->panel_stack));
    gboolean shown = gtk_widget_get_visible(p->bw->panel_stack);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, p->buttons);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GtkStyleContext *sc = gtk_widget_get_style_context(v);
        if (shown && g_strcmp0(cur, k) == 0) gtk_style_context_add_class(sc, "selected");
        else gtk_style_context_remove_class(sc, "selected");
    }
}

GtkWidget *panels_selector_button(Panels *p, const char *name)
{
    return g_hash_table_lookup(p->buttons, name);
}

void panels_focus_search(Panels *p, const char *name)
{
    if (g_str_equal(name, "bookmarks")) gtk_widget_grab_focus(p->bm_search);
    else if (g_str_equal(name, "history")) gtk_widget_grab_focus(p->hi_search);
    on_stack_child(NULL, NULL, p);
}

Panels *panels_new(BrowserWindow *bw)
{
    Panels *p = g_new0(Panels, 1);
    p->bw = bw;
    p->buttons = g_hash_table_new(g_str_hash, g_str_equal);

    bw->panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(bw->panel_box, "panel");
    bw->panel_selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_name(bw->panel_selector, "panel-selector");
    bw->panel_stack = gtk_stack_new();
    gtk_widget_set_name(bw->panel_stack, "panel-content");
    gtk_stack_set_transition_type(GTK_STACK(bw->panel_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_size_request(bw->panel_stack, 160, -1);

    for (guint i = 0; i < G_N_ELEMENTS(panel_defs); i++) {
        GtkWidget *b = gtk_button_new_from_icon_name(panel_defs[i].icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(b, panel_defs[i].title);
        gtk_widget_set_focus_on_click(b, FALSE);
        g_object_set_data(G_OBJECT(b), "panel", (gpointer)panel_defs[i].name);
        g_signal_connect(b, "clicked", G_CALLBACK(selector_clicked), p);
        gtk_box_pack_start(GTK_BOX(bw->panel_selector), b, FALSE, FALSE, 0);
        g_hash_table_insert(p->buttons, (gpointer)panel_defs[i].name, b);
    }

    GtkWidget *pages[] = { build_bookmarks(p), build_notes(p), build_transfers(p), build_history(p), build_windows(p) };
    for (guint i = 0; i < G_N_ELEMENTS(pages); i++) {
        g_object_set(pages[i], "margin", 4, NULL);
        gtk_stack_add_named(GTK_STACK(bw->panel_stack), pages[i], panel_defs[i].name);
    }
    g_signal_connect(bw->panel_stack, "notify::visible-child", G_CALLBACK(on_stack_child), p);
    g_signal_connect_swapped(bw->panel_stack, "show", G_CALLBACK(on_stack_child), p);
    g_signal_connect_swapped(bw->panel_stack, "hide", G_CALLBACK(on_stack_child), p);

    bw->panel_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(bw->panel_box), bw->panel_selector, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->panel_box), bw->panel_sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bw->panel_box), bw->panel_stack, TRUE, TRUE, 0);
    gtk_widget_show_all(bw->panel_box);
    return p;
}

void panels_free(Panels *p)
{
    if (!p) return;
    profile_disconnect_data(p);
    if (p->hi_timer) g_source_remove(p->hi_timer);
    if (p->no_timer) {
        g_source_remove(p->no_timer);
        notes_save();
    }
    g_hash_table_destroy(p->buttons);
    g_object_unref(p->bm_store);
    g_object_unref(p->hi_store);
    g_object_unref(p->no_store);
    g_object_unref(p->wi_store);
    g_free(p);
}
