/* A browser tab: web view, tab label, mouse gestures, context menu. */
#include "tuxbrowser.h"
#include <string.h>
#include <math.h>

#define GESTURE_STEP 24

static gboolean is_current(Tab *tab)
{
    return browser_window_current_tab(tab->win) == tab;
}

const char *tab_get_uri(Tab *tab)
{
    /* while a spare is being redirected to a link, report the link, not the start page */
    if (tab->replacing && tab->requested) return tab->requested;
    const char *u = webkit_web_view_get_uri(tab->view);
    if (u && *u) return u;
    return tab->requested ? tab->requested : "";
}

gboolean tab_is_start_page(Tab *tab)
{
    const char *u = tab_get_uri(tab);
    /* gbrowser://...: saved sessions from before the rename */
    return g_str_has_prefix(u, START_URI) || g_str_has_prefix(u, "gbrowser:");
}

const char *tab_get_title(Tab *tab)
{
    if (tab->replacing) return tab_get_uri(tab);
    if (tab_is_start_page(tab)) return "New tab";
    const char *t = webkit_web_view_get_title(tab->view);
    if (t && *t) return t;
    const char *u = tab_get_uri(tab);
    return *u ? u : "New tab";
}

void tab_load(Tab *tab, const char *uri)
{
    g_clear_pointer(&tab->typed, g_free);
    if (!g_str_has_prefix(uri, "javascript:")) {
        g_free(tab->requested);
        tab->requested = g_strdup(uri);
    }
    if (g_str_has_prefix(uri, "javascript:")) {
        char *code = g_uri_unescape_string(uri + 11, NULL);
        if (code) webkit_web_view_evaluate_javascript(tab->view, code, -1, NULL, NULL, NULL, NULL, NULL);
        g_free(code);
        return;
    }
    webkit_web_view_load_uri(tab->view, uri);
}

/* Navigate away from the start page (e.g. an adopted spare) without leaving it behind in
 * the tab's history: replace the current entry instead of adding one. */
void tab_load_replace(Tab *tab, const char *uri)
{
    if (g_str_has_prefix(uri, "javascript:") || webkit_web_view_is_loading(tab->view)) {
        tab_load(tab, uri);
        return;
    }
    g_clear_pointer(&tab->typed, g_free);
    g_free(tab->requested);
    tab->requested = g_strdup(uri);
    tab->replacing = TRUE;
    GString *js = g_string_new("location.replace(\"");
    for (const char *p = uri; *p; p++) {
        if ((guchar)*p < 0x20) continue;                     /* no control characters */
        if (*p == '\\' || *p == '"') g_string_append_c(js, '\\');
        g_string_append_c(js, *p);
    }
    g_string_append(js, "\")");
    webkit_web_view_evaluate_javascript(tab->view, js->str, -1, NULL, NULL, NULL, NULL, NULL);
    g_string_free(js, TRUE);
}

/* ---------------------------------------------------------------- */
/* label                                                             */

static void update_label(Tab *tab)
{
    const char *title = tab_get_title(tab);
    gtk_label_set_text(GTK_LABEL(tab->label), title);
    gtk_widget_set_tooltip_text(tab->label_box, title);
}

static void update_icon(Tab *tab)
{
    if (webkit_web_view_is_loading(tab->view) && !tab_is_start_page(tab)) {
        gtk_widget_hide(tab->icon);
        gtk_widget_show(tab->spinner);
        gtk_spinner_start(GTK_SPINNER(tab->spinner));
        return;
    }
    gtk_spinner_stop(GTK_SPINNER(tab->spinner));
    gtk_widget_hide(tab->spinner);
    gtk_widget_show(tab->icon);
    GdkPixbuf *pb = tab_is_start_page(tab) ? NULL : favicon_for_view(tab->view, 16);
    if (pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(tab->icon), pb);
        g_object_unref(pb);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(tab->icon),
            tab_is_start_page(tab) ? "emblem-documents-symbolic" : "text-html-symbolic", GTK_ICON_SIZE_MENU);
    }
}

