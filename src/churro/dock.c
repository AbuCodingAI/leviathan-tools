/*
 * Churro Dock
 *
 * Features:
 *   - config-driven colors / sizes / corner radius (churro-settings.h)
 *   - real app icons loaded via gdk-pixbuf from the icon theme, cached as
 *     Pixmaps (falls back to the classic colored square if none is found)
 *   - pinned favourites with a running-indicator dot (EWMH _NET_CLIENT_LIST)
 *   - DYNAMIC running apps: every non-pinned managed window gets its own dock
 *     icon (WM_CLASS -> icon theme, then _NET_WM_ICON, then a lettered square).
 *     Left-click focuses/raises it via _NET_ACTIVE_WINDOW.
 *   - right-click context menus (Focus / Close on windows, Launch on pins)
 *   - hover tooltips
 *   - event-driven redraw (Expose / click / motion / periodic refresh)
 *
 * The dock grows/shrinks and re-centres as apps open and close.
 * Every X resource / atom lookup is guarded so a missing WM property never
 * crashes the dock.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "churro-settings.h"

#define DOCK_GAP 12
#define DOCK_MARGIN 20
#define SEARCH_BAR_WIDTH 200
#define SEP_GAP 22
#define REFRESH_SECONDS 3
#define MAX_WIN_SLOTS 40
#define MAX_USER_PINS 16
#define DOCK_EDGE_GAP 10   /* gap between the dock and the screen edge */

Display *dpy;
Window dock_win;
Window root;
GC gc;
XFontStruct *font_label;

ChurroSettings cfg;
int dock_height;
int item_size;
int corner_radius;

int g_dock_x = 0, g_dock_y = 0, g_dock_w = 0;

/* Primary-monitor geometry (root coordinates). Defaults to the whole screen
 * and is refined by detect_primary_monitor() when RandR is available. */
int mon_x = 0, mon_y = 0, mon_w = 0, mon_h = 0;

/* EWMH / ICCCM atoms (interned once, guarded on use). */
Atom a_net_client_list, a_net_active_window, a_net_close_window, a_net_wm_icon;
Atom a_net_wm_window_type, a_type_desktop, a_type_dock, a_type_toolbar,
     a_type_menu, a_type_splash;
Atom a_net_wm_state, a_state_skip_taskbar;
Atom a_net_wm_strut, a_net_wm_strut_partial;
Atom a_wm_change_state;

typedef struct {
    const char *label;
    const char *cmd;
    const char *fallback_cmd;
    const char *icon;        /* icon-theme name to resolve */
    const char *match;       /* substring to match against WM_CLASS for "running" */
    unsigned long color;     /* fallback colored square */
    Pixmap icon_pixmap;      /* cached rendered icon, or None */
    int running;
} DockItem;

DockItem items[] = {
    {"Menu",      "",                          NULL,                   "start-here",          "",               0x3a5a8a, None, 0},
    {"Finder",    "nemo &",                    "nautilus &",           "system-file-manager", "nemo",           0x8a6a3a, None, 0},
    {"Infinitas", "infinitas &",               NULL,                   "applications-science","infinitas",      0x2a8a5a, None, 0},
    {"Dashboard", "leviathan-dashboard &",     "dada &",               "utilities-system-monitor","dashboard",  0x8a3a5a, None, 0},
    {"Terminal",  "alacritty &",               "gnome-terminal &",     "utilities-terminal",  "alacritty",      0x1a2a4a, None, 0},
    {"Settings",  "cinnamon-settings &",       "gnome-control-center &","preferences-system", "cinnamon-settings",0x4a4a6a,None, 0},
    {"Trash",     "nemo trash:/// &",          "nautilus trash:/// &", "user-trash",          "",               0x5a3a2a, None, 0},
};

#define NUM_ITEMS ((int)(sizeof(items) / sizeof(items[0])))

/* One dynamic (running, non-pinned) window shown on the dock. */
typedef struct {
    Window win;
    char label[128];
    unsigned long color;   /* fallback square colour derived from the class */
    Pixmap icon;           /* owned; freed when the slot goes away */
} WinSlot;

WinSlot win_slots[MAX_WIN_SLOTS];
int num_win_slots = 0;

/* User-pinned apps (persisted to ~/.config/churro/pinned.conf). Each is matched
 * against running windows by a lowercase WM_CLASS substring, exactly like the
 * static pins, so it shows a running dot and swallows its own dynamic slot. */
typedef struct {
    char cls[128];     /* lowercase match/launch key (from WM_CLASS) */
    char label[128];   /* display label (capitalised) */
    Pixmap icon;       /* owned; freed when the pin goes away */
    int running;
    Window win;        /* a matching running window, or None */
} UserPin;

UserPin user_pins[MAX_USER_PINS];
int num_user_pins = 0;

/* Rendered-slot layout, shared by draw + hit-testing so they never disagree. */
typedef enum { SLOT_SEARCH, SLOT_PINNED, SLOT_USERPIN, SLOT_WINDOW } SlotKind;
typedef struct { SlotKind kind; int index; int x; int w; } RSlot;
RSlot rslots[1 + NUM_ITEMS + MAX_USER_PINS + MAX_WIN_SLOTS];
int num_rslots = 0;
int sep_x = -1;   /* x of separator line between pins and windows, or -1 */

/* Tooltip + context-menu popups. */
Window tip_win = None;
GC tip_gc;
int tip_shown = 0;
int hovered_rslot = -1;

Window ctx_win = None;
GC ctx_gc;
#define CTX_MAX 5
#define CTX_ITEM_H 26
#define CTX_W 150
enum { ACT_FOCUS, ACT_MINIMIZE, ACT_PIN, ACT_UNPIN, ACT_CLOSE, ACT_LAUNCH };
char ctx_labels[CTX_MAX][24];
int ctx_actions[CTX_MAX];
int ctx_count = 0;
int ctx_hover = -1;
SlotKind ctx_kind;
int ctx_index = 0;      /* pinned index or window-slot index */
Window ctx_target = None;
char ctx_pin_cls[128];  /* class key for Pin, when the menu is on a window */

/* ---- Property helpers (all guarded) -------------------------------------- */

/* Read a property as an array; returns malloc'd Window/Atom array via *out and
 * count via *count (0 on failure). Caller must XFree(*out) if non-NULL. */
static unsigned char *get_prop(Window w, Atom prop, Atom type, unsigned long *count) {
    *count = 0;
    if (prop == None) return NULL;
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, (~0L), False, type,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) != Success)
        return NULL;
    if (!data) return NULL;
    *count = nitems;
    return data;
}

