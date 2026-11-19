/* Browser chrome theme, applied only to the browser's own widgets. */
#include "tuxbrowser.h"

static const char *CSS =
/* ---- tab bar ---- */
"#tabbar > header.top {"
"  background-image: linear-gradient(#dfe4ea, #c4ccd5);"
"  border: none; padding: 5px 4px 0 4px;"
"  box-shadow: inset 0 -1px #95a1ad;"
"}"
"#tabbar > header.top > tabs { margin: 0; }"
"#tabbar > stack { min-height: 0; }"
"#tabbar > header > tabs > tab {"
"  min-height: 20px; padding: 3px 4px 3px 8px; margin: 0 1px;"
"  border: 1px solid #949fab; border-bottom-width: 0;"
"  border-radius: 5px 5px 0 0;"
"  background-image: linear-gradient(#e9edf1, #cfd6de);"
"  box-shadow: inset 0 1px rgba(255,255,255,.75);"
"  color: #1d2733; outline: none;"
"}"
"#tabbar > header > tabs > tab:hover { background-image: linear-gradient(#f6f8fa, #dde3e9); }"
"#tabbar > header > tabs > tab:checked {"
"  background-image: linear-gradient(#ffffff, #eef1f4);"
"  border-color: #87939f; box-shadow: inset 0 1px #fff;"
"}"
"#tabbar > header > tabs > tab label { color: #1d2733; }"
"#tabbar > header > tabs > tab:checked label { color: #000; }"
"#tabbar > header > arrow { color: #36424e; min-width: 16px; }"
"button.tab-close { padding: 0; min-width: 16px; min-height: 16px; margin: 0; border-radius: 8px;"
"  color: #5b6672; background: none; border: none; box-shadow: none; -gtk-icon-shadow: none; }"
"button.tab-close:hover { background-color: #d8473c; color: #fff; }"
"#newtab-button, #trash-button { padding: 2px 4px; }"
"#newtab-button > image, #trash-button > image { color: #2f3b47; }"
"#newtab-button:hover, #trash-button:hover { background-image: linear-gradient(#f7f9fa, #dfe4e9);"
"  border: 1px solid #949fab; border-radius: 3px; }"

/* ---- main menu (burger) button ----
 * The colour is set only on the button's own icon (child combinator), because the
 * drop-down menu is attached to this button and would otherwise inherit it. */
"#main-menu { padding: 2px 6px; margin: 0 4px 0 0; min-height: 22px; border: 1px solid transparent; border-radius: 3px;"
"  background: none; box-shadow: none; -gtk-icon-shadow: none; }"
"#main-menu > image { color: #2b3846; }"
"#main-menu:hover { border-color: #95a1ad; background-image: linear-gradient(#fbfcfd, #e3e8ed); }"
"#main-menu:checked, #main-menu:active { border-color: #8a96a3; background-image: linear-gradient(#cdd4dc, #e3e8ed);"
"  box-shadow: inset 0 1px 2px rgba(0,0,0,.15); }"

/* ---- address bar ---- */
"#addressbar {"
"  background-image: linear-gradient(#eef1f4, #dce2e8);"
"  border-bottom: 1px solid #a2adb8; padding: 3px 4px;"
"}"
"#addressbar button.nav-button > image { color: #2b3846; }"
"#addressbar button.nav-button:disabled > image { color: #9aa4ae; }"
"#addressbar button.nav-button { padding: 2px 5px; min-height: 24px; border-radius: 3px;"
"  border: 1px solid transparent; background: none; box-shadow: none; }"
"#addressbar button.nav-button:hover { border-color: #95a1ad; background-image: linear-gradient(#fbfcfd, #e3e8ed); }"
"#addressbar button.nav-button:active { background-image: linear-gradient(#d5dce3, #e6ebef); }"
"#addressbar button.nav-button:disabled { color: #9aa4ae; border-color: transparent; background: none; }"
"#address {"
"  background-color: #fff; background-image: none; color: #111;"
"  border: 1px solid #8d99a6; border-radius: 3px; min-height: 24px;"
"  box-shadow: inset 0 1px 2px rgba(0,0,0,.12);"
"}"
"#address:focus { border-color: #4f7fc8; box-shadow: inset 0 1px 2px rgba(0,0,0,.12), 0 0 0 1px rgba(79,127,200,.35); }"
"#address image { color: #4a5866; }"
"#address progress { background-image: linear-gradient(#dbe8f8, #bfd4f0); border: none; border-radius: 2px; box-shadow: none; }"
"#address selection { background-color: #3b73c9; color: #fff; }"

/* ---- bookmarks bar ---- */