static void close_clicked(GtkButton *b, Tab *tab)
{
    browser_window_close_tab(tab->win, tab);
}

static gboolean label_pressed(GtkWidget *w, GdkEventButton *ev, Tab *tab)
{
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
        browser_window_close_tab(tab->win, tab);
        return TRUE;
    }
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        browser_window_tab_context_menu(tab->win, tab, ev);
        return TRUE;
    }
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1)
        return TRUE;    /* don't let double click reach the tab bar (it opens a tab there) */
    return FALSE;
}

static void build_label(Tab *tab)
{
    tab->label_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(tab->label_box), FALSE);
    gtk_widget_add_events(tab->label_box, GDK_BUTTON_PRESS_MASK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_add(GTK_CONTAINER(tab->label_box), box);

    tab->icon = gtk_image_new_from_icon_name("text-html-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(tab->icon), 16);
    tab->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(tab->spinner, 16, 16);
    gtk_widget_set_no_show_all(tab->spinner, TRUE);

    tab->label = gtk_label_new("New tab");
    gtk_label_set_ellipsize(GTK_LABEL(tab->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(tab->label), 17);
    gtk_label_set_max_width_chars(GTK_LABEL(tab->label), 17);
    gtk_label_set_xalign(GTK_LABEL(tab->label), 0);
    gtk_widget_set_hexpand(tab->label, TRUE);

    tab->close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(tab->close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(tab->close_btn, FALSE);
    gtk_widget_set_tooltip_text(tab->close_btn, "Close tab");
    gtk_style_context_add_class(gtk_widget_get_style_context(tab->close_btn), "tab-close");
    g_signal_connect(tab->close_btn, "clicked", G_CALLBACK(close_clicked), tab);

    gtk_box_pack_start(GTK_BOX(box), tab->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), tab->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), tab->label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), tab->close_btn, FALSE, FALSE, 0);
    g_signal_connect(tab->label_box, "button-press-event", G_CALLBACK(label_pressed), tab);
    gtk_widget_show_all(tab->label_box);
}

/* ---------------------------------------------------------------- */
/* web view signals                                                  */

static void on_title(WebKitWebView *v, GParamSpec *p, Tab *tab)
{
    update_label(tab);
    history_set_title(tab_get_uri(tab), webkit_web_view_get_title(v));
    browser_window_tab_changed(tab->win, tab);
}

static void on_uri(WebKitWebView *v, GParamSpec *p, Tab *tab)
{
    update_label(tab);
    browser_window_tab_changed(tab->win, tab);
}

static void on_progress(WebKitWebView *v, GParamSpec *p, Tab *tab)
{
    tab->progress = webkit_web_view_get_estimated_load_progress(v);
    browser_window_tab_changed(tab->win, tab);
}

static void on_loading(WebKitWebView *v, GParamSpec *p, Tab *tab)
{
    update_icon(tab);
    browser_window_tab_changed(tab->win, tab);
}

static void on_favicon(WebKitWebView *v, GParamSpec *p, Tab *tab)
{
    update_icon(tab);
    browser_window_tab_icon_changed(tab->win, tab);
}

static void on_zoom(WebKitWebView *v, GParamSpec *p, Tab *tab)
{
    browser_window_tab_changed(tab->win, tab);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, Tab *tab)
{
    if (ev == WEBKIT_LOAD_COMMITTED && tab->replacing && !g_str_has_prefix(webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : "", START_URI))
        tab->replacing = FALSE;     /* the link has taken over from the start page */
    if (ev == WEBKIT_LOAD_FINISHED)
        tab->replacing = FALSE;     /* e.g. the link was a download and the page stayed */
    if (ev == WEBKIT_LOAD_COMMITTED)
        history_add(tab_get_uri(tab), webkit_web_view_get_title(v));
    update_icon(tab);
    browser_window_tab_changed(tab->win, tab);
}

