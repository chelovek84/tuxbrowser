/* Ad blocking, in the spirit of the VIA browser:
 *   - filter lists in Adblock Plus syntax (EasyList by default), downloaded and kept up to date;
 *   - your own rules, including elements picked on a page with "Block Element...";
 *   - per-site exceptions ("Allow Ads on This Site");
 *   - one switch to turn it all off.
 *
 * The lists are converted to WebKit content blocker rules and compiled by WebKit, so requests
 * are blocked in the network layer and hidden elements never flash on screen. Compiled rules
 * are cached on disk; a start only loads them. */
#include "tuxbrowser.h"
#include <libsoup/soup.h>
#include <string.h>

#define WORLD "tuxblock"                 /* isolated script world: pages can't see or call it */
#define UPDATE_EVERY (4 * 24 * 3600)    /* re-download the lists every 4 days */

static WebKitUserContentFilterStore *store;
static WebKitUserContentFilter *f_lists, *f_user;
static GPtrArray *managers;              /* WebKitUserContentManager*, weak */
static gboolean updating;
static char *status_text;                /* shown in Preferences */

static char *lists_path(void) { return g_build_filename(profile->data_dir, "adblock-lists.txt", NULL); }
static char *rules_path(void) { return g_build_filename(profile->data_dir, "adblock-rules.txt", NULL); }

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_free(status_text);
    status_text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    profile_emit(EV_ADBLOCK);
}

static void clear_status(void)
{
    g_clear_pointer(&status_text, g_free);
    profile_emit(EV_ADBLOCK);
}

/* for Preferences: a message while something happens, else what is in use */
const char *adblock_status(void)
{
    static char *buf;
    if (status_text) return status_text;
    g_free(buf);
    gint64 t = pref_get_int("adblock_updated");
    if (!t || !f_lists) return buf = g_strdup(pref_get_bool("adblock") ? "Filter lists not loaded yet." : "");
    GDateTime *dt = g_date_time_new_from_unix_local(t);
    char *when = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    buf = g_strdup_printf("%d rules from the filter lists, updated %s.", pref_get_int("adblock_rule_count"), when);
    g_free(when);
    return buf;
}
gboolean adblock_updating(void) { return updating; }

/* ================================================================ */
/* Adblock Plus syntax -> WebKit content blocker JSON                */

static void json_str(GString *o, const char *s)
{
    g_string_append_c(o, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { g_string_append_c(o, '\\'); g_string_append_c(o, *s); }
        else if ((guchar)*s < 0x20) g_string_append_printf(o, "\\u%04x", *s);
        else g_string_append_c(o, *s);
    }
    g_string_append_c(o, '"');
}

static void json_list(GString *o, const char *key, GPtrArray *items)
{
    g_string_append_printf(o, ",\"%s\":[", key);
    for (guint i = 0; i < items->len; i++) {
        if (i) g_string_append_c(o, ',');
        json_str(o, items->pdata[i]);
    }
    g_string_append_c(o, ']');
}

static gboolean is_ascii(const char *s)
{
    for (; *s; s++) if ((guchar)*s >= 0x80) return FALSE;
    return TRUE;
}

/* "a.com|~b.com" or "a.com,~b.com" -> if-domain / unless-domain ("*a.com" also matches
 * subdomains). WebKit can't mix both in one rule; then the included ones win. */
static gboolean parse_domains(const char *list, const char *sep, GPtrArray *in, GPtrArray *out)
{
    char **ds = g_strsplit(list, sep, -1);
    for (char **d = ds; *d; d++) {
        char *x = g_strstrip(*d);
        gboolean neg = *x == '~';
        if (neg) x++;
        /* skip entity wildcards (example.*), regexes and non-ASCII names */
        if (!*x || strchr(x, '*') || strchr(x, '/') || !is_ascii(x)) continue;
        char *v = g_strconcat("*", x, NULL);
        g_ptr_array_add(neg ? out : in, g_ascii_strdown(v, -1));
        g_free(v);
    }
    g_strfreev(ds);
    return in->len || out->len;
}

/* URL pattern -> WebKit url-filter regex (subset: no alternation, no {n}) */
static gboolean pattern_to_regex(const char *pat, GString *re)
{
    if (!*pat || !is_ascii(pat)) return FALSE;
    if (pat[0] == '/' && strlen(pat) > 1 && pat[strlen(pat) - 1] == '/') return FALSE;   /* regex rules */
    char *p = g_strdup(pat);
    char *s = p;
    gboolean dom = g_str_has_prefix(s, "||"), start = !dom && *s == '|';
    if (dom) s += 2; else if (start) s++;
    gsize n = strlen(s);
    gboolean end = n && s[n - 1] == '|';
    if (end) s[--n] = 0;
    if (!dom && !start) while (*s == '*') s++;        /* unanchored: a search anyway */
    while (!end && n && s[n - 1] == '*') s[--n] = 0;
    if (strlen(s) < 3 && !dom) { g_free(p); return FALSE; }   /* too broad */
    if (dom) g_string_append(re, "^[a-z-]+://([^/:]*\\.)?");
    else if (start) g_string_append_c(re, '^');
    for (; *s; s++) {
        switch (*s) {
        case '*': g_string_append(re, ".*"); break;
        case '^': g_string_append(re, "[/:?=&]"); break;   /* separator */
        case '.': case '+': case '?': case '(': case ')': case '[': case ']':
        case '{': case '}': case '\\': case '$': case '|':
            g_string_append_c(re, '\\'); g_string_append_c(re, *s); break;
        default: g_string_append_c(re, *s);
        }
    }
    if (end) g_string_append_c(re, '$');
    g_free(p);
    return re->len > 0;
}

