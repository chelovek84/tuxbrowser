/* Which decoders play media.
 *
 * WebKitGTK plays all media with GStreamer, which picks, for each format, the decoder
 * with the highest rank. Tux Browser changes those ranks for its web processes through
 * GST_PLUGIN_FEATURE_RANK (they inherit the environment), so this must run before the
 * first web process starts. Three independent choices are applied, in this order:
 *
 *   video decoding   auto | software | hardware   hardware (VA-API...) vs CPU decoders
 *   decoder library  auto | ffmpeg | native       among CPU decoders: FFmpeg (gst-libav)
 *                                                 vs the codecs' own libraries (libvpx,
 *                                                 libaom, dav1d, OpenH264, libde265...)
 *   forced decoders  FORMAT:ELEMENT (command line only, for testing)
 *
 * Nothing is hard-coded: the decoders, the formats they take and whether they are
 * hardware come from GStreamer's registry. GStreamer is loaded at run time, so building
 * Tux Browser needs no GStreamer headers. The user's own GST_PLUGIN_FEATURE_RANK wins. */
#include "tuxbrowser.h"
#include <dlfcn.h>
#include <string.h>

/* GStreamer's ranks */
#define RANK_MARGINAL  64
#define RANK_SECONDARY 128
#define RANK_PRIMARY   256

const char *web_opt_video_decoding, *web_opt_decoders;   /* command-line overrides */
GPtrArray *web_opt_forced;                                 /* "format:element" strings */
gboolean web_video_software;
static gboolean mode_hw;           /* hardware decoding preferred */
static const char *library_mode = "auto";

/* the formats web pages use, by short name (as on the command line) */
static const struct { const char *name, *caps, *label; } formats[] = {
    { "h264",   "video/x-h264",   "H.264" },
    { "h265",   "video/x-h265",   "H.265" },
    { "vp8",    "video/x-vp8",    "VP8" },
    { "vp9",    "video/x-vp9",    "VP9" },
    { "av1",    "video/x-av1",    "AV1" },
    { "mpeg",   "video/mpeg",     "MPEG-1/2/4" },
    { "theora", "video/x-theora", "Theora" },
    { "aac",    "audio/mpeg",     "AAC/MP3" },   /* one GStreamer type for both */
    { "mp3",    "audio/mpeg",     "AAC/MP3" },
    { "opus",   "audio/x-opus",   "Opus" },
    { "vorbis", "audio/x-vorbis", "Vorbis" },
    { "flac",   "audio/x-flac",   "FLAC" },
};

typedef struct {
    char *name, *plugin;
    int rank, orig;
    gboolean hw;
    GPtrArray *caps;    /* media types it decodes, e.g. "video/x-h264" */
} Dec;

static GPtrArray *decoders;   /* Dec*, NULL if GStreamer is unavailable */

static void dec_free(Dec *d)
{
    g_free(d->name);
    g_free(d->plugin);
    g_ptr_array_unref(d->caps);
    g_free(d);
}

/* ---- the registry, through a few symbols loaded at run time ---- */

typedef struct { const char *name_template; int direction; } StaticPadTemplateHead;   /* GstStaticPadTemplate */
#define PAD_SINK 2