/* Does window `w` belong on a taskbar/dock? Skips desktops, docks, our own
 * window and anything flagged skip-taskbar. Missing props => treat as normal. */
static int is_taskbar_window(Window w) {
    if (w == dock_win || w == None) return 0;

    unsigned long n = 0;
    unsigned char *d = get_prop(w, a_net_wm_window_type, XA_ATOM, &n);
    if (d) {
        Atom *types = (Atom *)d;
        for (unsigned long i = 0; i < n; i++) {
            if (types[i] == a_type_desktop || types[i] == a_type_dock ||
                types[i] == a_type_toolbar || types[i] == a_type_menu ||
                types[i] == a_type_splash) {
                XFree(d);
                return 0;
            }
        }
        XFree(d);
    }

    n = 0;
    d = get_prop(w, a_net_wm_state, XA_ATOM, &n);
    if (d) {
        Atom *states = (Atom *)d;
        for (unsigned long i = 0; i < n; i++) {
            if (states[i] == a_state_skip_taskbar) { XFree(d); return 0; }
        }
        XFree(d);
    }
    return 1;
}

/* ---- Icon loading -------------------------------------------------------- */

/* Find an icon file for `name` at (roughly) `size`. Returns 1 on success. */
static int find_icon_path(const char *name, int size, char *out, size_t outlen) {
    if (!name || !name[0]) return 0;

    if (name[0] == '/') {
        if (access(name, R_OK) == 0) { strncpy(out, name, outlen - 1); out[outlen - 1] = 0; return 1; }
        return 0;
    }

    const char *themes[] = { "hicolor", "Adwaita", "Papirus", "gnome", "breeze" };
    const int sizes[] = { size, 48, 64, 32, 128, 256, 24 };
    const char *cats[] = { "apps", "categories", "devices", "places", "status" };
    const char *exts[] = { "png", "svg" };

    for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (size_t c = 0; c < sizeof(cats) / sizeof(cats[0]); c++) {
                for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
                    snprintf(out, outlen, "/usr/share/icons/%s/%dx%d/%s/%s.%s",
                             themes[t], sizes[s], sizes[s], cats[c], name, exts[e]);
                    if (access(out, R_OK) == 0) return 1;
                    snprintf(out, outlen, "/usr/share/icons/%s/scalable/%s/%s.svg",
                             themes[t], cats[c], name);
                    if (access(out, R_OK) == 0) return 1;
                }
            }
        }
    }

    const char *flat[] = { "/usr/share/pixmaps", "/usr/local/share/pixmaps" };
    const char *fexts[] = { "png", "svg", "xpm" };
    for (size_t d = 0; d < sizeof(flat) / sizeof(flat[0]); d++) {
        for (size_t e = 0; e < sizeof(fexts) / sizeof(fexts[0]); e++) {
            snprintf(out, outlen, "%s/%s.%s", flat[d], name, fexts[e]);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    return 0;
}

/* Composite one RGBA source (row-major) onto a size*size Pixmap over bg,
 * nearest-neighbour scaling from src w*h to size*size. */
static Pixmap composite_to_pixmap(const unsigned char *rgba, int sw, int sh,
                                  int size, unsigned long bg) {
    int screen = DefaultScreen(dpy);
    Visual *vis = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    int br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff, bb = bg & 0xff;

    unsigned int *buf = (unsigned int *)malloc((size_t)size * size * 4);
    if (!buf) return None;

    XImage *img = XCreateImage(dpy, vis, depth, ZPixmap, 0,
                               (char *)buf, size, size, 32, 0);
    if (!img) { free(buf); return None; }

    for (int y = 0; y < size; y++) {
        int sy = sh > 0 ? y * sh / size : 0;
        for (int x = 0; x < size; x++) {
            int sx = sw > 0 ? x * sw / size : 0;
            const unsigned char *p = rgba + ((size_t)sy * sw + sx) * 4;
            int r = p[0], g = p[1], b = p[2], a = p[3];
            int R = (r * a + br * (255 - a)) / 255;
            int G = (g * a + bgc * (255 - a)) / 255;
            int B = (b * a + bb * (255 - a)) / 255;
            unsigned long px = ((unsigned long)R << 16) | ((unsigned long)G << 8) | (unsigned long)B;
            XPutPixel(img, x, y, px);
        }
    }

    Pixmap pm = XCreatePixmap(dpy, dock_win, size, size, depth);
    GC tgc = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, tgc, bg);
    XFillRectangle(dpy, pm, tgc, 0, 0, size, size);
    XPutImage(dpy, pm, tgc, img, 0, 0, 0, 0, size, size);

    XDestroyImage(img);     /* frees buf */
    XFreeGC(dpy, tgc);
    return pm;
}

/* Render an icon-theme file onto a size*size Pixmap composited over bg. */
static Pixmap load_icon_pixmap(const char *name, int size, unsigned long bg) {
    char path[1024];
    if (!find_icon_path(name, size, path, sizeof(path))) return None;

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return None;
    }

    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    const guchar *pix = gdk_pixbuf_get_pixels(pb);

    /* Pack into contiguous RGBA centred on a size*size transparent canvas. */
    unsigned char *rgba = (unsigned char *)calloc((size_t)size * size, 4);
    if (!rgba) { g_object_unref(pb); return None; }
    int ox = (size - w) / 2, oy = (size - h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    for (int y = 0; y < h && (y + oy) < size; y++) {
        for (int x = 0; x < w && (x + ox) < size; x++) {
            const guchar *p = pix + y * rowstride + x * channels;
            unsigned char *o = rgba + (((size_t)(y + oy) * size) + (x + ox)) * 4;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            o[3] = (channels == 4) ? p[3] : 255;
        }
    }
    g_object_unref(pb);

    Pixmap pm = composite_to_pixmap(rgba, size, size, size, bg);
    free(rgba);
    return pm;
}

