/**
 * Infinitas Browser - Native ad/tracker blocker implementation
 */

#include "adblock.h"
#include <glib.h>

#define ADBLOCK_FILTER_ID "infinitas-adblock"

static WebKitUserContentFilterStore *g_store   = NULL;
static WebKitUserContentFilter       *g_filter = NULL;  /* ready once compiled */
static GPtrArray *g_managers = NULL;   /* content managers awaiting the filter */
static gboolean   g_enabled  = TRUE;

/* Add the compiled filter to a manager (only if enabled + ready). */
static void apply_to(WebKitUserContentManager *mgr) {
    if (!mgr || !g_filter || !g_enabled) return;
    webkit_user_content_manager_add_filter(mgr, g_filter);
}

/* Compilation finished — cache the filter and apply to everyone waiting. */
static void on_filter_saved(GObject *source, GAsyncResult *res, gpointer data) {
    (void)data;
    GError *err = NULL;
    WebKitUserContentFilter *f =
        webkit_user_content_filter_store_save_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err);
    if (!f) {
        if (err) { g_warning("adblock: filter compile failed: %s", err->message); g_error_free(err); }
        return;
    }
    g_filter = f;
    g_print("adblock: ruleset compiled — blocking active\n");
    if (g_managers) {
        for (guint i = 0; i < g_managers->len; i++)
            apply_to(g_ptr_array_index(g_managers, i));
    }
}

void adblock_init(const gchar *rules_path) {
    if (!g_managers)
        g_managers = g_ptr_array_new();

    gchar *json = NULL;
    gsize len = 0;
    if (!rules_path || !g_file_get_contents(rules_path, &json, &len, NULL)) {
        g_warning("adblock: rules file not found (%s) — blocking disabled",
                  rules_path ? rules_path : "(null)");
        return;
    }

    gchar *dir = g_build_filename(g_get_home_dir(), ".infinitas", "filters", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_store = webkit_user_content_filter_store_new(dir);
    g_free(dir);

    GBytes *bytes = g_bytes_new_take(json, len);   /* takes ownership of json */
    webkit_user_content_filter_store_save(
        g_store, ADBLOCK_FILTER_ID, bytes, NULL, on_filter_saved, NULL);
    g_bytes_unref(bytes);
}

void adblock_set_enabled(gboolean enabled) {
    g_enabled = enabled;
    if (!g_managers) return;
    for (guint i = 0; i < g_managers->len; i++) {
        WebKitUserContentManager *mgr = g_ptr_array_index(g_managers, i);
        if (!mgr) continue;
        if (enabled) apply_to(mgr);
        else if (g_filter)
            webkit_user_content_manager_remove_filter(mgr, g_filter);
    }
}

gboolean adblock_is_enabled(void) {
    return g_enabled;
}

void adblock_register_manager(WebKitUserContentManager *manager) {
    if (!manager) return;
    if (!g_managers)
        g_managers = g_ptr_array_new();
    g_ptr_array_add(g_managers, manager);
    apply_to(manager);   /* applies now if already compiled; else on_filter_saved will */
}