static gboolean scan_registry(void)
{
    void *lib = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return FALSE;
#define SYM(type, var, name) __typeof__(type) var = (__typeof__(type))dlsym(lib, name); if (!var) return FALSE
    SYM(gboolean (*)(int *, char ***, GError **), init, "gst_init_check");
    SYM(gpointer (*)(void), reg, "gst_registry_get");
    SYM(GList *(*)(gpointer, GType), list, "gst_registry_get_feature_list");
    SYM(GType (*)(void), ftype, "gst_element_factory_get_type");
    SYM(const char *(*)(gpointer, const char *), meta, "gst_element_factory_get_metadata");
    SYM(const GList *(*)(gpointer), templates, "gst_element_factory_get_static_pad_templates");
    SYM(gpointer (*)(gpointer), tcaps, "gst_static_pad_template_get_caps");
    SYM(guint (*)(gpointer), csize, "gst_caps_get_size");
    SYM(gpointer (*)(gpointer, guint), cstruct, "gst_caps_get_structure");
    SYM(const char *(*)(gpointer), sname, "gst_structure_get_name");
    SYM(void (*)(gpointer), unref, "gst_mini_object_unref");
    SYM(char *(*)(gpointer), oname, "gst_object_get_name");
    SYM(guint (*)(gpointer), frank, "gst_plugin_feature_get_rank");
    SYM(const char *(*)(gpointer), fplugin, "gst_plugin_feature_get_plugin_name");
    SYM(void (*)(GList *), lfree, "gst_plugin_feature_list_free");
#undef SYM
    if (!init(NULL, NULL, NULL)) return FALSE;   /* stays loaded: GStreamer can't be unloaded */

    decoders = g_ptr_array_new_with_free_func((GDestroyNotify)dec_free);
    GList *fs = list(reg(), ftype());
    for (GList *l = fs; l; l = l->next) {
        const char *klass = meta(l->data, "klass");
        if (!klass || !strstr(klass, "Decoder") || (!strstr(klass, "Video") && !strstr(klass, "Audio")))
            continue;
        Dec *d = g_new0(Dec, 1);
        d->name = oname(l->data);
        d->plugin = g_strdup(fplugin(l->data) ? fplugin(l->data) : "");
        d->rank = d->orig = frank(l->data);
        d->hw = strstr(klass, "Hardware") != NULL;
        d->caps = g_ptr_array_new_with_free_func(g_free);
        for (const GList *t = templates(l->data); t; t = t->next) {
            if (((StaticPadTemplateHead *)t->data)->direction != PAD_SINK) continue;
            gpointer caps = tcaps(t->data);
            for (guint i = 0; caps && i < csize(caps); i++)
                g_ptr_array_add(d->caps, g_strdup(sname(cstruct(caps, i))));
            if (caps) unref(caps);
        }
        g_ptr_array_add(decoders, d);
    }
    lfree(fs);
    return TRUE;
}

static gboolean dec_takes(Dec *d, const char *caps)
{
    for (guint i = 0; i < d->caps->len; i++)
        if (g_str_equal(d->caps->pdata[i], caps)) return TRUE;
    return FALSE;
}

/* decoders that take part in the choices: real decoders of a web format. The
 * "codecalpha" wrappers only handle VP8/VP9 with transparency and use libvpx inside. */
static gboolean dec_relevant(Dec *d)
{
    if (g_str_equal(d->plugin, "codecalpha") || g_str_equal(d->name, "fakevideodec")) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(formats); i++)
        if (dec_takes(d, formats[i].caps)) return TRUE;
    return FALSE;
}

static gboolean dec_is_ffmpeg(Dec *d) { return g_str_equal(d->plugin, "libav"); }

static Dec *dec_find(const char *name)
{
    for (guint i = 0; decoders && i < decoders->len; i++)
        if (g_str_equal(((Dec *)decoders->pdata[i])->name, name)) return decoders->pdata[i];
    return NULL;
}

/* ---- the three choices ---- */

static void apply_hw_choice(void)
{
    if (!web_video_software && !mode_hw) return;
    for (guint i = 0; i < decoders->len; i++) {
        Dec *d = decoders->pdata[i];
        if (!d->hw || !dec_relevant(d)) continue;
        /* software: only a fallback for formats no CPU decoder handles; hardware: first */
        d->rank = web_video_software ? RANK_MARGINAL : RANK_PRIMARY + 10;
    }
}

/* Among CPU decoders of each format that has both kinds, the preferred kind goes to
 * PRIMARY (keeping their order among themselves) and the other kind below it. Hardware
 * decoders are left where the first choice put them. */