/* Render the best _NET_WM_ICON image of `w` to a size*size Pixmap over bg. */
static Pixmap load_net_wm_icon(Window w, int size, unsigned long bg) {
    unsigned long n = 0;
    unsigned char *d = get_prop(w, a_net_wm_icon, XA_CARDINAL, &n);
    if (!d || n < 2) { if (d) XFree(d); return None; }

    unsigned long *raw = (unsigned long *)d;   /* w,h,ARGB... possibly repeated */
    unsigned long best_off = 0, best_w = 0, best_h = 0;
    long best_diff = -1;
    unsigned long i = 0;
    while (i + 2 <= n) {
        unsigned long iw = raw[i], ih = raw[i + 1];
        /* Bound dimensions BEFORE multiplying: a hostile _NET_WM_ICON with huge
         * w/h could otherwise overflow iw*ih past the length guard and lead to
         * an out-of-bounds read in the compositor. Real icons are <= 256. */
        if (iw == 0 || ih == 0 || iw > 1024 || ih > 1024 ||
            i + 2 + iw * ih > n) break;
        long diff = (long)iw - size;
        if (diff < 0) diff = -diff * 2;        /* prefer icons >= target */
        if (best_diff < 0 || diff < best_diff) {
            best_diff = diff; best_off = i + 2; best_w = iw; best_h = ih;
        }
        i += 2 + iw * ih;
    }
    if (best_w == 0 || best_h == 0) { XFree(d); return None; }

    unsigned char *rgba = (unsigned char *)malloc((size_t)best_w * best_h * 4);
    if (!rgba) { XFree(d); return None; }
    for (unsigned long p = 0; p < best_w * best_h; p++) {
        unsigned long argb = raw[best_off + p];
        rgba[p * 4 + 0] = (argb >> 16) & 0xff;  /* R */
        rgba[p * 4 + 1] = (argb >> 8) & 0xff;   /* G */
        rgba[p * 4 + 2] = argb & 0xff;          /* B */
        rgba[p * 4 + 3] = (argb >> 24) & 0xff;  /* A */
    }
    XFree(d);

    Pixmap pm = composite_to_pixmap(rgba, (int)best_w, (int)best_h, size, bg);
    free(rgba);
    return pm;
}

static void load_all_icons(void) {
    for (int i = 0; i < NUM_ITEMS; i++)
        items[i].icon_pixmap = load_icon_pixmap(items[i].icon, item_size, cfg.item_bg_color);
}

/* ---- User pins (persisted favourites) ------------------------------------ */

static void pinned_conf_path(char *buf, size_t n) {
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/churro/pinned.conf", home ? home : "/tmp");
}

/* True if `cls` is already a user pin. */
static int is_user_pinned(const char *cls) {
    for (int i = 0; i < num_user_pins; i++)
        if (strcmp(user_pins[i].cls, cls) == 0) return 1;
    return 0;
}

/* Build the icon + label for a freshly added pin entry. */
static void init_user_pin(UserPin *p, const char *cls) {
    snprintf(p->cls, sizeof(p->cls), "%s", cls);
    snprintf(p->label, sizeof(p->label), "%s", cls);
    if (p->label[0]) p->label[0] = (char)toupper((unsigned char)p->label[0]);
    p->icon = load_icon_pixmap(cls, item_size, cfg.item_bg_color);
    p->running = 0;
    p->win = None;
}

static void save_user_pins(void) {
    churro_ensure_config_dir();
    char path[512];
    pinned_conf_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < num_user_pins; i++)
        fprintf(f, "%s\n", user_pins[i].cls);
    fclose(f);
}

static void load_user_pins(void) {
    char path[512];
    pinned_conf_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f) && num_user_pins < MAX_USER_PINS) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = 0;
        if (!len || is_user_pinned(line)) continue;
        init_user_pin(&user_pins[num_user_pins], line);
        num_user_pins++;
    }
    fclose(f);
}

static void add_user_pin(const char *cls) {
    if (!cls || !cls[0] || num_user_pins >= MAX_USER_PINS || is_user_pinned(cls))
        return;
    init_user_pin(&user_pins[num_user_pins], cls);
    num_user_pins++;
    save_user_pins();
}

static void remove_user_pin(int idx) {
    if (idx < 0 || idx >= num_user_pins) return;
    if (user_pins[idx].icon != None) XFreePixmap(dpy, user_pins[idx].icon);
    for (int i = idx; i < num_user_pins - 1; i++) user_pins[i] = user_pins[i + 1];
    num_user_pins--;
    save_user_pins();
}

/* ---- Running-window collection ------------------------------------------ */

/* Lowercase a copy of `s` into `dst`. */
static void lower_copy(char *dst, size_t n, const char *s) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)s[i]);
    dst[i] = 0;
}

/* True if a lowercased class string matches any pinned item's `match`. */
static int matches_pinned(const char *lc) {
    for (int k = 0; k < NUM_ITEMS; k++)
        if (items[k].match && items[k].match[0] && strstr(lc, items[k].match))
            return 1;
    for (int k = 0; k < num_user_pins; k++)
        if (user_pins[k].cls[0] && strstr(lc, user_pins[k].cls))
            return 1;
    return 0;
}

