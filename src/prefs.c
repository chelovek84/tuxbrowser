/* Preferences, Delete Private Data, bookmark properties dialogs. */
#include "tuxbrowser.h"
#include <string.h>

static GtkWidget *row_label(const char *text)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *page_grid(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    g_object_set(g, "margin", 14, NULL);
    return g;
}

static GtkWidget *section(GtkWidget *grid, int row, const char *title)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (row > 0) gtk_widget_set_margin_top(l, 8);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 3, 1);
    return l;
}

static GtkWidget *check(GtkWidget *grid, int row, const char *label, const char *key)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), pref_get_bool(key));
    g_object_set_data(G_OBJECT(c), "key", (gpointer)key);
    gtk_grid_attach(GTK_GRID(grid), c, 0, row, 3, 1);
    return c;
}

/* ---------------------------------------------------------------- */
/* search engines model                                              */

enum { SE_NAME, SE_KEY, SE_URL, SE_N };

static void cell_edited(GtkCellRendererText *r, char *path, char *text, GtkListStore *store)
{
    GtkTreeIter it;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "col"));
    if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &it, path))
        gtk_list_store_set(store, &it, col, g_strstrip(text), -1);
}

static void se_add(GtkButton *b, GtkTreeView *tv)
{
    GtkListStore *s = GTK_LIST_STORE(gtk_tree_view_get_model(tv));
    GtkTreeIter it;
    gtk_list_store_insert_with_values(s, &it, -1, SE_NAME, "New engine", SE_KEY, "", SE_URL, "https://example.com/search?q=%s", -1);
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(s), &it);
    gtk_tree_view_set_cursor(tv, p, gtk_tree_view_get_column(tv, 0), TRUE);
    gtk_tree_path_free(p);
}

static void se_remove(GtkButton *b, GtkTreeView *tv)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv), &m, &it))
        gtk_list_store_remove(GTK_LIST_STORE(m), &it);
}

static void engines_store_save(GtkListStore *store)
{
    GKeyFile *kf = profile->settings;
    gsize n = 0;
    char **groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; i++)
        if (g_str_has_prefix(groups[i], "Search:")) g_key_file_remove_group(kf, groups[i], NULL);
    g_strfreev(groups);

    for (guint i = 0; i < profile->engines->len; i++) {
        SearchEngine *e = profile->engines->pdata[i];
        g_free(e->name); g_free(e->keyword); g_free(e->url); g_free(e);
    }
    g_ptr_array_set_size(profile->engines, 0);

    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it);
    while (ok) {
        char *name, *key, *url;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &it, SE_NAME, &name, SE_KEY, &key, SE_URL, &url, -1);
        if (name && *name && url && strstr(url, "%s") && !search_engine_by_name(name)) {
            char *grp = g_strconcat("Search:", name, NULL);
            g_key_file_set_string(kf, grp, "keyword", key ? key : "");
            g_key_file_set_string(kf, grp, "url", url);
            g_free(grp);
            SearchEngine *e = g_new0(SearchEngine, 1);
            e->name = name; e->keyword = g_strdup(key ? key : ""); e->url = url;
            g_ptr_array_add(profile->engines, e);
            name = url = NULL;
        }
        g_free(name); g_free(key); g_free(url);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it);
    }
}

