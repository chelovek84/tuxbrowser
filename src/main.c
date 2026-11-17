/* Tux Browser entry point. */
#include "tuxbrowser.h"
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static void quit_activated(GSimpleAction *a, GVariant *p, gpointer app)
{
    browsers_quit();
}

static void set_accels(GtkApplication *app)
{
    static const struct { const char *action; const char *accels[5]; } map[] = {
        { "win.new-tab",          { "<Control>t", NULL } },
        { "win.new-window",       { "<Control>n", NULL } },
        { "win.close-tab",        { "<Control>w", "<Control>F4", NULL } },
        { "win.reopen-tab",       { "<Control><Shift>t", NULL } },
        { "win.next-tab",         { "<Control>Tab", "<Control>Page_Down", NULL } },
        { "win.prev-tab",         { "<Control><Shift>Tab", "<Control><Shift>ISO_Left_Tab", "<Control>Page_Up", NULL } },
        { "win.open-location",    { "F8", "<Control>l", "<Alt>d", NULL } },
        { "win.focus-search",     { "<Shift>F8", "<Control>e", "<Control>k", NULL } },
        { "win.back",             { "<Alt>Left", "XF86Back", NULL } },
        { "win.forward",          { "<Alt>Right", "XF86Forward", NULL } },
        { "win.reload",           { "F5", "<Control>r", "XF86Reload", NULL } },
        { "win.reload-hard",      { "<Control>F5", "<Control><Shift>r", NULL } },
        { "win.bookmark",         { "<Control>d", NULL } },
        { "win.toggle-panel",     { "F4", NULL } },
        { "win.toggle-vtabs",     { "F9", NULL } },
        { "win.toggle-vbookmarks", { "F7", NULL } },
        { "win.panel-bookmarks",  { "<Control><Shift>b", NULL } },
        { "win.panel-history",    { "<Control><Shift>h", "<Control>h", NULL } },
        { "win.panel-transfers",  { "<Control>j", "<Control><Alt>t", NULL } },
        { "win.panel-notes",      { "<Control><Shift>n", NULL } },
        { "win.find",             { "<Control>f", NULL } },
        { "win.find-next",        { "F3", "<Control>g", NULL } },
        { "win.find-prev",        { "<Shift>F3", "<Control><Shift>g", NULL } },
        { "win.zoom-in",          { "<Control>plus", "<Control>equal", "<Control>KP_Add", NULL } },
        { "win.zoom-out",         { "<Control>minus", "<Control>KP_Subtract", NULL } },
        { "win.zoom-reset",       { "<Control>0", "<Control>KP_0", NULL } },
        { "win.fullscreen",       { "F11", NULL } },
        { "win.view-source",      { "<Control>u", NULL } },
        { "win.inspector",        { "<Control><Shift>i", "F12", NULL } },
        { "win.print",            { "<Control>p", NULL } },
        { "win.save-page",        { "<Control>s", NULL } },
        { "win.open-file",        { "<Control>o", NULL } },
        { "win.preferences",      { "<Control>F12", "<Alt>p", NULL } },
        { "win.private-data",     { "<Control><Shift>Delete", NULL } },
        { "app.quit",             { "<Control>q", NULL } },
    };
    for (guint i = 0; i < G_N_ELEMENTS(map); i++)
        gtk_application_set_accels_for_action(app, map[i].action, map[i].accels);

    for (int i = 1; i <= 9; i++) {
        char *acc[2] = { g_strdup_printf("<Alt>%d", i), NULL };
        char *det = g_strdup_printf("win.select-tab(%d)", i == 9 ? -1 : i - 1);
        gtk_application_set_accels_for_action(app, det, (const char *const *)acc);
        g_free(acc[0]); g_free(det);
    }
}

static gboolean on_unix_signal(gpointer data)
{
    browsers_quit();   /* saves session + settings, app exits when windows are gone */
    return G_SOURCE_REMOVE;
}

static void on_startup(GApplication *app, gpointer data)
{
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGHUP, on_unix_signal, NULL);
    watchdog_start();
    profile_init();
    web_init();
    theme_load();
    static const GActionEntry app_actions[] = { { "quit", quit_activated } };
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), app);
    set_accels(GTK_APPLICATION(app));
    g_object_set(gtk_settings_get_default(), "gtk-menu-images", TRUE, NULL);
}

static gboolean autosave(gpointer data)
{
    if (browser_windows()) session_save();
    return G_SOURCE_CONTINUE;
}

static void open_startup(GtkApplication *app)
{
    char *mode = pref_get_string("startup");
    gboolean restored = FALSE;
    if (g_str_equal(mode, "session")) restored = session_restore();
    if (!restored) {
        BrowserWindow *bw = browser_window_new(app);
        browser_window_open(bw, START_URI, FALSE);
    }
    g_free(mode);
    g_timeout_add_seconds(60, autosave, NULL);
}