/* A cheap, stable colour derived from a label (for the lettered fallback). */
static unsigned long color_from_label(const char *s) {
    unsigned long h = 5381;
    for (const char *p = s; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
    /* keep it mid-bright so white text/letters read on top */
    int r = 0x40 + (h & 0x5f);
    int g = 0x40 + ((h >> 8) & 0x5f);
    int b = 0x40 + ((h >> 16) & 0x5f);
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

/* Refresh both the pinned "running" dots and the dynamic window slots.
 * Returns 1 if anything visible changed (dots or the set of window slots). */
static int refresh_windows(void) {
    unsigned long n = 0;
    unsigned char *d = get_prop(root, a_net_client_list, XA_WINDOW, &n);
    Window *wins = (Window *)d;   /* may be NULL */

    /* --- pinned running dots --- */
    int dot_changed = 0;
    int pinned_run[NUM_ITEMS];
    for (int i = 0; i < NUM_ITEMS; i++) pinned_run[i] = 0;

    /* --- user-pin running state --- */
    int upin_run[MAX_USER_PINS];
    Window upin_win[MAX_USER_PINS];
    for (int i = 0; i < num_user_pins; i++) { upin_run[i] = 0; upin_win[i] = None; }

    /* --- fresh set of dynamic windows --- */
    Window newwin[MAX_WIN_SLOTS];
    char newlabel[MAX_WIN_SLOTS][128];
    int newcount = 0;

    for (unsigned long w = 0; wins && w < n; w++) {
        Window cw = wins[w];
        XClassHint ch; ch.res_name = ch.res_class = NULL;
        char lc[192] = "";
        char label[128] = "";
        if (XGetClassHint(dpy, cw, &ch)) {
            char lc1[128] = "", lc2[128] = "";
            lower_copy(lc1, sizeof(lc1), ch.res_name);
            lower_copy(lc2, sizeof(lc2), ch.res_class);
            snprintf(lc, sizeof(lc), "%s|%s", lc1, lc2);
            const char *base = (ch.res_class && ch.res_class[0]) ? ch.res_class
                             : (ch.res_name ? ch.res_name : "");
            snprintf(label, sizeof(label), "%.*s", (int)sizeof(label) - 1, base);
            if (label[0]) label[0] = (char)toupper((unsigned char)label[0]);
        }

        /* mark pinned dots regardless of taskbar filtering */
        for (int i = 0; i < NUM_ITEMS; i++)
            if (items[i].match && items[i].match[0] && strstr(lc, items[i].match))
                pinned_run[i] = 1;

        /* mark user-pin running state + remember a window to focus */
        for (int i = 0; i < num_user_pins; i++)
            if (user_pins[i].cls[0] && strstr(lc, user_pins[i].cls)) {
                upin_run[i] = 1;
                if (upin_win[i] == None && is_taskbar_window(cw)) upin_win[i] = cw;
            }

        int keep = is_taskbar_window(cw) && label[0] && !matches_pinned(lc);
        if (keep && newcount < MAX_WIN_SLOTS) {
            newwin[newcount] = cw;
            snprintf(newlabel[newcount], sizeof(newlabel[newcount]), "%s", label);
            newcount++;
        }

        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (d) XFree(d);

    for (int i = 0; i < NUM_ITEMS; i++) {
        if (pinned_run[i] != items[i].running) { items[i].running = pinned_run[i]; dot_changed = 1; }
    }
    for (int i = 0; i < num_user_pins; i++) {
        if (upin_run[i] != user_pins[i].running) { user_pins[i].running = upin_run[i]; dot_changed = 1; }
        user_pins[i].win = upin_win[i];
    }

    /* Same window set (by id + order)? then nothing structural changed. */
    int same = (newcount == num_win_slots);
    for (int i = 0; same && i < newcount; i++)
        if (newwin[i] != win_slots[i].win) same = 0;

    if (same) return dot_changed;

    /* Rebuild slots, reusing icons for windows that persisted. */
    WinSlot repl[MAX_WIN_SLOTS];
    int kept[MAX_WIN_SLOTS];
    for (int i = 0; i < num_win_slots; i++) kept[i] = 0;

    for (int j = 0; j < newcount; j++) {
        repl[j].win = newwin[j];
        snprintf(repl[j].label, sizeof(repl[j].label), "%.*s",
                 (int)sizeof(repl[j].label) - 1, newlabel[j]);
        repl[j].color = color_from_label(newlabel[j]);
        repl[j].icon = None;

        int found = -1;
        for (int i = 0; i < num_win_slots; i++)
            if (win_slots[i].win == newwin[j]) { found = i; break; }

        if (found >= 0) {
            repl[j].icon = win_slots[found].icon;   /* transfer ownership */
            kept[found] = 1;
        } else {
            char cls[128];
            lower_copy(cls, sizeof(cls), newlabel[j]);   /* label is capitalised class */
            repl[j].icon = load_icon_pixmap(cls, item_size, cfg.item_bg_color);
            if (repl[j].icon == None)
                repl[j].icon = load_net_wm_icon(newwin[j], item_size, cfg.item_bg_color);
        }
    }

    for (int i = 0; i < num_win_slots; i++)
        if (!kept[i] && win_slots[i].icon != None)
            XFreePixmap(dpy, win_slots[i].icon);

    for (int j = 0; j < newcount; j++) win_slots[j] = repl[j];
    num_win_slots = newcount;
    return 1;
}

/* ---- Geometry / layout --------------------------------------------------- */

static int item_top(void) { return (dock_height - item_size) / 2; }

static void build_layout(void) {
    num_rslots = 0;
    sep_x = -1;
    int x = DOCK_MARGIN;

    RSlot s;
    s.kind = SLOT_SEARCH; s.index = 0; s.x = x; s.w = SEARCH_BAR_WIDTH;
    rslots[num_rslots++] = s;
    x += SEARCH_BAR_WIDTH;

    for (int i = 0; i < NUM_ITEMS; i++) {
        x += DOCK_GAP;
        s.kind = SLOT_PINNED; s.index = i; s.x = x; s.w = item_size;
        rslots[num_rslots++] = s;
        x += item_size;
    }

    for (int i = 0; i < num_user_pins; i++) {
        x += DOCK_GAP;
        s.kind = SLOT_USERPIN; s.index = i; s.x = x; s.w = item_size;
        rslots[num_rslots++] = s;
        x += item_size;
    }

    if (num_win_slots > 0) {
        sep_x = x + SEP_GAP / 2;
        x += SEP_GAP;
        for (int j = 0; j < num_win_slots; j++) {
            if (j > 0) x += DOCK_GAP;
            s.kind = SLOT_WINDOW; s.index = j; s.x = x; s.w = item_size;
            rslots[num_rslots++] = s;
            x += item_size;
        }
    }

    g_dock_w = x + DOCK_MARGIN;
}

/* Which rendered slot is at pixel x (any y in the dock)? -1 if none. */
static int rslot_at(int px) {
    for (int i = 0; i < num_rslots; i++)
        if (px >= rslots[i].x && px < rslots[i].x + rslots[i].w) return i;
    return -1;
}

/* ---- Shape / window sizing ---------------------------------------------- */

static void apply_shape(int w) {
    Pixmap mask = XCreatePixmap(dpy, dock_win, w, dock_height, 1);
    GC sg = XCreateGC(dpy, mask, 0, NULL);
    XSetForeground(dpy, sg, 0);
    XFillRectangle(dpy, mask, sg, 0, 0, w, dock_height);
    XSetForeground(dpy, sg, 1);
    int r = corner_radius;
    if (r * 2 > dock_height) r = dock_height / 2;
    if (r * 2 > w) r = w / 2;
    XFillRectangle(dpy, mask, sg, r, 0, w - 2 * r, dock_height);
    XFillRectangle(dpy, mask, sg, 0, r, w, dock_height - 2 * r);
    XFillArc(dpy, mask, sg, 0, 0, r * 2, r * 2, 90 * 64, 90 * 64);
    XFillArc(dpy, mask, sg, w - r * 2, 0, r * 2, r * 2, 0, 90 * 64);
    XFillArc(dpy, mask, sg, w - r * 2, dock_height - r * 2, r * 2, r * 2, 270 * 64, 90 * 64);
    XFillArc(dpy, mask, sg, 0, dock_height - r * 2, r * 2, r * 2, 180 * 64, 90 * 64);
    XShapeCombineMask(dpy, dock_win, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreePixmap(dpy, mask);
    XFreeGC(dpy, sg);
}

/* Discover the primary monitor's geometry via RandR so the dock lands on it
 * rather than being centred across a spanned multi-head screen. Falls back to
 * the full X screen when RandR is missing or reports nothing usable. */
static void detect_primary_monitor(void) {
    int screen = DefaultScreen(dpy);
    mon_x = 0; mon_y = 0;
    mon_w = DisplayWidth(dpy, screen);
    mon_h = DisplayHeight(dpy, screen);

    int event_base, error_base;
    if (!XRRQueryExtension(dpy, &event_base, &error_base)) return;

    XRRScreenResources *res = XRRGetScreenResources(dpy, root);
    if (!res) return;

    RRCrtc crtc = None;
    RROutput primary = XRRGetOutputPrimary(dpy, root);
    if (primary != None) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, primary);
        if (oi) {
            if (oi->connection == RR_Connected && oi->crtc) crtc = oi->crtc;
            XRRFreeOutputInfo(oi);
        }
    }
    /* No primary flagged? use the first connected output that drives a CRTC. */
    if (crtc == None) {
        for (int i = 0; i < res->noutput && crtc == None; i++) {
            XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (oi) {
                if (oi->connection == RR_Connected && oi->crtc) crtc = oi->crtc;
                XRRFreeOutputInfo(oi);
            }
        }
    }
    if (crtc != None) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, crtc);
        if (ci) {
            if (ci->width > 0 && ci->height > 0) {
                mon_x = ci->x; mon_y = ci->y;
                mon_w = (int)ci->width; mon_h = (int)ci->height;
            }
            XRRFreeCrtcInfo(ci);
        }
    }
    XRRFreeScreenResources(res);
}

