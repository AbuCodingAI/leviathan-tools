# Infinitas C Browser Setup

## Quick Start

### Build Requirements

You need these development libraries installed:

- **GTK4** (4.8+)
- **WebKitGTK 6.0**
- **libsoup 3.0**
- **JSON-C**
- **SQLite3**
- **libcurl**

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install \
    libgtk-4-dev \
    libwebkitgtk-6.0-dev \
    libsoup-3.0-dev \
    libjson-c-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    build-essential
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install \
    gtk4-devel \
    webkit2gtk4.1-devel \
    libsoup3-devel \
    json-c-devel \
    sqlite-devel \
    libcurl-devel \
    gcc gcc-c++ make
```

### Linux (Arch)

```bash
sudo pacman -S \
    gtk4 \
    webkitgtk \
    json-c \
    sqlite \
    curl \
    base-devel
```

### macOS

```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install dependencies via Homebrew
brew install webkitgtk

# Note: On macOS, you may need to use the Xcode WebKit framework instead
# Adjust the Makefile to use -framework WebKit
```

### Windows (MinGW)

1. Install MSYS2 from https://www.msys2.org/
2. Open MSYS2 MinGW64 terminal
3. Install packages:
```bash
pacman -S --needed \
    base-devel \
    mingw-w64-x86_64-gtk4 \
    mingw-w64-x86_64-webkitgtk-6.0 \
    mingw-w64-x86_64-libsoup-3.0 \
    mingw-w64-x86_64-json-c \
    mingw-w64-x86_64-sqlite3 \
    mingw-w64-x86_64-curl
```

## Build

```bash
cd infinitas_c
make
```

## Run

```bash
./infinitas
```

## Features

### Built-in Schemes

- `infinitas://settings` - Browser settings
- `infinitas://fonts` - Font manager
- `infinitas://apps` - App launcher
- `infinitas://pwa` - PWA manager
- `infinitas://skribe` - PDF reader (upload support)
- `infinitas://incognito` - Incognito mode info

## Development

```bash
# Debug build
make debug

# Check dependencies
make check-deps

# Clean
make clean
```

## Project Structure

```
infinitas_c/
├── src/
│   ├── main.c              # Entry point
│   ├── browser.c/.h        # Browser window & tabs
│   ├── renderer.c/.h       # WebKitGTK wrapper
│   ├── http.c/.h           # libcurl HTTP handler
│   ├── storage.c/.h        # SQLite/JSON storage
│   ├── settings.c/.h       # Settings manager
│   ├── bookmarks.c/.h      # Bookmarks
│   ├── history.c/.h        # History
│   ├── search.c/.h         # Search engine
│   ├── ui.c/.h             # UI components
│   └── infinitas_scheme.c/.h # Custom scheme
├── Makefile
├── README.md
└── SETUP.md
```

## Architecture

- **GTK4** - UI framework
- **WebKitGTK** - Web rendering engine
- **libcurl** - HTTP client with caching
- **SQLite** - History database
- **JSON-C** - Settings & bookmarks storage

## Notes

- Settings are stored in `.infinitas/settings.json`
- Bookmarks are stored in `.infinitas/bookmarks.json`
- History is stored in `.infinitas/history.db`

## Troubleshooting

### "webkit2/webkit2.h not found"
Install WebKitGTK development package.

### "curl/curl.h not found"
Install libcurl development package.

### Linker errors
Make sure all libraries are installed and pkg-config can find them.

## License

MIT