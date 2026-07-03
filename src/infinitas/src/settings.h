/**
 * Infinitas Browser - Settings Manager Header
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <json-c/json.h>
#include <glib.h>

#define SETTINGS_FILE "settings.json"
#define DEFAULT_HOME_PAGE "infinitas://newtab"
#define DEFAULT_SEARCH_ENGINE "https://www.google.com/search?q="
#define DEFAULT_THEME "light"
#define DEFAULT_FONT_SIZE 12
#define BROWSER_WINDOW_WIDTH 1200
#define BROWSER_WINDOW_HEIGHT 800

typedef struct {
    gchar *data_dir;
    json_object *settings;
    gboolean dirty;
} SettingsManager;

typedef struct {
    gchar *home_page;
    gchar *search_engine;
    gchar *theme;
    gint font_size;
    gboolean enable_javascript;
    gboolean enable_cookies;
    gboolean cache_enabled;
    gint window_width;
    gint window_height;
    gboolean show_dev_tools;
} DefaultSettings;

SettingsManager* settings_new(const gchar *data_dir);
void settings_free(SettingsManager *settings);
int settings_load(SettingsManager *settings);
int settings_save(SettingsManager *settings);
const gchar* settings_get_string(SettingsManager *settings, const gchar *key);
gint settings_get_int(SettingsManager *settings, const gchar *key, gint default_val);
gboolean settings_get_bool(SettingsManager *settings, const gchar *key, gboolean default_val);
void settings_set_string(SettingsManager *settings, const gchar *key, const gchar *value);
void settings_set_int(SettingsManager *settings, const gchar *key, gint value);
void settings_set_bool(SettingsManager *settings, const gchar *key, gboolean value);
const DefaultSettings* settings_get_defaults(void);
void settings_apply_defaults(SettingsManager *settings);

#endif