/* Reserve the dock's screen band so maximized windows don't slide under it.
 * Struts are measured from the edges of the whole X screen, so the bottom
 * reservation runs from the dock's top down to the very bottom of the screen. */
static void set_strut(void) {
    int screen = DefaultScreen(dpy);
    int screen_h = DisplayHeight(dpy, screen);

    long bottom = screen_h - g_dock_y;
    if (bottom < 0) bottom = 0;

    /* _NET_WM_STRUT: left, right, top, bottom */
    long strut[4] = { 0, 0, 0, bottom };
    /* _NET_WM_STRUT_PARTIAL: the 4 above + start/end for each edge. Constrain
     * the bottom reservation to the dock's horizontal span. */
    long partial[12] = { 0, 0, 0, bottom,
                         0, 0, 0, 0,
                         0, 0,
                         g_dock_x, g_dock_x + g_dock_w - 1 };

    if (a_net_wm_strut != None)
        XChangeProperty(dpy, dock_win, a_net_wm_strut, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)strut, 4);
    if (a_net_wm_strut_partial != None)
        XChangeProperty(dpy, dock_win, a_net_wm_strut_partial, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)partial, 12);
}

/* Recompute layout; resize + re-centre the dock window if the width changed. */
static void relayout(void) {
    int old_w = g_dock_w, old_x = g_dock_x;
    build_layout();
    /* Keep the dock centred on the primary monitor. */
    g_dock_x = mon_x + (mon_w - g_dock_w) / 2;
    if (g_dock_x < mon_x) g_dock_x = mon_x;
    if (g_dock_w != old_w || g_dock_x != old_x) {
        XMoveResizeWindow(dpy, dock_win, g_dock_x, g_dock_y, g_dock_w, dock_height);
        if (g_dock_w != old_w) apply_shape(g_dock_w);
        set_strut();
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void draw_rounded_rect(int x, int y, int w, int h, int r) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    XFillRectangle(dpy, dock_win, gc, x + r, y, w - 2 * r, h);
    XFillRectangle(dpy, dock_win, gc, x, y + r, w, h - 2 * r);
    XFillArc(dpy, dock_win, gc, x, y, r * 2, r * 2, 90 * 64, 90 * 64);
    XFillArc(dpy, dock_win, gc, x + w - r * 2, y, r * 2, r * 2, 0, 90 * 64);
    XFillArc(dpy, dock_win, gc, x + w - r * 2, y + h - r * 2, r * 2, r * 2, 270 * 64, 90 * 64);
    XFillArc(dpy, dock_win, gc, x, y + h - r * 2, r * 2, r * 2, 180 * 64, 90 * 64);
}

static void draw_rounded_rect_outline(int x, int y, int w, int h, int r) {
    XDrawLine(dpy, dock_win, gc, x + r, y, x + w - r, y);
    XDrawLine(dpy, dock_win, gc, x + w, y + r, x + w, y + h - r);
    XDrawLine(dpy, dock_win, gc, x + w - r, y + h, x + r, y + h);
    XDrawLine(dpy, dock_win, gc, x, y + h - r, x, y + r);
    XDrawArc(dpy, dock_win, gc, x, y, r * 2, r * 2, 90 * 64, 90 * 64);
    XDrawArc(dpy, dock_win, gc, x + w - r * 2, y, r * 2, r * 2, 0, 90 * 64);
    XDrawArc(dpy, dock_win, gc, x + w - r * 2, y + h - r * 2, r * 2, r * 2, 270 * 64, 90 * 64);
    XDrawArc(dpy, dock_win, gc, x, y + h - r * 2, r * 2, r * 2, 180 * 64, 90 * 64);
}

/* Draw an icon square with a lettered fallback when no pixmap is available. */
static void draw_icon_slot(int x, int y, Pixmap pm, unsigned long fallback_col,
                           const char *label) {
    if (pm != None) {
        XCopyArea(dpy, pm, dock_win, gc, 0, 0, item_size, item_size, x, y);
        return;
    }
    XSetForeground(dpy, gc, cfg.item_bg_color);
    draw_rounded_rect(x, y, item_size, item_size, 8);
    XSetForeground(dpy, gc, fallback_col);
    XFillRectangle(dpy, dock_win, gc, x + 4, y + 4, item_size - 8, item_size - 8);
    if (label && label[0]) {
        char ch[2] = { (char)toupper((unsigned char)label[0]), 0 };
        XSetForeground(dpy, gc, cfg.text_color);
        XSetFont(dpy, gc, font_label->fid);
        XDrawString(dpy, dock_win, gc, x + item_size / 2 - 3, y + item_size / 2 + 5, ch, 1);
    }
    XSetForeground(dpy, gc, cfg.border_color);
    draw_rounded_rect_outline(x, y, item_size, item_size, 8);
}

static void draw_dock(void) {
    int w = g_dock_w;

    XSetForeground(dpy, gc, cfg.dock_bg_color);
    XFillRectangle(dpy, dock_win, gc, 0, 0, w, dock_height);

    XSetForeground(dpy, gc, cfg.item_bg_color);
    XFillRectangle(dpy, dock_win, gc, 0, 0, w, 2);

    XSetForeground(dpy, gc, cfg.border_color);
    draw_rounded_rect_outline(2, 2, w - 4, dock_height - 4, corner_radius);

    int iy = item_top();

    if (sep_x >= 0) {
        XSetForeground(dpy, gc, cfg.border_color);
        XDrawLine(dpy, dock_win, gc, sep_x, iy + 4, sep_x, iy + item_size - 4);
    }

    for (int r = 0; r < num_rslots; r++) {
        RSlot *rs = &rslots[r];
        if (rs->kind == SLOT_SEARCH) {
            XSetForeground(dpy, gc, cfg.item_bg_color);
            draw_rounded_rect(rs->x, iy, SEARCH_BAR_WIDTH, item_size, 8);
            XSetForeground(dpy, gc, cfg.border_color);
            draw_rounded_rect_outline(rs->x, iy, SEARCH_BAR_WIDTH, item_size, 8);
            XSetForeground(dpy, gc, cfg.text_color);
            XSetFont(dpy, gc, font_label->fid);
            XDrawString(dpy, dock_win, gc, rs->x + 14, iy + item_size / 2 + 5, "Search...", 9);
            continue;
        }

        const char *label;
        Pixmap pm;
        unsigned long fcol;
        int running = 0;
        if (rs->kind == SLOT_PINNED) {
            label = items[rs->index].label;
            pm = items[rs->index].icon_pixmap;
            fcol = items[rs->index].color;
            running = items[rs->index].running;
        } else if (rs->kind == SLOT_USERPIN) {
            label = user_pins[rs->index].label;
            pm = user_pins[rs->index].icon;
            fcol = color_from_label(user_pins[rs->index].label);
            running = user_pins[rs->index].running;
        } else {
            label = win_slots[rs->index].label;
            pm = win_slots[rs->index].icon;
            fcol = win_slots[rs->index].color;
            running = 1;   /* dynamic slots are always running */
        }

        draw_icon_slot(rs->x, iy, pm, fcol, label);

        /* label text under the icon */
        XSetForeground(dpy, gc, cfg.text_color);
        XSetFont(dpy, gc, font_label->fid);
        int label_len = (int)strlen(label);
        int draw_len = label_len > 10 ? 10 : label_len;
        int label_x = rs->x + (item_size / 2) - (draw_len * 3);
        int label_y = iy + item_size + 12;
        if (label_y < dock_height - 1)
            XDrawString(dpy, dock_win, gc, label_x, label_y, label, draw_len);

        if (running) {
            XSetForeground(dpy, gc, cfg.accent_color);
            int dot = 5;
            XFillArc(dpy, dock_win, gc, rs->x + item_size / 2 - dot / 2, iy - 6,
                     dot, dot, 0, 360 * 64);
        }
    }

    XFlush(dpy);
}

/* ---- EWMH actions -------------------------------------------------------- */

static void send_client_message(Window target, Atom type, long d0, long d1, long d2) {
    if (type == None) return;
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.type = ClientMessage;
    e.xclient.window = target;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = d0;
    e.xclient.data.l[1] = d1;
    e.xclient.data.l[2] = d2;
    XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &e);
    XFlush(dpy);
}

