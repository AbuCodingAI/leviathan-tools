/**
 * Infinitas Browser - Java Applet Plugin
 *
 * Revives Java applets by:
 *   1. Injecting a content script that detects <applet> tags in pages
 *   2. Replacing them with a link to infinitas://java/<encoded-params>
 *   3. When that link is visited, launching a JVM subprocess
 *   4. Reading the applet's rendered pixels via a pipe
 *   5. Displaying them in a native GTK window at ~30 FPS
 *
 * Build:
 *   cd plugins/java_applet && make
 *   cp java_plugin.so ~/.infinitas/plugins/
 *   cp AppletRunner.jar ~/.infinitas/plugins/
 *
 * Requirements:
 *   - Java 11+ (sudo apt install default-jdk)
 *   - GTK4 dev headers (already needed for Infinitas)
 */

#include "../../src/infinitas_plugin_api.h"
#include <gtk/gtk.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>

/* ── content script injected into every page ────────────────────────────── */

static const gchar APPLET_DETECTOR_JS[] =
"(function(){"
"  var applets = document.querySelectorAll('applet');"
"  if (!applets.length) return;"
"  applets.forEach(function(a, i){"
"    var code    = encodeURIComponent(a.getAttribute('code')    || '');"
"    var archive = encodeURIComponent(a.getAttribute('archive') || '');"
"    var width   = a.getAttribute('width')  || '300';"
"    var height  = a.getAttribute('height') || '200';"
"    var base    = encodeURIComponent(document.baseURI);"
"    var url = 'infinitas://java/launch?code='+code"
"             +'&archive='+archive"
"             +'&width='+width"
"             +'&height='+height"
"             +'&base='+base;"
"    var div = document.createElement('div');"
"    div.style.cssText = 'display:inline-block;width:'+width+'px;height:'+height+'px;"
"      border:2px solid #7c6af7;border-radius:8px;background:#0f0f13;"
"      display:flex;align-items:center;justify-content:center;flex-direction:column;"
"      color:#e2e8f0;font-family:system-ui;cursor:pointer;gap:8px';"
"    div.innerHTML = '<span style=\"font-size:32px\">&#x2615;</span>"
"      <strong>Java Applet</strong>"
"      <span style=\"font-size:12px;color:#94a3b8\">'+(a.getAttribute('code')||'unknown')+'</span>"
"      <a href=\"'+url+'\" style=\"background:#7c6af7;color:#fff;padding:6px 14px;"
"        border-radius:6px;text-decoration:none;font-size:13px\">Launch Applet</a>';"
"    a.parentNode.replaceChild(div, a);"
"  });"
"})();";

/* ── applet window state ─────────────────────────────────────────────────── */

typedef struct {
    GtkWidget   *window;
    GtkWidget   *picture;
    GdkPixbuf   *pixbuf;
    GPid         java_pid;
    int          fd_out;    /* read pixels from Java */
    int          fd_in;     /* write events to Java  */
    int          width;
    int          height;
    guint        timer_id;
} AppletWindow;

static void applet_window_destroy(AppletWindow *aw) {
    if (!aw) return;
    if (aw->timer_id) g_source_remove(aw->timer_id);
    if (aw->fd_out >= 0) close(aw->fd_out);
    if (aw->fd_in  >= 0) close(aw->fd_in);
    if (aw->java_pid > 0) kill(aw->java_pid, SIGTERM);
    if (aw->pixbuf) g_object_unref(aw->pixbuf);
    g_free(aw);
}

/* ── pixel reader (called every ~33ms) ───────────────────────────────────── */

static gboolean read_frame(gpointer user_data) {
    AppletWindow *aw = user_data;
    int w = aw->width, h = aw->height;
    gsize bytes = (gsize)(w * h * 4);
    guchar *buf = g_malloc(bytes);

    gsize got = 0;
    while (got < bytes) {
        gssize n = read(aw->fd_out, buf + got, bytes - got);
        if (n <= 0) {
            g_free(buf);
            return G_SOURCE_REMOVE; /* Java process died */
        }
        got += (gsize)n;
    }

    /* Java sends ARGB; GdkPixbuf wants RGB (no alpha) or RGBA */
    if (!aw->pixbuf) {
        aw->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    }
    guchar *dst      = gdk_pixbuf_get_pixels(aw->pixbuf);
    int     rowstride = gdk_pixbuf_get_rowstride(aw->pixbuf);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int si = (y * w + x) * 4;
            int di = y * rowstride + x * 4;
            /* Java ARGB → GdkPixbuf RGBA */
            dst[di + 0] = buf[si + 1]; /* R */
            dst[di + 1] = buf[si + 2]; /* G */
            dst[di + 2] = buf[si + 3]; /* B */
            dst[di + 3] = buf[si + 0]; /* A */
        }
    }
    g_free(buf);

    GdkTexture *tex = gdk_texture_new_for_pixbuf(aw->pixbuf);
    gtk_picture_set_paintable(GTK_PICTURE(aw->picture), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    return G_SOURCE_CONTINUE;
}

