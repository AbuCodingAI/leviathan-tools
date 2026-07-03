/**
 * Infinitas Browser - Public Plugin API
 *
 * Include this header in your plugin's source code.
 * Compile your plugin as a shared library:
 *
 *   gcc -shared -fPIC -o myplugin.so myplugin.c -I/path/to/infinitas/src \
 *       $(pkg-config --cflags glib-2.0 gtk4)
 *
 * Drop the .so into ~/.infinitas/plugins/ and restart Infinitas.
 *
 * SECURITY NOTE: Plugins run in the browser process with full system access.
 * Only install plugins you trust.
 */

#ifndef INFINITAS_PLUGIN_API_H
#define INFINITAS_PLUGIN_API_H

#include <glib.h>
#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INFINITAS_PLUGIN_API_VERSION 1

/* ── what the browser gives each plugin ────────────────────────────────── */

typedef struct InfinitasBrowserAPI {
    /* Navigate the active tab */
    void     (*navigate)(const gchar *url, gpointer browser);

    /* Open URL in a new tab */
    void     (*open_tab)(const gchar *url, gpointer browser);

    /* Get the current page URL (caller must g_free) */
    gchar*   (*get_url)(gpointer browser);

    /* Get the current page title (caller must g_free) */
    gchar*   (*get_title)(gpointer browser);

    /* Evaluate JavaScript in the current page (fire-and-forget) */
    void     (*eval_js)(const gchar *js, gpointer browser);

    /* Register a sub-path under infinitas:// for this plugin.
     * handler receives the part after your prefix and must return
     * heap-allocated HTML (browser g_frees it). */
    void     (*register_scheme_path)(const gchar *prefix,
                                      gchar* (*handler)(const gchar *subpath,
                                                         gpointer userdata),
                                      gpointer userdata, gpointer browser);

    /* Add a widget to the right side of the toolbar */
    void     (*add_toolbar_widget)(GtkWidget *widget, gpointer browser);

    /* Show a notification banner inside the browser window */
    void     (*notify)(const gchar *title, const gchar *body, gpointer browser);

    /* The browser pointer — pass as last arg to every function above */
    gpointer browser;
} InfinitasBrowserAPI;

/* ── navigation event hook ──────────────────────────────────────────────── */

typedef struct {
    const gchar *url;
    gboolean     cancel; /* set to TRUE to block the navigation */
} InfinitasNavEvent;

/* ── content plugin (handles a MIME type) ───────────────────────────────── */

typedef struct {
    /* Called when a page with this MIME type is about to load.
     * Return a GtkWidget* to embed, or NULL to let WebKit handle it. */
    GtkWidget* (*create_widget)(const gchar *url, const gchar *mime,
                                 gint width, gint height, gpointer userdata);
    gpointer userdata;
} InfinitasContentHandler;

/* ── the plugin descriptor ──────────────────────────────────────────────── */

typedef struct {
    /* Required metadata */
    const gchar *name;
    const gchar *version;
    const gchar *author;
    const gchar *description;

    /* Optional: handle specific MIME types (NULL-terminated list, or NULL) */
    const gchar **mime_types;
    InfinitasContentHandler content_handler;

    /* Optional: toolbar button */
    const gchar *toolbar_icon_name;   /* GTK icon name, e.g. "applications-internet" */
    const gchar *toolbar_tooltip;
    void (*toolbar_clicked)(const InfinitasBrowserAPI *api);

    /* Optional: navigation hook (called before every navigation) */
    void (*on_navigate)(InfinitasNavEvent *ev, gpointer userdata);
    gpointer nav_userdata;

    /* Optional: called when a page finishes loading */
    void (*on_load_finished)(const gchar *url, gpointer userdata);
    gpointer load_userdata;

    /* Called when the plugin is unloaded */
    void (*destroy)(void);
} InfinitasPlugin;

/* ── the ONE function your .so must export ──────────────────────────────── */

/**
 * infinitas_plugin_init - entry point for every Infinitas plugin
 *
 * @api: browser API (valid for the lifetime of the plugin)
 * @returns: heap-allocated InfinitasPlugin descriptor, or NULL on failure
 *
 * The browser will call destroy() on your descriptor before dlclose().
 */
InfinitasPlugin* infinitas_plugin_init(const InfinitasBrowserAPI *api);

/* ── helper macro for implementing a plugin ────────────────────────────── */

#define INFINITAS_PLUGIN_DEFINE(init_func) \
    __attribute__((visibility("default"))) \
    InfinitasPlugin* infinitas_plugin_init(const InfinitasBrowserAPI *api) { \
        return init_func(api); \
    }

#ifdef __cplusplus
}
#endif

#endif /* INFINITAS_PLUGIN_API_H */
