// Infinitas Browser - Extension Manager
// Loads both native Infinitas extensions and Chrome/WebExtension-compatible extensions.
// Extensions live in ~/.infinitas/extensions/<name>/ (unpacked directory format).
// Chrome .crx files are ZIP archives — extract them with: unzip ext.crx -d myext/

#ifndef EXTENSION_H
#define EXTENSION_H

#include <glib.h>
#include <webkit/webkit.h>

typedef struct {
    gchar    *id;
    gchar    *name;
    gchar    *version;
    gchar    *description;
    gchar    *path;
    gint      manifest_version; /* 1=native, 2=Chrome MV2, 3=Chrome MV3 */

    GPtrArray *content_scripts; /* gchar* JS source strings */
    GPtrArray *content_css;     /* gchar* CSS source strings */
    gchar    *content_match;    /* legacy single pattern */
    gchar   **url_patterns;     /* NULL-terminated, NULL = all URLs */
    WebKitUserScriptInjectionTime inject_at;

    gchar    *bg_script;

    gboolean  has_action;
    gchar    *action_title;
    gchar    *action_popup;

    gchar    *icon_path;        /* absolute path to best icon */
} InfinitasExtension;

typedef struct {
    GPtrArray *extensions;
    gchar     *ext_dir;
} ExtensionManager;

ExtensionManager* extension_manager_new(const gchar *ext_dir);
void              extension_manager_free(ExtensionManager *em);
void              extension_manager_load_all(ExtensionManager *em);

void extension_manager_inject(ExtensionManager *em,
                               WebKitWebView *view,
                               WebKitUserContentManager *mgr);

void extension_manager_register_handlers(ExtensionManager *em,
                                          WebKitUserContentManager *mgr,
                                          gpointer browser);

void extension_manager_add_toolbar_buttons(ExtensionManager *em,
                                            GtkWidget *toolbar,
                                            gpointer browser);

/* Register extension resources so infinitas://ext/<id>/<path> can serve them */
void extension_manager_register_resources(ExtensionManager *em);

#endif
