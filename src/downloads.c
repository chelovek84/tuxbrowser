/* Downloads ("Transfers"). */
#include "tuxbrowser.h"
#include <string.h>
#include <glib/gstdio.h>

GPtrArray *transfers;
static guint tick_id;

static gboolean tick(gpointer data)
{
    gboolean active = FALSE;
    for (guint i = 0; i < transfers->len; i++)
        if (((Transfer *)transfers->pdata[i])->state == 0) active = TRUE;
    profile_emit(EV_DOWNLOADS);
    if (!active) {
        tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void ensure_tick(void)
{
    if (!tick_id) tick_id = g_timeout_add(500, tick, NULL);
}

static Transfer *find_transfer(WebKitDownload *d)
{
    for (guint i = 0; i < transfers->len; i++)
        if (((Transfer *)transfers->pdata[i])->download == d) return transfers->pdata[i];
    return NULL;
}

static char *unique_path(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return path;
    const char *dot = strrchr(name, '.');
    char *base = dot && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
    const char *ext = dot && dot != name ? dot : "";
    for (int i = 1; i < 1000; i++) {
        g_free(path);
        char *n = g_strdup_printf("%s (%d)%s", base, i, ext);
        path = g_build_filename(dir, n, NULL);
        g_free(n);
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) break;
    }
    g_free(base);
    return path;
}

static gboolean decide_destination(WebKitDownload *d, const char *suggested, gpointer data)
{
    Transfer *t = find_transfer(d);
    char *name = g_path_get_basename(suggested && *suggested ? suggested : "download");
    if (!*name || g_str_equal(name, ".") || g_str_equal(name, "/")) {
        g_free(name);
        name = g_strdup("download");
    }
    char *dir = pref_get_string("download_dir");
    g_mkdir_with_parents(dir, 0755);
    char *path = NULL;

    if (pref_get_bool("ask_download_dir")) {
        BrowserWindow *bw = browser_window_get_active();
        GtkFileChooserNative *fc = gtk_file_chooser_native_new("Save File",
            bw ? GTK_WINDOW(bw->window) : NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), name);
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT)
            path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        g_object_unref(fc);
        if (!path) {
            webkit_download_cancel(d);
            g_free(dir); g_free(name);
            return TRUE;
        }
    } else {
        path = unique_path(dir, name);
    }
    webkit_download_set_allow_overwrite(d, TRUE);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    webkit_download_set_destination(d, uri);
    if (t) {
        g_free(t->dest); g_free(t->name);
        t->dest = g_strdup(path);
        t->name = g_path_get_basename(path);
    }
    g_free(uri); g_free(path); g_free(dir); g_free(name);
    profile_emit(EV_DOWNLOADS);
    return TRUE;
}

static void received_data(WebKitDownload *d, guint64 len, gpointer data)
{
    Transfer *t = find_transfer(d);
    if (!t) return;
    t->received = webkit_download_get_received_data_length(d);
    WebKitURIResponse *r = webkit_download_get_response(d);
    if (r) t->total = webkit_uri_response_get_content_length(r);
}

static void finished(WebKitDownload *d, gpointer data)
{
    Transfer *t = find_transfer(d);
    if (!t) return;
    if (t->state == 0) {
        t->state = 1;
        t->received = webkit_download_get_received_data_length(d);
        if (!t->total) t->total = t->received;
        BrowserWindow *bw = browser_window_get_active();
        if (bw) {
            char *s = g_strdup_printf("Transfer complete: %s", t->name ? t->name : "");
            browser_window_set_status(bw, s);
            g_free(s);
        }
    }
    profile_emit(EV_DOWNLOADS);
}

static void failed(WebKitDownload *d, GError *err, gpointer data)
{
    Transfer *t = find_transfer(d);
    if (!t) return;
    if (g_error_matches(err, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
        t->state = 3;
    } else {
        t->state = 2;
        g_free(t->error);
        t->error = g_strdup(err->message);
    }
    profile_emit(EV_DOWNLOADS);
}

static void download_started(WebKitWebContext *ctx, WebKitDownload *d, gpointer data)
{
    Transfer *t = g_new0(Transfer, 1);
    t->download = g_object_ref(d);
    t->started = g_get_real_time();
    WebKitURIRequest *rq = webkit_download_get_request(d);
    const char *u = rq ? webkit_uri_request_get_uri(rq) : NULL;
    char *b = u ? g_path_get_basename(u) : g_strdup("download");
    t->name = b;
    g_ptr_array_insert(transfers, 0, t);
    g_signal_connect(d, "decide-destination", G_CALLBACK(decide_destination), NULL);
    g_signal_connect(d, "received-data", G_CALLBACK(received_data), NULL);
    g_signal_connect(d, "finished", G_CALLBACK(finished), NULL);
    g_signal_connect(d, "failed", G_CALLBACK(failed), NULL);
    ensure_tick();
    profile_emit(EV_DOWNLOADS);

    /* say so without taking over the page; the Downloads tab lists it */
    BrowserWindow *bw = browser_window_get_active();
    if (bw) {
        char *msg = g_strdup_printf("Downloading %s", t->name);
        browser_window_flash_status(bw, msg);
        g_free(msg);
    }
}

static void transfer_free(Transfer *t)
{
    if (t->download) g_object_unref(t->download);
    g_free(t->dest); g_free(t->name); g_free(t->error);
    g_free(t);
}

void downloads_init(WebKitWebContext *ctx)
{
    transfers = g_ptr_array_new_with_free_func((GDestroyNotify)transfer_free);
    g_signal_connect(ctx, "download-started", G_CALLBACK(download_started), NULL);
}

void transfer_cancel(Transfer *t)
{
    if (t->state == 0) webkit_download_cancel(t->download);
}

void transfer_open(Transfer *t, gboolean folder)
{
    if (!t->dest) return;
    char *target = folder ? g_path_get_dirname(t->dest) : g_strdup(t->dest);
    char *uri = g_filename_to_uri(target, NULL, NULL);
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_free(target);
}

void transfers_clear_finished(void)
{
    for (guint i = transfers->len; i > 0; i--)
        if (((Transfer *)transfers->pdata[i - 1])->state != 0)
            g_ptr_array_remove_index(transfers, i - 1);
    profile_emit(EV_DOWNLOADS);
}