static gboolean on_tls_error(WebKitWebView *v, char *uri, GTlsCertificate *cert,
                             GTlsCertificateFlags errors, Tab *tab)
{
    GUri *gu = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    const char *host = gu ? g_uri_get_host(gu) : uri;
    GString *why = g_string_new(NULL);
    if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)    g_string_append(why, "\n• The issuer is not trusted");
    if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)  g_string_append(why, "\n• The certificate does not match the site name");
    if (errors & G_TLS_CERTIFICATE_NOT_ACTIVATED) g_string_append(why, "\n• The certificate is not yet valid");
    if (errors & G_TLS_CERTIFICATE_EXPIRED)       g_string_append(why, "\n• The certificate has expired");
    if (errors & G_TLS_CERTIFICATE_REVOKED)       g_string_append(why, "\n• The certificate has been revoked");
    if (errors & G_TLS_CERTIFICATE_INSECURE)      g_string_append(why, "\n• The certificate uses an insecure algorithm");

    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(tab->win->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Certificate problem with %s", host);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "The server's security certificate could not be verified:%s\n\n"
        "Someone could be impersonating this site. Continue only if you know what you are doing.", why->str);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, "_Accept certificate", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_string_free(why, TRUE);
    if (r == GTK_RESPONSE_ACCEPT && gu) {
        webkit_web_context_allow_tls_certificate_for_host(web_context(), cert, host);
        webkit_web_view_load_uri(v, uri);
    }
    if (gu) g_uri_unref(gu);
    return r == GTK_RESPONSE_ACCEPT;
}

static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d,
                                 WebKitPolicyDecisionType type, Tab *tab)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(d));
        guint button = webkit_navigation_action_get_mouse_button(a);
        guint mods = webkit_navigation_action_get_modifiers(a);
        if (webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
            && (button == 2 || (button == 1 && (mods & GDK_CONTROL_MASK)))) {
            const char *u = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
            gboolean bg = !(mods & GDK_SHIFT_MASK);
            browser_window_open(tab->win, u, bg);
            webkit_policy_decision_ignore(d);
            return TRUE;
        }
        /* web content may not navigate to our internal scheme */
        const char *u = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
        if (g_str_has_prefix(u, "tuxbrowser:") && !g_str_has_prefix(tab_get_uri(tab), "tuxbrowser:")
            && webkit_navigation_action_get_navigation_type(a) != WEBKIT_NAVIGATION_TYPE_OTHER
            && webkit_navigation_action_get_navigation_type(a) != WEBKIT_NAVIGATION_TYPE_BACK_FORWARD
            && webkit_navigation_action_get_navigation_type(a) != WEBKIT_NAVIGATION_TYPE_RELOAD) {
            webkit_policy_decision_ignore(d);
            return TRUE;
        }
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *rd = WEBKIT_RESPONSE_POLICY_DECISION(d);
        if (webkit_response_policy_decision_is_main_frame_main_resource(rd)
            && !webkit_response_policy_decision_is_mime_type_supported(rd)) {
            webkit_policy_decision_download(d);
            return TRUE;
        }
    }
    return FALSE;
}

static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, Tab *tab)
{
    Tab *nt = browser_window_add_tab(tab->win, v, FALSE);
    return GTK_WIDGET(nt->view);
}

static void on_close(WebKitWebView *v, Tab *tab)
{
    browser_window_close_tab(tab->win, tab);
}

static void on_mouse_target(WebKitWebView *v, WebKitHitTestResult *hit, guint mods, Tab *tab)
{
    g_clear_pointer(&tab->hover_uri, g_free);
    if (webkit_hit_test_result_context_is_link(hit))
        tab->hover_uri = g_strdup(webkit_hit_test_result_get_link_uri(hit));
    if (is_current(tab))
        browser_window_set_status(tab->win, tab->hover_uri);
}

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *req, Tab *tab)
{
    const char *what = "use a special feature";
    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req))       what = "know your location";
    else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req)) what = "show desktop notifications";
    else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req)) {
        WebKitUserMediaPermissionRequest *um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
        gboolean a = webkit_user_media_permission_is_for_audio_device(um);
        gboolean vi = webkit_user_media_permission_is_for_video_device(um);
        what = a && vi ? "use your camera and microphone" : vi ? "use your camera" : "use your microphone";
    } else if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST(req)) what = "lock the mouse pointer";
    else if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(req))     what = "read the clipboard";
    else if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req))   what = "list your media devices";
    else if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(req)) what = "play protected (DRM) media";
    else if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(req)) what = "access its cookies in this context";

    GUri *gu = g_uri_parse(tab_get_uri(tab), G_URI_FLAGS_NONE, NULL);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(tab->win->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s wants to %s",
        gu && g_uri_get_host(gu) ? g_uri_get_host(gu) : "This page", what);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Deny", GTK_RESPONSE_REJECT, "_Allow", GTK_RESPONSE_ACCEPT, NULL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (gu) g_uri_unref(gu);
    if (r == GTK_RESPONSE_ACCEPT) webkit_permission_request_allow(req);
    else webkit_permission_request_deny(req);
    return TRUE;
}

