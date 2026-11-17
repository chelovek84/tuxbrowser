/* Profile: settings, bookmarks (text files), history (sqlite). */
#include "tuxbrowser.h"
#include <errno.h>
#include <sqlite3.h>
#include <string.h>
#include <glib/gstdio.h>

Profile *profile = NULL;

typedef struct { ProfileCb cb; gpointer data; } Listener;
static GList *listeners[EV_LAST];

/* ---------------------------------------------------------------- */
/* defaults                                                          */

static const struct { const char *key, *val; } default_strings[] = {
    { "startup",        "session" },        /* session | start */
    { "search_engine",  "Google" },
    { "download_dir",   "" },
    { "user_agent",     "" },
    { "accept_languages", "" },      /* comma separated; empty = system languages */
    { "hw_accel",       "default" },        /* default | always | never */
    { "gpu_memory",     "auto" },           /* auto | normal | low (applies at start) */
    { "video_decoding", "auto" },           /* auto | software | hardware (applies at start) */
    { "decoder_library", "auto" },          /* auto | ffmpeg | native (applies at start) */
};
static const struct { const char *key; gboolean val; } default_bools[] = {
    { "mouse_gestures",      TRUE },
    { "load_images",         TRUE },
    { "enable_javascript",   TRUE },
    { "smooth_scrolling",    TRUE },
    { "ask_download_dir",    FALSE },
    { "new_tab_next",        TRUE },
    { "window_maximized",    FALSE },
    { "close_last_tab_closes_window", FALSE },
};
static const struct { const char *key; int val; } default_ints[] = {
    { "default_zoom",       100 },
    { "window_width",       1200 },
    { "window_height",      800 },
};
static const struct { const char *name, *keyword, *url; } default_engines[] = {
    { "DuckDuckGo", "d", "https://duckduckgo.com/?q=%s" },
    { "Google",     "g", "https://www.google.com/search?q=%s" },
    { "Bing",       "b", "https://www.bing.com/search?q=%s" },
    { "Wikipedia",  "w", "https://en.wikipedia.org/wiki/Special:Search?search=%s" },
    { "Startpage",  "s", "https://www.startpage.com/do/search?q=%s" },
    { "YouTube",    "y", "https://www.youtube.com/results?search_query=%s" },
};
#define G "General"

/* ---------------------------------------------------------------- */
/* small TSV helpers: fields escaped for \\ \t \n only (keeps UTF-8) */

static char *esc(const char *s)
{
    GString *o = g_string_new(NULL);
    for (; s && *s; s++) {
        switch (*s) {
        case '\\': g_string_append(o, "\\\\"); break;
        case '\t': g_string_append(o, "\\t"); break;
        case '\n': g_string_append(o, "\\n"); break;
        case '\r': break;
        default:   g_string_append_c(o, *s);
        }
    }
    return g_string_free(o, FALSE);
}

static char *unesc(const char *s)
{
    GString *o = g_string_new(NULL);
    for (; s && *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            g_string_append_c(o, *s == 't' ? '\t' : *s == 'n' ? '\n' : *s);
        } else {
            g_string_append_c(o, *s);
        }
    }
    return g_string_free(o, FALSE);
}

/* Returns array of char** rows (each NULL-terminated, unescaped). */
static GPtrArray *tsv_read(const char *name)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    char *path = g_build_filename(profile->data_dir, name, NULL);
    char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (char **l = lines; *l; l++) {
            if (!**l || **l == '#') continue;
            char **f = g_strsplit(*l, "\t", -1);
            for (char **p = f; *p; p++) {
                char *u = unesc(*p);
                g_free(*p);
                *p = u;
            }
            g_ptr_array_add(rows, f);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
    return rows;
}

static void tsv_write(const char *name, GString *data)
{
    char *path = g_build_filename(profile->data_dir, name, NULL);
    GError *err = NULL;
    if (!g_file_set_contents(path, data->str, data->len, &err)) {
        g_warning("Cannot save %s: %s", path, err->message);
        g_error_free(err);
    }
    g_free(path);
}

static void tsv_field(GString *o, const char *s, gboolean last)
{
    char *e = esc(s ? s : "");
    g_string_append(o, e);
    g_string_append_c(o, last ? '\n' : '\t');
    g_free(e);
}

/* ---------------------------------------------------------------- */
/* listeners                                                         */

void profile_connect(ProfileEvent ev, ProfileCb cb, gpointer data)
{
    Listener *l = g_new0(Listener, 1);
    l->cb = cb;
    l->data = data;
    listeners[ev] = g_list_append(listeners[ev], l);
}

