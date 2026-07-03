#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "churro-settings.h"

Display *dpy;
Window root;
int running = 1;

ChurroSettings cfg;

/* EWMH atoms for the window switcher. Guarded on use. */
Atom a_net_client_list, a_net_active_window;
Atom a_net_wm_window_type, a_type_desktop, a_type_dock;
Atom a_net_wm_name, a_utf8_string;

#define SW_MAX 64

void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* True if `w` is a normal, switch-to-able client (not a desktop/dock). */
static int is_switchable(Window w) {
    if (a_net_wm_window_type == None) return 1;
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, a_net_wm_window_type, 0, 16, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &data) != Success || !data)
        return 1;   /* no type set => assume normal */
    int ok = 1;
    Atom *types = (Atom *)data;
    for (unsigned long i = 0; i < nitems; i++)
        if (types[i] == a_type_desktop || types[i] == a_type_dock) { ok = 0; break; }
    XFree(data);
    return ok;
}

/* Fetch a human-readable title for `w` (prefers _NET_WM_NAME, then WM_NAME). */
static void get_win_title(Window w, char *out, size_t n) {
    out[0] = 0;
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    if (a_net_wm_name != None &&
        XGetWindowProperty(dpy, w, a_net_wm_name, 0, 256, False,
                           a_utf8_string ? a_utf8_string : AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &data) == Success && data) {
        snprintf(out, n, "%s", (char *)data);
        XFree(data);
        if (out[0]) return;
    }

    char *wm_name = NULL;
    if (XFetchName(dpy, w, &wm_name) && wm_name) {
        snprintf(out, n, "%s", wm_name);
        XFree(wm_name);
        if (out[0]) return;
    }

    /* Last resort: the window class. */
    XClassHint ch; ch.res_name = ch.res_class = NULL;
    if (XGetClassHint(dpy, w, &ch)) {
        const char *base = (ch.res_class && ch.res_class[0]) ? ch.res_class
                         : (ch.res_name ? ch.res_name : "Window");
        snprintf(out, n, "%s", base);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (!out[0]) snprintf(out, n, "Window");
}

/* Build the filtered, switch-to-able client list. Returns count; sets *cur to
 * the index of the currently active window (or -1). */
static int build_switch_list(Window *list, int max, int *cur) {
    *cur = -1;
    if (a_net_client_list == None) return 0;

    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a_net_client_list, 0, 1024, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &data) != Success || !data)
        return 0;

    Window *all = (Window *)data;
    int count = 0;
    for (unsigned long i = 0; i < nitems && count < max; i++)
        if (is_switchable(all[i])) list[count++] = all[i];
    XFree(data);
    if (count == 0) return 0;

    Window active = None;
    unsigned char *ad = NULL;
    unsigned long an = 0;
    if (a_net_active_window != None &&
        XGetWindowProperty(dpy, root, a_net_active_window, 0, 1, False, XA_WINDOW,
                           &actual_type, &actual_format, &an, &bytes_after,
                           &ad) == Success && ad && an > 0) {
        active = *(Window *)ad;
    }
    if (ad) XFree(ad);

    for (int i = 0; i < count; i++) if (list[i] == active) { *cur = i; break; }
    return count;
}

static void activate(Window w) {
    if (w == None || a_net_active_window == None) return;
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = a_net_active_window;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2;            /* source indication: pager */
    e.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &e);
    XRaiseWindow(dpy, w);
    XFlush(dpy);
}

/* Window switcher with a centred overlay. Grabs the keyboard, shows the list
 * of open windows with the selection highlighted, advances on each Super+`
 * press, and commits (activating the chosen window) when Super is released.
 * Escape cancels. Degrades to a plain focus-cycle if the grab/overlay fail. */