static const struct { const char *abp, *wk; } res_types[] = {
    { "script", "script" }, { "image", "image" }, { "stylesheet", "style-sheet" }, { "css", "style-sheet" },
    { "font", "font" }, { "media", "media" }, { "xmlhttprequest", "raw" }, { "xhr", "raw" },
    { "subdocument", "document" }, { "frame", "document" }, { "ping", "ping" }, { "websocket", "websocket" },
    { "other", "other" }, { "object", "other" }, { "popup", "popup" },
};
static const char *all_types[] = { "document", "image", "style-sheet", "script", "font", "raw", "svg-document",
                                   "media", "ping", "websocket", "other", NULL };

typedef struct {
    GString *blocks, *allows;            /* JSON objects, comma separated */
    guint n_blocks, n_allows, n_css;
    GHashTable *css_generic;             /* selector -> GPtrArray* of exception domains (or NULL) */
    GPtrArray *css_generic_order;
    GHashTable *css_specific;            /* "if|*a.com,*b.com" or "unless|..." -> GString selectors */
    GPtrArray *css_specific_order;
    GHashTable *css_except;              /* selector -> GPtrArray* domains ("a.com#@#sel") */
} Conv;

static void add_rule(GString *o, guint *n, const char *trigger, const char *action)
{
    if (o->len) g_string_append_c(o, ',');
    g_string_append_printf(o, "{\"trigger\":{%s},\"action\":{%s}}", trigger, action);
    (*n)++;
}

static void convert_network(Conv *c, const char *line)
{
    gboolean allow = g_str_has_prefix(line, "@@");
    if (allow) line += 2;
    char *pat = g_strdup(line);
    char *opts = NULL;
    char *dollar = strrchr(pat, '$');
    if (dollar && !(pat[0] == '/' && dollar < strrchr(pat, '/'))) {
        *dollar = 0;
        opts = dollar + 1;
    }
    GPtrArray *types = g_ptr_array_new(), *ntypes = g_ptr_array_new();
    GPtrArray *din = g_ptr_array_new_with_free_func(g_free), *dout = g_ptr_array_new_with_free_func(g_free);
    const char *load = NULL;
    gboolean case_sens = FALSE, skip = FALSE, doc = FALSE, child_frame = FALSE;
    if (opts) {
        char **os = g_strsplit(opts, ",", -1);
        for (char **o = os; *o && !skip; o++) {
            char *x = *o;
            gboolean neg = *x == '~';
            const char *nx = neg ? x + 1 : x;
            if (!strcmp(x, "third-party") || !strcmp(x, "3p")) load = "third-party";
            else if (!strcmp(x, "~third-party") || !strcmp(x, "first-party") || !strcmp(x, "1p")
                     || !strcmp(x, "~3p")) load = "first-party";
            else if (!strcmp(x, "match-case")) case_sens = TRUE;
            else if (g_str_has_prefix(x, "domain=")) { if (!parse_domains(x + 7, "|", din, dout)) skip = TRUE; }
            else if (!strcmp(x, "important")) ;
            else if (!strcmp(x, "document") || !strcmp(x, "doc")) doc = TRUE;
            else {
                gboolean found = FALSE;
                for (guint i = 0; i < G_N_ELEMENTS(res_types); i++)
                    if (!strcmp(nx, res_types[i].abp)) {
                        found = TRUE;
                        if (!strcmp(nx, "subdocument") || !strcmp(nx, "frame")) child_frame = !neg;
                        g_ptr_array_add(neg ? ntypes : types, (gpointer)res_types[i].wk);
                    }
                if (!found) skip = TRUE;    /* redirect=, csp=, removeparam, elemhide, ... */
            }
        }
        g_strfreev(os);
    }
    GString *re = g_string_new(NULL);
    if (!skip && !pattern_to_regex(pat, re)) {
        if (*pat || (!din->len && !dout->len)) skip = TRUE;   /* "$domain=x" alone is fine */
        else g_string_assign(re, ".*");
    }
    if (doc && !allow) skip = TRUE;     /* blocking whole pages: not ours to do */
    if (!skip) {
        GString *t = g_string_new("\"url-filter\":");
        if (doc && allow) {
            /* whole-page exception: every load on a page whose URL matches */
            json_str(t, ".*");
            GPtrArray *top = g_ptr_array_new();
            g_ptr_array_add(top, re->str);
            json_list(t, "if-top-url", top);
            g_ptr_array_free(top, TRUE);
        } else {
            json_str(t, re->str);
        }
        if (case_sens) g_string_append(t, ",\"url-filter-is-case-sensitive\":true");
        if (din->len) json_list(t, "if-domain", din);
        else if (dout->len) json_list(t, "unless-domain", dout);
        if (load) g_string_append_printf(t, ",\"load-type\":[\"%s\"]", load);
        GPtrArray *rt = g_ptr_array_new();
        if (types->len) {
            for (guint i = 0; i < types->len; i++) g_ptr_array_add(rt, types->pdata[i]);
        } else if (ntypes->len) {
            for (const char **a = all_types; *a; a++) {
                gboolean ex = FALSE;
                for (guint i = 0; i < ntypes->len; i++) if (!strcmp(*a, ntypes->pdata[i])) ex = TRUE;
                if (!ex) g_ptr_array_add(rt, (gpointer)*a);
            }
        }
        if (rt->len && !(doc && allow)) json_list(t, "resource-type", rt);
        if (child_frame && types->len == 1) g_string_append(t, ",\"load-context\":[\"child-frame\"]");
        g_ptr_array_free(rt, TRUE);
        if (allow) add_rule(c->allows, &c->n_allows, t->str, "\"type\":\"ignore-previous-rules\"");
        else add_rule(c->blocks, &c->n_blocks, t->str, "\"type\":\"block\"");
        g_string_free(t, TRUE);
    }
    g_string_free(re, TRUE);
    g_ptr_array_free(types, TRUE);
    g_ptr_array_free(ntypes, TRUE);
    g_ptr_array_free(din, TRUE);
    g_ptr_array_free(dout, TRUE);
    g_free(pat);
}

