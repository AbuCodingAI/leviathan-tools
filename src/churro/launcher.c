/*
 * Churro Launcher (Win+R / Cmd+Space)
 *
 * Scans .desktop files in the standard application directories, parses
 * Name/Exec/Icon, and offers fuzzy-matched results. Full keyboard control:
 *   - type to filter (subsequence fuzzy match, ranked)
 *   - Up/Down to move the selection, Enter to launch
 *   - Esc to dismiss
 * Grabs the keyboard so it owns focus while open. If you type something that
 * is not an app, Enter runs it as a shell command in a terminal.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

#include "churro-settings.h"

#define WIN_W 600
#define WIN_H 440
#define MAX_APPS 4096
#define MAX_VISIBLE 12

Display *dpy;
Window win;
GC gc;
XFontStruct *font;
char input[256] = "";
int input_len = 0;

ChurroSettings cfg;

typedef struct {
    char name[128];
    char exec[512];
} App;

App *apps = NULL;
int num_apps = 0;

/* Filtered view into `apps`, rebuilt on every keystroke. */
int filtered[MAX_APPS];
int num_filtered = 0;
int selected = 0;

/* ---- .desktop discovery -------------------------------------------------- */

static void strip_field_codes(char *exec) {
    /* Remove %f %F %u %U %i %c %k and squeeze the result. */
    char out[512];
    int o = 0;
    for (int i = 0; exec[i] && o < (int)sizeof(out) - 1; i++) {
        if (exec[i] == '%' && exec[i + 1]) {
            i++;                 /* skip the code letter too */
            continue;
        }
        out[o++] = exec[i];
    }
    out[o] = 0;
    /* trim trailing spaces */
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    snprintf(exec, 512, "%s", out);
}

static int already_have(const char *name) {
    for (int i = 0; i < num_apps; i++)
        if (strcasecmp(apps[i].name, name) == 0) return 1;
    return 0;
}

static void parse_desktop_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char name[128] = "";
    char exec[512] = "";
    int in_entry = 0;
    int no_display = 0;
    int is_app = 1;             /* assume Application unless Type says otherwise */

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;

        if (line[0] == '[') {
            in_entry = (strcmp(line, "[Desktop Entry]") == 0);
            continue;
        }
        if (!in_entry) continue;

        if (strncmp(line, "Name=", 5) == 0 && name[0] == 0) {
            snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, line + 5);
        } else if (strncmp(line, "Exec=", 5) == 0 && exec[0] == 0) {
            snprintf(exec, sizeof(exec), "%.*s", (int)sizeof(exec) - 1, line + 5);
        } else if (strncmp(line, "NoDisplay=", 10) == 0) {
            no_display = (strcasecmp(line + 10, "true") == 0);
        } else if (strncmp(line, "Hidden=", 7) == 0) {
            if (strcasecmp(line + 7, "true") == 0) no_display = 1;
        } else if (strncmp(line, "Type=", 5) == 0) {
            is_app = (strcasecmp(line + 5, "Application") == 0);
        }
    }
    fclose(f);

    if (!is_app || no_display || name[0] == 0 || exec[0] == 0) return;
    if (num_apps >= MAX_APPS) return;
    if (already_have(name)) return;

    strip_field_codes(exec);
    if (exec[0] == 0) return;

    snprintf(apps[num_apps].name, sizeof(apps[num_apps].name), "%s", name);
    snprintf(apps[num_apps].exec, sizeof(apps[num_apps].exec), "%s", exec);
    num_apps++;
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop") != 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        parse_desktop_file(path);
    }
    closedir(d);
}

static void discover_apps(void) {
    apps = (App *)calloc(MAX_APPS, sizeof(App));
    if (!apps) return;

    scan_dir("/usr/share/applications");
    scan_dir("/usr/local/share/applications");

    const char *home = getenv("HOME");
    if (home) {
        char user_dir[512];
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications", home);
        scan_dir(user_dir);
    }
}

/* ---- Fuzzy matching ------------------------------------------------------ */

/* Return a match score for `query` against `name` (higher is better),
 * or -1 for no match. Subsequence match; bonuses for prefix / word-start. */
static int fuzzy_score(const char *query, const char *name) {
    if (!query[0]) return 0;

    int score = 0;
    const char *n = name;
    const char *q = query;
    int at_word_start = 1;
    int consecutive = 0;

    /* prefix bonus */
    if (strncasecmp(name, query, strlen(query)) == 0) score += 100;

    while (*q && *n) {
        if (tolower((unsigned char)*q) == tolower((unsigned char)*n)) {
            score += 2;
            if (at_word_start) score += 10;
            score += consecutive;   /* reward runs */
            consecutive++;
            q++;
        } else {
            consecutive = 0;
        }
        at_word_start = (*n == ' ' || *n == '-' || *n == '_');
        n++;
    }
    if (*q) return -1;              /* not all query chars consumed */
    return score;
}

static void rebuild_filter(void) {
    num_filtered = 0;

    /* collect matches with scores */
    static int scores[MAX_APPS];
    for (int i = 0; i < num_apps; i++) {
        int s = fuzzy_score(input, apps[i].name);
        if (s >= 0) {
            scores[num_filtered] = s;
            filtered[num_filtered] = i;
            num_filtered++;
        }
    }

    /* simple insertion sort by score desc (lists are short after filtering) */
    for (int i = 1; i < num_filtered; i++) {
        int idx = filtered[i], sc = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < sc) {
            scores[j + 1] = scores[j];
            filtered[j + 1] = filtered[j];
            j--;
        }
        scores[j + 1] = sc;
        filtered[j + 1] = idx;
    }

    if (selected >= num_filtered) selected = num_filtered - 1;
    if (selected < 0) selected = 0;
}