void profile_disconnect_data(gpointer data)
{
    for (int i = 0; i < EV_LAST; i++) {
        GList *n = listeners[i];
        while (n) {
            GList *next = n->next;
            Listener *l = n->data;
            if (l->data == data) {
                g_free(l);
                listeners[i] = g_list_delete_link(listeners[i], n);
            }
            n = next;
        }
    }
}

void profile_emit(ProfileEvent ev)
{
    GList *copy = g_list_copy(listeners[ev]);
    for (GList *n = copy; n; n = n->next) {
        if (g_list_find(listeners[ev], n->data)) {
            Listener *l = n->data;
            l->cb(l->data);
        }
    }
    g_list_free(copy);
}

/* ---------------------------------------------------------------- */
/* settings                                                          */

char *pref_get_string(const char *key)
{
    char *v = g_key_file_get_string(profile->settings, G, key, NULL);
    return v ? v : g_strdup("");
}

gboolean pref_get_bool(const char *key)
{
    return g_key_file_get_boolean(profile->settings, G, key, NULL);
}

int pref_get_int(const char *key)
{
    return g_key_file_get_integer(profile->settings, G, key, NULL);
}

void pref_set_string(const char *key, const char *val) { g_key_file_set_string(profile->settings, G, key, val ? val : ""); }
void pref_set_bool(const char *key, gboolean val)      { g_key_file_set_boolean(profile->settings, G, key, val); }
void pref_set_int(const char *key, int val)            { g_key_file_set_integer(profile->settings, G, key, val); }

void prefs_save(void)
{
    char *path = g_build_filename(profile->config_dir, "settings.ini", NULL);
    GError *err = NULL;
    if (!g_key_file_save_to_file(profile->settings, path, &err)) {
        g_warning("Cannot save settings: %s", err->message);
        g_error_free(err);
    }
    g_free(path);
    profile_emit(EV_SETTINGS);
}

static void load_settings(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = g_build_filename(profile->config_dir, "settings.ini", NULL);
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(path);
    profile->settings = kf;

    for (guint i = 0; i < G_N_ELEMENTS(default_strings); i++)
        if (!g_key_file_has_key(kf, G, default_strings[i].key, NULL))
            g_key_file_set_string(kf, G, default_strings[i].key, default_strings[i].val);
    for (guint i = 0; i < G_N_ELEMENTS(default_bools); i++)
        if (!g_key_file_has_key(kf, G, default_bools[i].key, NULL))
            g_key_file_set_boolean(kf, G, default_bools[i].key, default_bools[i].val);
    for (guint i = 0; i < G_N_ELEMENTS(default_ints); i++)
        if (!g_key_file_has_key(kf, G, default_ints[i].key, NULL))
            g_key_file_set_integer(kf, G, default_ints[i].key, default_ints[i].val);

    char *dl = pref_get_string("download_dir");
    if (!*dl) {
        const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        char *def = d ? g_strdup(d) : g_build_filename(g_get_home_dir(), "Downloads", NULL);
        pref_set_string("download_dir", def);
        g_free(def);
    }
    g_free(dl);

    /* search engines: groups [Search:<name>] */
    for (guint i = 0; i < G_N_ELEMENTS(default_engines); i++) {
        char *grp = g_strconcat("Search:", default_engines[i].name, NULL);
        if (!g_key_file_has_group(kf, grp)) {
            g_key_file_set_string(kf, grp, "keyword", default_engines[i].keyword);
            g_key_file_set_string(kf, grp, "url", default_engines[i].url);
        }
        g_free(grp);
    }
    profile->engines = g_ptr_array_new();
    gsize n = 0;
    char **groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; i++) {
        if (!g_str_has_prefix(groups[i], "Search:")) continue;
        SearchEngine *e = g_new0(SearchEngine, 1);
        e->name = g_strdup(groups[i] + 7);
        e->keyword = g_key_file_get_string(kf, groups[i], "keyword", NULL);
        e->url = g_key_file_get_string(kf, groups[i], "url", NULL);
        if (!e->keyword) e->keyword = g_strdup("");
        if (!e->url || !strstr(e->url, "%s")) {
            g_free(e->name); g_free(e->keyword); g_free(e->url); g_free(e);
            continue;
        }
        g_ptr_array_add(profile->engines, e);
    }
    g_strfreev(groups);
}

SearchEngine *search_engine_by_name(const char *name)
{
    for (guint i = 0; i < profile->engines->len; i++) {
        SearchEngine *e = profile->engines->pdata[i];
        if (g_strcmp0(e->name, name) == 0) return e;
    }
    return NULL;
}

SearchEngine *search_engine_default(void)
{
    char *name = pref_get_string("search_engine");
    SearchEngine *e = search_engine_by_name(name);
    g_free(name);
    if (!e && profile->engines->len) e = profile->engines->pdata[0];
    return e;
}

