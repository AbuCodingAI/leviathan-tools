#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIDTH 1200
#define HEIGHT 60

Display *dpy;
Window win;
GC gc;
XFontStruct *font_time, *font_date;

// Find ARGB visual for transparency
Visual *find_argb_visual(Display *dpy, int screen) {
    XVisualInfo *xvi;
    XVisualInfo vtemplate;
    int nvi;

    vtemplate.screen = screen;
    vtemplate.depth = 32;
    vtemplate.c_class = TrueColor;

    xvi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &vtemplate, &nvi);

    if (xvi) {
        Visual *visual = xvi[0].visual;
        XFree(xvi);
        return visual;
    }

    return DefaultVisual(dpy, screen);
}

void draw_text(int x, int y, const char *text, XFontStruct *font) {
    XSetFont(dpy, gc, font->fid);
    XDrawString(dpy, win, gc, x, y, text, strlen(text));
}

void update_time() {
    time_t now_time = time(NULL);
    struct tm *now = localtime(&now_time);

    // Time: HH:MM with 12-hour format + AM/PM
    int hour = now->tm_hour % 12;
    if (hour == 0) hour = 12;
    const char *ampm = (now->tm_hour >= 12) ? "PM" : "AM";

    char time_str[15];
    snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour, now->tm_min, ampm);

    // Day of week
    const char *weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char *weekday = weekdays[now->tm_wday];

    // Day of month
    char day_str[5];
    snprintf(day_str, sizeof(day_str), "%d", now->tm_mday);

    // Month and year
    const char *months[] = {"January", "February", "March", "April", "May", "June",
                            "July", "August", "September", "October", "November", "December"};
    char date_str[20];
    snprintf(date_str, sizeof(date_str), "%s %d", months[now->tm_mon], now->tm_year + 1900);

    // Combined text: "11:37 AM Sunday 7 June 2026"
    char full_text[80];
    snprintf(full_text, sizeof(full_text), "%s %s %s %s", time_str, weekday, day_str, date_str);

    // Draw all together (white)
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    draw_text(20, 20, full_text, font_time);

    XFlush(dpy);
}

int main(int argc, char *argv[]) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    // Load fonts - force monospace with 'm' (monospace) in XLFD
    font_time = XLoadQueryFont(dpy, "-*-courier-bold-r-*-*-18-*-*-*-m-*-*-*");
    if (!font_time) font_time = XLoadQueryFont(dpy, "-*-fixed-bold-r-*-*-18-*-*-*-*-*-*-*");
    if (!font_time) font_time = XLoadQueryFont(dpy, "fixed");

    font_date = XLoadQueryFont(dpy, "-*-courier-medium-r-*-*-14-*-*-*-m-*-*-*");
    if (!font_date) font_date = XLoadQueryFont(dpy, "-*-fixed-medium-r-*-*-14-*-*-*-*-*-*-*");
    if (!font_date) font_date = font_time;

    // Find ARGB visual
    Visual *visual = find_argb_visual(dpy, screen);
    Colormap colormap = XCreateColormap(dpy, root, visual, AllocNone);

    // Create window with ARGB visual for transparency
    XSetWindowAttributes attrs;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.colormap = colormap;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask;

    // Position: top-center, full width style (for system tray area)
    int x_pos = (DisplayWidth(dpy, screen) - WIDTH) / 2;
    int y_pos = 0;  // Top of screen

    win = XCreateWindow(dpy, root,
                       x_pos, y_pos,
                       WIDTH, HEIGHT, 0,
                       32,  // 32-bit depth for ARGB
                       InputOutput,
                       visual,
                       CWBackPixel | CWBorderPixel | CWColormap | CWOverrideRedirect | CWEventMask,
                       &attrs);

    // Set window properties
    Atom net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom net_wm_window_type_normal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(dpy, win, net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)&net_wm_window_type_normal, 1);

    // Keep window BELOW all others
    Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom net_wm_state_below = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
    XChangeProperty(dpy, win, net_wm_state, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)&net_wm_state_below, 1);

    // Create graphics context
    XGCValues gc_values;
    gc_values.foreground = WhitePixel(dpy, screen);
    gc_values.background = 0;
    gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gc_values);

    XMapWindow(dpy, win);
    XFlush(dpy);

    // Update every second
    while (1) {
        update_time();
        sleep(1);
    }

    XFreeGC(dpy, gc);
    XFreeColormap(dpy, colormap);
    XCloseDisplay(dpy);
    return 0;
}
