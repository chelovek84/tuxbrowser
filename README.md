# Tux Browser

A fast, lightweight web browser written in C on WebKitGTK.

## Build

    sudo apt install libwebkit2gtk-4.1-dev libsoup-3.0-dev build-essential
    make
    ./tuxbrowser [url-or-search ...]

`sudo make install` installs to `/usr/local` (binary and .desktop file).

## Features

- The address bar sits in the window title bar, sharing a row with the window buttons:
  the tab bar toggle in the top-left corner (full height, so it can be hit without looking), the ☰ menu,
  back/forward/reload, the zoom button, the address/search box, the downloads button, the ★ bookmarks bar toggle, then the window controls.
  The window icon and window menu are left out of the title bar so nothing sits left of the toggle. This layout is
  fixed; there is no separate toolbar row.
- **☰ menu** button: one short flat list with only what has no button of its own — New Window;
  History; Find, Print, Save Page As, Open File, Full Screen, Developer Tools;
  Light Theme / Dark Theme; Preferences; Keyboard Shortcuts, About. Ctrl+Q quits (closes all windows). Tabs are managed in the tab
  bar and its right-click menu, bookmarks in the bookmarks bar, zoom with the zoom button, and
  images, JavaScript and mouse gestures in Preferences. There is no menu bar.
- **Vertical tab bar** on the left, toggled with the title bar button or **F9**. It slides
  in over the page without resizing it. Its header holds the tab controls: new tab (+) and
  closed tabs (trash). Click a tab to switch, middle-click to close, right-click for more,
  drag a tab up or down to reorder.
- While the vertical tab bar is closed, a narrow **favicon strip** stays on the left edge with
  one 24 px favicon per tab, so favicon changes (unread counts, alerts) are always visible. The
  current tab is marked with a blue edge. Click to switch, middle-click to close, right-click for
  more, hover for the title and address; the + at the top opens a new tab. Empty (start page) tabs show a
  blank-page icon. The strip sits next to
  the page instead of covering it, and the opened tab bar slides over it. The tab bar and the strip
  are the only things on the left side; neither has settings.
- **Vertical bookmarks bar** on the right, toggled with the ★ title bar button, **F7** or **Ctrl+Shift+B**. It is the
  only bookmark manager. It also
  slides in over the page. Its header holds bookmark management: bookmark this page (+),
  properties, and delete. Below that are a quick-find field and the bookmarks with
  folders. Click to open, middle-click for a background tab, right-click for more (open in new
  tab/window, properties, delete). **F2** edits and **Del** deletes the selected bookmark.
  Drag bookmarks to reorder them or to move them into/out of folders (not while Quick find is
  filtering).
- Both vertical bars start closed in every window; their open/closed state isn't remembered.
- **Start page** on new tabs: a plain screen with one search box, which has the keyboard as soon as
  the tab opens. It works like the address bar: addresses, search keywords (`w linux`) and the
  default engine. It follows the system's light or dark theme. There is no home page or Speed Dial;
  saved sessions with old Speed Dial tabs open the start page.
- **Zoom button** (magnifier) in the title bar: − / slider / + and the percentage, which resets the
  zoom when clicked. When a page isn't at 100%, the button shows its zoom.
- No status bar. A link's address shows in a small label over the bottom-left corner of the page
  while the mouse is on the link, as do short messages ("Bookmark added"). Loading progress is shown
  in the address bar.
- **History** (☰ → History, **Ctrl+H**) drops down as a popup under the ☰ button, **Downloads**
  (the downloads button next to the address bar, **Ctrl+J**) under its button; in full
  screen both drop from the top of the page. History is grouped by day and has a search field; Del
  deletes the selected entry. Double-click an entry to open it in the current tab (the popup closes),
  middle-click for a background tab. Press the shortcut again or Esc to close.