static gboolean on_enter_fullscreen(WebKitWebView *v, Tab *tab)
{
    browser_window_fullscreen(tab->win, TRUE);
    return FALSE;
}

static gboolean on_leave_fullscreen(WebKitWebView *v, Tab *tab)
{
    browser_window_fullscreen(tab->win, FALSE);
    return FALSE;
}

static void on_crashed(WebKitWebView *v, WebKitWebProcessTerminationReason r, Tab *tab)
{
    if (r == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) return;
    char *html = g_strdup_printf(
        "<html><body style='font:14px sans-serif;background:#eef1f4;color:#333;padding:40px'>"
        "<h2>This page has crashed</h2><p>%s</p><p>Press F5 to reload.</p></body></html>",
        r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "It used too much memory." : "The web process terminated unexpectedly.");
    webkit_web_view_load_alternate_html(v, html, tab_get_uri(tab), NULL);
    g_free(html);
}

static void on_script_message(WebKitUserContentManager *m, WebKitJavascriptResult *res, Tab *tab)
{
    if (!tab_is_start_page(tab)) return;   /* only our own page may talk to us */
    JSCValue *val = webkit_javascript_result_get_js_value(res);
    if (!jsc_value_is_string(val)) return;
    char *s = jsc_value_to_string(val);
    start_page_handle_message(tab, s);
    g_free(s);
}

/* ---------------------------------------------------------------- */
/* context menu                                                      */

static void add_action_item(WebKitContextMenu *menu, int pos, BrowserWindow *bw,
                            const char *action, const char *label, GVariant *target)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(bw->window), action);
    if (!a) return;
    WebKitContextMenuItem *it = webkit_context_menu_item_new_from_gaction(a, label, target);
    if (pos < 0) webkit_context_menu_append(menu, it);
    else webkit_context_menu_insert(menu, it, pos);
}

static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *menu, GdkEvent *ev,
                                WebKitHitTestResult *hit, Tab *tab)
{
    BrowserWindow *bw = tab->win;
    guint ctx = webkit_hit_test_result_get_context(hit);

    if (webkit_hit_test_result_context_is_link(hit)) {
        const char *link = webkit_hit_test_result_get_link_uri(hit);
        /* drop "Open Link in New Window": links open in tabs */
        GList *items = webkit_context_menu_get_items(menu);
        for (GList *l = items; l; l = l->next) {
            WebKitContextMenuItem *it = l->data;
            if (webkit_context_menu_item_get_stock_action(it) == WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW) {
                webkit_context_menu_remove(menu, it);
                break;
            }
        }
        add_action_item(menu, 1, bw, "open-tab", "Open in New _Tab", g_variant_new_string(link));
        add_action_item(menu, 2, bw, "open-background-tab", "Open in _Background Tab", g_variant_new_string(link));
        add_action_item(menu, 3, bw, "open-window", "Open in New _Window", g_variant_new_string(link));
        webkit_context_menu_insert(menu, webkit_context_menu_item_new_separator(), 4);
        add_action_item(menu, 5, bw, "bookmark-link", "Bookmark Lin_k...", g_variant_new_string(link));
    }
    if (ctx & WEBKIT_HIT_TEST_RESULT_CONTEXT_SELECTION) {
        SearchEngine *e = search_engine_default();
        webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
        if (e) {
            char *l = g_strdup_printf("_Search with %s", e->name);
            add_action_item(menu, -1, bw, "search-selection", l, NULL);
            g_free(l);
        }
        add_action_item(menu, -1, bw, "note-selection", "Copy to _Note", NULL);
    }
    if (!(ctx & (WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK | WEBKIT_HIT_TEST_RESULT_CONTEXT_IMAGE |
                 WEBKIT_HIT_TEST_RESULT_CONTEXT_MEDIA | WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE |
                 WEBKIT_HIT_TEST_RESULT_CONTEXT_SELECTION)) && !tab_is_start_page(tab)) {
        webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
        add_action_item(menu, -1, bw, "bookmark", "Add to _Bookmarks...", NULL);
        add_action_item(menu, -1, bw, "duplicate-tab", "D_uplicate Tab", NULL);
        add_action_item(menu, -1, bw, "view-source", "View _Source", NULL);
    }
    return FALSE;
}