/* ---- downloads button: a nav button whose icon gets a progress ring while downloading ---- */
"#downloads-button image, #notify-button image { color: #2b3846; }"
"#addressbar #downloads-button.active image { color: #2f6fd0; }"
/* ---- History and Downloads popups ---- */
".tux-popup label { color: #1d2733; }"
".tux-popup label.dim-label { color: #5b6672; }"
".tux-popup .popup-header button { color: #2b3846; padding: 3px; }"
".tux-popup .popup-title { font-weight: bold; }"

/* ---- find bar & status bar ---- */
"#findbar { background-image: linear-gradient(#e7ebef, #d4dae0); border-top: 1px solid #a2adb8; padding: 3px 6px; }"
"#findbar label, #findbar check + label, #findbar checkbutton label { color: #1d2733; }"
"#status-float { background-color: rgba(244,246,248,.97); border: 1px solid #a2adb8; border-left: none; border-bottom: none;"
"  border-radius: 0 4px 0 0; padding: 2px 8px; }"
"#status-float label { color: #1d2733; }"
"#zoom-button label { font-size: 9pt; margin-left: 2px; }"
"#zoom-popover scale trough { min-height: 4px; }"
"#zoom-label { min-width: 38px; }"
/* ---- tabs merged into the title bar ---- */
"#tabtitlebar {"
"  background-image: linear-gradient(#dfe4ea, #c4ccd5);"
"  border: none; padding: 0 4px 0 0; min-height: 0;"
"  box-shadow: inset 0 -1px #95a1ad, inset 0 1px rgba(255,255,255,.6);"
"}"
"#tabtitlebar:backdrop { background-image: linear-gradient(#e6eaee, #d3d9e0); }"
"#tabtitlebar #tabbar > header.top { background: none; box-shadow: none; padding: 4px 0; }"
"#tabtitlebar #addressbar { background: none; border: none; box-shadow: none; padding: 4px 0; }"
"#tabtitlebar #tabbar > header > tabs > tab { border-bottom-width: 1px; border-radius: 5px; margin: 0 1px; }"
"#tabtitlebar #main-menu { margin: 0 4px 0 0; }"
"#tabtitlebar button.titlebutton { color: #2b3846; padding: 2px; min-height: 20px; min-width: 20px;"
"  margin: 0 0 0 2px; border: 1px solid transparent; border-radius: 3px; background: none; box-shadow: none; }"
"#tabtitlebar > box { margin: 0; }"
"#tabtitlebar button.titlebutton:hover { border-color: #95a1ad; background-image: linear-gradient(#fbfcfd, #e3e8ed); }"
"#tabtitlebar button.titlebutton.close:hover { border-color: #9b2a21; background-image: linear-gradient(#ee5b4f, #c9291e); color: #fff; }"
"#tabtitlebar separator.titlebutton { min-width: 0; margin: 0; background: none; }"
/* ---- vertical tab bar (overlay on the left) ---- */
/* tab bar toggle: the window's top-left corner, full height, as wide as the favicon strip */
"#toolrow #vtabs-button { padding: 0; margin: 0; border: none; border-radius: 0;"
"  background: none; box-shadow: none; }"
"#toolrow #vtabs-button:hover { background-image: linear-gradient(#fbfcfd, #e3e8ed); }"
"#toolrow #vtabs-button:checked { background-image: linear-gradient(#cdd4dc, #e3e8ed); box-shadow: inset 0 1px 2px rgba(0,0,0,.15); }"
"#vtabs-button > image { color: #2f3b47; -gtk-icon-transform: scaleX(-1); }"   /* drawn for a right-hand bar; ours is on the left */
"#toolrow { background-image: linear-gradient(#eef1f4, #dce2e8); border-bottom: 1px solid #a2adb8; }"
"#toolrow > #addressbar { background: none; border-bottom: none; }"
"#tabtitlebar #toolrow { background: none; border-bottom: none; }"
"#vtabs-button:checked { background-image: linear-gradient(#cdd4dc, #e3e8ed); border: 1px solid #8a96a3; border-radius: 3px;"
"  box-shadow: inset 0 1px 2px rgba(0,0,0,.15); }"
"#vtabs, #vtabs scrolledwindow, #vtabs viewport, #vtabs list { background-color: #eef1f4; }"
"#vtabs { border-right: 1px solid #8f9aa6;"
"  box-shadow: 3px 0 10px rgba(0,0,0,.22); }"
"#vtabs .vtabs-header { background-image: linear-gradient(#e9edf1, #d5dbe2); border-bottom: 1px solid #a2adb8; padding: 3px 4px 3px 10px; }"
"#vtabs .vtabs-header > label { color: #1d2733; font-weight: bold; }"
"#vtabs .vtabs-header > button > image { color: #2f3b47; }"