static void activate_window(Window w) {
    if (w == None) return;
    send_client_message(w, a_net_active_window, 2, CurrentTime, 0);
    XRaiseWindow(dpy, w);
    XFlush(dpy);
}

static void close_window(Window w) {
    if (w == None) return;
    send_client_message(w, a_net_close_window, CurrentTime, 2, 0);
}

/* Iconify (minimize) via the ICCCM WM_CHANGE_STATE mechanism, which every
 * EWMH WM honours and which maps to _NET_WM_STATE_HIDDEN under the hood. */
static void minimize_window(Window w) {
    if (w == None) return;
    if (a_wm_change_state != None) {
        send_client_message(w, a_wm_change_state, IconicState, 0, 0);
    } else {
        XIconifyWindow(dpy, w, DefaultScreen(dpy));
    }
    XFlush(dpy);
}

/* Best-effort launch of a user-pinned app by its lowercase class key. */
static void launch_by_class(const char *cls) {
    if (!cls || !cls[0]) return;
    pid_t pid = fork();
    if (pid == 0) { execl("/bin/sh", "sh", "-c", cls, NULL); _exit(127); }
}

static void launch_pinned(int item_index) {
    if (item_index < 0 || item_index >= NUM_ITEMS) return;
    if (strcmp(items[item_index].label, "Menu") == 0) {
        pid_t pid = fork();
        if (pid == 0) { execl("/bin/sh", "sh", "-c", "churro-launcher &", NULL); _exit(127); }
        return;
    }
    if (!items[item_index].cmd || strlen(items[item_index].cmd) == 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", items[item_index].cmd, NULL);
        if (items[item_index].fallback_cmd)
            execl("/bin/sh", "sh", "-c", items[item_index].fallback_cmd, NULL);
        _exit(127);
    }
}

/* ---- Tooltip ------------------------------------------------------------- */

static void hide_tooltip(void) {
    if (tip_shown) { XUnmapWindow(dpy, tip_win); tip_shown = 0; }
}

static const char *rslot_label(int r) {
    if (r < 0 || r >= num_rslots) return NULL;
    if (rslots[r].kind == SLOT_SEARCH) return "Search / Run apps";
    if (rslots[r].kind == SLOT_PINNED) return items[rslots[r].index].label;
    if (rslots[r].kind == SLOT_USERPIN) return user_pins[rslots[r].index].label;
    return win_slots[rslots[r].index].label;
}

static void show_tooltip(int r) {
    const char *text = rslot_label(r);
    if (!text || !text[0] || tip_win == None) { hide_tooltip(); return; }

    int len = (int)strlen(text);
    int tw = XTextWidth(font_label, text, len) + 16;
    int th = 22;
    int cx = g_dock_x + rslots[r].x + rslots[r].w / 2;
    int tx = cx - tw / 2;
    int ty = g_dock_y - th - 6;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;

    XMoveResizeWindow(dpy, tip_win, tx, ty, tw, th);
    XMapRaised(dpy, tip_win);
    tip_shown = 1;

    XSetForeground(dpy, tip_gc, cfg.dock_bg_color);
    XFillRectangle(dpy, tip_win, tip_gc, 0, 0, tw, th);
    XSetForeground(dpy, tip_gc, cfg.border_color);
    XDrawRectangle(dpy, tip_win, tip_gc, 0, 0, tw - 1, th - 1);
    XSetForeground(dpy, tip_gc, cfg.text_color);
    XSetFont(dpy, tip_gc, font_label->fid);
    XDrawString(dpy, tip_win, tip_gc, 8, 15, text, len);
    XFlush(dpy);
}