/* ---------------------------------------------------------------- */
/* mouse gestures (right button + move), rocker scroll, ctrl+wheel   */

static void run_gesture(Tab *tab, const char *seq)
{
    BrowserWindow *bw = tab->win;
    const char *name = NULL;
    if (g_str_equal(seq, "L")) {
        name = "Back";
        webkit_web_view_go_back(tab->view);
    } else if (g_str_equal(seq, "R")) {
        name = "Forward";
        webkit_web_view_go_forward(tab->view);
    } else if (g_str_equal(seq, "U")) {
        name = "Stop";
        webkit_web_view_stop_loading(tab->view);
    } else if (g_str_equal(seq, "D")) {
        name = "New tab";
        browser_window_open(bw, START_URI, FALSE);
    } else if (g_str_equal(seq, "UD")) {
        name = "Reload";
        webkit_web_view_reload(tab->view);
    } else if (g_str_equal(seq, "DR")) {
        name = "Close tab";
        browser_window_close_tab(bw, tab);
        return;
    } else if (g_str_equal(seq, "DU")) {
        name = "Duplicate tab";
        browser_window_open(bw, tab_get_uri(tab), FALSE);
    } else if (g_str_equal(seq, "UL")) {
        name = "Parent directory";
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(bw->window), "go-up");
        if (a) g_action_activate(a, NULL);
    } else if (g_str_equal(seq, "LR") || g_str_equal(seq, "RL")) {
        name = "Reopen closed tab";
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(bw->window), "reopen-tab");
        if (a) g_action_activate(a, NULL);
    }
    char *s = g_strdup_printf("Gesture: %s", name ? name : "unknown");
    browser_window_set_status(bw, s);
    g_free(s);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, Tab *tab)
{
    if (tab->g_passthrough) return FALSE;
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    if (ev->button == 8) { webkit_web_view_go_back(tab->view); return TRUE; }
    if (ev->button == 9) { webkit_web_view_go_forward(tab->view); return TRUE; }
    if (ev->button == 3 && pref_get_bool("mouse_gestures")) {
        tab->g_down = TRUE;
        tab->g_done = FALSE;
        tab->g_x = ev->x;
        tab->g_y = ev->y;
        g_string_truncate(tab->g_seq, 0);
        g_clear_pointer(&tab->g_press, gdk_event_free);
        tab->g_press = gdk_event_copy((GdkEvent *)ev);
        return TRUE;
    }
    /* rocker gesture: hold left, click right = forward; hold right, click left = back */
    if (ev->button == 1 && tab->g_down) {
        webkit_web_view_go_back(tab->view);
        tab->g_done = TRUE;
        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, Tab *tab)
{
    if (!tab->g_down) return FALSE;
    double dx = ev->x - tab->g_x, dy = ev->y - tab->g_y;
    if (fabs(dx) < GESTURE_STEP && fabs(dy) < GESTURE_STEP) return FALSE;
    char dir = fabs(dx) > fabs(dy) ? (dx > 0 ? 'R' : 'L') : (dy > 0 ? 'D' : 'U');
    if (!tab->g_seq->len || tab->g_seq->str[tab->g_seq->len - 1] != dir)
        g_string_append_c(tab->g_seq, dir);
    tab->g_x = ev->x;
    tab->g_y = ev->y;
    return FALSE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, Tab *tab)
{
    if (tab->g_passthrough || ev->button != 3 || !tab->g_down) return FALSE;
    tab->g_down = FALSE;
    if (tab->g_seq->len) {
        char *seq = g_strdup(tab->g_seq->str);
        g_clear_pointer(&tab->g_press, gdk_event_free);
        run_gesture(tab, seq);      /* may free the tab */
        g_free(seq);
        return TRUE;
    }
    if (tab->g_done) {
        g_clear_pointer(&tab->g_press, gdk_event_free);
        return TRUE;
    }
    /* plain right click: replay it so the page / context menu sees it */
    tab->g_passthrough = TRUE;
    if (tab->g_press) gtk_widget_event(w, tab->g_press);
    GdkEvent *rel = gdk_event_copy((GdkEvent *)ev);
    gtk_widget_event(w, rel);
    gdk_event_free(rel);
    tab->g_passthrough = FALSE;
    g_clear_pointer(&tab->g_press, gdk_event_free);
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, Tab *tab)
{
    int dir = 0;
    if (ev->direction == GDK_SCROLL_UP) dir = -1;
    else if (ev->direction == GDK_SCROLL_DOWN) dir = 1;
    else if (ev->direction == GDK_SCROLL_SMOOTH && fabs(ev->delta_y) > 0.01) dir = ev->delta_y > 0 ? 1 : -1;
    if (!dir) return FALSE;

    if (tab->g_down) {               /* right button held + wheel = cycle tabs */
        tab->g_done = TRUE;
        browser_window_cycle_tab(tab->win, dir);
        return TRUE;
    }
    if (ev->state & GDK_CONTROL_MASK) {
        double z = webkit_web_view_get_zoom_level(tab->view);
        z = CLAMP(z + (dir < 0 ? 0.1 : -0.1), 0.2, 5.0);
        webkit_web_view_set_zoom_level(tab->view, z);
        return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Spare tab: launching a web content process takes ~0.6 s, so one hidden view is kept
 * with its process running and the start page already loaded. A new tab
 * adopts it instantly, then another spare is prepared in the background. */

static WebKitWebView *spare_view;
static WebKitUserContentManager *spare_ucm;
static guint spare_id;
static int spare_listener;   /* address used as the profile-listener key */

static WebKitWebView *create_view(WebKitWebView *related, WebKitUserContentManager *ucm)
{
    WebKitWebView *v;
    if (related)
        v = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related,
                                         "user-content-manager", ucm, "settings", web_settings(), NULL));
    else
        v = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", web_context(),
                                         "user-content-manager", ucm, "settings", web_settings(), NULL));
    g_object_ref_sink(v);
    webkit_web_view_set_zoom_level(v, pref_get_int("default_zoom") / 100.0);
    return v;
}