/* procedural / extended selectors that WebKit's CSS can't do */
static gboolean selector_ok(const char *sel)
{
    static const char *bad[] = { ":-abp-", ":xpath(", ":style(", ":remove(", ":upward(", ":matches-css",
                                 ":has-text(", ":contains(", ":-ext-", ":min-text-length", ":watch-attr",
                                 ":matches-attr", ":matches-path", ":matches-media", ":others(", ":if(",
                                 ":if-not(", ":nth-ancestor(", ":remove-attr", ":remove-class", "{", NULL };
    if (!*sel || !is_ascii(sel)) return FALSE;
    for (const char **b = bad; *b; b++) if (strstr(sel, *b)) return FALSE;
    return TRUE;
}

static void convert_cosmetic(Conv *c, const char *line, const char *mark)
{
    gboolean except = g_str_has_prefix(mark, "#@#");
    const char *sel = mark + (except ? 3 : 2);
    if (!selector_ok(sel)) return;
    char *domains = g_strndup(line, mark - line);
    GPtrArray *din = g_ptr_array_new_with_free_func(g_free), *dout = g_ptr_array_new_with_free_func(g_free);
    gboolean has = *domains && parse_domains(domains, ",", din, dout);
    if (*domains && !has) goto out;        /* only unusable domains: drop */
    if (except) {
        GPtrArray *a = g_hash_table_lookup(c->css_except, sel);
        if (!a) {
            a = g_ptr_array_new_with_free_func(g_free);
            g_hash_table_insert(c->css_except, g_strdup(sel), a);
        }
        for (guint i = 0; i < din->len; i++) g_ptr_array_add(a, g_strdup(din->pdata[i]));
    } else if (!has) {
        if (!g_hash_table_contains(c->css_generic, sel)) {
            char *k = g_strdup(sel);
            g_hash_table_insert(c->css_generic, k, NULL);
            g_ptr_array_add(c->css_generic_order, k);
        }
    } else {
        GString *key = g_string_new(din->len ? "if|" : "unless|");
        GPtrArray *d = din->len ? din : dout;
        for (guint i = 0; i < d->len; i++) {
            if (i) g_string_append_c(key, ',');
            g_string_append(key, d->pdata[i]);
        }
        GString *sels = g_hash_table_lookup(c->css_specific, key->str);
        if (!sels) {
            sels = g_string_new(NULL);
            char *k = g_strdup(key->str);
            g_hash_table_insert(c->css_specific, k, sels);
            g_ptr_array_add(c->css_specific_order, k);
        } else {
            g_string_append(sels, ", ");
        }
        g_string_append(sels, sel);
        g_string_free(key, TRUE);
    }
out:
    g_ptr_array_free(din, TRUE);
    g_ptr_array_free(dout, TRUE);
    g_free(domains);
}