/* ---- Context menu -------------------------------------------------------- */

static void close_context_menu(void) {
    if (ctx_win != None) {
        XUngrabPointer(dpy, CurrentTime);
        XFreeGC(dpy, ctx_gc);
        XDestroyWindow(dpy, ctx_win);
        ctx_win = None;
        ctx_count = 0;
        ctx_hover = -1;
    }
}

static void draw_context_menu(void) {
    if (ctx_win == None) return;
    int h = ctx_count * CTX_ITEM_H;
    XSetForeground(dpy, ctx_gc, cfg.dock_bg_color);
    XFillRectangle(dpy, ctx_win, ctx_gc, 0, 0, CTX_W, h);
    XSetForeground(dpy, ctx_gc, cfg.border_color);
    XDrawRectangle(dpy, ctx_win, ctx_gc, 0, 0, CTX_W - 1, h - 1);

    for (int i = 0; i < ctx_count; i++) {
        int y = i * CTX_ITEM_H;
        if (i == ctx_hover) {
            XSetForeground(dpy, ctx_gc, cfg.accent_color);
            XFillRectangle(dpy, ctx_win, ctx_gc, 1, y + 1, CTX_W - 2, CTX_ITEM_H - 1);
            XSetForeground(dpy, ctx_gc, 0x0a0a14);
        } else {
            XSetForeground(dpy, ctx_gc, cfg.text_color);
        }
        XSetFont(dpy, ctx_gc, font_label->fid);
        XDrawString(dpy, ctx_win, ctx_gc, 12, y + 17, ctx_labels[i], (int)strlen(ctx_labels[i]));
    }
    XFlush(dpy);
}