/* ── mouse event forwarding ──────────────────────────────────────────────── */

static void on_motion(GtkEventControllerMotion *c, gdouble x, gdouble y,
                       gpointer ud) {
    (void)c;
    AppletWindow *aw = ud;
    gchar msg[64];
    int len = g_snprintf(msg, sizeof(msg), "M %d %d\n", (int)x, (int)y);
    if (write(aw->fd_in, msg, len) < 0) { /* event dropped if pipe full */ }
}

static void on_click(GtkGestureClick *g, gint n, gdouble x, gdouble y,
                      gpointer ud) {
    (void)n;
    AppletWindow *aw = ud;
    gchar msg[64];
    int len = g_snprintf(msg, sizeof(msg), "C %d %d %d\n",
                          gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)),
                          (int)x, (int)y);
    if (write(aw->fd_in, msg, len) < 0) { /* event dropped if pipe full */ }
}

/* ── launch applet subprocess ────────────────────────────────────────────── */

static void launch_applet(const gchar *code, const gchar *archive,
                           int width, int height, const gchar *base,
                           const gchar *runner_jar) {
    /* pipes: parent reads stdout, writes stdin */
    int pipe_out[2], pipe_in[2];
    if (pipe(pipe_out) < 0 || pipe(pipe_in) < 0) {
        g_printerr("[JAVA] pipe() failed\n");
        return;
    }

    GPid pid;
    gchar width_s[16], height_s[16];
    g_snprintf(width_s,  sizeof(width_s),  "%d", width);
    g_snprintf(height_s, sizeof(height_s), "%d", height);

    gchar *argv[] = {
        "java",
        "--add-modules", "java.desktop",
        "-cp", (gchar*)runner_jar,
        "AppletRunner",
        (gchar*)code,
        (gchar*)archive,
        width_s, height_s,
        (gchar*)base,
        NULL
    };

    GError *err = NULL;
    gboolean ok = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL,
        &pid,
        &pipe_in[0],   /* child stdin  ← parent writes here  */
        &pipe_out[1],  /* child stdout → parent reads here   */
        NULL,          /* stderr: let it go to terminal      */
        &err);

    if (!ok) {
        g_printerr("[JAVA] Failed to spawn JVM: %s\n",
                   err ? err->message : "unknown");
        g_clear_error(&err);
        close(pipe_out[0]); close(pipe_out[1]);
        close(pipe_in[0]);  close(pipe_in[1]);
        return;
    }
    /* close the child ends in parent */
    close(pipe_out[1]);
    close(pipe_in[0]);

    AppletWindow *aw = g_new0(AppletWindow, 1);
    aw->java_pid = pid;
    aw->fd_out   = pipe_out[0];
    aw->fd_in    = pipe_in[1];
    aw->width    = width;
    aw->height   = height;

    /* build the GTK window */
    aw->window = gtk_window_new();
    gchar *title = g_strdup_printf("Java Applet — %s", code);
    gtk_window_set_title(GTK_WINDOW(aw->window), title);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(aw->window), width, height);
    gtk_window_set_resizable(GTK_WINDOW(aw->window), FALSE);

    aw->picture = gtk_picture_new();
    gtk_widget_set_size_request(aw->picture, width, height);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), aw);
    gtk_widget_add_controller(aw->picture, motion);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), aw);
    gtk_widget_add_controller(aw->picture, GTK_EVENT_CONTROLLER(click));

    gtk_window_set_child(GTK_WINDOW(aw->window), aw->picture);
    g_signal_connect_swapped(aw->window, "destroy",
                              G_CALLBACK(applet_window_destroy), aw);

    gtk_window_present(GTK_WINDOW(aw->window));

    /* start the pixel reader timer (33ms ≈ 30 FPS) */
    aw->timer_id = g_timeout_add(33, read_frame, aw);
}