static void emit_css(Conv *c, GString *o)
{
    /* generic selectors in groups (fewer rules compile faster); ones with exceptions alone */
    GString *group = g_string_new(NULL);
    guint in_group = 0;
    for (guint i = 0; i <= c->css_generic_order->len; i++) {
        const char *sel = i < c->css_generic_order->len ? c->css_generic_order->pdata[i] : NULL;
        GPtrArray *ex = sel ? g_hash_table_lookup(c->css_except, sel) : NULL;
        if (sel && ex && ex->len) {
            GString *t = g_string_new("\"url-filter\":\".*\"");
            json_list(t, "unless-domain", ex);
            GString *a = g_string_new("\"type\":\"css-display-none\",\"selector\":");
            json_str(a, sel);
            add_rule(o, &c->n_css, t->str, a->str);
            g_string_free(t, TRUE);
            g_string_free(a, TRUE);
            continue;
        }
        if (sel) {
            if (in_group) g_string_append(group, ", ");
            g_string_append(group, sel);
            in_group++;
        }
        if ((!sel && in_group) || in_group >= 250) {
            GString *a = g_string_new("\"type\":\"css-display-none\",\"selector\":");
            json_str(a, group->str);
            add_rule(o, &c->n_css, "\"url-filter\":\".*\"", a->str);
            g_string_free(a, TRUE);
            g_string_truncate(group, 0);
            in_group = 0;
        }
    }
    g_string_free(group, TRUE);
    for (guint i = 0; i < c->css_specific_order->len; i++) {
        const char *key = c->css_specific_order->pdata[i];
        GString *sels = g_hash_table_lookup(c->css_specific, key);
        gboolean inc = g_str_has_prefix(key, "if|");
        char **ds = g_strsplit(strchr(key, '|') + 1, ",", -1);
        GPtrArray *d = g_ptr_array_new();
        for (char **x = ds; *x; x++) g_ptr_array_add(d, *x);
        GString *t = g_string_new("\"url-filter\":\".*\"");
        json_list(t, inc ? "if-domain" : "unless-domain", d);
        GString *a = g_string_new("\"type\":\"css-display-none\",\"selector\":");
        json_str(a, sels->str);
        add_rule(o, &c->n_css, t->str, a->str);
        g_string_free(t, TRUE);
        g_string_free(a, TRUE);
        g_ptr_array_free(d, TRUE);
        g_strfreev(ds);
    }
}

static void gstring_free_all(gpointer s) { g_string_free(s, TRUE); }

/* Returns the JSON rule list; *count gets the number of rules. */
static GBytes *convert(const char *text, guint *count)
{
    Conv c = { 0 };
    c.blocks = g_string_new(NULL);
    c.allows = g_string_new(NULL);
    c.css_generic = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    c.css_generic_order = g_ptr_array_new();
    c.css_specific = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, gstring_free_all);
    c.css_specific_order = g_ptr_array_new();
    c.css_except = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *line = g_strstrip(*l);
        if (!*line || *line == '!' || *line == '[') continue;
        const char *m;
        if ((m = strstr(line, "#@#")) || (m = strstr(line, "##"))) convert_cosmetic(&c, line, m);
        else if (strstr(line, "#?#") || strstr(line, "#$#") || strstr(line, "#%#") || strstr(line, "#@?#")
                 || strstr(line, "#@$#")) continue;        /* extended / scriptlet syntax */
        else convert_network(&c, line);
    }
    g_strfreev(lines);

    GString *css = g_string_new(NULL);
    emit_css(&c, css);
    GString *out = g_string_new("[");
    g_string_append(out, c.blocks->str);
    if (c.blocks->len && css->len) g_string_append_c(out, ',');
    g_string_append(out, css->str);
    if ((c.blocks->len || css->len) && c.allows->len) g_string_append_c(out, ',');
    g_string_append(out, c.allows->str);   /* exceptions last: they cancel what came before */
    g_string_append_c(out, ']');
    *count = c.n_blocks + c.n_css + c.n_allows;

    g_string_free(css, TRUE);
    g_string_free(c.blocks, TRUE);
    g_string_free(c.allows, TRUE);
    g_hash_table_destroy(c.css_generic);
    g_ptr_array_free(c.css_generic_order, TRUE);
    g_hash_table_destroy(c.css_specific);
    g_ptr_array_free(c.css_specific_order, TRUE);
    g_hash_table_destroy(c.css_except);
    if (!*count) {   /* WebKit refuses an empty list: one rule that never matches */
        g_string_assign(out, "[{\"trigger\":{\"url-filter\":\"^tuxbrowser-never-matches:\"},"
                             "\"action\":{\"type\":\"block\"}}]");
    }
    return g_string_free_to_bytes(out);
}

/* ================================================================ */
/* applying the compiled filters to every web view                   */

static gboolean host_allowed(const char *host)
{
    if (!host || !*host) return FALSE;
    char *list = pref_get_string("adblock_allow");
    char **hs = g_strsplit_set(list, ", \n\t", -1);
    gboolean hit = FALSE;
    for (char **h = hs; *h && !hit; h++) {
        if (!**h) continue;
        gsize hl = strlen(host), nl = strlen(*h);
        hit = !g_ascii_strcasecmp(host, *h)
           || (hl > nl && host[hl - nl - 1] == '.' && !g_ascii_strcasecmp(host + hl - nl, *h));
    }
    g_strfreev(hs);
    g_free(list);
    return hit;
}