static GtkWidget *engines_view(GtkListStore *store)
{
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    const char *titles[] = { "Name", "Keyword", "Address (%s = search terms)" };
    for (int c = 0; c < SE_N; c++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "editable", TRUE, NULL);
        if (c == SE_URL) g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        g_object_set_data(G_OBJECT(r), "col", GINT_TO_POINTER(c));
        g_signal_connect(r, "edited", G_CALLBACK(cell_edited), store);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(titles[c], r, "text", c, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        if (c == SE_URL) gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    return tv;
}

/* ---------------------------------------------------------------- */

static void open_private_data(GtkButton *b, GtkWindow *parent)
{
    private_data_dialog_show(parent);
}

/* ---------------------------------------------------------------- */
/* user agent presets                                                */

#define UA_CUSTOM "custom"
#define UA_WEBKIT "webkit"

static const struct { const char *id, *label, *ua; } ua_presets[] = {
    { "safari-mac",     "Safari on macOS (default)", DEFAULT_USER_AGENT },
    { "chrome-linux",   "Chrome on Linux",
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36" },
    { "chrome-windows", "Chrome on Windows",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36" },
    { "edge-windows",   "Edge on Windows",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36 Edg/155.0.0.0" },
    { "firefox-linux",  "Firefox on Linux",
      "Mozilla/5.0 (X11; Linux x86_64; rv:158.0) Gecko/20100101 Firefox/158.0" },
    { "chrome-android", "Chrome on Android (mobile sites)",
      "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Mobile Safari/537.36" },
    { UA_WEBKIT,        "WebKitGTK (engine's own)", NULL },   /* filled in at runtime */
};

static const char *webkit_own_ua(void)
{
    static char *ua;
    if (!ua) {
        WebKitSettings *s = webkit_settings_new();
        ua = g_strdup(webkit_settings_get_user_agent(s));
        g_object_unref(s);
    }
    return ua;
}

static const char *ua_preset_string(guint i)
{
    return ua_presets[i].ua ? ua_presets[i].ua : webkit_own_ua();
}

static void ua_combo_changed(GtkComboBox *combo, GtkEntry *entry)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    if (g_str_equal(id, UA_CUSTOM)) {
        gtk_editable_set_editable(GTK_EDITABLE(entry), TRUE);
        gtk_widget_set_can_focus(GTK_WIDGET(entry), TRUE);
        gtk_widget_grab_focus(GTK_WIDGET(entry));
        return;
    }
    for (guint i = 0; i < G_N_ELEMENTS(ua_presets); i++)
        if (g_str_equal(id, ua_presets[i].id)) gtk_entry_set_text(entry, ua_preset_string(i));
    gtk_editable_set_editable(GTK_EDITABLE(entry), FALSE);   /* shown for reference only */
}

/* ---------------------------------------------------------------- */
/* Ad Blocking page helpers                                          */

/* a multi-line text field in a frame; the text view is kept as data "view" */
static GtkWidget *text_area(const char *text, gboolean mono, int height)
{
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), mono);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), mono ? GTK_WRAP_NONE : GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 2);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), text ? text : "", -1);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sw, -1, height);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    g_object_set_data(G_OBJECT(sw), "view", tv);
    return sw;
}

static char *text_area_get(GtkWidget *sw)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(sw), "view")));
    GtkTextIter a, e;
    gtk_text_buffer_get_bounds(b, &a, &e);
    return gtk_text_buffer_get_text(b, &a, &e, FALSE);
}

static GtkWidget *hint_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    return l;
}

static void adblock_status_changed(gpointer label)
{
    gtk_label_set_text(GTK_LABEL(label), adblock_status());
    GtkWidget *btn = g_object_get_data(G_OBJECT(label), "button");
    if (btn) gtk_widget_set_sensitive(btn, !adblock_updating());
}

static void adblock_status_gone(GtkWidget *label, gpointer data) { profile_disconnect_data(label); }

/* one address per line, blank lines and spaces dropped */
static char *clean_lines(const char *text)
{
    char **ls = g_strsplit_set(text, " \n\t,", -1);
    GString *o = g_string_new(NULL);
    for (char **l = ls; *l; l++) {
        if (!**l) continue;
        if (o->len) g_string_append_c(o, '\n');
        g_string_append(o, *l);
    }
    g_strfreev(ls);
    return g_string_free(o, FALSE);
}

static void adblock_update_clicked(GtkButton *b, GtkWidget *lists)
{
    char *l = text_area_get(lists), *c = clean_lines(l);
    pref_set_string("adblock_lists", c);   /* update from what is in the field now */
    g_free(l);
    g_free(c);
    adblock_update_lists();
}

/* ---------------------------------------------------------------- */
/* language presets: same pattern as the user agent (a list, the string shown below it,
 * editable only for Custom); "System default" is stored as "" */

#define LANG_CUSTOM "custom"
#define LANG_SYSTEM "system"

