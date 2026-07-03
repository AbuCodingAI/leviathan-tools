/**
 * Infinitas Browser - Storage Manager Header
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <sqlite3.h>
#include <json-c/json.h>
#include <glib.h>

#define HISTORY_DB "history.db"
#define HISTORY_CREATE_SQL \
    "CREATE TABLE IF NOT EXISTS history (" \
    "id INTEGER PRIMARY KEY AUTOINCREMENT," \
    "title TEXT NOT NULL," \
    "url TEXT NOT NULL UNIQUE," \
    "timestamp TEXT NOT NULL," \
    "visit_count INTEGER DEFAULT 1" \
    ");"

typedef struct {
    gchar *data_dir;
    sqlite3 *history_db;
    json_object *bookmarks_json;
    json_object *settings_json;
    gboolean dirty;
} StorageManager;

StorageManager* storage_new(const gchar *data_dir);
void storage_free(StorageManager *storage);
int storage_init_history(StorageManager *storage);
int storage_add_history_entry(StorageManager *storage, const gchar *title, const gchar *url);
GArray* storage_get_history(StorageManager *storage, gint limit);
int storage_clear_history(StorageManager *storage);
int storage_load_json(StorageManager *storage, const gchar *filename, json_object **out_json);
int storage_save_json(StorageManager *storage, const gchar *filename, json_object *json);

typedef struct {
    gint id;
    gchar *title;
    gchar *url;
    gchar *timestamp;
    gint visit_count;
} HistoryEntry;

#endif