static char *uri_host(const char *uri)
{
    GUri *u = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    char *h = u && g_uri_get_host(u) ? g_ascii_strdown(g_uri_get_host(u), -1) : NULL;
    if (u) g_uri_unref(u);
    return h;
}

/* site name used for exceptions and picked elements: the host without "www." */
char *adblock_site_for_uri(const char *uri)
{
    if (!uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) return NULL;
    char *h = uri_host(uri);
    if (h && g_str_has_prefix(h, "www.")) {
        char *x = g_strdup(h + 4);
        g_free(h);
        h = x;
    }
    return h;
}

gboolean adblock_site_is_allowed(const char *uri)
{
    char *h = uri_host(uri);
    gboolean a = host_allowed(h);
    g_free(h);
    return a;
}

static void apply(WebKitUserContentManager *ucm)
{
    gboolean on = pref_get_bool("adblock") && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ucm), "adblock-allowed")) != 1;
    webkit_user_content_manager_remove_all_filters(ucm);
    if (on && f_lists) webkit_user_content_manager_add_filter(ucm, f_lists);
    if (on && f_user) webkit_user_content_manager_add_filter(ucm, f_user);
}

static void apply_all(void)
{
    for (guint i = 0; managers && i < managers->len; i++) apply(managers->pdata[i]);
}

/* The page (main frame) of this content manager's view is going to this URI: turn the
 * filters off for allowed sites, on for others. Called when a load starts, is redirected
 * and commits, before the page's own resources are requested. */
void adblock_page_changed(WebKitUserContentManager *ucm, const char *uri)
{
    gboolean allowed = adblock_site_is_allowed(uri);
    if (allowed == GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ucm), "adblock-allowed"))) return;
    g_object_set_data(G_OBJECT(ucm), "adblock-allowed", GINT_TO_POINTER(allowed));
    apply(ucm);
}

static void picked(WebKitUserContentManager *ucm, WebKitJavascriptResult *res, gpointer data);

static void manager_gone(gpointer data, GObject *dead)
{
    g_ptr_array_remove(managers, dead);
}

/* the element picker remembers what was right-clicked, so "Block Element..." starts there */
static const char *CONTEXT_JS =
    "document.addEventListener('contextmenu', function (e) { window.__tuxCtx = e.target; }, true);";

void adblock_attach(WebKitUserContentManager *ucm)
{
    if (!managers) managers = g_ptr_array_new();
    g_ptr_array_add(managers, ucm);
    g_object_weak_ref(G_OBJECT(ucm), manager_gone, NULL);
    webkit_user_content_manager_register_script_message_handler_in_world(ucm, "tuxpick", WORLD);
    g_signal_connect(ucm, "script-message-received::tuxpick", G_CALLBACK(picked), NULL);
    WebKitUserScript *s = webkit_user_script_new_for_world(CONTEXT_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                           WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, WORLD, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, s);
    webkit_user_script_unref(s);
    apply(ucm);
}

/* ================================================================ */
/* compiling                                                         */

typedef struct { char *id; guint count; } Compile;

static void compiled(GObject *src, GAsyncResult *res, gpointer data)
{
    Compile *c = data;
    GError *err = NULL;
    WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(store, res, &err);
    if (f) {
        WebKitUserContentFilter **slot = g_str_equal(c->id, "lists") ? &f_lists : &f_user;
        if (*slot) webkit_user_content_filter_unref(*slot);
        *slot = f;
        apply_all();
        if (g_str_equal(c->id, "lists")) {
            pref_set_int("adblock_rule_count", c->count);
            prefs_save();
        }
        clear_status();
    } else {
        g_warning("adblock: compiling %s failed: %s", c->id, err ? err->message : "?");
        set_status("Could not use the %s: %s", g_str_equal(c->id, "lists") ? "filter lists" : "your rules",
                   err ? err->message : "unknown error");
        g_clear_error(&err);
    }
    if (g_str_equal(c->id, "lists")) updating = FALSE;
    profile_emit(EV_ADBLOCK);
    g_free(c->id);
    g_free(c);
}

static void compile_text(const char *id, const char *text)
{
    Compile *c = g_new0(Compile, 1);
    c->id = g_strdup(id);
    GBytes *json = convert(text, &c->count);
    webkit_user_content_filter_store_save(store, id, json, NULL, compiled, c);
    g_bytes_unref(json);
}

static void compile_file(const char *id, const char *path)
{
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) text = g_strdup("");
    if (g_str_equal(id, "lists")) updating = TRUE;
    compile_text(id, text);
    g_free(text);
}

void adblock_rules_changed(void)
{
    char *p = rules_path();
    compile_file("user", p);
    g_free(p);
}

/* ================================================================ */
/* downloading the lists                                             */

typedef struct {
    SoupSession *session;
    char **urls;
    guint i, ok, failed;
    GString *text;
} Fetch;

static void fetch_next(Fetch *f);