static void run_switcher(int grave_code, unsigned int super_mask, int first_dir) {
    Window list[SW_MAX];
    char titles[SW_MAX][160];
    int cur = -1;
    int count = build_switch_list(list, SW_MAX, &cur);
    if (count == 0) return;
    for (int i = 0; i < count; i++) get_win_title(list[i], titles[i], sizeof(titles[i]));

    int sel = (cur < 0) ? (first_dir > 0 ? 0 : count - 1)
                        : ((cur + first_dir) % count + count) % count;

    int screen = DefaultScreen(dpy);

    /* Metrics + font. */
    XFontStruct *font = XLoadQueryFont(dpy, "-*-*-medium-r-normal-*-14-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "-*-fixed-medium-r-*-*-13-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "fixed");

    int pad = 14, row_h = 26, title_h = 24;
    int row_w = 360;
    for (int i = 0; i < count; i++) {
        int tw = font ? XTextWidth(font, titles[i], (int)strlen(titles[i])) : 8 * (int)strlen(titles[i]);
        if (tw + 2 * pad + 24 > row_w) row_w = tw + 2 * pad + 24;
    }
    if (row_w > DisplayWidth(dpy, screen) - 40) row_w = DisplayWidth(dpy, screen) - 40;
    int ov_w = row_w;
    int ov_h = title_h + count * row_h + pad;
    int ov_x = (DisplayWidth(dpy, screen) - ov_w) / 2;
    int ov_y = (DisplayHeight(dpy, screen) - ov_h) / 2;
    if (ov_x < 0) ov_x = 0;
    if (ov_y < 0) ov_y = 0;

    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixel = cfg.dock_bg_color;
    a.border_pixel = cfg.border_color;
    a.event_mask = ExposureMask;
    Window ov = XCreateWindow(dpy, root, ov_x, ov_y, ov_w, ov_h, 2,
                              DefaultDepth(dpy, screen), InputOutput,
                              DefaultVisual(dpy, screen),
                              CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &a);
    GC gc = XCreateGC(dpy, ov, 0, NULL);
    XMapRaised(dpy, ov);

    /* Grab the keyboard so we see the modifier release even though the WM has
     * key grabs of its own. If it fails, fall back to a one-shot activate. */
    if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync,
                      CurrentTime) != GrabSuccess) {
        activate(list[sel]);
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, ov);
        if (font) XFreeFont(dpy, font);
        return;
    }

    int committed = 1;   /* commit unless Escape cancels */
    int redraw = 1;
    int alive = 1;
    while (alive) {
        if (redraw) {
            if (font) XSetFont(dpy, gc, font->fid);
            XSetForeground(dpy, gc, cfg.dock_bg_color);
            XFillRectangle(dpy, ov, gc, 0, 0, ov_w, ov_h);
            XSetForeground(dpy, gc, cfg.border_color);
            XDrawRectangle(dpy, ov, gc, 0, 0, ov_w - 1, ov_h - 1);

            XSetForeground(dpy, gc, cfg.text_color);
            const char *hdr = "Windows";
            XDrawString(dpy, ov, gc, pad, 16, hdr, (int)strlen(hdr));

            for (int i = 0; i < count; i++) {
                int y = title_h + i * row_h;
                if (i == sel) {
                    XSetForeground(dpy, gc, cfg.accent_color);
                    XFillRectangle(dpy, ov, gc, 4, y + 2, ov_w - 8, row_h - 4);
                    XSetForeground(dpy, gc, 0x0a0a14);
                } else {
                    XSetForeground(dpy, gc, cfg.text_color);
                }
                int len = (int)strlen(titles[i]);
                XDrawString(dpy, ov, gc, pad + 8, y + row_h - 8, titles[i], len);
            }
            XFlush(dpy);
            redraw = 0;
        }

        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.window == ov) {
            redraw = 1;
        } else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) { committed = 0; alive = 0; }
            else if ((int)ev.xkey.keycode == grave_code) {
                int dir = (ev.xkey.state & ShiftMask) ? -1 : +1;
                sel = ((sel + dir) % count + count) % count;
                redraw = 1;
            } else if (ks == XK_Return) {
                alive = 0;
            }
        } else if (ev.type == KeyRelease) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Super_L || ks == XK_Super_R ||
                ks == XK_Alt_L   || ks == XK_Alt_R) {
                alive = 0;
            }
        }
        (void)super_mask;
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, ov);
    if (font) XFreeFont(dpy, font);
    XFlush(dpy);

    if (committed && sel >= 0 && sel < count) activate(list[sel]);
}

void grab_key(int keycode, unsigned int modifiers) {
    XGrabKey(dpy, keycode, modifiers, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, modifiers | LockMask, root, True, GrabModeAsync, GrabModeAsync);
}

void launch_cmd(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        exit(0);
    }
}