static void apply_library_choice(void)
{
    gboolean want_ffmpeg = g_str_equal(library_mode, "ffmpeg");
    if (!want_ffmpeg && !g_str_equal(library_mode, "native")) return;
    for (guint f = 0; f < G_N_ELEMENTS(formats); f++) {
        gboolean has_ff = FALSE, has_native = FALSE;
        for (guint i = 0; i < decoders->len; i++) {
            Dec *d = decoders->pdata[i];
            if (d->hw || !dec_relevant(d) || !dec_takes(d, formats[f].caps) || d->orig < RANK_MARGINAL) continue;
            if (dec_is_ffmpeg(d)) has_ff = TRUE; else has_native = TRUE;
        }
        if (!has_ff || !has_native) continue;   /* nothing to choose between */
        for (guint i = 0; i < decoders->len; i++) {
            Dec *d = decoders->pdata[i];
            if (d->hw || !dec_relevant(d) || !dec_takes(d, formats[f].caps) || d->orig < RANK_MARGINAL) continue;
            if (dec_is_ffmpeg(d) == want_ffmpeg)
                d->rank = RANK_PRIMARY - 16 + MIN(d->orig, RANK_PRIMARY) / 16;   /* 244..256, order kept */
            else
                d->rank = MIN(d->orig, RANK_SECONDARY);
        }
    }
}

static int format_index(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(formats); i++)
        if (g_ascii_strcasecmp(formats[i].name, name) == 0) return i;
    if (g_ascii_strcasecmp(name, "hevc") == 0) return format_index("h265");
    return -1;
}

static void list_formats_and_decoders(void)
{
    g_printerr("Formats and their installed decoders:\n");
    for (guint f = 0; f < G_N_ELEMENTS(formats); f++) {
        g_printerr("  %-7s", formats[f].name);
        for (guint i = 0; decoders && i < decoders->len; i++) {
            Dec *d = decoders->pdata[i];
            if (dec_relevant(d) && dec_takes(d, formats[f].caps)) g_printerr(" %s", d->name);
        }
        g_printerr("\n");
    }
}

/* --decoder=FORMAT:ELEMENT puts ELEMENT first for FORMAT, every other decoder of it
 * (hardware too) becomes a fallback. Bad values stop the program: this is a test tool. */
static void apply_forced(void)
{
    for (guint k = 0; web_opt_forced && k < web_opt_forced->len; k++) {
        const char *spec = web_opt_forced->pdata[k], *colon = strchr(spec, ':');
        char *fname = colon ? g_strndup(spec, colon - spec) : g_strdup(spec);
        int f = format_index(fname);
        Dec *want = colon ? dec_find(colon + 1) : NULL;
        if (f < 0 || !want || !dec_takes(want, formats[f].caps)) {
            g_printerr("tuxbrowser: --decoder=%s: %s\n", spec,
                       f < 0 ? "unknown format" : !want ? "no such decoder installed" : "that decoder doesn't take this format");
            list_formats_and_decoders();
            exit(2);
        }
        g_free(fname);
        for (guint i = 0; i < decoders->len; i++) {
            Dec *d = decoders->pdata[i];
            if (d != want && dec_takes(d, formats[f].caps) && d->rank > RANK_MARGINAL) d->rank = RANK_MARGINAL;
        }
        want->rank = RANK_PRIMARY + 50;
    }
}

/* the decoder GStreamer will pick for a format: the highest usable rank */
static Dec *chosen(guint f)
{
    Dec *best = NULL;
    for (guint i = 0; i < decoders->len; i++) {
        Dec *d = decoders->pdata[i];
        if (!dec_relevant(d) || !dec_takes(d, formats[f].caps) || d->rank < RANK_MARGINAL) continue;
        /* GStreamer's order: rank, then name */
        if (!best || d->rank > best->rank || (d->rank == best->rank && strcmp(d->name, best->name) < 0)) best = d;
    }
    return best;
}

