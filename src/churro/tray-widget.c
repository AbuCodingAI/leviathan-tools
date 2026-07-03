/*
 * Churro Tray — live system status widget.
 *
 * Real readings (polled on a sane interval, not hardcoded):
 *   - battery %% + charging state from /sys/class/power_supply/BAT*
 *   - wifi signal from /proc/net/wireless
 *   - volume from wpctl (PipeWire) or amixer (ALSA), if present
 *
 * The quick-toggle menu keeps the real actions (rfkill / nmcli / bluetoothctl)
 * and reflects live rfkill state where we can read it.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <glob.h>
#include <sys/select.h>
#include <sys/time.h>

#include "churro-settings.h"

#define TRAY_WIDTH 400
#define TRAY_HEIGHT 60
#define MENU_WIDTH 280
#define MENU_HEIGHT 200
#define POLL_SECONDS 2

Display *dpy;
Window tray_win, menu_win = None;
GC gc, menu_gc;
XFontStruct *font_tray, *font_menu;
int menu_visible = 0;

ChurroSettings cfg;

typedef struct {
    int battery_percent;   /* -1 = no battery */
    int is_charging;
    int wifi_strength;     /* 0-100, -1 = no wifi */
    int wifi_connected;
    int volume;            /* 0-100, -1 = unknown */
    int muted;
    int airplane_mode;
    int wifi_enabled;
    int bluetooth_enabled;
    char power_mode[20];
} SystemStatus;

SystemStatus status = { -1, 0, -1, 0, -1, 0, 0, 1, 1, "Balanced" };

/* ---- Live readers -------------------------------------------------------- */

