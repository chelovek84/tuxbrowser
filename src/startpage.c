/* Start page: tuxbrowser://start, a plain screen with one search box.
 *
 * What is typed goes through the same handling as the address bar (addresses, search
 * keywords such as "w linux", the default engine), via a script message to Tux Browser.
 * Addresses from before (gbrowser://start, gbrowser://speeddial in saved sessions) are
 * turned into this page when a tab opens them. */
#include "tuxbrowser.h"
#include <string.h>

static const char *PAGE_CSS =
    ":root { color-scheme: light dark; --bg: #f4f6f8; --fg: #1d2733; --box: #fff; --line: #b9c3cd;"
    "  --focus: #4f7fc8; --muted: #6c7884; }"
    "@media (prefers-color-scheme: dark) { :root { --bg: #1e2227; --fg: #e3e7eb; --box: #2a2f36;"
    "  --line: #49525c; --focus: #7aa2e0; --muted: #9aa4ae; } }"
    "html, body { height: 100%; margin: 0; }"
    "body { background: var(--bg); color: var(--fg); font: 16px system-ui, sans-serif;"
    "  display: flex; align-items: center; justify-content: center; }"
    "main { width: min(640px, 100% - 32px); margin-top: -12vh; text-align: center; }"
    "h1 { font-weight: 300; font-size: 34px; letter-spacing: .02em; margin: 0 0 24px; color: var(--muted); }"
    "form { display: flex; }"
    "input { flex: 1; font: inherit; font-size: 20px; padding: 12px 20px; color: var(--fg);"
    "  background: var(--box); border: 1px solid var(--line); border-radius: 26px; outline: none;"
    "  box-shadow: 0 1px 3px rgba(0,0,0,.08); }"
    "input:focus { border-color: var(--focus); box-shadow: 0 0 0 3px color-mix(in srgb, var(--focus) 25%, transparent); }";

static const char *PAGE_JS =
    "const q = document.getElementById('q');"
    "function focusBox() { q.focus(); }"
    "document.getElementById('f').addEventListener('submit', e => {"
    "  e.preventDefault();"
    "  const v = q.value.trim();"
    "  if (v) window.webkit.messageHandlers.start.postMessage('go\\t' + v);"
    "});"
    "document.addEventListener('visibilitychange', () => { if (!document.hidden) focusBox(); });"
    "focusBox();";

static char *page_html(void)
{
    SearchEngine *e = search_engine_default();
    char *ph = g_markup_printf_escaped("Search with %s or enter address", e ? e->name : "the web");
    char *html = g_strdup_printf(
        "<!doctype html><html><head><meta charset='utf-8'><title>New Tab</title>"
        "<style>%s</style></head><body><main><h1>Tux Browser</h1>"
        "<form id='f'><input id='q' type='text' autocomplete='off' spellcheck='false' placeholder='%s'></form>"
        "</main><script>%s</script></body></html>",
        PAGE_CSS, ph, PAGE_JS);
    g_free(ph);
    return html;
}

static void scheme_cb(WebKitURISchemeRequest *req, gpointer data)
{
    const char *path = webkit_uri_scheme_request_get_path(req);
    const char *uri = webkit_uri_scheme_request_get_uri(req);
    const char *rest = g_str_has_prefix(uri, "tuxbrowser://") ? uri + strlen("tuxbrowser://") : path ? path : "";
    if (g_str_has_prefix(rest, "start") || g_str_has_prefix(rest, "speeddial")) {
        char *html = page_html();
        GBytes *b = g_bytes_new_take(html, strlen(html));
        GInputStream *st = g_memory_input_stream_new_from_bytes(b);
        webkit_uri_scheme_request_finish(req, st, g_bytes_get_size(b), "text/html");
        g_object_unref(st);
        g_bytes_unref(b);
        return;
    }
    GError *err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown page");
    webkit_uri_scheme_request_finish_error(req, err);
    g_error_free(err);
}

void start_page_init(WebKitWebContext *ctx)
{
    webkit_web_context_register_uri_scheme(ctx, "tuxbrowser", scheme_cb, NULL, NULL);
    WebKitSecurityManager *sm = webkit_web_context_get_security_manager(ctx);
    webkit_security_manager_register_uri_scheme_as_local(sm, "tuxbrowser");
    webkit_security_manager_register_uri_scheme_as_secure(sm, "tuxbrowser");
}

/* "go\t<text>" from the page's search box */
void start_page_handle_message(Tab *tab, const char *msg)
{
    if (!g_str_has_prefix(msg, "go\t")) return;
    char *uri = browser_resolve_input(msg + 3);
    if (uri) tab_load_replace(tab, uri);   /* the start page stays out of Back history */
    g_free(uri);
}

/* gives the keyboard to the page's search box */
void start_page_focus(Tab *tab)
{
    gtk_widget_grab_focus(GTK_WIDGET(tab->view));
    webkit_web_view_evaluate_javascript(tab->view, "window.focusBox && focusBox()", -1, NULL, NULL, NULL, NULL, NULL);
}
