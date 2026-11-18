/* Shared WebKit context, settings and helpers. */
#include "tuxbrowser.h"
#include <string.h>

static WebKitWebContext *ctx;
static WebKitSettings *settings;

WebKitWebContext *web_context(void) { return ctx; }
WebKitSettings *web_settings(void) { return settings; }

void web_apply_settings(void)
{
    webkit_settings_set_auto_load_images(settings, pref_get_bool("load_images"));
    webkit_settings_set_enable_javascript(settings, pref_get_bool("enable_javascript"));
    webkit_settings_set_enable_smooth_scrolling(settings, pref_get_bool("smooth_scrolling"));

    char *ua = pref_get_string("user_agent");
    webkit_settings_set_user_agent(settings, *ua ? ua : DEFAULT_USER_AGENT);
    g_free(ua);

    char *hw = pref_get_string("hw_accel");
    if (g_str_equal(hw, "never"))
        webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    else if (g_str_equal(hw, "always"))
        webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
    g_free(hw);
}

/* ---------------------------------------------------------------- */
/* languages (Accept-Language, navigator.languages, spell checking) */

static gboolean lang_seen(GPtrArray *a, const char *tag)
{
    for (guint i = 0; i < a->len; i++)
        if (g_ascii_strcasecmp(a->pdata[i], tag) == 0) return TRUE;
    return FALSE;
}

/* Turns a locale name ("de_DE.UTF-8@euro") into a language tag ("de-DE") and adds it
 * once. Each regional tag is followed by its base language ("de-DE" then "de"). */
static void lang_add(GPtrArray *a, const char *name)
{
    char *t = g_strndup(name, strcspn(name, ".@"));
    g_strstrip(t);
    g_strdelimit(t, "_", '-');
    if (*t && g_ascii_strcasecmp(t, "C") != 0 && g_ascii_strcasecmp(t, "POSIX") != 0 && !lang_seen(a, t)) {
        g_ptr_array_add(a, g_strdup(t));
        char *dash = strchr(t, '-');
        if (dash) {
            *dash = 0;
            if (!lang_seen(a, t)) g_ptr_array_add(a, g_strdup(t));
        }
    }
    g_free(t);
}