- **Downloads button:** while something downloads, a ring around its icon fills up with the overall
  progress (a turning arc when the size isn't known); the tooltip shows how many and the percentage.
- **Ad blocking** (on by default; Preferences → Ad Blocking switches it off and on), working like the VIA
  browser's:
  - **Filter lists** in Adblock Plus format, EasyList by default. They are downloaded on first use and
    again every 4 days; Preferences → Ad Blocking takes more list addresses (e.g. a list for your
    language) and has Update Now. The lists are turned into WebKit content blocker rules, so ads are
    blocked in the network layer and hidden elements never flash on screen. The compiled rules are
    cached, so starting the browser doesn't recompile them.
  - **Block Element...** in a page's right-click menu: the element you right-clicked is highlighted;
    move the mouse to pick another, use the wheel or ↑/↓ to widen or narrow to the surrounding area,
    click to block it, Esc to cancel. It is hidden on that site from then on.
  - **Allow Ads on This Site** / **Block Ads on This Site** in the right-click menu; the list of
    allowed sites can be edited in Preferences.
  - **Your rules** (Preferences → Ad Blocking): your own lines in Adblock Plus format, including the
    elements you blocked; delete a line to show an element again.
  - Not supported from the lists: scriptlets, `$redirect`/`$csp`/`$removeparam` and other rewriting
    options, regular-expression rules and extended CSS (`:has-text()`, `:xpath()`...). These lines are
    skipped.
- **Light and dark theme** for the browser's own window (☰ → Light Theme / Dark Theme, light by
  default). In the dark theme the start page and pages that support it are dark too.
- Address bar: shows load progress inside the field, a padlock and site info, a bookmark
  star, and completion from bookmarks and history. **Shift+Enter** opens the address in a
  new tab.
- **Paste and Go:** right-click the address bar for "Paste and Go" (or "Paste and Search" when the
  clipboard isn't an address), or press **Ctrl+Shift+V** in the address bar. An address copied across
  two lines is joined back together.
- **One box for addresses and search.** Type an address or a search; the first suggestion shows
  what Enter will do ("Go to address" or "Search with …"), followed by matching bookmarks and
  history. Keywords pick an engine, e.g. `g cats` (Google) or `w linux` (Wikipedia). The default
  engine and keywords are set in Preferences → Search. **Ctrl+K** / **Ctrl+E** focus the box.
- **Instant new tabs:** a spare tab with its web process running and the start page loaded is kept
  ready in the background. Ctrl+T shows a finished page in about 10 ms instead of ~0.7 s, and
  links opened in a new tab (middle/Ctrl+click, "Open in New Tab", bookmarks, history) start
  loading within ~20 ms instead of 0.15-0.6 s. The start page is replaced, not kept in Back history.
- Identifies as **Safari on macOS** by default. The engine is WebKit, the same as Safari's, so sites
  send code made for it, video sites pick formats it plays, and sign-in and bot checks see an identity
  that matches the browser. Sites serve their full desktop version. Some may offer Mac downloads or show
  ⌘ in shortcut hints. A different one can be picked in Preferences → Web Pages: Chrome on Linux,
  Chrome/Edge on Windows, Firefox on Linux, Chrome on Android (mobile sites), WebKitGTK's own, or a
  custom string.
- Asks sites for **your system languages** (from `LANGUAGE`/`LANG`, e.g. `en-US, en`). A different
  language can be picked in Preferences → Web Pages → Languages, like the user agent: System default,
  a list of common languages (each with English as a fallback, e.g. `de-DE, de, en`), or Custom...,
  which lets you type your own list. The field under the list shows exactly what is sent.
- **Mouse gestures** (hold the right button):

  | Gesture | Action |
  |---|---|
  | ← / → | back / forward |
  | ↑ | stop |
  | ↓ | new tab |
  | ↑↓ | reload |
  | ↓→ | close tab |
  | ↓↑ | duplicate tab |
  | ↑← | parent directory |

  Holding the right button and turning the wheel cycles tabs. Holding the right button
  and clicking left goes back.
- **Graphics memory use** (Preferences → Advanced → Performance, applies after a restart). In *Low*
  mode pages are painted by the CPU without compositing (unless Hardware acceleration is set to
  *Always*) and video frames don't go through OpenGL, which keeps video memory free for the desktop
  and other programs. *Automatic* picks Low for cards on the old `radeon` driver and
  for cards with 1 GB of video memory or less. On such cards, running out of video memory shows up as
  freezing windows, corrupted video, and blank video in other browsers.
- **Video decoding** (Preferences → Advanced → Performance, applies after a restart): *Software*
  prefers CPU decoders (FFmpeg through `gstreamer1.0-libav`, libvpx), *Hardware* prefers the graphics
  card's VA-API decoders. Hardware decoders stay available as a fallback in software mode for formats
  no software decoder handles. *Automatic* picks Software on the same cards as Low graphics memory and
  leaves GStreamer's own choice elsewhere. Install `gstreamer1.0-libav` for software H.264/H.265.
- **Software decoders** (same place): when several CPU decoders handle a format, *FFmpeg first* uses
  FFmpeg (`gstreamer1.0-libav`) and *Codec libraries first* uses each codec's own library (libvpx,
  libaom/dav1d, OpenH264, libde265, mpg123...). The other kind stays as a fallback. *Automatic* keeps
  GStreamer's choice (today: libvpx for VP8/VP9, FFmpeg for H.264). WebKitGTK always plays media through
  GStreamer; FFmpeg is used as GStreamer plugins, not as a separate engine.