static void on_activate(GApplication *app, gpointer data)
{
    if (!browser_windows()) {
        open_startup(GTK_APPLICATION(app));
        return;
    }
    /* launched again while running: new window with the start page */
    BrowserWindow *bw = browser_window_new(GTK_APPLICATION(app));
    browser_window_open(bw, START_URI, FALSE);
    gtk_window_present(GTK_WINDOW(bw->window));
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer data)
{
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    const char *cwd = g_application_command_line_get_cwd(cl);
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (g_application_command_line_get_is_remote(cl))
                g_application_command_line_printerr(cl, "tuxbrowser: already running, %s ignored "
                                                        "(it applies when Tux Browser starts)\n", argv[i]);
            continue;
        }
        char *path = g_path_is_absolute(argv[i]) || !cwd ? g_strdup(argv[i]) : g_build_filename(cwd, argv[i], NULL);
        char *uri = g_file_test(path, G_FILE_TEST_EXISTS) ? g_filename_to_uri(path, NULL, NULL)
                                                         : browser_resolve_input(argv[i]);
        g_free(path);
        if (uri) g_ptr_array_add(uris, uri);
    }
    g_strfreev(argv);
    if (!uris->len) {
        on_activate(app, NULL);
    } else {
        gboolean fresh = browser_windows() == NULL;
        BrowserWindow *bw = fresh ? browser_window_new(GTK_APPLICATION(app)) : browser_window_get_active();
        for (guint i = 0; i < uris->len; i++)
            browser_window_open(bw, uris->pdata[i], i > 0);
        if (fresh) g_timeout_add_seconds(60, autosave, NULL);
        gtk_window_present(GTK_WINDOW(bw->window));
    }
    g_ptr_array_unref(uris);
    return 0;
}

static void on_shutdown(GApplication *app, gpointer data)
{
    profile_shutdown();
}

static const char usage[] =
    "Usage: tuxbrowser [OPTION...] [URL or file...]\n"
    "\n"
    "Options (they apply to this run only and override Preferences; ignored when\n"
    "Tux Browser is already running, unless TUXBROWSER_PROFILE starts a separate one):\n"
    "  --video-decoding=auto|software|hardware\n"
    "        software: FFmpeg/libav and libvpx on the CPU; hardware: VA-API decoders\n"
    "  --decoders=auto|ffmpeg|native\n"
    "        which CPU decoders come first: FFmpeg (gstreamer1.0-libav) or each\n"
    "        codec's own library (libvpx, libaom, dav1d, OpenH264, libde265...)\n"
    "  --decoder=FORMAT:ELEMENT\n"
    "        use ELEMENT first for FORMAT, e.g. h264:openh264dec or vp9:avdec_vp9\n"
    "        (repeatable; formats: h264 h265 vp8 vp9 av1 mpeg theora aac mp3 opus\n"
    "        vorbis flac; a wrong value lists the installed decoders)\n"
    "  --gpu-memory=auto|normal|low\n"
    "        low: pages painted by the CPU, video frames not sent through OpenGL\n"
    "  -h, --help    show this help\n"
    "\n"
    "When one of these options is given, the resulting setup (with the decoder\n"
    "each format will use) is printed to stderr.\n";

/* returns the value of --name=value if arg is that option */
static const char *option_value(const char *arg, const char *name, const char *const *allowed)
{
    size_t n = strlen(name);
    if (strncmp(arg, name, n) || arg[n] != '=') return NULL;
    for (const char *const *a = allowed; *a; a++)
        if (g_str_equal(arg + n + 1, *a)) return *a;
    g_printerr("tuxbrowser: invalid value in %s\n\n%s", arg, usage);
    exit(2);
}

static void parse_options(int argc, char **argv)
{
    static const char *const vd[] = { "auto", "software", "hardware", NULL };
    static const char *const gm[] = { "auto", "normal", "low", NULL };
    static const char *const dl[] = { "auto", "ffmpeg", "native", NULL };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if (a[0] != '-') continue;
        if (g_str_equal(a, "-h") || g_str_equal(a, "--help")) { fputs(usage, stdout); exit(0); }
        if ((v = option_value(a, "--video-decoding", vd))) web_opt_video_decoding = v;
        else if ((v = option_value(a, "--gpu-memory", gm))) web_opt_gpu_memory = v;
        else if ((v = option_value(a, "--decoders", dl))) web_opt_decoders = v;
        else if (g_str_has_prefix(a, "--decoder=") && strchr(a + 10, ':')) {
            if (!web_opt_forced) web_opt_forced = g_ptr_array_new();
            g_ptr_array_add(web_opt_forced, (char *)a + 10);
        }
        else { g_printerr("tuxbrowser: unknown option %s\n\n%s", a, usage); exit(2); }
    }
}

int main(int argc, char **argv)
{
    parse_options(argc, argv);
    g_set_application_name(APP_NAME);
    g_set_prgname("tuxbrowser");
    /* a separate profile is a separate browser: don't hand its URLs to the main instance */
    GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
    if (g_getenv("TUXBROWSER_PROFILE")) flags |= G_APPLICATION_NON_UNIQUE;
    GtkApplication *app = gtk_application_new(APP_ID, flags);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
