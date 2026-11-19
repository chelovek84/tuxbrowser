/* Web notifications: kept in the browser's own list (the bell popover and the tray icon)
 * instead of the desktop's notification bubbles, and the per-site Allow/Deny answers. */
#include "tuxbrowser.h"
#include <string.h>

#define MAX_NOTICES 50

GPtrArray *notices;   /* Notice*, newest first */

/* ---------------------------------------------------------------- */
/* saved permissions: settings "notify_allowed" / "notify_denied",   */
/* one origin ("https://mail.example.com") per line                  */

static char *origin_for_uri(const char *uri)
{
    if (!uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) return NULL;
    WebKitSecurityOrigin *o = webkit_security_origin_new_for_uri(uri);
    char *s = o ? webkit_security_origin_to_string(o) : NULL;
    if (o) webkit_security_origin_unref(o);
    return s;
}

static gboolean list_has(const char *key, const char *origin)
{
    char *v = pref_get_string(key);
    char **lines = g_strsplit(v, "\n", -1);
    gboolean found = FALSE;
    for (char **l = lines; *l && !found; l++)
        if (g_str_equal(g_strstrip(*l), origin)) found = TRUE;
    g_strfreev(lines);
    g_free(v);
    return found;
}

/* Puts origin on one list (or neither, when list is NULL) and takes it off the other. */
static void list_set(const char *origin, const char *list)
{
    static const char *keys[] = { "notify_allowed", "notify_denied" };
    for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
        char *v = pref_get_string(keys[k]);
        char **lines = g_strsplit(v, "\n", -1);
        GString *o = g_string_new(NULL);
        for (char **l = lines; *l; l++) {
            char *s = g_strstrip(*l);
            if (!*s || g_str_equal(s, origin)) continue;
            g_string_append_printf(o, "%s%s", o->len ? "\n" : "", s);
        }
        if (list && g_str_equal(list, keys[k])) g_string_append_printf(o, "%s%s", o->len ? "\n" : "", origin);
        pref_set_string(keys[k], o->str);
        g_string_free(o, TRUE);
        g_strfreev(lines);
        g_free(v);
    }
}

static GList *origins_from(const char *key)
{
    GList *out = NULL;
    char *v = pref_get_string(key);
    char **lines = g_strsplit(v, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *s = g_strstrip(*l);
        WebKitSecurityOrigin *o = *s ? webkit_security_origin_new_for_uri(s) : NULL;
        if (o) out = g_list_prepend(out, o);
    }
    g_strfreev(lines);
    g_free(v);
    return out;
}

/* Tells WebKit the saved answers, so Notification.permission is right as pages load. */
void notifications_apply_permissions(void)
{
    GList *allowed = origins_from("notify_allowed"), *denied = origins_from("notify_denied");
    webkit_web_context_initialize_notification_permissions(web_context(), allowed, denied);
    g_list_free_full(allowed, (GDestroyNotify)webkit_security_origin_unref);
    g_list_free_full(denied, (GDestroyNotify)webkit_security_origin_unref);
}

/* origins, one per line, from what was typed: "mail.example.com" means https:// */
static char *normalize_sites(const char *text)
{
    GString *o = g_string_new(NULL);
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    for (char **l = lines; *l; l++) {
        char *t = g_strstrip(*l);
        if (!*t) continue;
        char *u = strstr(t, "://") ? g_strdup(t) : g_strconcat("https://", t, NULL);
        char *origin = origin_for_uri(u);
        char *have = g_strconcat("\n", o->str, "\n", NULL), *needle = g_strconcat("\n", origin, "\n", NULL);
        if (origin && !strstr(have, needle)) g_string_append_printf(o, "%s%s", o->len ? "\n" : "", origin);
        g_free(have);
        g_free(needle);
        g_free(origin);
        g_free(u);
    }
    g_strfreev(lines);
    return g_string_free(o, FALSE);
}

/* From Preferences; a site on both lists stays allowed. */
void notifications_set_sites(const char *allowed, const char *denied)
{
    char *a = normalize_sites(allowed), *d = normalize_sites(denied);
    pref_set_string("notify_allowed", a);
    pref_set_string("notify_denied", d);
    char **al = g_strsplit(a, "\n", -1);
    for (char **l = al; *l; l++)
        if (**l && list_has("notify_denied", *l)) list_set(*l, "notify_allowed");
    g_strfreev(al);
    g_free(a);
    g_free(d);
    notifications_apply_permissions();
}

static void on_init_permissions(WebKitWebContext *ctx, gpointer data) { notifications_apply_permissions(); }

int notifications_saved_permission(const char *uri)
{
    char *o = origin_for_uri(uri);
    int r = !o ? -1 : list_has("notify_allowed", o) ? 1 : list_has("notify_denied", o) ? 0 : -1;
    g_free(o);
    return r;
}

void notifications_remember(const char *uri, gboolean allow)
{
    char *o = origin_for_uri(uri);
    if (!o) return;
    list_set(o, allow ? "notify_allowed" : "notify_denied");
    g_free(o);
    prefs_save();
    notifications_apply_permissions();
}

/* ---------------------------------------------------------------- */
/* the list                                                          */

static void notice_closed(WebKitNotification *n, Notice *x);

static void notice_free(Notice *x)
{
    if (x->n) {
        g_signal_handlers_disconnect_by_func(x->n, notice_closed, x);
        g_object_unref(x->n);
    }
    if (x->view) g_object_remove_weak_pointer(G_OBJECT(x->view), (gpointer *)&x->view);
    g_clear_object(&x->icon);
    g_free(x->uri); g_free(x->host); g_free(x->title); g_free(x->body); g_free(x->tag);
    g_free(x);
}