int main() {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[Churro Keybind] Cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    churro_load_settings(&cfg);

    a_net_client_list    = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    a_net_active_window  = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    a_net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    a_type_desktop       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    a_type_dock          = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a_net_wm_name        = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_utf8_string        = XInternAtom(dpy, "UTF8_STRING", False);

    // Grab Win+R (Super+R)
    KeyCode r_code = XKeysymToKeycode(dpy, XK_r);
    KeyCode tab_code = XKeysymToKeycode(dpy, XK_Tab);
    KeyCode space_code = XKeysymToKeycode(dpy, XK_space);
    KeyCode print_code = XKeysymToKeycode(dpy, XK_Print);  // PrintScreen
    KeyCode grave_code = XKeysymToKeycode(dpy, XK_grave);  // ` — window switcher
    KeyCode esc_code = XKeysymToKeycode(dpy, XK_Escape);   // Ctrl+Shift+Esc → Helm
    KeyCode e_code   = XKeysymToKeycode(dpy, XK_e);        // Super+E → Finder
    unsigned int super_mask = Mod4Mask;    // Windows / Command key
    unsigned int ctrl_mask  = ControlMask; // what Cmd becomes in Mac-keyboard mode
    unsigned int numlock    = Mod2Mask;

    grab_key(r_code, super_mask);
    grab_key(tab_code, super_mask);
    // Spotlight-style launcher: Cmd+Space. On a Mac keyboard with ⌘=Ctrl this
    // arrives as Ctrl+Space; on a normal keyboard as Super+Space. Grab both.
    grab_key(space_code, super_mask);
    grab_key(space_code, ctrl_mask);
    // Screenshot: PrintScreen = full screen, Shift+PrintScreen = select a region.
    // Grab with/without NumLock so it works either way (grab_key adds CapsLock).
    grab_key(print_code, 0);
    grab_key(print_code, numlock);
    grab_key(print_code, ShiftMask);
    grab_key(print_code, ShiftMask | numlock);
    // Window switcher: Super+` cycles forward, Super+Shift+` cycles back.
    // (Super+Tab is the workspace switcher; window cycling lives on Super+`.)
    // Guard against an unmapped keysym so we never grab AnyKey (keycode 0).
    if (grave_code != 0) {
        grab_key(grave_code, super_mask);
        grab_key(grave_code, super_mask | ShiftMask);
    }
    // Task-Manager reflex: Ctrl+Shift+Esc → Helm (system dashboard).
    if (esc_code != 0) {
        grab_key(esc_code, ControlMask | ShiftMask);
        grab_key(esc_code, ControlMask | ShiftMask | numlock);
    }
    // File-Explorer reflex: Super+E → Finder (nemo).
    if (e_code != 0) {
        grab_key(e_code, super_mask);
        grab_key(e_code, super_mask | numlock);
    }

    fprintf(stderr, "[Churro Keybind] Listening (Win+R, Win+Tab, Cmd/Super+Space, "
                    "Super+` window switch, PrintScreen)\n");

    XEvent event;
    while (running) {
        XNextEvent(dpy, &event);

        if (event.type == KeyPress) {
            XKeyEvent *ke = &event.xkey;
            KeyCode code = ke->keycode;

            if (code == r_code && (ke->state & super_mask)) {
                fprintf(stderr, "[Churro Keybind] Win+R pressed\n");
                launch_cmd("churro-launcher");
            } else if (code == space_code &&
                       (ke->state & (super_mask | ctrl_mask))) {
                fprintf(stderr, "[Churro Keybind] Cmd/Super+Space pressed\n");
                launch_cmd("churro-launcher");
            } else if (code == tab_code && (ke->state & super_mask)) {
                fprintf(stderr, "[Churro Keybind] Win+Tab pressed\n");
                launch_cmd("churro-desktops");
            } else if (code == grave_code && (ke->state & super_mask)) {
                fprintf(stderr, "[Churro Keybind] Super+` window switch\n");
                run_switcher(grave_code, super_mask, (ke->state & ShiftMask) ? -1 : +1);
            } else if (code == esc_code &&
                       (ke->state & ControlMask) && (ke->state & ShiftMask)) {
                fprintf(stderr, "[Churro Keybind] Ctrl+Shift+Esc → Helm\n");
                launch_cmd("leviathan-dashboard");
            } else if (code == e_code && (ke->state & super_mask)) {
                fprintf(stderr, "[Churro Keybind] Super+E → Finder\n");
                launch_cmd("nemo");
            } else if (code == print_code) {
                // Save to ~/Pictures/Screenshots (timestamped) + copy to clipboard.
                // scrot does the strftime; single-quote the pattern so the shell
                // leaves the % specifiers alone.
                if (ke->state & ShiftMask) {
                    fprintf(stderr, "[Churro Keybind] Shift+PrintScreen (region)\n");
                    launch_cmd("mkdir -p \"$HOME/Pictures/Screenshots\"; "
                               "cd \"$HOME/Pictures/Screenshots\" && "
                               "scrot -s '%Y-%m-%d_%H-%M-%S.png' "
                               "-e 'xclip -selection clipboard -t image/png -i $f 2>/dev/null'");
                } else {
                    fprintf(stderr, "[Churro Keybind] PrintScreen (full)\n");
                    launch_cmd("mkdir -p \"$HOME/Pictures/Screenshots\"; "
                               "cd \"$HOME/Pictures/Screenshots\" && "
                               "scrot '%Y-%m-%d_%H-%M-%S.png' "
                               "-e 'xclip -selection clipboard -t image/png -i $f 2>/dev/null'");
                }
            }
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