static const struct { const char *id, *label, *langs; } lang_presets[] = {
    { LANG_SYSTEM, NULL, NULL },   /* label and value come from the system at runtime */
    { "en-us", "English (US)",            "en-US, en" },
    { "en-gb", "English (UK)",            "en-GB, en" },
    { "hy",    "Armenian",                "hy-AM, hy, en" },
    { "zh-cn", "Chinese (Simplified)",    "zh-CN, zh, en" },
    { "zh-tw", "Chinese (Traditional)",   "zh-TW, zh, en" },
    { "nl",    "Dutch",                   "nl-NL, nl, en" },
    { "fr",    "French",                  "fr-FR, fr, en" },
    { "de",    "German",                  "de-DE, de, en" },
    { "it",    "Italian",                 "it-IT, it, en" },
    { "ja",    "Japanese",                "ja-JP, ja, en" },
    { "ko",    "Korean",                  "ko-KR, ko, en" },
    { "pl",    "Polish",                  "pl-PL, pl, en" },
    { "pt-br", "Portuguese (Brazil)",     "pt-BR, pt, en" },
    { "ru",    "Russian",                 "ru-RU, ru, en" },
    { "es",    "Spanish",                 "es-ES, es, en" },
    { "tr",    "Turkish",                 "tr-TR, tr, en" },
    { "uk",    "Ukrainian",               "uk-UA, uk, en" },
};

static char *system_langs_string(void)
{
    char **sys = web_system_languages();
    char *s = g_strjoinv(", ", sys);
    g_strfreev(sys);
    return s;
}

static void lang_combo_changed(GtkComboBox *combo, GtkEntry *entry)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    if (g_str_equal(id, LANG_CUSTOM)) {
        gtk_editable_set_editable(GTK_EDITABLE(entry), TRUE);
        gtk_widget_grab_focus(GTK_WIDGET(entry));
        return;
    }
    if (g_str_equal(id, LANG_SYSTEM)) {
        char *sys = system_langs_string();
        gtk_entry_set_text(entry, sys);
        g_free(sys);
    } else {
        for (guint i = 1; i < G_N_ELEMENTS(lang_presets); i++)
            if (g_str_equal(id, lang_presets[i].id)) gtk_entry_set_text(entry, lang_presets[i].langs);
    }
    gtk_editable_set_editable(GTK_EDITABLE(entry), FALSE);   /* shown for reference only */
}