static void fetched(GObject *src, GAsyncResult *res, gpointer data)
{
    Fetch *f = data;
    GError *err = NULL;
    GBytes *b = soup_session_send_and_read_finish(f->session, res, &err);
    SoupMessage *m = soup_session_get_async_result_message(f->session, res);
    guint st = m ? soup_message_get_status(m) : 0;
    if (b && st == 200) {
        gsize n;
        const char *d = g_bytes_get_data(b, &n);
        g_string_append_len(f->text, d, n);
        g_string_append_c(f->text, '\n');
        f->ok++;
    } else {
        g_warning("adblock: %s: %s", f->urls[f->i], err ? err->message : soup_status_get_phrase(st));
        f->failed++;
    }
    if (b) g_bytes_unref(b);
    g_clear_error(&err);
    f->i++;
    fetch_next(f);
}

static void fetch_next(Fetch *f)
{
    while (f->urls[f->i] && !*f->urls[f->i]) f->i++;
    if (f->urls[f->i]) {
        SoupMessage *m = soup_message_new("GET", f->urls[f->i]);
        if (m) {
            soup_session_send_and_read_async(f->session, m, G_PRIORITY_LOW, NULL, fetched, f);
            g_object_unref(m);
            return;
        }
        f->failed++;
        f->i++;
        fetch_next(f);
        return;
    }
    char *path = lists_path();
    if (f->ok && !f->failed) {
        g_file_set_contents(path, f->text->str, f->text->len, NULL);
        pref_set_int("adblock_updated", (int)(g_get_real_time() / G_USEC_PER_SEC));
        prefs_save();
        set_status("Updating...");
        compile_file("lists", path);
    } else {
        updating = FALSE;
        set_status("Could not download %u of the filter lists; the previous rules stay in use.", f->failed);
    }
    g_free(path);
    g_object_unref(f->session);
    g_strfreev(f->urls);
    g_string_free(f->text, TRUE);
    g_free(f);
}

void adblock_update_lists(void)
{
    if (updating) return;
    updating = TRUE;
    set_status("Downloading filter lists...");
    Fetch *f = g_new0(Fetch, 1);
    f->session = soup_session_new_with_options("timeout", 60, "user-agent", APP_NAME "/" APP_VERSION, NULL);
    char *l = pref_get_string("adblock_lists");
    f->urls = g_strsplit_set(l, " \n\t", -1);
    g_free(l);
    f->text = g_string_new(NULL);
    fetch_next(f);
}

static gboolean update_if_stale(gpointer data)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (pref_get_bool("adblock") && now - pref_get_int("adblock_updated") > UPDATE_EVERY) adblock_update_lists();
    return G_SOURCE_CONTINUE;   /* checked again every few hours for long sessions */
}

/* ================================================================ */
/* start                                                             */

static void loaded(GObject *src, GAsyncResult *res, gpointer data)
{
    const char *id = data;
    WebKitUserContentFilter *f = webkit_user_content_filter_store_load_finish(store, res, NULL);
    gboolean lists = g_str_equal(id, "lists");
    if (f) {
        if (lists) f_lists = f; else f_user = f;
        apply_all();
        return;
    }
    /* nothing compiled yet (or by another WebKit version): build it from the text */
    char *p = lists ? lists_path() : rules_path();
    if (g_file_test(p, G_FILE_TEST_EXISTS)) compile_file(id, p);
    else if (lists && pref_get_bool("adblock")) adblock_update_lists();
    g_free(p);
}

void adblock_init(void)
{
    char *dir = g_build_filename(profile->cache_dir, "adblock", NULL);
    store = webkit_user_content_filter_store_new(dir);
    g_free(dir);
    webkit_user_content_filter_store_load(store, "lists", NULL, loaded, "lists");
    webkit_user_content_filter_store_load(store, "user", NULL, loaded, "user");
    g_timeout_add_seconds(30, update_if_stale, NULL);   /* once the start is over */
    g_timeout_add_seconds(6 * 3600, update_if_stale, NULL);
}

/* Preferences → Ad Blocking → Block ads */
void adblock_set_enabled(gboolean on)
{
    pref_set_bool("adblock", on);
    prefs_save();
    apply_all();
    if (on && !f_lists && !updating) {
        char *p = lists_path();
        if (g_file_test(p, G_FILE_TEST_EXISTS)) compile_file("lists", p);
        else adblock_update_lists();
        g_free(p);
    }
    profile_emit(EV_ADBLOCK);
}