static void execute_cmd(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
}

static void launch_selected(void) {
    if (num_filtered > 0 && selected >= 0 && selected < num_filtered) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "%s &", apps[filtered[selected]].exec);
        execute_cmd(cmd);
    } else if (input_len > 0) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "alacritty -e bash -c '%s; read -p \"Press Enter to close...\"' &", input);
        execute_cmd(cmd);
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void draw_launcher(void) {
    XSetForeground(dpy, gc, 0x0a0a14);
    XFillRectangle(dpy, win, gc, 0, 0, WIN_W, WIN_H);

    XSetForeground(dpy, gc, cfg.text_color);
    XSetFont(dpy, gc, font->fid);
    XDrawString(dpy, win, gc, 20, 34, "Run / Search Apps", 17);

    /* input box */
    XSetForeground(dpy, gc, cfg.border_color);
    XDrawRectangle(dpy, win, gc, 20, 50, WIN_W - 40, 40);
    XSetForeground(dpy, gc, 0xdddddd);
    XDrawString(dpy, win, gc, 30, 75, input, input_len);
    XDrawLine(dpy, win, gc, 30 + input_len * 6, 60, 30 + input_len * 6, 82);

    /* results */
    int y = 120;
    int shown = num_filtered < MAX_VISIBLE ? num_filtered : MAX_VISIBLE;

    /* keep the selection on screen */
    int first = 0;
    if (selected >= MAX_VISIBLE) first = selected - MAX_VISIBLE + 1;

    for (int k = 0; k < shown && (first + k) < num_filtered; k++) {
        int i = first + k;
        App *a = &apps[filtered[i]];

        if (i == selected) {
            XSetForeground(dpy, gc, cfg.accent_color);
            XFillRectangle(dpy, win, gc, 20, y - 16, WIN_W - 40, 24);
            XSetForeground(dpy, gc, 0x0a0a14);
        } else {
            XSetForeground(dpy, gc, cfg.text_color);
        }
        XDrawString(dpy, win, gc, 30, y, a->name, strlen(a->name));
        y += 26;
    }

    if (num_filtered == 0 && input_len > 0) {
        XSetForeground(dpy, gc, 0x888888);
        char msg[320];
        snprintf(msg, sizeof(msg), "No app matches - Enter runs \"%s\" as a command", input);
        XDrawString(dpy, win, gc, 30, 120, msg, strlen(msg));
    }

    XFlush(dpy);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    churro_load_settings(&cfg);
    discover_apps();
    rebuild_filter();

    XSetWindowAttributes attrs;
    attrs.background_pixel = 0x0a0a14;
    attrs.border_pixel = cfg.border_color;
    attrs.event_mask = KeyPressMask | ExposureMask;
    attrs.override_redirect = True;

    int x = (DisplayWidth(dpy, screen) - WIN_W) / 2;
    int y = (DisplayHeight(dpy, screen) - WIN_H) / 3;

    win = XCreateWindow(dpy, root, x, y, WIN_W, WIN_H, 2,
                        DefaultDepth(dpy, screen), InputOutput,
                        DefaultVisual(dpy, screen),
                        CWBackPixel | CWBorderPixel | CWEventMask | CWOverrideRedirect,
                        &attrs);

    font = XLoadQueryFont(dpy, "-*-fixed-medium-r-*-*-13-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "fixed");

    XGCValues gc_values;
    gc_values.foreground = WhitePixel(dpy, screen);
    gc_values.background = BlackPixel(dpy, screen);
    gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gc_values);

    XMapWindow(dpy, win);
    XRaiseWindow(dpy, win);

    /* Own the keyboard so the launcher has focus regardless of the WM. */
    for (int tries = 0; tries < 100; tries++) {
        if (XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync,
                          CurrentTime) == GrabSuccess)
            break;
        usleep(10000);
    }

    XFlush(dpy);

    while (1) {
        XEvent event;
        XNextEvent(dpy, &event);

        if (event.type == KeyPress) {
            KeySym sym = XLookupKeysym(&event.xkey, 0);
            char buf[32];
            int count = XLookupString(&event.xkey, buf, sizeof(buf), NULL, NULL);

            if (sym == XK_Escape) {
                XUngrabKeyboard(dpy, CurrentTime);
                XDestroyWindow(dpy, win);
                XCloseDisplay(dpy);
                free(apps);
                return 0;
            } else if (sym == XK_Return) {
                XUngrabKeyboard(dpy, CurrentTime);
                launch_selected();
                XDestroyWindow(dpy, win);
                XCloseDisplay(dpy);
                free(apps);
                return 0;
            } else if (sym == XK_Down || sym == XK_Tab) {
                if (num_filtered > 0) selected = (selected + 1) % num_filtered;
            } else if (sym == XK_Up) {
                if (num_filtered > 0) selected = (selected - 1 + num_filtered) % num_filtered;
            } else if (sym == XK_BackSpace) {
                if (input_len > 0) { input_len--; input[input_len] = 0; }
                selected = 0;
                rebuild_filter();
            } else if (count > 0 && input_len < 255) {
                for (int i = 0; i < count; i++) {
                    if (isprint((unsigned char)buf[i])) input[input_len++] = buf[i];
                }
                input[input_len] = 0;
                selected = 0;
                rebuild_filter();
            }
            draw_launcher();
        } else if (event.type == Expose) {
            draw_launcher();
        }
    }

    return 0;
}