/* The system's languages in preference order (LANGUAGE, then LC_ALL/LC_MESSAGES/LANG). */
char **web_system_languages(void)
{
    GPtrArray *a = g_ptr_array_new();
    for (const char *const *n = g_get_language_names(); n && *n; n++)
        lang_add(a, *n);
    if (!a->len) {                        /* "C" locale: fall back to English */
        g_ptr_array_add(a, g_strdup("en-US"));
        g_ptr_array_add(a, g_strdup("en"));
    }
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

/* Applies the "accept_languages" preference (comma separated); empty means the system's. */
void web_apply_languages(void)
{
    char *pref = pref_get_string("accept_languages");
    char **langs;
    if (*g_strstrip(pref)) {
        GPtrArray *a = g_ptr_array_new();
        char **parts = g_strsplit(pref, ",", -1);
        for (char **p = parts; *p; p++) lang_add(a, *p);
        g_strfreev(parts);
        g_ptr_array_add(a, NULL);
        langs = (char **)g_ptr_array_free(a, FALSE);
    } else {
        langs = web_system_languages();
    }
    g_free(pref);
    if (!langs[0]) {
        g_strfreev(langs);
        langs = web_system_languages();
    }
    webkit_web_context_set_preferred_languages(ctx, (const char *const *)langs);

    /* spell checker wants locale-style names ("en_US"); use the first language */
    char *sp = g_strdup(langs[0]);
    g_strdelimit(sp, "-", '_');
    const char *spl[] = { sp, NULL };
    webkit_web_context_set_spell_checking_languages(ctx, spl);
    g_free(sp);
    g_strfreev(langs);
}

/* True for GPUs that struggle with WebKit's GPU paths: the legacy "radeon" kernel
 * driver (HD 2000-7000 series), or any card with 1 GB of video memory or less. */
static gboolean gpu_is_small(void)
{
    gboolean small = FALSE;
    GDir *d = g_dir_open("/sys/class/drm", 0, NULL);
    const char *n;
    while (d && !small && (n = g_dir_read_name(d))) {
        if (!g_str_has_prefix(n, "card") || strchr(n, '-')) continue;   /* skip connectors */
        char *link = g_strdup_printf("/sys/class/drm/%s/device/driver", n);
        char *target = g_file_read_link(link, NULL);
        if (target && g_str_equal(strrchr(target, '/') ? strrchr(target, '/') + 1 : target, "radeon")) small = TRUE;
        g_free(target);
        g_free(link);
        char *vram = g_strdup_printf("/sys/class/drm/%s/device/mem_info_vram_total", n);
        char *txt = NULL;
        if (g_file_get_contents(vram, &txt, NULL, NULL) && g_ascii_strtoull(txt, NULL, 10) > 0
            && g_ascii_strtoull(txt, NULL, 10) <= 1024ull * 1024 * 1024)
            small = TRUE;
        g_free(txt);
        g_free(vram);
    }
    if (d) g_dir_close(d);
    return small;
}

/* Command-line overrides for this run (NULL: use the preference). */
const char *web_opt_gpu_memory;

/* Must run before the first web process starts (they inherit the environment). In
 * low mode pages are painted by the CPU and video frames don't go through GL, which
 * keeps video memory free for the desktop and other programs (on a 1 GB card, running
 * out shows up as frozen windows, corrupted video and blank video in other browsers).
 * Compositing stays on. Variables already set by the user win. */
gboolean web_low_gpu_memory;

static void apply_gpu_memory_mode(gboolean small)
{
    char *mode = web_opt_gpu_memory ? g_strdup(web_opt_gpu_memory) : pref_get_string("gpu_memory");
    web_low_gpu_memory = g_str_equal(mode, "low") || (g_str_equal(mode, "auto") && small);
    g_free(mode);
    if (!web_low_gpu_memory) return;
    g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", FALSE);
    g_setenv("WEBKIT_GST_DISABLE_GL_SINK", "1", FALSE);
}

gboolean web_gpu_small;

void web_init(void)
{
    gboolean small = web_gpu_small = gpu_is_small();
    apply_gpu_memory_mode(small);
    media_apply(small);   /* which decoders play media; prints the setup for command-line options */
    char *data = g_build_filename(profile->data_dir, "website-data", NULL);
    char *cache = g_build_filename(profile->cache_dir, "website-data", NULL);
    WebKitWebsiteDataManager *mgr = webkit_website_data_manager_new(
        "base-data-directory", data, "base-cache-directory", cache, NULL);
    ctx = webkit_web_context_new_with_website_data_manager(mgr);
    g_object_unref(mgr);
    g_free(data);
    g_free(cache);

    char *cookies = g_build_filename(profile->data_dir, "cookies.sqlite", NULL);
    WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(ctx);
    webkit_cookie_manager_set_persistent_storage(cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    g_free(cookies);

    char *fav = g_build_filename(profile->cache_dir, "favicons", NULL);
    webkit_web_context_set_favicon_database_directory(ctx, fav);
    g_free(fav);

    webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    webkit_web_context_set_spell_checking_enabled(ctx, TRUE);
    web_apply_languages();

    settings = webkit_settings_new();
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_back_forward_navigation_gestures(settings, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_fullscreen(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_default_charset(settings, "utf-8");
    web_apply_settings();

    start_page_init(ctx);
    downloads_init(ctx);
    notifications_init(ctx);
}

static void clear_done(GObject *src, GAsyncResult *res, gpointer data)
{
    webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(src), res, NULL);
}

void web_clear_private_data(gboolean history, gboolean cookies, gboolean cache, gboolean other)
{
    WebKitWebsiteDataManager *mgr = webkit_web_context_get_website_data_manager(ctx);
    WebKitWebsiteDataTypes types = 0;
    if (cookies) types |= WEBKIT_WEBSITE_DATA_COOKIES | WEBKIT_WEBSITE_DATA_HSTS_CACHE;
    if (cache)   types |= WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE
                        | WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE | WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS
                        | WEBKIT_WEBSITE_DATA_DOM_CACHE;
    if (other)   types |= WEBKIT_WEBSITE_DATA_LOCAL_STORAGE | WEBKIT_WEBSITE_DATA_SESSION_STORAGE
                        | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES | WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT
                        | WEBKIT_WEBSITE_DATA_ITP;
    if (types)
        webkit_website_data_manager_clear(mgr, types, 0, NULL, clear_done, NULL);
    if (cache)
        webkit_web_context_clear_cache(ctx);
    if (history) {
        history_delete(NULL);
        g_ptr_array_set_size(profile->closed_tabs, 0);
        profile_emit(EV_CLOSED_TABS);
    }
}

/* ---------------------------------------------------------------- */

char *format_size(guint64 bytes)
{
    return g_format_size_full(bytes, G_FORMAT_SIZE_DEFAULT);
}

GdkPixbuf *favicon_for_view(WebKitWebView *view, int size)
{
    cairo_surface_t *s = webkit_web_view_get_favicon(view);
    if (!s) return NULL;
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    if (w <= 0 || h <= 0) return NULL;
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(s, 0, 0, w, h);
    if (!pb) return NULL;
    if (w != size || h != size) {
        GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, size, size, GDK_INTERP_BILINEAR);
        g_object_unref(pb);
        pb = sc;
    }
    return pb;
}

/* The favicon as a size x size (logical pixels) surface for a display with the given
 * scale factor, drawn from the largest image WebKit has with good filtering. */
cairo_surface_t *favicon_surface_for_view(WebKitWebView *view, int size, int scale)
{
    cairo_surface_t *s = webkit_web_view_get_favicon(view);
    if (!s || cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE) return NULL;
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    if (w <= 0 || h <= 0) return NULL;
    if (scale < 1) scale = 1;
    int px = size * scale;
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(out);
    double f = (double)px / MAX(w, h);
    cairo_translate(cr, (px - w * f) / 2, (px - h * f) / 2);   /* keep the aspect ratio */
    cairo_scale(cr, f, f);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_set_device_scale(out, scale, scale);
    return out;
}