void prefs_dialog_show(GtkWindow *parent)
{
    GtkWidget *d = gtk_dialog_new_with_buttons("Preferences", parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(d), 600, 480);
    GtkWidget *nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), nb, TRUE, TRUE, 0);
    GList *checks = NULL;

    /* General */
    GtkWidget *g = page_grid();
    int r = 0;
    section(g, r++, "Startup");
    gtk_grid_attach(GTK_GRID(g), row_label("_Startup:"), 0, r, 1, 1);
    GtkWidget *startup = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(startup), "session", "Continue from last time");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(startup), "start", "Start with the start page");
    char *s = pref_get_string("startup");
    /* "home" and "speeddial" were earlier choices; both now mean the start page */
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(startup), g_str_equal(s, "session") ? s : "start"))
        gtk_combo_box_set_active(GTK_COMBO_BOX(startup), 0);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), startup, 1, r++, 2, 1);

    section(g, r++, "Downloads");
    gtk_grid_attach(GTK_GRID(g), row_label("Save files _to:"), 0, r, 1, 1);
    GtkWidget *dldir = gtk_file_chooser_button_new("Download Folder", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    s = pref_get_string("download_dir");
    g_mkdir_with_parents(s, 0755);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dldir), s);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), dldir, 1, r++, 2, 1);
    checks = g_list_append(checks, check(g, r++, "_Ask where to save each file", "ask_download_dir"));

    section(g, r++, "Tabs");
    checks = g_list_append(checks, check(g, r++, "Open new tabs _next to the active tab", "new_tab_next"));
    checks = g_list_append(checks, check(g, r++, "Closing the _last tab closes the window", "close_last_tab_closes_window"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("General"));

    /* Search */
    g = page_grid();
    r = 0;
    section(g, r++, "Search engines");
    GtkListStore *se_store = gtk_list_store_new(SE_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (guint i = 0; i < profile->engines->len; i++) {
        SearchEngine *e = profile->engines->pdata[i];
        gtk_list_store_insert_with_values(se_store, NULL, -1, SE_NAME, e->name, SE_KEY, e->keyword, SE_URL, e->url, -1);
    }
    GtkWidget *se_tv = engines_view(se_store);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), se_tv);
    gtk_grid_attach(GTK_GRID(g), sw, 0, r++, 3, 1);
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *add = gtk_button_new_with_mnemonic("_Add");
    GtkWidget *rem = gtk_button_new_with_mnemonic("_Remove");
    g_signal_connect(add, "clicked", G_CALLBACK(se_add), se_tv);
    g_signal_connect(rem, "clicked", G_CALLBACK(se_remove), se_tv);
    gtk_box_pack_start(GTK_BOX(bb), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb), rem, FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new("Type a keyword before your search in the address bar, e.g. \"w linux\".");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_box_pack_start(GTK_BOX(bb), hint, FALSE, FALSE, 8);
    gtk_grid_attach(GTK_GRID(g), bb, 0, r++, 3, 1);
    gtk_grid_attach(GTK_GRID(g), row_label("_Default engine:"), 0, r, 1, 1);
    GtkWidget *def = gtk_combo_box_text_new();
    SearchEngine *de = search_engine_default();
    for (guint i = 0; i < profile->engines->len; i++) {
        SearchEngine *e = profile->engines->pdata[i];
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(def), e->name, e->name);
        if (e == de) gtk_combo_box_set_active(GTK_COMBO_BOX(def), i);
    }
    gtk_grid_attach(GTK_GRID(g), def, 1, r++, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Search"));

    /* Web pages */
    g = page_grid();
    r = 0;
    section(g, r++, "Display");
    gtk_grid_attach(GTK_GRID(g), row_label("Default _zoom (%):"), 0, r, 1, 1);
    GtkWidget *zoom = gtk_spin_button_new_with_range(30, 300, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(zoom), pref_get_int("default_zoom"));
    gtk_grid_attach(GTK_GRID(g), zoom, 1, r++, 1, 1);
    checks = g_list_append(checks, check(g, r++, "Load _images", "load_images"));
    checks = g_list_append(checks, check(g, r++, "Enable _JavaScript", "enable_javascript"));
    checks = g_list_append(checks, check(g, r++, "_Smooth scrolling", "smooth_scrolling"));
    section(g, r++, "Identity");
    gtk_grid_attach(GTK_GRID(g), row_label("_User agent:"), 0, r, 1, 1);
    GtkWidget *ua_combo = gtk_combo_box_text_new();
    for (guint i = 0; i < G_N_ELEMENTS(ua_presets); i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ua_combo), ua_presets[i].id, ua_presets[i].label);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ua_combo), UA_CUSTOM, "Custom...");
    gtk_grid_attach(GTK_GRID(g), ua_combo, 1, r++, 2, 1);
    GtkWidget *ua = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ua), "Enter a user agent string");
    gtk_widget_set_hexpand(ua, TRUE);
    gtk_grid_attach(GTK_GRID(g), ua, 1, r++, 2, 1);
    /* select the preset that matches the saved string ("" = default), else Custom */
    s = pref_get_string("user_agent");
    const char *sel = UA_CUSTOM;
    if (!*s) {
        sel = ua_presets[0].id;
    } else {
        for (guint i = 0; i < G_N_ELEMENTS(ua_presets); i++)
            if (g_str_equal(s, ua_preset_string(i))) sel = ua_presets[i].id;
    }
    gtk_entry_set_text(GTK_ENTRY(ua), *s ? s : DEFAULT_USER_AGENT);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ua_combo), sel);
    gtk_editable_set_editable(GTK_EDITABLE(ua), g_str_equal(sel, UA_CUSTOM));
    g_signal_connect(ua_combo, "changed", G_CALLBACK(ua_combo_changed), ua);
    g_free(s);

    gtk_grid_attach(GTK_GRID(g), row_label("_Languages:"), 0, r, 1, 1);
    GtkWidget *lang_combo = gtk_combo_box_text_new();
    char *sys_str = system_langs_string();
    char *sys_label = g_strdup_printf("System default (%s)", sys_str);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), LANG_SYSTEM, sys_label);
    g_free(sys_label);
    for (guint i = 1; i < G_N_ELEMENTS(lang_presets); i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), lang_presets[i].id, lang_presets[i].label);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), LANG_CUSTOM, "Custom...");
    gtk_grid_attach(GTK_GRID(g), lang_combo, 1, r++, 2, 1);
    GtkWidget *langs_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(langs_entry), "Languages, most preferred first, e.g. de-DE, en");
    gtk_widget_set_tooltip_text(langs_entry, "Languages web pages should use, most preferred first, "
                                "separated by commas, e.g. \"de-DE, de, en\".");
    gtk_widget_set_hexpand(langs_entry, TRUE);
    gtk_grid_attach(GTK_GRID(g), langs_entry, 1, r++, 2, 1);
    /* select the preset that matches the saved string ("" = system default), else Custom */
    s = g_strstrip(pref_get_string("accept_languages"));
    sel = LANG_CUSTOM;
    if (!*s) {
        sel = LANG_SYSTEM;
    } else {
        for (guint i = 1; i < G_N_ELEMENTS(lang_presets); i++)
            if (g_str_equal(s, lang_presets[i].langs)) sel = lang_presets[i].id;
    }
    gtk_entry_set_text(GTK_ENTRY(langs_entry), *s ? s : sys_str);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(lang_combo), sel);
    gtk_editable_set_editable(GTK_EDITABLE(langs_entry), g_str_equal(sel, LANG_CUSTOM));
    g_signal_connect(lang_combo, "changed", G_CALLBACK(lang_combo_changed), langs_entry);
    g_free(sys_str);
    g_free(s);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Web Pages"));

    /* Notifications */
    g = page_grid();
    r = 0;
    gtk_grid_attach(GTK_GRID(g), hint_label("Notifications from web pages are collected under the bell next to the "
                                      "address bar. They only arrive while the page is open in a tab."), 0, r++, 3, 1);
    checks = g_list_append(checks, check(g, r++, "Also show them as _desktop notifications", "notify_desktop"));
    section(g, r++, "Sites allowed to show notifications");
    gtk_grid_attach(GTK_GRID(g), hint_label("One site per line, e.g. https://mail.example.com. A site that asks is "
                                      "added here or below by your answer; delete its line to be asked again."), 0, r++, 3, 1);
    s = pref_get_string("notify_allowed");
    GtkWidget *nt_allow = text_area(s, TRUE, 70);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), nt_allow, 0, r++, 3, 1);
    section(g, r++, "Sites not allowed");
    s = pref_get_string("notify_denied");
    GtkWidget *nt_deny = text_area(s, TRUE, 70);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), nt_deny, 0, r++, 3, 1);
    section(g, r++, "Tray icon");
    checks = g_list_append(checks, check(g, r++, "Show an icon in the system _tray", "tray_icon"));
    checks = g_list_append(checks, check(g, r++, "_Blink it while there are new notifications", "tray_blink"));
    checks = g_list_append(checks, check(g, r++, "_Keep running in the tray when the last window is closed",
                                         "close_to_tray"));
    gtk_grid_attach(GTK_GRID(g), hint_label("Pages stay open in the background, so they can still send notifications. "
                                      "Quit with Ctrl+Q or from the tray icon's menu."), 0, r++, 3, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Notifications"));

    /* Advanced */
    g = page_grid();
    r = 0;
    section(g, r++, "Browsing");
    checks = g_list_append(checks, check(g, r++, "Enable _mouse gestures", "mouse_gestures"));
    section(g, r++, "Performance");
    gtk_grid_attach(GTK_GRID(g), row_label("_Hardware acceleration:"), 0, r, 1, 1);
    GtkWidget *hw = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hw), "default", "Default");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hw), "always", "Always");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hw), "never", "Never");
    s = pref_get_string("hw_accel");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(hw), s)) gtk_combo_box_set_active(GTK_COMBO_BOX(hw), 0);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), hw, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(g), row_label("_Graphics memory use:"), 0, r, 1, 1);
    GtkWidget *gm = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(gm), "auto",
        web_gpu_small ? "Automatic (low on this graphics card)" : "Automatic (normal on this graphics card)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(gm), "normal", "Normal");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(gm), "low", "Low (pages and video drawn by the CPU)");
    s = pref_get_string("gpu_memory");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(gm), s)) gtk_combo_box_set_active(GTK_COMBO_BOX(gm), 0);
    g_free(s);
    gtk_widget_set_tooltip_text(gm, "Low keeps video memory free for the desktop and other programs. "
                                    "Use it with older or 1 GB graphics cards. Takes effect after a restart.");
    gtk_grid_attach(GTK_GRID(g), gm, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(g), row_label("_Video decoding:"), 0, r, 1, 1);
    GtkWidget *vd = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(vd), "auto",
        web_gpu_small ? "Automatic (software on this graphics card)"
                                                 : "Automatic (system default on this graphics card)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(vd), "software", "Software (FFmpeg/libav, libvpx; uses the CPU)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(vd), "hardware", "Hardware (VA-API; uses the graphics card)");
    s = pref_get_string("video_decoding");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(vd), s)) gtk_combo_box_set_active(GTK_COMBO_BOX(vd), 0);
    g_free(s);
    gtk_widget_set_tooltip_text(vd, "Which decoders play video. Software works with every graphics card; hardware "
                                    "saves CPU on cards with a good video decoder. Takes effect after a restart.");
    gtk_grid_attach(GTK_GRID(g), vd, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(g), row_label("_Software decoders:"), 0, r, 1, 1);
    GtkWidget *dl = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dl), "auto", "Automatic (GStreamer's choice)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dl), "ffmpeg", "FFmpeg first");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dl), "native", "Codec libraries first (libvpx, dav1d, OpenH264...)");
    s = pref_get_string("decoder_library");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(dl), s)) gtk_combo_box_set_active(GTK_COMBO_BOX(dl), 0);
    g_free(s);
    gtk_widget_set_tooltip_text(dl, "Which CPU decoders play a format when several are installed: FFmpeg "
                                    "(gstreamer1.0-libav) or each codec's own library. The other kind stays "
                                    "available as a fallback. Takes effect after a restart.");
    gtk_grid_attach(GTK_GRID(g), dl, 1, r++, 1, 1);
    section(g, r++, "Privacy");
    GtkWidget *pd = gtk_button_new_with_mnemonic("_Delete Private Data...");
    gtk_widget_set_halign(pd, GTK_ALIGN_START);
    g_signal_connect(pd, "clicked", G_CALLBACK(open_private_data), d);
    gtk_grid_attach(GTK_GRID(g), pd, 0, r++, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Advanced"));

    /* Ad Blocking */
    g = page_grid();
    r = 0;
    GtkWidget *ab_on = gtk_check_button_new_with_mnemonic("_Block ads");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ab_on), pref_get_bool("adblock"));
    gtk_grid_attach(GTK_GRID(g), ab_on, 0, r++, 3, 1);
    section(g, r++, "Filter lists");
    gtk_grid_attach(GTK_GRID(g), hint_label("Addresses of lists in Adblock Plus format, one per line, e.g. EasyList or a "
                                      "list for your language. They are downloaded again every 4 days."), 0, r++, 3, 1);
    s = pref_get_string("adblock_lists");
    GtkWidget *ab_lists = text_area(s, TRUE, 60);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), ab_lists, 0, r++, 3, 1);
    GtkWidget *ab_status = gtk_label_new(adblock_status());
    gtk_label_set_xalign(GTK_LABEL(ab_status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(ab_status), TRUE);
    gtk_widget_set_hexpand(ab_status, TRUE);
    GtkWidget *ab_update = gtk_button_new_with_mnemonic("_Update Now");
    gtk_widget_set_halign(ab_update, GTK_ALIGN_END);
    gtk_widget_set_valign(ab_update, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(ab_status), "button", ab_update);
    gtk_widget_set_sensitive(ab_update, !adblock_updating());
    g_signal_connect(ab_update, "clicked", G_CALLBACK(adblock_update_clicked), ab_lists);
    profile_connect(EV_ADBLOCK, adblock_status_changed, ab_status);
    g_signal_connect(ab_status, "destroy", G_CALLBACK(adblock_status_gone), NULL);
    gtk_grid_attach(GTK_GRID(g), ab_status, 0, r, 2, 1);
    gtk_grid_attach(GTK_GRID(g), ab_update, 2, r++, 1, 1);
    section(g, r++, "Sites where ads are allowed");
    gtk_grid_attach(GTK_GRID(g), hint_label("One site per line; its subdomains are included. A page's right-click menu "
                                      "also has \u201cAllow Ads on This Site\u201d."), 0, r++, 3, 1);
    s = pref_get_string("adblock_allow");
    GtkWidget *ab_allow = text_area(s, TRUE, 50);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), ab_allow, 0, r++, 3, 1);
    section(g, r++, "Your rules");
    gtk_grid_attach(GTK_GRID(g), hint_label("In Adblock Plus format. \u201cBlock Element...\u201d in a page's right-click "
                                      "menu adds a line here (site##element); delete a line to show it again."), 0, r++, 3, 1);
    s = adblock_get_rules();
    GtkWidget *ab_rules = text_area(s, TRUE, 80);
    g_free(s);
    gtk_grid_attach(GTK_GRID(g), ab_rules, 0, r++, 3, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Ad Blocking"));

    gtk_widget_show_all(d);
    /* quitting while the dialog is open destroys it with its window: notice that */
    g_object_add_weak_pointer(G_OBJECT(d), (gpointer *)&d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK && d) {
        {   /* notification sites */
            char *a = text_area_get(nt_allow), *dn = text_area_get(nt_deny);
            notifications_set_sites(a, dn);
            g_free(a); g_free(dn);
        }
        {   /* ad blocking */
            char *t = text_area_get(ab_allow), *c = clean_lines(t);
            adblock_set_allowed_sites(c);
            g_free(t); g_free(c);
            t = text_area_get(ab_rules);
            adblock_set_rules(t);
            g_free(t);
            t = text_area_get(ab_lists);
            c = clean_lines(t);
            char *old = pref_get_string("adblock_lists");
            gboolean lists_changed = g_strcmp0(old, c) != 0;
            pref_set_string("adblock_lists", c);
            g_free(old); g_free(t); g_free(c);
            gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ab_on));
            if (on != pref_get_bool("adblock")) adblock_set_enabled(on);
            if (lists_changed && on) adblock_update_lists();
        }
        pref_set_string("startup", gtk_combo_box_get_active_id(GTK_COMBO_BOX(startup)));
        char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dldir));
        if (dir) pref_set_string("download_dir", dir);
        g_free(dir);
        for (GList *l = checks; l; l = l->next)
            pref_set_bool(g_object_get_data(G_OBJECT(l->data), "key"),
                          gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(l->data)));
        pref_set_int("default_zoom", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(zoom)));
        {   /* default preset is stored as "" so future default updates apply automatically */
            const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ua_combo));
            char *val = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(ua))));
            if (!id || g_str_equal(id, ua_presets[0].id)) pref_set_string("user_agent", "");
            else pref_set_string("user_agent", val);
            g_free(val);
        }
        {   /* system default is stored as "" so it keeps following the system */
            const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(lang_combo));
            char *l = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(langs_entry))));
            pref_set_string("accept_languages", !id || g_str_equal(id, LANG_SYSTEM) ? "" : l);
            g_free(l);
            web_apply_languages();
        }
        pref_set_string("hw_accel", gtk_combo_box_get_active_id(GTK_COMBO_BOX(hw)));
        pref_set_string("gpu_memory", gtk_combo_box_get_active_id(GTK_COMBO_BOX(gm)));
        pref_set_string("video_decoding", gtk_combo_box_get_active_id(GTK_COMBO_BOX(vd)));
        pref_set_string("decoder_library", gtk_combo_box_get_active_id(GTK_COMBO_BOX(dl)));
        const char *dn = gtk_combo_box_get_active_id(GTK_COMBO_BOX(def));
        char *defname = g_strdup(dn ? dn : "");
        engines_store_save(se_store);
        if (*defname) pref_set_string("search_engine", defname);
        g_free(defname);
        web_apply_settings();
        prefs_save();
        tray_update();
    }
    g_list_free(checks);
    g_object_unref(se_store);
    if (d) {
        g_object_remove_weak_pointer(G_OBJECT(d), (gpointer *)&d);
        gtk_widget_destroy(d);
    }
}

