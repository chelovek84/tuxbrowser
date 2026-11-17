/* Tux Browser - a fast, lightweight WebKitGTK web browser. */
#ifndef TUXBROWSER_H
#define TUXBROWSER_H

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#define APP_NAME     "Tux Browser"
#define APP_ID       "org.tuxbrowser.Browser"
#define APP_VERSION  "1.0"
#define START_URI "tuxbrowser://start"
/* Sent when no custom user agent is set: current Safari on macOS. The engine is WebKit, so
 * sites send code written and tested for it, and the identity matches what the engine
 * really supports (no Chrome client hints that bot checks would find missing). Sites still
 * serve their full desktop version, unlike for an "unknown Safari on Linux". */
#define DEFAULT_USER_AGENT "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Safari/605.1.15"

/* ------------------------------------------------------------------ */
/* profile.c : settings, bookmarks, history, session */

typedef enum {
    EV_SETTINGS, EV_BOOKMARKS, EV_HISTORY,
    EV_CLOSED_TABS, EV_DOWNLOADS, EV_ADBLOCK,
    EV_LAST
} ProfileEvent;

typedef void (*ProfileCb)(gpointer data);

typedef struct {
    char *url;
    char *title;
    char *folder;      /* "" = top level */
} Bookmark;

typedef struct {
    char *url;
    char *title;
} Dial;

typedef struct {
    char *url;
    char *title;
    gint64 last_visit;   /* unix seconds */
    int visits;
} HistoryItem;

typedef struct {
    char *name;
    char *keyword;
    char *url;     /* contains %s */
} SearchEngine;

typedef struct {
    char *config_dir, *data_dir, *cache_dir;
    GKeyFile *settings;
    GPtrArray *bookmarks;    /* Bookmark* */
    GPtrArray *engines;      /* SearchEngine* */
    GPtrArray *closed_tabs;  /* Dial* (url,title), most recent first */
    void *db;                /* sqlite3* */
} Profile;

extern Profile *profile;

void   profile_init(void);
void   profile_shutdown(void);
void   profile_connect(ProfileEvent ev, ProfileCb cb, gpointer data);
void   profile_disconnect_data(gpointer data);
void   profile_emit(ProfileEvent ev);

char    *pref_get_string(const char *key);
gboolean pref_get_bool(const char *key);
int      pref_get_int(const char *key);
void     pref_set_string(const char *key, const char *val);
void     pref_set_bool(const char *key, gboolean val);
void     pref_set_int(const char *key, int val);
void     prefs_save(void);

gboolean  bookmark_exists(const char *url);
void      bookmark_add(const char *url, const char *title, const char *folder);
void      bookmark_remove(const char *url);
void      bookmark_update(const char *old_url, const char *url, const char *title, const char *folder);
void      bookmarks_save(void);



void      history_add(const char *url, const char *title);
void      history_set_title(const char *url, const char *title);
GPtrArray *history_query(const char *text, int limit);   /* HistoryItem*, free with g_ptr_array_unref */
GPtrArray *history_complete(const char *text, int limit);
void      history_delete(const char *url);  /* NULL = all */

SearchEngine *search_engine_default(void);
SearchEngine *search_engine_by_name(const char *name);
char     *search_url_for(SearchEngine *e, const char *query);

void      closed_tab_push(const char *url, const char *title);

void      session_save(void);
gboolean  session_restore(void);

/* ------------------------------------------------------------------ */
/* window.c / tab.c                                                    */

typedef struct _BrowserWindow BrowserWindow;
typedef struct _Tab Tab;

struct _Tab {
    BrowserWindow *win;
    WebKitWebView *view;
    WebKitUserContentManager *ucm;
    GtkWidget *nb_child;     /* empty widget living in the tab-bar notebook */
    GtkWidget *label_box, *icon, *spinner, *label, *close_btn;
    char *requested;         /* last URI passed to tab_load (until WebKit reports one) */
    gboolean replacing;      /* adopted spare is being redirected to "requested" */
    char *typed;             /* text typed in the address bar but not yet submitted */
    char *hover_uri;
    double progress;
    /* mouse gestures */
    gboolean g_down, g_passthrough, g_done;
    double g_x, g_y;
    GString *g_seq;
    GdkEvent *g_press;
};

struct _BrowserWindow {
    GtkApplicationWindow *window;
    GtkWidget *notebook;          /* acts as tab bar */
    GtkWidget *menu_button, *trash_button, *newtab_button;
    GtkWidget *toolbar;
    GtkWidget *back_btn, *fwd_btn, *reload_btn, *zoom_btn, *zoom_btn_label;
    GtkWidget *downloads_btn, *downloads_ring;  /* title bar button; progress ring around its icon */
    double downloads_frac;        /* overall progress 0..1, < 0 while sizes are unknown */
    int downloads_running, downloads_spin;
    GtkWidget *address;
    GtkListStore *completion_store;
    GtkWidget *content_stack;
    GtkWidget *findbar, *find_entry, *find_label, *find_case;
    GtkWidget *status_float, *status_label, *zoom_scale, *zoom_label;
    GMenu *trash_menu;
    gboolean updating_zoom;
    gboolean setting_address;
    gboolean fullscreen;
    guint tabs_refresh_id, status_timeout_id, save_size_id;
    char *temp_status;
    GtkWidget *content_box, *find_revealer;
    GtkWidget *vtabs_revealer, *vtabs_list, *vtabs_button, *vtabs_title, *vtabs_scroll;
    GtkWidget *vstrip, *vstrip_scroll, *vstrip_box;   /* favicon strip shown while the tab bar is collapsed */
    GtkWidget *vstrip_current;    /* strip button of the current tab */
    guint vstrip_refresh_id;
    GtkWidget *vbm_revealer, *vbm_view, *vbm_search, *vbm_button, *vbm_title;
    GtkTreeStore *vbm_store;
    /* drag and drop state */
    Tab *vtabs_drag_tab;
    double vtabs_press_x, vtabs_press_y;
    gboolean vtabs_dragging;
    guint vbm_sync_id;
    GList *tabs;                  /* Tab* in creation order; notebook defines visual order */
};