/* "Allow Ads on This Site" / "Block Ads on This Site" */
void adblock_set_site_allowed(const char *uri, gboolean allow)
{
    char *site = adblock_site_for_uri(uri);
    if (!site) return;
    char *list = pref_get_string("adblock_allow");
    char **hs = g_strsplit_set(list, ", \n\t", -1);
    GString *o = g_string_new(NULL);
    char *host = uri_host(uri);
    for (char **h = hs; *h; h++) {
        if (!**h) continue;
        /* when allowing again, drop every entry that covers this page */
        gsize hl = strlen(host), nl = strlen(*h);
        gboolean covers = !g_ascii_strcasecmp(host, *h)
                       || (hl > nl && host[hl - nl - 1] == '.' && !g_ascii_strcasecmp(host + hl - nl, *h));
        if (covers || !g_ascii_strcasecmp(*h, site)) continue;
        if (o->len) g_string_append_c(o, '\n');
        g_string_append(o, *h);
    }
    if (allow) {
        if (o->len) g_string_append_c(o, '\n');
        g_string_append(o, site);
    }
    pref_set_string("adblock_allow", o->str);
    prefs_save();
    /* "unknown": the next adblock_page_changed() applies the filters whatever they were */
    for (guint i = 0; managers && i < managers->len; i++)
        g_object_set_data(managers->pdata[i], "adblock-allowed", GINT_TO_POINTER(2));
    g_string_free(o, TRUE);
    g_strfreev(hs);
    g_free(list);
    g_free(host);
    g_free(site);
    profile_emit(EV_ADBLOCK);
}

/* Preferences: the whole list of allowed sites */
void adblock_set_allowed_sites(const char *text)
{
    char *old = pref_get_string("adblock_allow");
    if (g_strcmp0(old, text)) {
        pref_set_string("adblock_allow", text);
        for (guint i = 0; managers && i < managers->len; i++)
            g_object_set_data(managers->pdata[i], "adblock-allowed", GINT_TO_POINTER(2));
        profile_emit(EV_ADBLOCK);
    }
    g_free(old);
}

/* ================================================================ */
/* Block Element...                                                  */

/* Runs in the isolated world. Highlights the element under the mouse (starting with the one
 * that was right-clicked); wheel or Up/Down widens or narrows to the parent/child; click
 * blocks it, Esc cancels. Reports a CSS selector for the element. */
static const char *PICKER_JS =
"(function () {\n"
"  if (window.__tuxPicking) return;\n"
"  window.__tuxPicking = true;\n"
"  var cur = null, trail = [];\n"
"  var box = document.createElement('div');\n"
"  box.style.cssText = 'position:fixed;z-index:2147483647;pointer-events:none;box-sizing:border-box;'\n"
"    + 'background:rgba(47,111,208,.22);outline:2px solid #2f6fd0;transition:all .05s';\n"
"  var tip = document.createElement('div');\n"
"  tip.style.cssText = 'position:fixed;z-index:2147483647;left:50%;bottom:12px;transform:translateX(-50%);'\n"
"    + 'font:13px sans-serif;background:#1d2733;color:#fff;padding:6px 12px;border-radius:4px;'\n"
"    + 'pointer-events:none;box-shadow:0 2px 8px rgba(0,0,0,.3);max-width:80%;white-space:nowrap;'\n"
"    + 'overflow:hidden;text-overflow:ellipsis';\n"
"  document.documentElement.appendChild(box);\n"
"  document.documentElement.appendChild(tip);\n"
"  function esc(s) { return window.CSS && CSS.escape ? CSS.escape(s) : s; }\n"
"  function part(el) {\n"
"    var p = el.localName;\n"
"    var cls = Array.prototype.filter.call(el.classList, function (c) { return /^[A-Za-z_-][\\w-]*$/.test(c); });\n"
"    if (cls.length) p += '.' + cls.map(esc).join('.');\n"
"    var parent = el.parentElement;\n"
"    if (parent) {\n"
"      var same = Array.prototype.filter.call(parent.children, function (c) { return c.localName === el.localName; });\n"
"      if (same.length > 1 && !cls.length) p += ':nth-of-type(' + (same.indexOf(el) + 1) + ')';\n"
"    }\n"
"    return p;\n"
"  }\n"
"  function selector(el) {\n"
"    if (el.id && /^[A-Za-z][\\w-]*$/.test(el.id) && document.querySelectorAll('#' + el.id).length === 1)\n"
"      return '#' + el.id;\n"
"    var path = [], e = el;\n"
"    while (e && e.nodeType === 1 && e !== document.documentElement) {\n"
"      if (e !== el && e.id && /^[A-Za-z][\\w-]*$/.test(e.id)) { path.unshift('#' + e.id); break; }\n"
"      path.unshift(part(e));\n"
"      var s = path.join(' > ');\n"
"      try { if (document.querySelectorAll(s).length === 1) return s; } catch (x) {}\n"
"      e = e.parentElement;\n"
"    }\n"
"    return path.join(' > ');\n"
"  }\n"
"  function ok(el) { return el && el.nodeType === 1 && el !== box && el !== tip\n"
"                       && el !== document.documentElement && el !== document.body; }\n"
"  function show(el) {\n"
"    if (!ok(el)) return;\n"
"    cur = el;\n"
"    var r = el.getBoundingClientRect();\n"
"    box.style.left = r.left + 'px'; box.style.top = r.top + 'px';\n"
"    box.style.width = r.width + 'px'; box.style.height = r.height + 'px';\n"
"    tip.textContent = 'Click to block \\u2014 wheel or \\u2191\\u2193 for a bigger/smaller area \\u2014 Esc to cancel   '\n"
"      + selector(el);\n"
"  }\n"
"  function done(sel) {\n"
"    window.__tuxPicking = false;\n"
"    document.removeEventListener('mousemove', move, true);\n"
"    document.removeEventListener('click', click, true);\n"
"    document.removeEventListener('mousedown', swallow, true);\n"
"    document.removeEventListener('mouseup', swallow, true);\n"
"    document.removeEventListener('keydown', key, true);\n"
"    document.removeEventListener('wheel', wheel, {capture: true, passive: false});\n"
"    box.remove(); tip.remove();\n"
"    window.webkit.messageHandlers.tuxpick.postMessage(sel);\n"
"  }\n"
"  function move(e) { trail = []; show(e.target); }\n"
"  function swallow(e) { e.preventDefault(); e.stopPropagation(); }\n"
"  function click(e) {\n"
"    swallow(e);\n"
"    if (!cur) return;\n"
"    var sel = selector(cur);\n"
"    try { document.querySelectorAll(sel).forEach(function (n) { n.style.setProperty('display', 'none', 'important'); }); } catch (x) {}\n"
"    done(sel);\n"
"  }\n"
"  function up() { if (cur && ok(cur.parentElement)) { trail.push(cur); show(cur.parentElement); } }\n"
"  function down() { if (trail.length) show(trail.pop()); }\n"
"  function key(e) {\n"
"    if (e.key === 'Escape') { swallow(e); done(''); }\n"
"    else if (e.key === 'ArrowUp') { swallow(e); up(); }\n"
"    else if (e.key === 'ArrowDown') { swallow(e); down(); }\n"
"    else if (e.key === 'Enter') { click(e); }\n"
"  }\n"
"  function wheel(e) { swallow(e); if (e.deltaY < 0) up(); else down(); }\n"
"  document.addEventListener('mousemove', move, true);\n"
"  document.addEventListener('click', click, true);\n"
"  document.addEventListener('mousedown', swallow, true);\n"
"  document.addEventListener('mouseup', swallow, true);\n"
"  document.addEventListener('keydown', key, true);\n"
"  document.addEventListener('wheel', wheel, {capture: true, passive: false});\n"
"  show(window.__tuxCtx && document.contains(window.__tuxCtx) ? window.__tuxCtx : null);\n"
"  if (!cur) tip.textContent = 'Point at the element to block \\u2014 Esc to cancel';\n"
"})();";

