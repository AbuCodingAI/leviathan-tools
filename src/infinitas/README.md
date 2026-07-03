# Infinitas Browser

A lightweight web browser written in C using GTK4 and WebKitGTK 6.0.
Ships as the default browser in LeviathanOS (Debian-based).

**173 KB binary. Zero telemetry. No Chromium.**

---

## Features

- **Full web rendering** via WebKitGTK (same engine as Safari)
- **Tabbed browsing** with Ctrl+T / Ctrl+W
- **Google sign-in** — sign in once, YouTube/Gmail/Drive all work
- **Bookmarks & history** with per-item removal
- **Encrypted offline archive** — starred pages saved as `.infx` binaries (SHA256-CTR + HMAC), served automatically when offline
- **PWA install** — every page has an Install button, creates a `.desktop` file in your app menu
- **Custom `infinitas://` shortcuts** — reroute, mask, or OS-launch via `infinitas://protocol`
- **Plugin system** — native `.so` plugins via `dlopen` with a full C API
- **Extension system** — Chrome MV2/MV3 compatible JS extensions
- **Audio player** (`infinitas://pno`) — drag-and-drop MP3/FLAC/WAV/OGG with playlist, shuffle, loop
- **PDF reader** (`infinitas://skribe`) — drop any PDF, rendered natively by WebKit
- **IP ping** — type a bare IP in the address bar, get a ping result page
- **Font manager** — browse all system fonts with live preview, apply globally
- **Dark theme** — GTK CSS, rounded address bar, purple accent

---

## Built-in Pages

| Address | Description |
|---------|-------------|
| `infinitas://tab` | New tab — search bar, quick links, recent history |
| `infinitas://bookmarks` | Bookmark manager |
| `infinitas://history` | Full browsing history |
| `infinitas://offline` | Encrypted offline archive |
| `infinitas://fonts` | System font browser |
| `infinitas://settings` | Browser preferences |
| `infinitas://protocol` | Custom shortcut manager |
| `infinitas://pwa` | Installed PWA manager |
| `infinitas://extensions` | Loaded extensions |
| `infinitas://pno` | Audio player (Plentiful Notes for Orchestra) |
| `infinitas://skribe` | PDF reader |
| `infinitas://incognito` | Private browsing info |
| `infinitas://ping/<ip>` | Async ping with 10s timeout |
| `infinitas://help` | All pages and keyboard shortcuts |
| `infinitas://chat` | → ChatGPT |
| `infinitas://cpp` | → cpp.sh |
| `infinitas://mail` | → Gmail |
| `infinitas://account` | → Google Account |

---

## Build Requirements

- GTK4 (4.8+)
- WebKitGTK 6.0
- libsoup 3.0
- JSON-C
- SQLite3
- libcurl

## Build Instructions

### Linux (Ubuntu / Debian / Mint)

```bash
sudo apt-get install \
  libgtk-4-dev libwebkitgtk-6.0-dev libsoup-3.0-dev \
  libjson-c-dev libsqlite3-dev libcurl4-openssl-dev

make
```

### Linux (Fedora / RHEL)

```bash
sudo dnf install gtk4-devel webkit2gtk4.1-devel libsoup3-devel \
  json-c-devel sqlite-devel libcurl-devel

make
```

### Linux (Arch)

```bash
sudo pacman -S gtk4 webkitgtk json-c sqlite curl base-devel

make
```

---

## Usage

```bash
make              # build
./infinitas       # run

make debug        # build with -g -DDEBUG
make clean        # remove binary
make check-deps   # verify all libraries are present
```

### Install system-wide

```bash
sudo make install

# Set as default browser
xdg-settings set default-web-browser infinitas.desktop

# Uninstall
sudo make uninstall
```

### Open a URL or launch a PWA

```bash
infinitas https://example.com
infinitas --app=https://example.com
```

---

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+L | Focus address bar |
| Ctrl+D | Bookmark page |
| Ctrl+Shift+I | Developer console |
| F5 | Reload |
| F11 | Fullscreen |

---

## File Structure

```
infinitas_c/
├── src/
│   ├── main.c                  # Entry point, --app flag
│   ├── browser.c/.h            # Window, tabs, navigation, Google sign-in
│   ├── renderer.c/.h           # WebKitGTK wrapper, PWA button injection
│   ├── http.c/.h               # libcurl HTTP handler
│   ├── storage.c/.h            # SQLite/JSON storage
│   ├── settings.c/.h           # Browser preferences
│   ├── bookmarks.c/.h          # Bookmarks
│   ├── history.c/.h            # History (SQLite)
│   ├── search.c/.h             # URL/query/IP detection
│   ├── ui.c/.h                 # Toolbar, menu, account button
│   ├── offstore.c/.h           # Encrypted offline archive
│   ├── protocol.c/.h           # Custom infinitas:// shortcuts
│   ├── plugin.c/.h             # Native .so plugin loader
│   ├── extension.c/.h          # Chrome MV2/MV3 extension loader
│   ├── infinitas_plugin_api.h  # Public plugin API
│   └── infinitas_scheme.c/.h   # All infinitas:// page handlers
├── data/
│   ├── infinitas.desktop
│   └── infinitas.svg
├── plugins/
├── Makefile
└── README.md
```

## Data Storage

All user data lives in `~/.infinitas/`:

| File | Contents |
|------|----------|
| `settings.json` | Preferences |
| `bookmarks.json` | Bookmarks |
| `history.db` | Browsing history |
| `offline.db` | Offline archive index |
| `offline.key` | 32-byte encryption key (chmod 600) |
| `offline/*.infx` | Encrypted page archives |
| `protocols.db` | Custom shortcuts |
| `pwas.db` | Installed PWAs |
| `extensions/` | Chrome extensions |
| `plugins/` | Native plugins |

---

## Writing a Plugin

```c
#include "infinitas_plugin_api.h"

static InfinitasPlugin* init(const InfinitasBrowserAPI *api) {
    static InfinitasPlugin p = {
        .name        = "My Plugin",
        .version     = "1.0",
        .description = "Example plugin",
    };
    GtkWidget *btn = gtk_button_new_with_label("Hello");
    api->add_toolbar_widget(btn, api->browser);
    return &p;
}

INFINITAS_PLUGIN_DEFINE(init)
```

```bash
gcc -shared -fPIC -o myplugin.so myplugin.c \
    -I/path/to/infinitas/src $(pkg-config --cflags glib-2.0 gtk4)
cp myplugin.so ~/.infinitas/plugins/
```

---

## License

MIT — see LICENSE.
