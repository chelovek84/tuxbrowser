/* Debug aid: with TUXBROWSER_WATCHDOG=1, report UI-thread stalls (> 250 ms) to stderr
 * with a backtrace of the UI thread taken while it is stuck. */
#include "tuxbrowser.h"
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

static volatile gint64 beat;
static pthread_t ui_thread;
static gint64 t0;

static gboolean heartbeat(gpointer d) { beat = g_get_monotonic_time(); return G_SOURCE_CONTINUE; }

static void dump(int sig)
{
    void *frames[48];
    int n = backtrace(frames, 48);
    static const char hdr[] = "---- UI thread stack ----\n";
    if (write(2, hdr, sizeof hdr - 1) < 0) return;
    backtrace_symbols_fd(frames, n, 2);
}

static gpointer watch(gpointer d)
{
    gint64 reported = 0;
    for (;;) {
        g_usleep(100000);
        gint64 now = g_get_monotonic_time(), b = beat;
        if (now - b > 250000 && reported != b) {
            reported = b;
            fprintf(stderr, "[watchdog] t=%.2f UI thread stalled %lld ms\n", (b - t0) / 1e6, (long long)(now - b) / 1000);
            pthread_kill(ui_thread, SIGUSR2);
        } else if (reported == b && now - b > 1000000 && (now - b) % 1000000 < 100000) {
            fprintf(stderr, "[watchdog] still stalled %lld ms\n", (long long)(now - b) / 1000);
            pthread_kill(ui_thread, SIGUSR2);
        }
    }
    return NULL;
}

void watchdog_start(void)
{
    if (!g_getenv("TUXBROWSER_WATCHDOG")) return;
    ui_thread = pthread_self();
    signal(SIGUSR2, dump);
    t0 = g_get_monotonic_time();
    beat = g_get_monotonic_time();
    g_timeout_add_full(G_PRIORITY_HIGH, 50, heartbeat, NULL, NULL);
    g_thread_unref(g_thread_new("watchdog", watch, NULL));
}
