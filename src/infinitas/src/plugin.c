/**
 * Infinitas Browser - Plugin Manager Implementation
 */

#include "plugin.h"
#include <dlfcn.h>
#include <string.h>

typedef InfinitasPlugin* (*PluginInitFn)(const InfinitasBrowserAPI *);

PluginManager* plugin_manager_new(const gchar *plugin_dir) {
    PluginManager *pm = g_new0(PluginManager, 1);
    pm->plugins    = g_ptr_array_new_with_free_func(NULL);
    pm->plugin_dir = g_strdup(plugin_dir);
    g_mkdir_with_parents(plugin_dir, 0755);
    return pm;
}

static void loaded_plugin_free(LoadedPlugin *lp) {
    if (!lp) return;
    if (lp->descriptor && lp->descriptor->destroy)
        lp->descriptor->destroy();
    g_free(lp->descriptor); /* plugin allocated this */
    g_free(lp->api);
    if (lp->dl_handle) dlclose(lp->dl_handle);
    g_free(lp->path);
    g_free(lp->name);
    g_free(lp);
}

void plugin_manager_free(PluginManager *pm) {
    if (!pm) return;
    for (guint i = 0; i < pm->plugins->len; i++)
        loaded_plugin_free(g_ptr_array_index(pm->plugins, i));
    g_ptr_array_free(pm->plugins, TRUE);
    g_free(pm->plugin_dir);
    g_free(pm);
}

void plugin_manager_load_all(PluginManager *pm, InfinitasBrowserAPI *shared_api) {
    GDir *dir = g_dir_open(pm->plugin_dir, 0, NULL);
    if (!dir) return;

    const gchar *fname;
    while ((fname = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(fname, ".so")) continue;

        gchar *path = g_build_filename(pm->plugin_dir, fname, NULL);
        void *dl = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (!dl) {
            g_printerr("[PLUGIN] dlopen failed: %s — %s\n", path, dlerror());
            g_free(path);
            continue;
        }

        PluginInitFn init = (PluginInitFn)dlsym(dl, "infinitas_plugin_init");
        if (!init) {
            g_printerr("[PLUGIN] %s has no infinitas_plugin_init\n", fname);
            dlclose(dl);
            g_free(path);
            continue;
        }

        /* give each plugin its own API copy with the browser pointer */
        InfinitasBrowserAPI *api = g_new(InfinitasBrowserAPI, 1);
        memcpy(api, shared_api, sizeof(InfinitasBrowserAPI));

        InfinitasPlugin *desc = init(api);
        if (!desc) {
            g_printerr("[PLUGIN] %s init returned NULL\n", fname);
            g_free(api);
            dlclose(dl);
            g_free(path);
            continue;
        }

        LoadedPlugin *lp = g_new0(LoadedPlugin, 1);
        lp->path       = path;
        lp->name       = g_strdup(desc->name ? desc->name : fname);
        lp->dl_handle  = dl;
        lp->descriptor = desc;
        lp->api        = api;
        g_ptr_array_add(pm->plugins, lp);

        g_print("[PLUGIN] Loaded: %s %s by %s\n",
                desc->name    ? desc->name    : "?",
                desc->version ? desc->version : "",
                desc->author  ? desc->author  : "?");
    }
    g_dir_close(dir);
}

gboolean plugin_manager_fire_navigate(PluginManager *pm, const gchar *url) {
    if (!pm) return TRUE;
    InfinitasNavEvent ev = { .url = url, .cancel = FALSE };
    for (guint i = 0; i < pm->plugins->len; i++) {
        LoadedPlugin *lp = g_ptr_array_index(pm->plugins, i);
        if (lp->descriptor->on_navigate)
            lp->descriptor->on_navigate(&ev, lp->descriptor->nav_userdata);
        if (ev.cancel) return FALSE;
    }
    return TRUE;
}

void plugin_manager_fire_load_finished(PluginManager *pm, const gchar *url) {
    if (!pm) return;
    for (guint i = 0; i < pm->plugins->len; i++) {
        LoadedPlugin *lp = g_ptr_array_index(pm->plugins, i);
        if (lp->descriptor->on_load_finished)
            lp->descriptor->on_load_finished(url, lp->descriptor->load_userdata);
    }
}

GtkWidget* plugin_manager_create_content_widget(PluginManager *pm,
                                                  const gchar *url,
                                                  const gchar *mime,
                                                  gint w, gint h) {
    if (!pm || !mime) return NULL;
    for (guint i = 0; i < pm->plugins->len; i++) {
        LoadedPlugin *lp = g_ptr_array_index(pm->plugins, i);
        if (!lp->descriptor->mime_types) continue;
        for (int j = 0; lp->descriptor->mime_types[j]; j++) {
            if (g_strcmp0(lp->descriptor->mime_types[j], mime) == 0) {
                if (lp->descriptor->content_handler.create_widget)
                    return lp->descriptor->content_handler.create_widget(
                        url, mime, w, h,
                        lp->descriptor->content_handler.userdata);
            }
        }
    }
    return NULL;
}