static int read_battery(int *charging) {
    glob_t g;
    int pct = -1;
    *charging = 0;

    if (glob("/sys/class/power_supply/BAT*/capacity", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        FILE *f = fopen(g.gl_pathv[0], "r");
        if (f) {
            if (fscanf(f, "%d", &pct) != 1) pct = -1;
            fclose(f);
        }
    }
    globfree(&g);

    if (glob("/sys/class/power_supply/BAT*/status", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        FILE *f = fopen(g.gl_pathv[0], "r");
        if (f) {
            char st[32] = {0};
            if (fgets(st, sizeof(st), f))
                *charging = (strncmp(st, "Charging", 8) == 0 || strncmp(st, "Full", 4) == 0);
            fclose(f);
        }
    }
    globfree(&g);

    return pct;
}

static int read_wifi(int *connected) {
    FILE *f = fopen("/proc/net/wireless", "r");
    *connected = 0;
    if (!f) return -1;

    char line[256];
    int lineno = 0;
    int quality = -1;
    while (fgets(line, sizeof(line), f)) {
        if (++lineno <= 2) continue;   /* skip the two header rows */
        char iface[32];
        int stat_field;
        float link = 0;
        if (sscanf(line, " %31[^:]: %d %f", iface, &stat_field, &link) >= 3) {
            quality = (int)link;       /* link quality, typically out of 70 */
            *connected = quality > 0;
            break;
        }
    }
    fclose(f);

    if (quality >= 0) {
        quality = quality * 100 / 70;
        if (quality > 100) quality = 100;
    }
    return quality;
}

static int read_volume(int *muted) {
    *muted = 0;
    int vol = -1;

    FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (p) {
        char buf[128];
        if (fgets(buf, sizeof(buf), p)) {
            float v;
            if (sscanf(buf, "Volume: %f", &v) == 1) {
                vol = (int)(v * 100 + 0.5f);
                if (strstr(buf, "MUTED")) *muted = 1;
            }
        }
        pclose(p);
    }

    if (vol < 0) {
        p = popen("amixer get Master 2>/dev/null | grep -o '[0-9]*%' | head -1", "r");
        if (p) {
            char b[32];
            if (fgets(b, sizeof(b), p)) vol = atoi(b);
            pclose(p);
        }
    }
    return vol;
}

static int read_rfkill_blocked(const char *type) {
    /* returns 1 if all devices of `type` are soft-blocked */
    char path[128];
    glob_t g;
    int blocked = -1;
    snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill*/type");
    if (glob(path, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r");
            if (!f) continue;
            char t[32] = {0};
            if (fgets(t, sizeof(t), f)) {
                t[strcspn(t, "\n")] = 0;
                if (strcmp(t, type) == 0) {
                    fclose(f);
                    char sp[160];
                    char dir[128];
                    strncpy(dir, g.gl_pathv[i], sizeof(dir) - 1);
                    dir[sizeof(dir) - 1] = 0;
                    char *slash = strrchr(dir, '/');
                    if (slash) *slash = 0;
                    snprintf(sp, sizeof(sp), "%s/soft", dir);
                    FILE *sf = fopen(sp, "r");
                    if (sf) {
                        int s = 0;
                        if (fscanf(sf, "%d", &s) == 1) blocked = (blocked == -1) ? s : (blocked && s);
                        fclose(sf);
                    }
                    continue;
                }
            }
            fclose(f);
        }
    }
    globfree(&g);
    return blocked;
}

static void poll_status(void) {
    status.battery_percent = read_battery(&status.is_charging);
    status.wifi_strength   = read_wifi(&status.wifi_connected);
    status.volume          = read_volume(&status.muted);

    int wblock = read_rfkill_blocked("wlan");
    if (wblock >= 0) status.wifi_enabled = !wblock;
    int air = read_rfkill_blocked("wlan");
    int bt = read_rfkill_blocked("bluetooth");
    if (bt >= 0) status.bluetooth_enabled = !bt;
    if (air >= 0 && bt >= 0) status.airplane_mode = (air && bt);
}

/* ---- Drawing ------------------------------------------------------------- */

static void draw_text(Window win, GC gc_param, int x, int y, const char *text, XFontStruct *fnt) {
    XSetFont(dpy, gc_param, fnt->fid);
    XDrawString(dpy, win, gc_param, x, y, text, strlen(text));
}

static void draw_tray(void) {
    char tray_text[160];
    char bat[32], wifi[32], vol[32];

    if (status.battery_percent >= 0)
        snprintf(bat, sizeof(bat), "%s %d%%",
                 status.is_charging ? "CHG" : "BAT", status.battery_percent);
    else
        snprintf(bat, sizeof(bat), "AC");

    if (status.wifi_strength >= 0 && status.wifi_connected)
        snprintf(wifi, sizeof(wifi), "WIFI %d%%", status.wifi_strength);
    else
        snprintf(wifi, sizeof(wifi), "WIFI --");

    if (status.volume >= 0)
        snprintf(vol, sizeof(vol), "%s %d%%", status.muted ? "MUTE" : "VOL", status.volume);
    else
        snprintf(vol, sizeof(vol), "VOL --");

    snprintf(tray_text, sizeof(tray_text), "%s   %s   %s", bat, wifi, vol);

    XClearWindow(dpy, tray_win);
    XSetForeground(dpy, gc, cfg.text_color);
    draw_text(tray_win, gc, 20, 35, tray_text, font_tray);
    XFlush(dpy);
}

static void draw_menu(void) {
    if (!menu_visible) return;

    XSetForeground(dpy, menu_gc, cfg.dock_bg_color);
    XFillRectangle(dpy, menu_win, menu_gc, 0, 0, MENU_WIDTH, MENU_HEIGHT);

    XSetForeground(dpy, menu_gc, cfg.border_color);
    XDrawRectangle(dpy, menu_win, menu_gc, 0, 0, MENU_WIDTH - 1, MENU_HEIGHT - 1);

    XSetForeground(dpy, menu_gc, cfg.text_color);

    draw_text(menu_win, menu_gc, 15, 25, "Airplane Mode", font_menu);
    draw_text(menu_win, menu_gc, 220, 25, status.airplane_mode ? "ON" : "OFF", font_menu);

    draw_text(menu_win, menu_gc, 15, 50, "WiFi", font_menu);
    draw_text(menu_win, menu_gc, 220, 50, status.wifi_enabled ? "ON" : "OFF", font_menu);

    draw_text(menu_win, menu_gc, 15, 75, "Bluetooth", font_menu);
    draw_text(menu_win, menu_gc, 220, 75, status.bluetooth_enabled ? "ON" : "OFF", font_menu);

    draw_text(menu_win, menu_gc, 15, 100, "Power Mode", font_menu);
    draw_text(menu_win, menu_gc, 140, 100, status.power_mode, font_menu);

    char detail[64];
    snprintf(detail, sizeof(detail), "Battery: %s",
             status.battery_percent >= 0 ? "" : "on AC power");
    if (status.battery_percent >= 0)
        snprintf(detail, sizeof(detail), "Battery: %d%% %s",
                 status.battery_percent, status.is_charging ? "(charging)" : "");
    draw_text(menu_win, menu_gc, 15, 130, detail, font_menu);

    snprintf(detail, sizeof(detail), "Volume: %d%%%s",
             status.volume >= 0 ? status.volume : 0, status.muted ? " (muted)" : "");
    draw_text(menu_win, menu_gc, 15, 155, detail, font_menu);

    XFlush(dpy);
}

/* ---- Toggle actions (unchanged behaviour) -------------------------------- */

static void run_cmd(const char *cmd) {
    int rc = system(cmd);
    (void)rc;
}

static void toggle_airplane_mode(void) {
    status.airplane_mode = !status.airplane_mode;
    run_cmd(status.airplane_mode ? "rfkill block all" : "rfkill unblock all");
}

static void toggle_wifi(void) {
    status.wifi_enabled = !status.wifi_enabled;
    run_cmd(status.wifi_enabled ? "nmcli radio wifi on" : "nmcli radio wifi off");
}

static void toggle_bluetooth(void) {
    status.bluetooth_enabled = !status.bluetooth_enabled;
    run_cmd(status.bluetooth_enabled ? "bluetoothctl power on" : "bluetoothctl power off");
}

static void cycle_power_mode(void) {
    if (strcmp(status.power_mode, "Balanced") == 0) {
        strcpy(status.power_mode, "Performance");
        run_cmd("echo 'performance' | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor");
    } else if (strcmp(status.power_mode, "Performance") == 0) {
        strcpy(status.power_mode, "Power Saver");
        run_cmd("echo 'powersave' | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor");
    } else {
        strcpy(status.power_mode, "Balanced");
        run_cmd("echo 'schedutil' | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor");
    }
}

static void handle_menu_click(int y) {
    if (y > 15 && y < 40) toggle_airplane_mode();
    else if (y > 40 && y < 65) toggle_wifi();
    else if (y > 65 && y < 90) toggle_bluetooth();
    else if (y > 90 && y < 115) cycle_power_mode();
}

static Visual *find_argb_visual(Display *d, int screen) {
    XVisualInfo vtemplate;
    int nvi;
    vtemplate.screen = screen;
    vtemplate.depth = 32;
    vtemplate.c_class = TrueColor;
    XVisualInfo *xvi = XGetVisualInfo(d, VisualScreenMask | VisualDepthMask | VisualClassMask,
                                      &vtemplate, &nvi);
    if (xvi) {
        Visual *visual = xvi[0].visual;
        XFree(xvi);
        return visual;
    }
    return DefaultVisual(d, screen);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    churro_load_settings(&cfg);

    font_tray = XLoadQueryFont(dpy, "-*-fixed-bold-r-*-*-16-*-*-*-*-*-*-*");
    if (!font_tray) font_tray = XLoadQueryFont(dpy, "fixed");
    font_menu = XLoadQueryFont(dpy, "-*-fixed-medium-r-*-*-12-*-*-*-*-*-*-*");
    if (!font_menu) font_menu = font_tray;

    Visual *visual = find_argb_visual(dpy, screen);
    Colormap colormap = XCreateColormap(dpy, root, visual, AllocNone);

    XSetWindowAttributes attrs;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.colormap = colormap;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask;

    int tray_x = DisplayWidth(dpy, screen) - TRAY_WIDTH - 20;
    int tray_y = 0;

    tray_win = XCreateWindow(dpy, root, tray_x, tray_y, TRAY_WIDTH, TRAY_HEIGHT, 0,
                             32, InputOutput, visual,
                             CWBackPixel | CWBorderPixel | CWColormap | CWOverrideRedirect | CWEventMask,
                             &attrs);

    Atom net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, tray_win, net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&net_wm_window_type_dock, 1);

    XGCValues gc_values;
    gc_values.foreground = WhitePixel(dpy, screen);
    gc_values.background = 0;
    gc = XCreateGC(dpy, tray_win, GCForeground | GCBackground, &gc_values);

    XMapWindow(dpy, tray_win);
    XFlush(dpy);

    poll_status();
    draw_tray();

    int xfd = ConnectionNumber(dpy);
    time_t last_poll = time(NULL);

    while (1) {
        /* Drain pending X events without busy-spinning. */
        while (XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);

            if (event.type == ButtonPress && event.xbutton.window == tray_win) {
                menu_visible = !menu_visible;
                if (menu_visible) {
                    attrs.event_mask = ExposureMask | ButtonPressMask;
                    menu_win = XCreateWindow(dpy, root, tray_x, tray_y + TRAY_HEIGHT,
                                             MENU_WIDTH, MENU_HEIGHT, 0, 32, InputOutput, visual,
                                             CWBackPixel | CWBorderPixel | CWColormap | CWOverrideRedirect | CWEventMask,
                                             &attrs);
                    menu_gc = XCreateGC(dpy, menu_win, GCForeground | GCBackground, &gc_values);
                    XMapWindow(dpy, menu_win);
                    draw_menu();
                } else if (menu_win) {
                    XDestroyWindow(dpy, menu_win);
                    XFreeGC(dpy, menu_gc);
                    menu_win = None;
                }
            } else if (event.type == ButtonPress && event.xbutton.window == menu_win) {
                handle_menu_click(event.xbutton.y);
                poll_status();
                draw_menu();
                draw_tray();
            } else if (event.type == Expose) {
                if (event.xexpose.window == tray_win) draw_tray();
                else if (event.xexpose.window == menu_win) draw_menu();
            }
        }

        /* Block on the X fd with a timeout so we can poll on interval. */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv;
        tv.tv_sec = POLL_SECONDS;
        tv.tv_usec = 0;
        select(xfd + 1, &fds, NULL, NULL, &tv);

        if (time(NULL) - last_poll >= POLL_SECONDS) {
            poll_status();
            draw_tray();
            if (menu_visible) draw_menu();
            last_poll = time(NULL);
        }
    }

    XFreeGC(dpy, gc);
    XFreeColormap(dpy, colormap);
    XCloseDisplay(dpy);
    return 0;
}