BrowserWindow *browser_window_new(GtkApplication *app);
BrowserWindow *browser_window_get_active(void);
Tab   *browser_window_current_tab(BrowserWindow *bw);
Tab   *browser_window_open(BrowserWindow *bw, const char *uri, gboolean background);
Tab   *browser_window_add_tab(BrowserWindow *bw, WebKitWebView *related, gboolean background);
void   browser_window_close_tab(BrowserWindow *bw, Tab *tab);
void   browser_window_update_chrome(BrowserWindow *bw);
void   browser_window_set_status(BrowserWindow *bw, const char *text);
GList *browser_window_tabs_in_order(BrowserWindow *bw);
GList *browser_windows(void);
void   browser_window_tab_changed(BrowserWindow *bw, Tab *tab);
char  *browser_resolve_input(const char *text);
void   browser_window_fullscreen(BrowserWindow *bw, gboolean on);
void   browser_window_select_tab(BrowserWindow *bw, Tab *tab);
void   browser_window_cycle_tab(BrowserWindow *bw, int dir);
void   browser_window_tab_context_menu(BrowserWindow *bw, Tab *tab, GdkEventButton *ev);
void   browser_window_flash_status(BrowserWindow *bw, const char *text);
void   browsers_quit(void);

Tab  *tab_new(BrowserWindow *bw, WebKitWebView *related);
Tab  *tab_new_full(BrowserWindow *bw, WebKitWebView *related, gboolean want_start);
void  tab_prepare_spare(guint delay_ms);
void  tab_free(Tab *tab);
void  tab_load(Tab *tab, const char *uri);
void  tab_load_replace(Tab *tab, const char *uri);
const char *tab_get_uri(Tab *tab);
const char *tab_get_title(Tab *tab);
gboolean tab_is_start_page(Tab *tab);

/* ------------------------------------------------------------------ */
/* web.c : shared WebKit context / settings                           */

WebKitWebContext *web_context(void);
WebKitSettings   *web_settings(void);
void              web_init(void);
void              web_apply_settings(void);
void              web_apply_languages(void);
char            **web_system_languages(void);
void              web_clear_private_data(gboolean history, gboolean cookies, gboolean cache, gboolean other);

/* startpage.c */
void start_page_init(WebKitWebContext *ctx);
void start_page_handle_message(Tab *tab, const char *msg);
void start_page_focus(Tab *tab);

/* downloads.c */
typedef struct {
    WebKitDownload *download;
    char *dest;
    char *name;
    guint64 total, received;
    int state;   /* 0 running, 1 done, 2 failed, 3 cancelled */
    char *error;
    gint64 started;
} Transfer;
extern GPtrArray *transfers;
void downloads_init(WebKitWebContext *ctx);
void transfer_cancel(Transfer *t);
void transfer_open(Transfer *t, gboolean folder);
void transfers_clear_finished(void);

/* adblock.c : filter lists, own rules, per-site exceptions, element picker */
void        adblock_init(void);
void        adblock_attach(WebKitUserContentManager *ucm);
void        adblock_page_changed(WebKitUserContentManager *ucm, const char *uri);
void        adblock_set_enabled(gboolean on);
void        adblock_update_lists(void);
gboolean    adblock_updating(void);
const char *adblock_status(void);
char       *adblock_site_for_uri(const char *uri);   /* host without www., NULL if not a web page */
gboolean    adblock_site_is_allowed(const char *uri);
void        adblock_set_site_allowed(const char *uri, gboolean allow);
void        adblock_set_allowed_sites(const char *text);
void        adblock_pick_element(WebKitWebView *view);
char       *adblock_get_rules(void);
void        adblock_set_rules(const char *text);
#define ADBLOCK_DEFAULT_LISTS "https://easylist.to/easylist/easylist.txt"

/* popups.c : History and Downloads popovers */
void history_popup_toggle(BrowserWindow *bw);
void downloads_popup_toggle(BrowserWindow *bw);

/* prefs.c */
void prefs_dialog_show(GtkWindow *parent);
void private_data_dialog_show(GtkWindow *parent);
void bookmark_edit_dialog(GtkWindow *parent, const char *url, const char *title, gboolean is_new);

/* theme.c */
void theme_load(void);
void theme_apply(void);

/* util */
char *format_size(guint64 bytes);
void   watchdog_start(void);
gboolean browser_any_tab_loading(void);
extern gboolean web_low_gpu_memory;   /* low graphics memory mode is active */
extern gboolean web_video_software;   /* software video decoders are preferred */
extern gboolean web_gpu_small;        /* old or 1 GB graphics card detected */
extern const char *web_opt_gpu_memory, *web_opt_video_decoding, *web_opt_decoders;   /* command-line overrides */
extern GPtrArray *web_opt_forced;     /* --decoder=FORMAT:ELEMENT values */
void   media_apply(gboolean small_gpu);
GdkPixbuf *favicon_for_view(WebKitWebView *view, int size);
cairo_surface_t *favicon_surface_for_view(WebKitWebView *view, int size, int scale);
void   browser_window_tab_icon_changed(BrowserWindow *bw, Tab *tab);

#endif
