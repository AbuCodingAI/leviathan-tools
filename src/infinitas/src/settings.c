/**
 * Infinitas Browser - Settings Manager Implementation
 */

#include "settings.h"
#include "storage.h"
#include <string.h>

static const DefaultSettings DEFAULT_SETTINGS = {
    .home_page = DEFAULT_HOME_PAGE,
    .search_engine = DEFAULT_SEARCH_ENGINE,
    .theme = DEFAULT_THEME,
    .font_size = DEFAULT_FONT_SIZE,
    .enable_javascript = TRUE,
    .enable_cookies = TRUE,
    .cache_enabled = TRUE,
    .window_width = BROWSER_WINDOW_WIDTH,
    .window_height = BROWSER_WINDOW_HEIGHT,
    .show_dev_tools = FALSE
};

SettingsManager* settings_new(const gchar *data_dir) {
    SettingsManager *settings = g_new0(SettingsManager, 1);
    settings->data_dir = g_strdup(data_dir ? data_dir : ".infinitas");
    settings->settings = json_object_new_object();
    settings->dirty = FALSE;
    settings_apply_defaults(settings);
    settings_load(settings);
    return settings;
}

void settings_free(SettingsManager *settings) {
    if (!settings) return;
    if (settings->data_dir) g_free(settings->data_dir);
    if (settings->settings) json_object_put(settings->settings);
    g_free(settings);
}

int settings_load(SettingsManager *settings) {
    if (!settings) return -1;
    StorageManager *storage = storage_new(settings->data_dir);
    int rc = storage_load_json(storage, SETTINGS_FILE, &settings->settings);
    storage_free(storage);
    return rc;
}

int settings_save(SettingsManager *settings) {
    if (!settings) return -1;
    StorageManager *storage = storage_new(settings->data_dir);
    int rc = storage_save_json(storage, SETTINGS_FILE, settings->settings);
    storage_free(storage);
    if (rc == 0) settings->dirty = FALSE;
    return rc;
}

const gchar* settings_get_string(SettingsManager *settings, const gchar *key) {
    if (!settings || !settings->settings || !key) return NULL;
    json_object *obj = json_object_object_get(settings->settings, key);
    if (obj && json_object_is_type(obj, json_type_string)) {
        return json_object_get_string(obj);
    }
    return NULL;
}

gint settings_get_int(SettingsManager *settings, const gchar *key, gint default_val) {
    if (!settings || !settings->settings || !key) return default_val;
    json_object *obj = json_object_object_get(settings->settings, key);
    if (obj && json_object_is_type(obj, json_type_int)) {
        return (gint)json_object_get_int(obj);
    }
    return default_val;
}

gboolean settings_get_bool(SettingsManager *settings, const gchar *key, gboolean default_val) {
    if (!settings || !settings->settings || !key) return default_val;
    json_object *obj = json_object_object_get(settings->settings, key);
    if (obj && json_object_is_type(obj, json_type_boolean)) {
        return json_object_get_boolean(obj);
    }
    return default_val;
}

void settings_set_string(SettingsManager *settings, const gchar *key, const gchar *value) {
    if (!settings || !settings->settings || !key) return;
    json_object_object_add(settings->settings, key, json_object_new_string(value));
    settings->dirty = TRUE;
}

void settings_set_int(SettingsManager *settings, const gchar *key, gint value) {
    if (!settings || !settings->settings || !key) return;
    json_object_object_add(settings->settings, key, json_object_new_int(value));
    settings->dirty = TRUE;
}

void settings_set_bool(SettingsManager *settings, const gchar *key, gboolean value) {
    if (!settings || !settings->settings || !key) return;
    json_object_object_add(settings->settings, key, json_object_new_boolean(value));
    settings->dirty = TRUE;
}

const DefaultSettings* settings_get_defaults(void) {
    return &DEFAULT_SETTINGS;
}

void settings_apply_defaults(SettingsManager *settings) {
    if (!settings) return;
    const DefaultSettings *defaults = settings_get_defaults();
    
    json_object_object_add(settings->settings, "home_page", 
                          json_object_new_string(defaults->home_page));
    json_object_object_add(settings->settings, "search_engine", 
                          json_object_new_string(defaults->search_engine));
    json_object_object_add(settings->settings, "theme", 
                          json_object_new_string(defaults->theme));
    json_object_object_add(settings->settings, "font_size", 
                          json_object_new_int(defaults->font_size));
    json_object_object_add(settings->settings, "enable_javascript", 
                          json_object_new_boolean(defaults->enable_javascript));
    json_object_object_add(settings->settings, "enable_cookies", 
                          json_object_new_boolean(defaults->enable_cookies));
    json_object_object_add(settings->settings, "cache_enabled", 
                          json_object_new_boolean(defaults->cache_enabled));
    json_object_object_add(settings->settings, "window_width", 
                          json_object_new_int(defaults->window_width));
    json_object_object_add(settings->settings, "window_height", 
                          json_object_new_int(defaults->window_height));
    json_object_object_add(settings->settings, "show_dev_tools", 
                          json_object_new_boolean(defaults->show_dev_tools));
}