void adblock_pick_element(WebKitWebView *view)
{
    gtk_widget_grab_focus(GTK_WIDGET(view));   /* so Esc and the arrows reach the page */
    webkit_web_view_evaluate_javascript(view, PICKER_JS, -1, WORLD, NULL, NULL, NULL, NULL);
}

static Tab *tab_for_manager(WebKitUserContentManager *ucm)
{
    for (GList *w = browser_windows(); w; w = w->next)
        for (GList *t = ((BrowserWindow *)w->data)->tabs; t; t = t->next)
            if (((Tab *)t->data)->ucm == ucm) return t->data;
    return NULL;
}

static void picked(WebKitUserContentManager *ucm, WebKitJavascriptResult *res, gpointer data)
{
    JSCValue *v = webkit_javascript_result_get_js_value(res);
    Tab *tab = tab_for_manager(ucm);
    if (!tab || !jsc_value_is_string(v)) return;
    char *sel = jsc_value_to_string(v);
    char *site = adblock_site_for_uri(tab_get_uri(tab));
    /* a selector is one line of CSS; anything else is not from our picker */
    if (site && *sel && !strchr(sel, '\n') && !strstr(sel, "##")) {
        char *p = rules_path();
        char *old = NULL;
        g_file_get_contents(p, &old, NULL, NULL);
        GString *o = g_string_new(old ? old : "");
        if (o->len && o->str[o->len - 1] != '\n') g_string_append_c(o, '\n');
        g_string_append_printf(o, "%s##%s\n", site, sel);
        g_file_set_contents(p, o->str, o->len, NULL);
        g_string_free(o, TRUE);
        g_free(old);
        g_free(p);
        adblock_rules_changed();
        char *msg = g_strdup_printf("Element blocked on %s (Preferences \xe2\x86\x92 Ad Blocking lists your rules)", site);
        browser_window_flash_status(tab->win, msg);
        g_free(msg);
    }
    g_free(site);
    g_free(sel);
}

/* Preferences: read and replace your own rules */
char *adblock_get_rules(void)
{
    char *p = rules_path(), *t = NULL;
    if (!g_file_get_contents(p, &t, NULL, NULL)) t = g_strdup("");
    g_free(p);
    return t;
}

void adblock_set_rules(const char *text)
{
    char *old = adblock_get_rules();
    if (g_strcmp0(old, text)) {
        char *p = rules_path();
        g_file_set_contents(p, text, -1, NULL);
        g_free(p);
        adblock_rules_changed();
    }
    g_free(old);
}