/* the start page names the default search engine, which a settings change may change */
static void spare_settings_changed(gpointer data)
{
    if (spare_view) webkit_web_view_reload(spare_view);
}

static gboolean spare_create(gpointer data)
{
    spare_id = 0;
    if (spare_view) return G_SOURCE_REMOVE;
    /* starting a web process competes with a page that is loading: wait for it (10 s max) */
    static int waited;
    if (browser_any_tab_loading() && waited < 10) {
        waited++;
        spare_id = g_timeout_add(1000, spare_create, NULL);
        return G_SOURCE_REMOVE;
    }
    waited = 0;
    static gboolean listening;
    if (!listening) {
        profile_connect(EV_SETTINGS, spare_settings_changed, &spare_listener);
        listening = TRUE;
    }
    spare_ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(spare_ucm, "start");
    spare_view = create_view(NULL, spare_ucm);
    webkit_web_view_load_uri(spare_view, START_URI);
    return G_SOURCE_REMOVE;
}

void tab_prepare_spare(guint delay_ms)
{
    if (!spare_view && !spare_id) spare_id = g_timeout_add(delay_ms, spare_create, NULL);
}

Tab *tab_new(BrowserWindow *bw, WebKitWebView *related)
{
    return tab_new_full(bw, related, FALSE);
}