"#vtabs row { padding: 4px 4px 4px 9px; border-bottom: 1px solid rgba(0,0,0,.07); border-left: 3px solid transparent; outline: none; }"
"#vtabs row:hover { background-color: #dfe5ec; }"
"#vtabs row:selected { background-image: linear-gradient(#ffffff, #eef1f4); border-left-color: #4f7fc8; }"
"#vtabs row > box > label { color: #1d2733; }"
"#vtabs row:selected > box > label { color: #000; }"
"#vtabs .vtabs-header > button, #vbookmarks .vbar-header > button { padding: 2px 4px; min-height: 22px; min-width: 22px; }"
/* ---- favicon strip (collapsed tab bar) ---- */
"#vstrip { background-color: #e4e9ee; border-right: 1px solid #a2adb8; }"
"#vstrip scrolledwindow, #vstrip viewport { background: none; }"
"#vstrip button.vstrip-tab { min-width: 0; min-height: 0; padding: 3px 3px 3px 2px; margin: 0;"
"  border: none; border-left: 3px solid transparent;"
"  border-radius: 0; background: none; box-shadow: none; outline: none; }"
"#vstrip button.vstrip-tab:hover { background-color: #d2dae3; }"
"#vstrip button.vstrip-tab.current { background-color: #ffffff; border-left-color: #3d72c4;"
"  box-shadow: inset 0 1px 0 rgba(0,0,0,.08), inset 0 -1px 0 rgba(0,0,0,.08); }"
"#vstrip .vstrip-tab image { color: #3a4654; }"
"#vstrip button.vstrip-add { min-height: 0; min-width: 0; padding: 3px; margin: 0; border-radius: 0;"
"  border: none; border-bottom: 1px solid #c3cbd3; background: none; box-shadow: none; }"
"#vstrip button.vstrip-add:hover { background-color: #d2dae3; }"
"#vstrip .vstrip-add image { color: #2f3b47; }"
/* ---- vertical bookmarks bar (overlay on the right) ---- */
"#vbm-button { padding: 2px 4px; }"
"#vbm-button > image { color: #2f3b47; }"
"#vbm-button:checked { background-image: linear-gradient(#cdd4dc, #e3e8ed); border: 1px solid #8a96a3; border-radius: 3px;"
"  box-shadow: inset 0 1px 2px rgba(0,0,0,.15); }"
"#vbookmarks, #vbookmarks scrolledwindow, #vbookmarks viewport, #vbookmarks treeview.view { background-color: #eef1f4; }"
"#vbookmarks { border-left: 1px solid #8f9aa6; box-shadow: -3px 0 10px rgba(0,0,0,.22); }"
"#vbookmarks .vbar-header { background-image: linear-gradient(#e9edf1, #d5dbe2); border-bottom: 1px solid #a2adb8; padding: 3px 4px 3px 10px; }"
"#vbookmarks .vbar-header > label { color: #1d2733; font-weight: bold; }"
"#vbookmarks .vbar-header > button > image { color: #2f3b47; }"
"#vbookmarks .vbar-search { padding: 5px 6px; border-bottom: 1px solid #c3cbd3; }"
"#vbookmarks treeview.view { color: #1d2733; }"
"#vbookmarks treeview.view:selected { background-color: #d3dff1; color: #000; }";

/* Dark theme: loaded on top of the light one (same selectors, dark colours). GTK's own
 * widgets (dialogs, menus, popovers, lists) follow through gtk-application-prefer-dark-theme,
 * which WebKit also reports to pages as prefers-color-scheme: dark. */