static const char *library_label(Dec *d)
{
    static const struct { const char *plugin, *label; } names[] = {
        { "libav", "FFmpeg" }, { "vpx", "libvpx" }, { "aom", "libaom" }, { "dav1d", "dav1d" },
        { "rsdav1d", "dav1d" }, { "openh264", "OpenH264" }, { "de265", "libde265" }, { "va", "VA-API" },
        { "vaapi", "VA-API" }, { "faad", "FAAD" }, { "mpg123", "mpg123" }, { "opus", "libopus" },
        { "vorbis", "libvorbis" }, { "flac", "libFLAC" }, { "theora", "libtheora" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++)
        if (g_str_equal(d->plugin, names[i].plugin)) return names[i].label;
    return d->plugin;
}

static void print_summary(void)
{
    g_printerr("tuxbrowser: graphics memory: %s\n", web_low_gpu_memory ? "low (CPU painting, no compositing, no GL video sink)" : "normal");
    g_printerr("tuxbrowser: video decoding: %s; software decoders: %s\n",
               web_video_software ? "software preferred" : mode_hw ? "hardware preferred" : "GStreamer default",
               g_str_equal(library_mode, "ffmpeg") ? "FFmpeg first"
               : g_str_equal(library_mode, "native") ? "codec libraries first" : "GStreamer default");
    if (!decoders) {
        g_printerr("tuxbrowser: GStreamer could not be queried; decoders unchanged\n");
        return;
    }
    const char *last = NULL;
    for (guint f = 0; f < G_N_ELEMENTS(formats); f++) {
        if (last && g_str_equal(last, formats[f].label)) continue;   /* aac and mp3 share a line */
        last = formats[f].label;
        Dec *d = chosen(f);
        g_printerr("tuxbrowser:   %-11s %s%s%s%s\n", formats[f].label, d ? d->name : "(no decoder installed)",
                   d ? " (" : "", d ? library_label(d) : "", d ? (d->hw ? ", hardware)" : ")") : "");
    }
}

void media_apply(gboolean small_gpu)
{
    char *mode = web_opt_video_decoding ? g_strdup(web_opt_video_decoding) : pref_get_string("video_decoding");
    web_video_software = g_str_equal(mode, "software") || (g_str_equal(mode, "auto") && small_gpu);
    mode_hw = g_str_equal(mode, "hardware");
    g_free(mode);
    char *lib = web_opt_decoders ? g_strdup(web_opt_decoders) : pref_get_string("decoder_library");
    library_mode = g_str_equal(lib, "ffmpeg") ? "ffmpeg" : g_str_equal(lib, "native") ? "native" : "auto";
    g_free(lib);

    gboolean verbose = web_opt_video_decoding || web_opt_decoders || web_opt_gpu_memory
                    || (web_opt_forced && web_opt_forced->len);
    gboolean change = web_video_software || mode_hw || !g_str_equal(library_mode, "auto")
                   || (web_opt_forced && web_opt_forced->len);
    /* loading GStreamer costs a little start time, so only when needed */
    if ((change || verbose) && scan_registry()) {
        apply_hw_choice();
        apply_library_choice();
        apply_forced();
        GString *ranks = g_string_new(NULL);
        for (guint i = 0; i < decoders->len; i++) {
            Dec *d = decoders->pdata[i];
            if (d->rank != d->orig) g_string_append_printf(ranks, "%s%s:%d", ranks->len ? "," : "", d->name, d->rank);
        }
        const char *user = g_getenv("GST_PLUGIN_FEATURE_RANK");   /* last, so it wins */
        if (user && *user) g_string_append_printf(ranks, "%s%s", ranks->len ? "," : "", user);
        if (ranks->len) g_setenv("GST_PLUGIN_FEATURE_RANK", ranks->str, TRUE);
        g_string_free(ranks, TRUE);
    } else if (web_opt_forced && web_opt_forced->len) {
        g_printerr("tuxbrowser: GStreamer could not be queried, --decoder can't be applied\n");
        exit(2);
    }
    if (verbose) print_summary();
}