Tab *tab_new_full(BrowserWindow *bw, WebKitWebView *related, gboolean want_start)
{
    Tab *tab = g_new0(Tab, 1);
    tab->win = bw;
    tab->g_seq = g_string_new(NULL);

    if (want_start && !related && spare_view) {
        /* adopt the ready spare (process running, start page loaded) */
        tab->view = spare_view;
        tab->ucm = spare_ucm;
        spare_view = NULL;
        spare_ucm = NULL;
        tab->requested = g_strdup(START_URI);
    } else {
        tab->ucm = webkit_user_content_manager_new();
        webkit_user_content_manager_register_script_message_handler(tab->ucm, "start");
        tab->view = create_view(related, tab->ucm);
    }
    g_signal_connect(tab->ucm, "script-message-received::start", G_CALLBACK(on_script_message), tab);
    tab_prepare_spare(400);   /* refill in the background */

    build_label(tab);
    tab->nb_child = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data(G_OBJECT(tab->nb_child), "tab", tab);
    gtk_widget_show(tab->nb_child);

    GtkWidget *w = GTK_WIDGET(tab->view);
    gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                             GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(w, "button-press-event", G_CALLBACK(on_button_press), tab);
    g_signal_connect(w, "button-release-event", G_CALLBACK(on_button_release), tab);
    g_signal_connect(w, "motion-notify-event", G_CALLBACK(on_motion), tab);
    g_signal_connect(w, "scroll-event", G_CALLBACK(on_scroll), tab);

    g_signal_connect(tab->view, "notify::title", G_CALLBACK(on_title), tab);
    g_signal_connect(tab->view, "notify::uri", G_CALLBACK(on_uri), tab);
    g_signal_connect(tab->view, "notify::estimated-load-progress", G_CALLBACK(on_progress), tab);
    g_signal_connect(tab->view, "notify::is-loading", G_CALLBACK(on_loading), tab);
    g_signal_connect(tab->view, "notify::favicon", G_CALLBACK(on_favicon), tab);
    g_signal_connect(tab->view, "notify::zoom-level", G_CALLBACK(on_zoom), tab);
    g_signal_connect(tab->view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), tab);
    g_signal_connect(tab->view, "decide-policy", G_CALLBACK(on_decide_policy), tab);
    g_signal_connect(tab->view, "create", G_CALLBACK(on_create), tab);
    g_signal_connect(tab->view, "close", G_CALLBACK(on_close), tab);
    g_signal_connect(tab->view, "mouse-target-changed", G_CALLBACK(on_mouse_target), tab);
    g_signal_connect(tab->view, "permission-request", G_CALLBACK(on_permission), tab);
    g_signal_connect(tab->view, "context-menu", G_CALLBACK(on_context_menu), tab);
    g_signal_connect(tab->view, "enter-fullscreen", G_CALLBACK(on_enter_fullscreen), tab);
    g_signal_connect(tab->view, "leave-fullscreen", G_CALLBACK(on_leave_fullscreen), tab);
    g_signal_connect(tab->view, "web-process-terminated", G_CALLBACK(on_crashed), tab);

    gtk_widget_show(w);
    update_label(tab);
    update_icon(tab);
    return tab;
}

void tab_free(Tab *tab)
{
    g_signal_handlers_disconnect_by_data(tab->view, tab);
    g_signal_handlers_disconnect_by_data(tab->ucm, tab);
    webkit_user_content_manager_unregister_script_message_handler(tab->ucm, "start");
    webkit_web_view_try_close(tab->view);
    g_object_unref(tab->view);
    g_object_unref(tab->ucm);
    g_clear_pointer(&tab->g_press, gdk_event_free);
    g_string_free(tab->g_seq, TRUE);
    g_free(tab->typed);
    g_free(tab->requested);
    g_free(tab->hover_uri);
    g_free(tab);
}