/* ── scheme handler for infinitas://java/launch?... ─────────────────────── */

static gchar *g_runner_jar = NULL;

static gchar* java_scheme_handler(const gchar *subpath, gpointer userdata) {
    (void)userdata;

    /* only handle "launch?..." */
    if (!g_str_has_prefix(subpath, "launch?"))
        return g_strdup("<html><body>Java plugin: unknown path</body></html>");

    const gchar *qs = subpath + strlen("launch?");

    /* parse query string */
    gchar *code = NULL, *archive = NULL, *base = NULL;
    int width = 300, height = 200;

    gchar **pairs = g_strsplit(qs, "&", 0);
    for (int i = 0; pairs[i]; i++) {
        gchar **kv = g_strsplit(pairs[i], "=", 2);
        if (kv[0] && kv[1]) {
            gchar *val = g_uri_unescape_string(kv[1], NULL);
            if (!g_strcmp0(kv[0], "code"))    { g_free(code);    code    = val; val = NULL; }
            if (!g_strcmp0(kv[0], "archive")) { g_free(archive); archive = val; val = NULL; }
            if (!g_strcmp0(kv[0], "base"))    { g_free(base);    base    = val; val = NULL; }
            if (!g_strcmp0(kv[0], "width"))   { width  = atoi(kv[1]); }
            if (!g_strcmp0(kv[0], "height"))  { height = atoi(kv[1]); }
            g_free(val);
        }
        g_strfreev(kv);
    }
    g_strfreev(pairs);

    if (!code || !*code) {
        g_free(code); g_free(archive); g_free(base);
        return g_strdup("<html><body>Java plugin: missing code attribute</body></html>");
    }

    /* schedule launch on the main GTK thread */
    gchar *c = code, *a = archive ? archive : g_strdup(""), *b = base ? base : g_strdup("");
    launch_applet(c, a, width, height, b, g_runner_jar);

    g_free(code); g_free(archive); g_free(base);

    return g_strdup(
        "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
        "<body style='background:#0f0f13;color:#e2e8f0;font-family:system-ui;"
        "display:flex;align-items:center;justify-content:center;height:100vh;"
        "flex-direction:column;gap:12px'>"
        "<span style='font-size:48px'>&#x2615;</span>"
        "<h2>Java Applet Launched</h2>"
        "<p style='color:#94a3b8'>The applet is running in a separate window.</p>"
        "</body></html>");
}

/* ── plugin entry point ──────────────────────────────────────────────────── */

static const InfinitasBrowserAPI *g_api = NULL;

static void on_load_finished(const gchar *url, gpointer ud) {
    (void)ud;
    if (!url || g_str_has_prefix(url, "infinitas://")) return;
    /* inject detector into every external page */
    g_api->eval_js(APPLET_DETECTOR_JS, g_api->browser);
}

static InfinitasPlugin g_plugin = {
    .name        = "Java Applet Support",
    .version     = "1.0",
    .author      = "Infinitas",
    .description = "Revives Java applets via JVM subprocess rendering",
    .on_load_finished = on_load_finished,
    .load_userdata    = NULL,
};

InfinitasPlugin* infinitas_plugin_init(const InfinitasBrowserAPI *api) {
    g_api = api;

    /* find AppletRunner.jar next to this .so */
    gchar *so_path = NULL;
    /* use /proc/self/maps to find our own path, or just look in known dirs */
    const gchar *search[] = {
        g_get_home_dir(),
        NULL
    };
    (void)search;

    gchar *jar = g_build_filename(g_get_home_dir(), ".infinitas", "plugins",
                                   "AppletRunner.jar", NULL);
    if (!g_file_test(jar, G_FILE_TEST_EXISTS)) {
        g_printerr("[JAVA] AppletRunner.jar not found at %s\n", jar);
        g_printerr("[JAVA] Build it: cd plugins/java_applet && make jar\n");
        g_free(jar);
        return NULL;
    }
    g_runner_jar = jar;
    (void)so_path;

    /* register our infinitas://java/ sub-path */
    api->register_scheme_path("java/", java_scheme_handler, NULL, api->browser);

    g_print("[JAVA] Java Applet plugin ready. JAR: %s\n", g_runner_jar);
    return &g_plugin;
}