char *search_url_for(SearchEngine *e, const char *query)
{
    char *q = g_uri_escape_string(query, NULL, TRUE);
    GString *s = g_string_new(e->url);
    g_string_replace(s, "%s", q, 1);
    g_free(q);
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------- */
/* bookmarks                                                         */

static void bookmark_free(Bookmark *b) { g_free(b->url); g_free(b->title); g_free(b->folder); g_free(b); }

static void load_bookmarks(void)
{
    profile->bookmarks = g_ptr_array_new_with_free_func((GDestroyNotify)bookmark_free);
    GPtrArray *rows = tsv_read("bookmarks.tsv");
    for (guint i = 0; i < rows->len; i++) {
        char **f = rows->pdata[i];
        if (!f[0] || !*f[0]) continue;
        Bookmark *b = g_new0(Bookmark, 1);
        b->url = g_strdup(f[0]);
        b->title = g_strdup(f[1] ? f[1] : f[0]);
        b->folder = g_strdup(f[1] && f[2] ? f[2] : "");
        g_ptr_array_add(profile->bookmarks, b);
    }
    g_ptr_array_unref(rows);
}

void bookmarks_save(void)
{
    GString *o = g_string_new("# url\ttitle\tfolder\n");
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        tsv_field(o, b->url, FALSE);
        tsv_field(o, b->title, FALSE);
        tsv_field(o, b->folder, TRUE);
    }
    tsv_write("bookmarks.tsv", o);
    g_string_free(o, TRUE);
    profile_emit(EV_BOOKMARKS);
}

gboolean bookmark_exists(const char *url)
{
    if (!url) return FALSE;
    for (guint i = 0; i < profile->bookmarks->len; i++)
        if (g_strcmp0(((Bookmark *)profile->bookmarks->pdata[i])->url, url) == 0)
            return TRUE;
    return FALSE;
}

void bookmark_add(const char *url, const char *title, const char *folder)
{
    if (!url || !*url || bookmark_exists(url)) return;
    Bookmark *b = g_new0(Bookmark, 1);
    b->url = g_strdup(url);
    b->title = g_strdup(title && *title ? title : url);
    b->folder = g_strdup(folder ? folder : "");
    g_ptr_array_add(profile->bookmarks, b);
    bookmarks_save();
}

void bookmark_remove(const char *url)
{
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        if (g_strcmp0(((Bookmark *)profile->bookmarks->pdata[i])->url, url) == 0) {
            g_ptr_array_remove_index(profile->bookmarks, i);
            break;
        }
    }
    bookmarks_save();
}

void bookmark_update(const char *old_url, const char *url, const char *title, const char *folder)
{
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        if (g_strcmp0(b->url, old_url) == 0) {
            g_free(b->url); g_free(b->title); g_free(b->folder);
            b->url = g_strdup(url);
            b->title = g_strdup(title && *title ? title : url);
            b->folder = g_strdup(folder ? folder : "");
            break;
        }
    }
    bookmarks_save();
}

/* ---------------------------------------------------------------- */
/* closed tabs (trash can)                                           */

static void dial_free(Dial *d) { g_free(d->url); g_free(d->title); g_free(d); }

void closed_tab_push(const char *url, const char *title)
{
    if (!url || !*url || g_str_has_prefix(url, "tuxbrowser:") || g_str_equal(url, "about:blank"))
        return;
    Dial *d = g_new0(Dial, 1);
    d->url = g_strdup(url);
    d->title = g_strdup(title && *title ? title : url);
    g_ptr_array_insert(profile->closed_tabs, 0, d);
    while (profile->closed_tabs->len > 20)
        g_ptr_array_remove_index(profile->closed_tabs, profile->closed_tabs->len - 1);
    profile_emit(EV_CLOSED_TABS);
}

/* ---------------------------------------------------------------- */
/* history (sqlite)                                                  */

#define DB ((sqlite3 *)profile->db)

static void history_open(void)
{
    char *path = g_build_filename(profile->data_dir, "history.sqlite", NULL);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        g_warning("Cannot open history database %s", path);
        sqlite3_close(db);
        sqlite3_open(":memory:", &db);
    }
    g_free(path);
    sqlite3_exec(db,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"   /* WAL stays consistent; no disk flush on every visit */
        "CREATE TABLE IF NOT EXISTS history (id INTEGER PRIMARY KEY, url TEXT UNIQUE,"
        " title TEXT, visits INTEGER DEFAULT 1, last_visit INTEGER);"
        "CREATE INDEX IF NOT EXISTS history_time ON history(last_visit);",
        NULL, NULL, NULL);
    profile->db = db;
}

static gboolean history_ignored(const char *url)
{
    return !url || !*url || g_str_has_prefix(url, "tuxbrowser:") || g_str_has_prefix(url, "about:")
        || g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "blob:");
}