/* ---------------------------------------------------------------- */

void private_data_dialog_show(GtkWindow *parent)
{
    GtkWidget *d = gtk_dialog_new_with_buttons("Delete Private Data", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Delete", GTK_RESPONSE_OK, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g_object_set(box, "margin", 14, NULL);
    GtkWidget *l = gtk_label_new("Choose what to delete:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    GtkWidget *hist = gtk_check_button_new_with_mnemonic("Browsing _history and closed tabs");
    GtkWidget *cook = gtk_check_button_new_with_mnemonic("_Cookies");
    GtkWidget *cache = gtk_check_button_new_with_mnemonic("C_ache");
    GtkWidget *other = gtk_check_button_new_with_mnemonic("_Local storage and site databases");
    GtkWidget *dl = gtk_check_button_new_with_mnemonic("_Finished transfers list");
    /* everything is selected to start with; untick what should stay */
    GtkWidget *all[] = { hist, cook, cache, other, dl };
    for (guint i = 0; i < G_N_ELEMENTS(all); i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(all[i]), TRUE);
    gtk_box_pack_start(GTK_BOX(box), hist, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cook, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cache, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), other, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    g_object_add_weak_pointer(G_OBJECT(d), (gpointer *)&d);   /* quitting destroys it with its window */
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK && d) {
        web_clear_private_data(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hist)),
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cook)),
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cache)),
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(other)));
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dl))) transfers_clear_finished();
    }
    if (d) {
        g_object_remove_weak_pointer(G_OBJECT(d), (gpointer *)&d);
        gtk_widget_destroy(d);
    }
}

