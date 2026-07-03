/*
 * Churro Desktop Switcher (Win+Tab)
 *
 * Real EWMH workspace switching: reads _NET_NUMBER_OF_DESKTOPS and
 * _NET_CURRENT_DESKTOP from the root window and switches by sending a
 * _NET_CURRENT_DESKTOP ClientMessage to the root window (the way any
 * EWMH-compliant window manager expects). Shows the live current/total.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "churro-settings.h"

Display *dpy;
Window win, root;
GC gc;
XFontStruct *font;

Atom net_num_desktops, net_current_desktop;

int current_desktop = 0;
int num_desktops = 1;

ChurroSettings cfg;

/* Read a single CARDINAL property (format 32) from the root window. */
static long read_root_card(Atom prop, long fallback) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    long value = fallback;

    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) == Success) {
        if (data && nitems > 0 && actual_format == 32) {
            value = (long)(*(unsigned long *)data);
        }
        if (data) XFree(data);
    }
    return value;
}

static void refresh_state(void) {
    num_desktops = (int)read_root_card(net_num_desktops, num_desktops);
    if (num_desktops < 1) num_desktops = 1;
    current_desktop = (int)read_root_card(net_current_desktop, current_desktop);
    if (current_desktop < 0) current_desktop = 0;
    if (current_desktop >= num_desktops) current_desktop = num_desktops - 1;
}

/* Ask the window manager to switch to `target` via EWMH ClientMessage. */
static void switch_desktop(int target) {
    if (target < 0 || target >= num_desktops) return;

    XEvent e;
    memset(&e, 0, sizeof(e));
    e.type = ClientMessage;
    e.xclient.window = root;
    e.xclient.message_type = net_current_desktop;
    e.xclient.format = 32;
    e.xclient.data.l[0] = target;
    e.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy, root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &e);
    XFlush(dpy);

    current_desktop = target;
}

static void draw_desktops(void) {
    unsigned int win_w = 800, win_h = 200;

    XSetForeground(dpy, gc, 0x0a0a14);
    XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

    char header[64];
    snprintf(header, sizeof(header),
             "Desktop Switcher (Win+Tab)   %d / %d",
             current_desktop + 1, num_desktops);
    XSetForeground(dpy, gc, cfg.text_color);
    XSetFont(dpy, gc, font->fid);
    XDrawString(dpy, win, gc, 20, 40, header, strlen(header));

    int box_width = 150;
    int box_height = 100;
    int spacing = 20;
    int start_x = 50;
    int start_y = 80;

    /* Clamp the number of visible boxes so we never draw off-window. */
    int max_boxes = (win_w - start_x) / (box_width + spacing);
    int shown = num_desktops < max_boxes ? num_desktops : max_boxes;

    for (int i = 0; i < shown; i++) {
        int x = start_x + i * (box_width + spacing);

        if (i == current_desktop) {
            XSetForeground(dpy, gc, cfg.accent_color);
            XFillRectangle(dpy, win, gc, x, start_y, box_width, box_height);
        } else {
            XSetForeground(dpy, gc, cfg.border_color);
            XDrawRectangle(dpy, win, gc, x, start_y, box_width, box_height);
        }

        char label[32];
        snprintf(label, sizeof(label), "Desktop %d", i + 1);
        XSetForeground(dpy, gc, i == current_desktop ? 0x0a0a14 : cfg.text_color);
        XDrawString(dpy, win, gc, x + 30, start_y + 50, label, strlen(label));
    }

    XFlush(dpy);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    churro_load_settings(&cfg);

    net_num_desktops   = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    refresh_state();

    XSetWindowAttributes attrs;
    attrs.background_pixel = 0x0a0a14;
    attrs.border_pixel = cfg.border_color;
    attrs.event_mask = KeyPressMask | ExposureMask;

    win = XCreateWindow(dpy, root, 100, 100, 800, 200, 2,
                       DefaultDepth(dpy, screen), InputOutput,
                       DefaultVisual(dpy, screen),
                       CWBackPixel | CWBorderPixel | CWEventMask,
                       &attrs);

    font = XLoadQueryFont(dpy, "-*-fixed-medium-r-*-*-14-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "fixed");

    XGCValues gc_values;
    gc_values.foreground = WhitePixel(dpy, screen);
    gc_values.background = BlackPixel(dpy, screen);
    gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gc_values);

    XMapWindow(dpy, win);
    XRaiseWindow(dpy, win);
    /* Take the keyboard so arrow/Tab navigation works even without a WM focus. */
    XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_desktops();

    while (1) {
        XEvent event;
        XNextEvent(dpy, &event);

        if (event.type == KeyPress) {
            KeySym sym = XLookupKeysym(&event.xkey, 0);

            if (sym == XK_Right || sym == XK_Tab) {
                switch_desktop((current_desktop + 1) % num_desktops);
                draw_desktops();
            } else if (sym == XK_Left) {
                switch_desktop((current_desktop - 1 + num_desktops) % num_desktops);
                draw_desktops();
            } else if (sym >= XK_1 && sym <= XK_9) {
                switch_desktop((int)(sym - XK_1));
                draw_desktops();
            } else if (sym == XK_Escape || sym == XK_Return) {
                XUngrabKeyboard(dpy, CurrentTime);
                XDestroyWindow(dpy, win);
                XCloseDisplay(dpy);
                return 0;
            }
        } else if (event.type == Expose) {
            refresh_state();
            draw_desktops();
        }
    }

    return 0;
}