void history_add(const char *url, const char *title)
{
    if (history_ignored(url)) return;
    sqlite3_stmt *st;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    sqlite3_prepare_v2(DB, "INSERT INTO history(url, title, visits, last_visit) VALUES(?1, ?2, 1, ?3) "
                           "ON CONFLICT(url) DO UPDATE SET visits=visits+1, last_visit=?3, "
                           "title=COALESCE(NULLIF(?2,''), title)", -1, &st, NULL);
    sqlite3_bind_text(st, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_step(st);
    sqlite3_finalize(st);
    profile_emit(EV_HISTORY);
}

void history_set_title(const char *url, const char *title)
{
    if (history_ignored(url) || !title || !*title) return;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(DB, "UPDATE history SET title=?1 WHERE url=?2", -1, &st, NULL);
    sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void history_item_free(HistoryItem *h) { g_free(h->url); g_free(h->title); g_free(h); }

static GPtrArray *history_run(const char *sql, const char *text, int limit)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)history_item_free);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(DB, sql, -1, &st, NULL) != SQLITE_OK) return out;
    char *like = g_strdup_printf("%%%s%%", text ? text : "");
    sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        HistoryItem *h = g_new0(HistoryItem, 1);
        h->url = g_strdup((const char *)sqlite3_column_text(st, 0));
        h->title = g_strdup((const char *)sqlite3_column_text(st, 1));
        h->last_visit = sqlite3_column_int64(st, 2);
        h->visits = sqlite3_column_int(st, 3);
        g_ptr_array_add(out, h);
    }
    sqlite3_finalize(st);
    g_free(like);
    return out;
}

GPtrArray *history_query(const char *text, int limit)
{
    return history_run("SELECT url, title, last_visit, visits FROM history "
                       "WHERE url LIKE ?1 OR title LIKE ?1 ORDER BY last_visit DESC LIMIT ?2",
                       text, limit);
}

GPtrArray *history_complete(const char *text, int limit)
{
    return history_run("SELECT url, title, last_visit, visits FROM history "
                       "WHERE url LIKE ?1 OR title LIKE ?1 "
                       "ORDER BY (visits + last_visit / 86400.0) DESC LIMIT ?2",
                       text, limit);
}

void history_delete(const char *url)
{
    if (!url) {
        sqlite3_exec(DB, "DELETE FROM history", NULL, NULL, NULL);
    } else {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(DB, "DELETE FROM history WHERE url=?1", -1, &st, NULL);
        sqlite3_bind_text(st, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    profile_emit(EV_HISTORY);
}

/* ---------------------------------------------------------------- */

/* The browser was called GBrowser before: on the first start under the new name, its
 * folders are moved over (same file system, so nothing is copied) and kept that way. */
static void migrate_dir(const char *base, const char *new_dir)
{
    char *old_dir = g_build_filename(base, "gbrowser", NULL);
    if (!g_file_test(new_dir, G_FILE_TEST_EXISTS) && g_file_test(old_dir, G_FILE_TEST_IS_DIR)) {
        if (g_rename(old_dir, new_dir) == 0)
            g_message("moved %s to %s", old_dir, new_dir);
        else
            g_warning("could not move %s to %s: %s", old_dir, new_dir, g_strerror(errno));
    }
    g_free(old_dir);
}

void profile_init(void)
{
    profile = g_new0(Profile, 1);
    const char *override = g_getenv("TUXBROWSER_PROFILE");
    if (override && *override) {
        profile->config_dir = g_strdup(override);
        profile->data_dir = g_strdup(override);
        profile->cache_dir = g_build_filename(override, "cache", NULL);
    } else {
        profile->config_dir = g_build_filename(g_get_user_config_dir(), "tuxbrowser", NULL);
        profile->data_dir = g_build_filename(g_get_user_data_dir(), "tuxbrowser", NULL);
        profile->cache_dir = g_build_filename(g_get_user_cache_dir(), "tuxbrowser", NULL);
        migrate_dir(g_get_user_config_dir(), profile->config_dir);
        migrate_dir(g_get_user_data_dir(), profile->data_dir);
        migrate_dir(g_get_user_cache_dir(), profile->cache_dir);
    }
    g_mkdir_with_parents(profile->config_dir, 0700);
    g_mkdir_with_parents(profile->data_dir, 0700);
    g_mkdir_with_parents(profile->cache_dir, 0700);

    load_settings();
    load_bookmarks();
    profile->closed_tabs = g_ptr_array_new_with_free_func((GDestroyNotify)dial_free);
    history_open();
}

void profile_shutdown(void)
{
    prefs_save();
    if (profile->db) sqlite3_close(DB);
    profile->db = NULL;
}