/* ---------------------------------------------------------------- */

void bookmark_edit_dialog(GtkWindow *parent, const char *url, const char *title, gboolean is_new)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(is_new ? "Add Bookmark" : "Bookmark Properties", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
    if (!is_new) gtk_dialog_add_button(GTK_DIALOG(d), "_Delete", 1);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(d), 460, -1);

    const char *folder = "";
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        if (g_strcmp0(b->url, url) == 0) { folder = b->folder; if (!is_new) title = b->title; }
    }

    GtkWidget *g = page_grid();
    gtk_grid_attach(GTK_GRID(g), row_label("_Name:"), 0, 0, 1, 1);
    GtkWidget *en = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(en), title ? title : "");
    gtk_entry_set_activates_default(GTK_ENTRY(en), TRUE);
    gtk_widget_set_hexpand(en, TRUE);
    gtk_grid_attach(GTK_GRID(g), en, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), row_label("_Address:"), 0, 1, 1, 1);
    GtkWidget *eu = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(eu), url ? url : "");
    gtk_entry_set_activates_default(GTK_ENTRY(eu), TRUE);
    gtk_grid_attach(GTK_GRID(g), eu, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), row_label("_Folder:"), 0, 2, 1, 1);
    GtkWidget *ef = gtk_combo_box_text_new_with_entry();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < profile->bookmarks->len; i++) {
        Bookmark *b = profile->bookmarks->pdata[i];
        if (*b->folder && !g_hash_table_contains(seen, b->folder)) {
            g_hash_table_add(seen, b->folder);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ef), b->folder);
        }
    }
    g_hash_table_destroy(seen);
    GtkWidget *fe = gtk_bin_get_child(GTK_BIN(ef));
    gtk_entry_set_text(GTK_ENTRY(fe), folder);
    gtk_entry_set_placeholder_text(GTK_ENTRY(fe), "(top level)");
    gtk_entry_set_activates_default(GTK_ENTRY(fe), TRUE);
    gtk_grid_attach(GTK_GRID(g), ef, 1, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), g, TRUE, TRUE, 0);
    gtk_widget_show_all(d);

    char *old = g_strdup(url);
    g_object_add_weak_pointer(G_OBJECT(d), (gpointer *)&d);   /* quitting destroys it with its window */
    int r = gtk_dialog_run(GTK_DIALOG(d));
    if (r == GTK_RESPONSE_OK && d) {
        char *nu = browser_resolve_input(gtk_entry_get_text(GTK_ENTRY(eu)));
        const char *nt = gtk_entry_get_text(GTK_ENTRY(en));
        char *nf = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(fe))));
        if (nu) {
            if (bookmark_exists(old)) bookmark_update(old, nu, nt, nf);
            else bookmark_add(nu, nt, nf);
        }
        g_free(nu);
        g_free(nf);
    } else if (r == 1) {
        bookmark_remove(old);
    }
    g_free(old);
    if (d) {
        g_object_remove_weak_pointer(G_OBJECT(d), (gpointer *)&d);
        gtk_widget_destroy(d);
    }
}
