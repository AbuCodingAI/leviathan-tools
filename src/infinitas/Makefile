# Infinitas Browser - Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99
LDFLAGS = 

# Detect platform
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    CPPFLAGS += $(shell pkg-config --cflags gtk4 webkitgtk-6.0 libsoup-3.0 json-c sqlite3 libcurl)
    LIBS     += $(shell pkg-config --libs   gtk4 webkitgtk-6.0 libsoup-3.0 json-c sqlite3 libcurl) -ldl
else ifeq ($(UNAME_S),Darwin)
    CPPFLAGS += $(shell pkg-config --cflags webkitgtk-6.0)
    LIBS     += $(shell pkg-config --libs   webkitgtk-6.0) -framework WebKit -framework Cocoa -lcurl
else
    CPPFLAGS += $(shell pkg-config --cflags gtk4 webkitgtk-6.0 libsoup-3.0 json-c sqlite3 curl)
    LIBS     += $(shell pkg-config --libs   gtk4 webkitgtk-6.0 libsoup-3.0 json-c sqlite3 curl)
endif

SRCDIR = src
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/browser.c \
          $(SRCDIR)/renderer.c \
          $(SRCDIR)/http.c \
          $(SRCDIR)/storage.c \
          $(SRCDIR)/settings.c \
          $(SRCDIR)/bookmarks.c \
          $(SRCDIR)/history.c \
          $(SRCDIR)/search.c \
          $(SRCDIR)/ui.c \
          $(SRCDIR)/infinitas_scheme.c \
          $(SRCDIR)/offstore.c \
          $(SRCDIR)/plugin.c \
          $(SRCDIR)/extension.c \
          $(SRCDIR)/protocol.c \
          $(SRCDIR)/download.c \
          $(SRCDIR)/adblock.c \
          $(SRCDIR)/ezlist.c \
          $(SRCDIR)/passwords.c

HEADERS = $(SRCDIR)/browser.h \
          $(SRCDIR)/renderer.h \
          $(SRCDIR)/http.h \
          $(SRCDIR)/storage.h \
          $(SRCDIR)/settings.h \
          $(SRCDIR)/bookmarks.h \
          $(SRCDIR)/history.h \
          $(SRCDIR)/search.h \
          $(SRCDIR)/ui.h \
          $(SRCDIR)/infinitas_scheme.h \
          $(SRCDIR)/offstore.h \
          $(SRCDIR)/plugin.h \
          $(SRCDIR)/extension.h \
          $(SRCDIR)/infinitas_plugin_api.h \
          $(SRCDIR)/protocol.h \
          $(SRCDIR)/download.h \
          $(SRCDIR)/ezlist.h \
          $(SRCDIR)/passwords.h

TARGET  = infinitas
PREFIX  ?= /usr/local
DESTDIR ?=

# EasyList -> WebKit content-blocker JSON (offline, build-time; no network) ---
EZTOOL      = tools/ezlist2json
EZLIST_SRC  = data/easylist.txt
ADBLOCK_JSON = data/adblock-rules.json
# glib-only flags for the standalone converter tool
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS   = $(shell pkg-config --libs   glib-2.0)

BINDIR  = $(DESTDIR)$(PREFIX)/bin
APPDIR  = $(DESTDIR)/usr/share/applications
ICONDIR = $(DESTDIR)/usr/share/icons/hicolor/scalable/apps
ICONPNG = $(DESTDIR)/usr/share/icons/hicolor/1024x1024/apps

all: $(ADBLOCK_JSON) $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS) $(LIBS)

# Build the offline EasyList converter (glib-only, no WebKit).
$(EZTOOL): tools/ezlist2json.c $(SRCDIR)/ezlist.c $(SRCDIR)/ezlist.h
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -o $@ tools/ezlist2json.c $(SRCDIR)/ezlist.c $(GLIB_LIBS)

# Bake the bundled EasyList snapshot into the content-blocker JSON at build time.
$(ADBLOCK_JSON): $(EZTOOL) $(EZLIST_SRC)
	$(EZTOOL) $(EZLIST_SRC) $(ADBLOCK_JSON)

# Regenerate the blocklist on demand.
adblock: $(ADBLOCK_JSON)

debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)

install: $(TARGET) $(ADBLOCK_JSON)
	install -Dm755 $(TARGET)                    $(BINDIR)/infinitas
	install -Dm644 $(ADBLOCK_JSON)              $(DESTDIR)$(PREFIX)/share/infinitas/adblock-rules.json
	install -Dm644 data/infinitas.desktop       $(APPDIR)/infinitas.desktop
	install -Dm644 data/infinitas.svg           $(ICONDIR)/infinitas.svg
	install -Dm644 data/infinitas.png           $(ICONPNG)/infinitas.png
	@update-desktop-database $(APPDIR) 2>/dev/null || true
	@gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
	@echo ""
	@echo "Infinitas installed. To set as default browser:"
	@echo "  xdg-settings set default-web-browser infinitas.desktop"

uninstall:
	rm -f $(BINDIR)/infinitas
	rm -f $(APPDIR)/infinitas.desktop
	rm -f $(ICONDIR)/infinitas.svg
	rm -f $(ICONPNG)/infinitas.png
	@update-desktop-database $(APPDIR) 2>/dev/null || true
	@echo "Infinitas uninstalled."

clean:
	rm -f $(TARGET) $(EZTOOL)

run: $(TARGET)
	./$(TARGET)

check-deps:
	@echo "Checking for required libraries..."
	@pkg-config --exists gtk4 && echo "✓ GTK4 found" || echo "✗ GTK4 not found"
	@pkg-config --exists webkitgtk-6.0 && echo "✓ WebKitGTK found" || echo "✗ WebKitGTK not found"
	@pkg-config --exists libsoup-3.0 && echo "✓ libsoup found" || echo "✗ libsoup not found"
	@pkg-config --exists json-c && echo "✓ JSON-C found" || echo "✗ JSON-C not found"
	@pkg-config --exists sqlite3 && echo "✓ SQLite3 found" || echo "✗ SQLite3 not found"
	@pkg-config --exists curl && echo "✓ libcurl found" || echo "✗ libcurl not found"

install-deps-linux:
	@echo "Installing dependencies for Linux..."
	@echo "Ubuntu/Debian: sudo apt-get install libgtk-4-dev libwebkitgtk-6.0-dev libsoup-3.0-dev libjson-c-dev libsqlite3-dev libcurl4-openssl-dev"
	@echo "Fedora/RHEL: sudo dnf install gtk4-devel webkit2gtk4.1-devel libsoup3-devel json-c-devel sqlite-devel libcurl-devel"
	@echo "Arch: sudo pacman -S gtk4 webkitgtk json-c sqlite curl base-devel"

install-deps-macos:
	@echo "Install Xcode Command Line Tools: xcode-select --install"
	@echo "Install WebKitGTK: brew install webkitgtk"

install-deps-windows:
	@echo "Install MSYS2 from https://www.msys2.org/"
	@echo "Then run: pacman -S mingw-w64-x86_64-gtk4 mingw-w64-x86_64-webkitgtk-6.0 mingw-w64-x86_64-libsoup-3.0 mingw-w64-x86_64-json-c mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-curl base-devel"

tags: $(SOURCES) $(HEADERS)
	etags $(SOURCES) $(HEADERS)

help:
	@echo "Infinitas Browser Build System"
	@echo "Available targets: all, debug, clean, run, check-deps, install-deps-*, tags, help"

.PHONY: all debug clean run install uninstall check-deps install-deps-linux install-deps-macos install-deps-windows tags help adblock
