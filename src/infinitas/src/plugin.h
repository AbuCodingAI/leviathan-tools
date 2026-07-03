/**
 * Infinitas Browser - Plugin Manager Header
 */

#ifndef PLUGIN_H
#define PLUGIN_H

#include <glib.h>
#include <gtk/gtk.h>
#include "infinitas_plugin_api.h"

typedef struct {
    gchar            *path;       /* path to .so */
    gchar            *name;       /* from descriptor */
    void             *dl_handle;  /* dlopen handle */
    InfinitasPlugin  *descriptor;
    InfinitasBrowserAPI *api;
} LoadedPlugin;

typedef struct {
    GPtrArray *plugins;  /* LoadedPlugin* */
    gchar     *plugin_dir;
} PluginManager;

PluginManager* plugin_manager_new(const gchar *plugin_dir);
void           plugin_manager_free(PluginManager *pm);
void           plugin_manager_load_all(PluginManager *pm,
                                        InfinitasBrowserAPI *api);

/* Called by browser on each navigation — plugins can cancel */
gboolean plugin_manager_fire_navigate(PluginManager *pm,
                                       const gchar *url);

/* Called by browser when a page finishes loading */
void plugin_manager_fire_load_finished(PluginManager *pm, const gchar *url);

/* Returns a GtkWidget* if any plugin handles this MIME type, else NULL */
GtkWidget* plugin_manager_create_content_widget(PluginManager *pm,
                                                  const gchar *url,
                                                  const gchar *mime,
                                                  gint w, gint h);

#endif