/* The page closed it. Sites like Teams close their own toasts after a few seconds, so
 * this is no sign it was read: the entry stays in the list (and unread) until you look
 * at the tab or dismiss it; only the page's object, now dead, is let go. A replacement
 * with the same tag still takes its place in on_show_notification(). */
static void notice_closed(WebKitNotification *n, Notice *x)
{
    g_signal_handlers_disconnect_by_func(x->n, notice_closed, x);
    g_clear_object(&x->n);
}

static Tab *tab_for_view(WebKitWebView *view)
{
    if (!view) return NULL;
    for (GList *w = browser_windows(); w; w = w->next)
        for (GList *t = ((BrowserWindow *)w->data)->tabs; t; t = t->next)
            if (((Tab *)t->data)->view == view) return t->data;
    return NULL;
}

static gboolean on_show_notification(WebKitWebView *view, WebKitNotification *n, gpointer data)
{
    const char *uri = webkit_web_view_get_uri(view);
    const char *tag = webkit_notification_get_tag(n);
    char *host = adblock_site_for_uri(uri);
    if (!host) host = g_strdup(uri && *uri ? uri : "Web page");

    /* a newer message with the same tag takes the old one's place */
    if (tag && *tag)
        for (guint i = notices->len; i > 0; i--) {
            Notice *o = notices->pdata[i - 1];
            if (o->tag && g_str_equal(o->tag, tag) && g_str_equal(o->host, host))
                g_ptr_array_remove_index(notices, i - 1);
        }

    Notice *x = g_new0(Notice, 1);
    x->n = g_object_ref(n);
    x->view = view;
    g_object_add_weak_pointer(G_OBJECT(view), (gpointer *)&x->view);
    x->uri = g_strdup(uri ? uri : "");
    x->host = host;
    x->title = g_strdup(webkit_notification_get_title(n));
    x->body = g_strdup(webkit_notification_get_body(n));
    x->tag = g_strdup(tag);
    x->icon = favicon_for_view(view, 16);
    x->when = g_get_real_time();
    /* news only if it comes from a tab you are not looking at */
    Tab *t = tab_for_view(view);
    x->unread = !t || browser_window_current_tab(t->win) != t || !gtk_window_is_active(GTK_WINDOW(t->win->window));
    g_signal_connect(n, "closed", G_CALLBACK(notice_closed), x);
    g_ptr_array_insert(notices, 0, x);
    while (notices->len > MAX_NOTICES) g_ptr_array_remove_index(notices, notices->len - 1);
    profile_emit(EV_NOTIFICATIONS);

    /* TRUE keeps WebKit from also showing a desktop notification */
    return !pref_get_bool("notify_desktop");
}

void notifications_attach(WebKitWebView *view)
{
    g_signal_connect(view, "show-notification", G_CALLBACK(on_show_notification), NULL);
}

void notifications_init(WebKitWebContext *ctx)
{
    notices = g_ptr_array_new_with_free_func((GDestroyNotify)notice_free);
    g_signal_connect(ctx, "initialize-notification-permissions", G_CALLBACK(on_init_permissions), NULL);
}

gboolean notice_alive(Notice *x)
{
    for (guint i = 0; i < notices->len; i++)
        if (notices->pdata[i] == x) return TRUE;
    return FALSE;
}

guint notices_unread(void)
{
    guint n = 0;
    for (guint i = 0; i < notices->len; i++)
        if (((Notice *)notices->pdata[i])->unread) n++;
    return n;
}

void notices_mark_read(void)
{
    gboolean changed = FALSE;
    for (guint i = 0; i < notices->len; i++) {
        Notice *x = notices->pdata[i];
        if (x->unread) {
            x->unread = FALSE;
            changed = TRUE;
        }
    }
    if (changed) profile_emit(EV_NOTIFICATIONS);
}

/* The tab was looked at: what it said is no longer news. */
void notices_seen(WebKitWebView *view)
{
    if (!view || !notices) return;
    gboolean changed = FALSE;
    for (guint i = 0; i < notices->len; i++) {
        Notice *x = notices->pdata[i];
        if (x->view == view && x->unread) {
            x->unread = FALSE;
            changed = TRUE;
        }
    }
    if (changed) profile_emit(EV_NOTIFICATIONS);
}

/* Removes it from the list and tells the page it was closed. */
void notice_dismiss(Notice *x)
{
    if (!notice_alive(x)) return;
    if (x->n) {
        g_signal_handlers_disconnect_by_func(x->n, notice_closed, x);
        webkit_notification_close(x->n);
    }
    g_ptr_array_remove(notices, x);
    profile_emit(EV_NOTIFICATIONS);
}

void notices_clear(void)
{
    while (notices->len) {
        Notice *x = notices->pdata[0];
        if (x->n) {
            g_signal_handlers_disconnect_by_func(x->n, notice_closed, x);
            webkit_notification_close(x->n);
        }
        g_ptr_array_remove_index(notices, 0);
    }
    profile_emit(EV_NOTIFICATIONS);
}

/* Shows the tab it came from and lets the page react to the click (open the chat, the
 * mail...). If that tab is gone, the page is opened again in a new tab. */
void notice_activate(Notice *x, BrowserWindow *fallback)
{
    if (!notice_alive(x)) return;
    Tab *tab = tab_for_view(x->view);
    if (tab) {
        browser_window_select_tab(tab->win, tab);
        browser_window_show(tab->win);
        if (x->n) webkit_notification_clicked(x->n);
    } else {
        BrowserWindow *bw = fallback ? fallback : browser_window_get_active();
        if (bw && *x->uri) {
            browser_window_open(bw, x->uri, FALSE);
            browser_window_show(bw);
        }
    }
    notice_dismiss(x);
}