- Delete Private Data (Ctrl+Shift+Del, or Preferences → Advanced), Preferences (Ctrl+F12 or Alt+P).
- The session is restored on startup.

The full shortcut list is under ☰ → Keyboard Shortcuts.

## Command line

    tuxbrowser [--video-decoding=auto|software|hardware] [--decoders=auto|ffmpeg|native]
             [--decoder=FORMAT:ELEMENT ...] [--gpu-memory=auto|normal|low] [URL or file...]

`--decoder` (command line only) puts one decoder first for a format, e.g. `--decoder=h264:openh264dec`
or `--decoder=vp9:avdec_vp9`; a wrong value lists the installed decoders per format.

The options apply to that run only and override Preferences; the resulting setup is printed to
stderr, including the decoder each format will use. They take effect when Tux Browser starts, so they are ignored if it is already running
(combine them with `TUXBROWSER_PROFILE` to start a separate instance). `tuxbrowser --help` lists them.

## Files

- Before the rename the browser was called GBrowser. On the first start, Tux Browser moves the old
  `gbrowser` folders below to their new names, and old `gbrowser://` tabs in saved sessions open the
  start page.
- Settings: `~/.config/tuxbrowser/settings.ini`
- Optional CSS overrides for the chrome: `~/.config/tuxbrowser/user.css`
- Data: `~/.local/share/tuxbrowser/` (`bookmarks.tsv`, `history.sqlite`,
  `session.ini`, cookies, `adblock-lists.txt` (the downloaded filter lists), `adblock-rules.txt` (your
  rules)). Compiled ad blocking rules are cached in `~/.cache/tuxbrowser/adblock/`. Files from older versions (`notes.tsv`, `speeddial.tsv`, `thumbnails/`) are
  no longer used and can be deleted.
- Set `TUXBROWSER_PROFILE=/some/dir` to use a separate profile. It runs as its own instance, next to a
  normal Tux Browser window.

## Diagnosing freezes

Run `TUXBROWSER_WATCHDOG=1 ./tuxbrowser 2>freeze.log`. Whenever the window stops responding for
more than 250 ms, a line with the time and duration is written to the log, followed by a backtrace of
the UI thread at that moment (repeated every second while it stays frozen).

`TUXBROWSER_SCREENSHOT=window.png ./tuxbrowser` saves a picture of the first window (title bar included)
six seconds after it opens, rendered by Tux Browser itself.

## License

[MIT](LICENSE) © 2026 chelovek84

Tux Browser uses [WebKitGTK](https://webkitgtk.org/) and [GTK 3](https://www.gtk.org/) (both LGPL-2.1-or-later) and [SQLite](https://sqlite.org/) (public domain), linked dynamically.