static const char *DARK_CSS =
"#main-menu > image, #addressbar button.nav-button > image, #vtabs-button > image, #vbm-button > image,"
"  #downloads-button image, #notify-button image, #newtab-button > image, #trash-button > image { color: #d5dbe1; }"
"#addressbar #downloads-button.active image { color: #6ea0e8; }"
"#main-menu:hover, #addressbar button.nav-button:hover, #tabtitlebar button.titlebutton:hover,"
"  #toolrow #vtabs-button:hover {"
"  border-color: #5a6672; background-image: linear-gradient(#46505b, #3a434d); }"
"#main-menu:checked, #main-menu:active, #addressbar button.nav-button:active, #vbm-button:checked,"
"  #toolrow #vtabs-button:checked, #vtabs-button:checked {"
"  border-color: #5a6672; background-image: linear-gradient(#2a3037, #353c44); box-shadow: inset 0 1px 2px rgba(0,0,0,.4); }"
"#addressbar button.nav-button:disabled > image { color: #6b757f; }"
"#addressbar { background-image: linear-gradient(#353b42, #2c3137); border-bottom-color: #1b1f23; }"
"#address { background-color: #1f2328; color: #e3e7eb; border-color: #555f6a; box-shadow: inset 0 1px 2px rgba(0,0,0,.4); }"
"#address image { color: #aab3bc; }"
"#address progress { background-image: linear-gradient(#2f4a6e, #284061); }"
"#tabtitlebar { background-image: linear-gradient(#383e45, #2b3036); box-shadow: inset 0 -1px #1b1f23, inset 0 1px rgba(255,255,255,.06); }"
"#tabtitlebar:backdrop { background-image: linear-gradient(#30353b, #2a2f34); }"
"#tabtitlebar button.titlebutton { color: #d5dbe1; }"
"#toolrow { background-image: linear-gradient(#353b42, #2c3137); border-bottom-color: #1b1f23; }"
"#findbar { background-image: linear-gradient(#353b42, #2c3137); border-top-color: #1b1f23; }"
"#findbar label, #findbar check + label, #findbar checkbutton label { color: #e3e7eb; }"
"#status-float { background-color: rgba(40,45,51,.97); border-color: #4a535d; }"
"#status-float label { color: #e3e7eb; }"
".tux-popup label, .tux-popup .popup-header button { color: #e3e7eb; }"
".tux-popup label.dim-label { color: #9aa4ae; }"
"button.tab-close { color: #aab3bc; }"
"#vtabs, #vtabs scrolledwindow, #vtabs viewport, #vtabs list { background-color: #2a2f35; }"
"#vtabs { border-right-color: #1b1f23; box-shadow: 3px 0 10px rgba(0,0,0,.5); }"
"#vtabs .vtabs-header, #vbookmarks .vbar-header { background-image: linear-gradient(#383e45, #2f353b); border-bottom-color: #1b1f23; }"
"#vtabs .vtabs-header > label, #vbookmarks .vbar-header > label { color: #e3e7eb; }"
"#vtabs .vtabs-header > button > image, #vbookmarks .vbar-header > button > image { color: #d5dbe1; }"
"#vtabs row { border-bottom-color: rgba(255,255,255,.05); }"
"#vtabs row:hover { background-color: #343a41; }"
"#vtabs row:selected { background-image: linear-gradient(#3d4550, #343b44); border-left-color: #5b8fd9; }"
"#vtabs row > box > label { color: #d5dbe1; }"
"#vtabs row:selected > box > label { color: #fff; }"
"#vstrip { background-color: #262a30; border-right-color: #1b1f23; }"
"#vstrip button.vstrip-tab:hover, #vstrip button.vstrip-add:hover { background-color: #343a41; }"
"#vstrip button.vstrip-tab.current { background-color: #3a414a; border-left-color: #5b8fd9;"
"  box-shadow: inset 0 1px 0 rgba(0,0,0,.3), inset 0 -1px 0 rgba(0,0,0,.3); }"
"#vstrip .vstrip-tab image, #vstrip .vstrip-add image { color: #c3cad1; }"
"#vstrip button.vstrip-add { border-bottom-color: #1b1f23; }"
"#vbookmarks, #vbookmarks scrolledwindow, #vbookmarks viewport, #vbookmarks treeview.view { background-color: #2a2f35; }"
"#vbookmarks { border-left-color: #1b1f23; box-shadow: -3px 0 10px rgba(0,0,0,.5); }"
"#vbookmarks .vbar-search { border-bottom-color: #1b1f23; }"
"#vbookmarks treeview.view { color: #d5dbe1; }"
"#vbookmarks treeview.view:selected { background-color: #3d4f69; color: #fff; }";

static GtkCssProvider *dark_provider;

/* "light" (default) or "dark"; called at start and whenever the setting changes */
void theme_apply(void)
{
    char *t = pref_get_string("theme");
    gboolean dark = g_str_equal(t, "dark");
    g_free(t);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, NULL);
    GdkScreen *scr = gdk_screen_get_default();
    if (dark && !dark_provider) {
        dark_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(dark_provider, DARK_CSS, -1, NULL);
        /* above the light theme, below the user's own user.css */
        gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(dark_provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    } else if (!dark && dark_provider) {
        gtk_style_context_remove_provider_for_screen(scr, GTK_STYLE_PROVIDER(dark_provider));
        g_clear_object(&dark_provider);
    }
}

void theme_load(void)
{
    GtkCssProvider *p = gtk_css_provider_new();
    GError *err = NULL;
    if (!gtk_css_provider_load_from_data(p, CSS, -1, &err)) {
        g_warning("CSS error: %s", err->message);
        g_error_free(err);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);

    /* optional user overrides: ~/.config/tuxbrowser/user.css */
    char *path = g_build_filename(profile->config_dir, "user.css", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *u = gtk_css_provider_new();
        if (gtk_css_provider_load_from_path(u, path, NULL))
            gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(u),
                                                      GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_unref(u);
    }
    g_free(path);
    theme_apply();
}