static void open_context_menu(int r, int root_x) {
    if (r < 0 || r >= num_rslots || rslots[r].kind == SLOT_SEARCH) return;
    hide_tooltip();
    close_context_menu();

    ctx_kind = rslots[r].kind;
    ctx_index = rslots[r].index;
    ctx_count = 0;
    ctx_pin_cls[0] = 0;

    if (ctx_kind == SLOT_WINDOW) {
        ctx_target = win_slots[ctx_index].win;
        lower_copy(ctx_pin_cls, sizeof(ctx_pin_cls), win_slots[ctx_index].label);
        snprintf(ctx_labels[ctx_count], 24, "Focus");    ctx_actions[ctx_count++] = ACT_FOCUS;
        snprintf(ctx_labels[ctx_count], 24, "Minimize"); ctx_actions[ctx_count++] = ACT_MINIMIZE;
        if (!is_user_pinned(ctx_pin_cls) && num_user_pins < MAX_USER_PINS) {
            snprintf(ctx_labels[ctx_count], 24, "Pin");  ctx_actions[ctx_count++] = ACT_PIN;
        }
        snprintf(ctx_labels[ctx_count], 24, "Close");    ctx_actions[ctx_count++] = ACT_CLOSE;
    } else if (ctx_kind == SLOT_USERPIN) {
        ctx_target = user_pins[ctx_index].win;
        if (user_pins[ctx_index].running) {
            snprintf(ctx_labels[ctx_count], 24, "Focus");    ctx_actions[ctx_count++] = ACT_FOCUS;
            snprintf(ctx_labels[ctx_count], 24, "Minimize"); ctx_actions[ctx_count++] = ACT_MINIMIZE;
        } else {
            snprintf(ctx_labels[ctx_count], 24, "Launch");   ctx_actions[ctx_count++] = ACT_LAUNCH;
        }
        snprintf(ctx_labels[ctx_count], 24, "Unpin");        ctx_actions[ctx_count++] = ACT_UNPIN;
    } else { /* static pinned */
        ctx_target = None;
        if (strcmp(items[ctx_index].label, "Menu") == 0)
            snprintf(ctx_labels[ctx_count], 24, "Open Menu");
        else
            snprintf(ctx_labels[ctx_count], 24, "Launch");
        ctx_actions[ctx_count++] = ACT_LAUNCH;
    }

    int h = ctx_count * CTX_ITEM_H;
    int mx = root_x - CTX_W / 2;
    if (mx < 0) mx = 0;
    int screen = DefaultScreen(dpy);
    if (mx + CTX_W > DisplayWidth(dpy, screen)) mx = DisplayWidth(dpy, screen) - CTX_W;
    int my = g_dock_y - h - 6;
    if (my < 0) my = 0;

    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixel = cfg.dock_bg_color;
    a.border_pixel = cfg.border_color;
    a.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask;
    ctx_win = XCreateWindow(dpy, root, mx, my, CTX_W, h, 0,
                            DefaultDepth(dpy, screen), InputOutput,
                            DefaultVisual(dpy, screen),
                            CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &a);
    ctx_gc = XCreateGC(dpy, ctx_win, 0, NULL);
    XMapRaised(dpy, ctx_win);
    XGrabPointer(dpy, ctx_win, False,
                 ButtonPressMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    draw_context_menu();
}

static void context_menu_action(int idx) {
    if (idx < 0 || idx >= ctx_count) return;
    switch (ctx_actions[idx]) {
        case ACT_FOCUS:    activate_window(ctx_target); break;
        case ACT_MINIMIZE: minimize_window(ctx_target); break;
        case ACT_CLOSE:    close_window(ctx_target);    break;
        case ACT_PIN:
            add_user_pin(ctx_pin_cls);
            if (refresh_windows()) { relayout(); draw_dock(); }
            break;
        case ACT_UNPIN:
            if (ctx_kind == SLOT_USERPIN) {
                remove_user_pin(ctx_index);
                relayout();
                draw_dock();
            }
            break;
        case ACT_LAUNCH:
            if (ctx_kind == SLOT_PINNED)       launch_pinned(ctx_index);
            else if (ctx_kind == SLOT_USERPIN) launch_by_class(user_pins[ctx_index].cls);
            break;
    }
}

/* ---- Click dispatch ------------------------------------------------------ */

static void handle_left_click(int x) {
    int r = rslot_at(x);
    if (r < 0) return;
    if (rslots[r].kind == SLOT_SEARCH) {
        pid_t pid = fork();
        if (pid == 0) { execl("/bin/sh", "sh", "-c", "churro-launcher &", NULL); _exit(127); }
    } else if (rslots[r].kind == SLOT_PINNED) {
        launch_pinned(rslots[r].index);
    } else if (rslots[r].kind == SLOT_USERPIN) {
        UserPin *p = &user_pins[rslots[r].index];
        if (p->win != None) activate_window(p->win);
        else launch_by_class(p->cls);
    } else {
        activate_window(win_slots[rslots[r].index].win);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    churro_load_settings(&cfg);
    dock_height   = cfg.dock_height;
    item_size     = cfg.icon_size;
    corner_radius = cfg.corner_radius;

    /* Intern atoms once. Message atoms use False (created if absent); the
     * results are still guarded everywhere they are used. */
    a_net_client_list   = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    a_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    a_net_close_window  = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    a_net_wm_icon       = XInternAtom(dpy, "_NET_WM_ICON", False);
    a_net_wm_window_type= XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    a_type_desktop      = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    a_type_dock         = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a_type_toolbar      = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    a_type_menu         = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    a_type_splash       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    a_net_wm_state      = XInternAtom(dpy, "_NET_WM_STATE", False);
    a_state_skip_taskbar= XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    a_net_wm_strut      = XInternAtom(dpy, "_NET_WM_STRUT", False);
    a_net_wm_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    a_wm_change_state   = XInternAtom(dpy, "WM_CHANGE_STATE", False);

    font_label = XLoadQueryFont(dpy, "-*-fixed-medium-r-*-*-10-*-*-*-*-*-*-*");
    if (!font_label) font_label = XLoadQueryFont(dpy, "fixed");
    if (!font_label) { fprintf(stderr, "Cannot load any font\n"); return 1; }

    XSetWindowAttributes attrs;
    attrs.background_pixel = cfg.dock_bg_color;
    attrs.border_pixel = cfg.border_color;
    attrs.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask;

    detect_primary_monitor();

    build_layout();
    g_dock_x = mon_x + (mon_w - g_dock_w) / 2;
    if (g_dock_x < mon_x) g_dock_x = mon_x;
    g_dock_y = mon_y + mon_h - dock_height - DOCK_EDGE_GAP;

    dock_win = XCreateWindow(dpy, root, g_dock_x, g_dock_y, g_dock_w, dock_height, 1,
                             DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
                             CWBackPixel | CWBorderPixel | CWEventMask, &attrs);

    Atom net_wm_window_type = a_net_wm_window_type;
    Atom net_wm_window_type_dock = a_type_dock;
    if (net_wm_window_type != None && net_wm_window_type_dock != None)
        XChangeProperty(dpy, dock_win, net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&net_wm_window_type_dock, 1);

    set_strut();
    apply_shape(g_dock_w);

    XGCValues gc_values;
    gc_values.foreground = WhitePixel(dpy, screen);
    gc_values.background = BlackPixel(dpy, screen);
    gc = XCreateGC(dpy, dock_win, GCForeground | GCBackground, &gc_values);

    /* Persistent tooltip window (created once, mapped on demand). */
    XSetWindowAttributes ta;
    ta.override_redirect = True;
    ta.background_pixel = cfg.dock_bg_color;
    ta.border_pixel = cfg.border_color;
    tip_win = XCreateWindow(dpy, root, 0, 0, 10, 10, 1,
                            DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
                            CWOverrideRedirect | CWBackPixel | CWBorderPixel, &ta);
    tip_gc = XCreateGC(dpy, tip_win, 0, NULL);

    load_all_icons();
    load_user_pins();   /* needs dock_win for icon pixmaps */

    XMapWindow(dpy, dock_win);
    XRaiseWindow(dpy, dock_win);
    XFlush(dpy);

    printf("[Churro Dock] Ready (%d pinned, %d user-pinned)\n",
           NUM_ITEMS, num_user_pins);

    refresh_windows();
    relayout();   /* picks up user-pin widths + re-centres on the primary monitor */
    draw_dock();

    int xfd = ConnectionNumber(dpy);

    while (1) {
        while (XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);

            /* Context menu owns the pointer while open (grabbed, owner_events
             * False => all pointer events arrive relative to ctx_win). */
            if (ctx_win != None) {
                if (event.type == Expose && event.xexpose.window == ctx_win) {
                    draw_context_menu();
                    continue;
                }
                if (event.type == MotionNotify) {
                    int mx = event.xmotion.x, my = event.xmotion.y;
                    int nh = (mx >= 0 && mx < CTX_W && my >= 0 && my < ctx_count * CTX_ITEM_H)
                             ? my / CTX_ITEM_H : -1;
                    if (nh != ctx_hover) { ctx_hover = nh; draw_context_menu(); }
                    continue;
                }
                if (event.type == ButtonPress) {
                    int mx = event.xbutton.x, my = event.xbutton.y;
                    int idx = (mx >= 0 && mx < CTX_W && my >= 0 && my < ctx_count * CTX_ITEM_H)
                              ? my / CTX_ITEM_H : -1;
                    close_context_menu();
                    if (idx >= 0) context_menu_action(idx);
                    continue;
                }
                /* fall through for Expose on other windows, etc. */
            }

            if (event.type == ButtonPress && event.xbutton.window == dock_win) {
                hide_tooltip();
                if (event.xbutton.button == Button3) {
                    int r = rslot_at(event.xbutton.x);
                    open_context_menu(r, g_dock_x + event.xbutton.x);
                } else if (event.xbutton.button == Button1) {
                    handle_left_click(event.xbutton.x);
                }
            } else if (event.type == MotionNotify && event.xmotion.window == dock_win) {
                int r = rslot_at(event.xmotion.x);
                if (r != hovered_rslot) {
                    hovered_rslot = r;
                    if (r >= 0) show_tooltip(r);
                    else hide_tooltip();
                }
            } else if (event.type == LeaveNotify && event.xcrossing.window == dock_win) {
                hovered_rslot = -1;
                hide_tooltip();
            } else if (event.type == Expose && event.xexpose.window == dock_win) {
                draw_dock();
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv;
        tv.tv_sec = REFRESH_SECONDS;
        tv.tv_usec = 0;
        select(xfd + 1, &fds, NULL, NULL, &tv);

        if (refresh_windows()) {
            relayout();
            draw_dock();
        }
    }

    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
