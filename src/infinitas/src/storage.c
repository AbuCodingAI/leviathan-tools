/**
 * Infinitas Browser - Storage Manager Implementation
 */

#include "storage.h"
#include <stdio.h>
#include <sys/stat.h>

static gchar* storage_get_path(StorageManager *storage, const gchar *filename);

StorageManager* storage_new(const gchar *data_dir) {
    StorageManager *storage = g_new0(StorageManager, 1);
    storage->data_dir = g_strdup(data_dir ? data_dir : ".infinitas");
    storage->history_db = NULL;
    storage->bookmarks_json = NULL;
    storage->settings_json = NULL;
    storage->dirty = FALSE;
    
    struct stat st;
    if (stat(storage->data_dir, &st) != 0) {
        g_mkdir_with_parents(storage->data_dir, 0755);
    }
    return storage;
}

void storage_free(StorageManager *storage) {
    if (!storage) return;
    if (storage->history_db) sqlite3_close(storage->history_db);
    if (storage->bookmarks_json) json_object_put(storage->bookmarks_json);
    if (storage->settings_json) json_object_put(storage->settings_json);
    g_free(storage->data_dir);
    g_free(storage);
}

int storage_init_history(StorageManager *storage) {
    if (!storage) return -1;
    gchar *db_path = storage_get_path(storage, HISTORY_DB);
    int rc = sqlite3_open(db_path, &storage->history_db);
    g_free(db_path);
    
    if (rc != SQLITE_OK) {
        g_printerr("Failed to open history database: %s\n", sqlite3_errmsg(storage->history_db));
        return -1;
    }
    
    char *error_msg = NULL;
    rc = sqlite3_exec(storage->history_db, HISTORY_CREATE_SQL, NULL, NULL, &error_msg);
    if (rc != SQLITE_OK) {
        g_printerr("Failed to create history table: %s\n", error_msg);
        sqlite3_free(error_msg);
        return -1;
    }
    return 0;
}

int storage_add_history_entry(StorageManager *storage, const gchar *title, const gchar *url) {
    if (!storage || !storage->history_db || !title || !url) return -1;
    
    gchar *check_sql = g_strdup_printf("SELECT id, visit_count FROM history WHERE url = ?");
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(storage->history_db, check_sql, -1, &stmt, NULL);
    g_free(check_sql);
    
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            gint id = sqlite3_column_int(stmt, 0);
            gint visit_count = sqlite3_column_int(stmt, 1) + 1;
            gchar *update_sql = g_strdup_printf(
                "UPDATE history SET visit_count = %d, timestamp = datetime('now') WHERE id = %d",
                visit_count, id);
            rc = sqlite3_exec(storage->history_db, update_sql, NULL, NULL, NULL);
            g_free(update_sql);
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_finalize(stmt);
    }
    
    gchar *insert_sql = g_strdup_printf(
        "INSERT INTO history (title, url, timestamp) VALUES ('%s', '%s', datetime('now'))", title, url);
    rc = sqlite3_exec(storage->history_db, insert_sql, NULL, NULL, NULL);
    g_free(insert_sql);
    return (rc == SQLITE_OK) ? 0 : -1;
}

GArray* storage_get_history(StorageManager *storage, gint limit) {
    if (!storage || !storage->history_db) return NULL;
    
    GArray *history = g_array_new(FALSE, FALSE, sizeof(HistoryEntry));
    gchar *sql = g_strdup_printf(
        "SELECT id, title, url, timestamp, visit_count FROM history ORDER BY timestamp DESC LIMIT %d", limit);
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(storage->history_db, sql, -1, &stmt, NULL);
    g_free(sql);
    
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry entry;
            entry.id = sqlite3_column_int(stmt, 0);
            entry.title = g_strdup((const gchar*)sqlite3_column_text(stmt, 1));
            entry.url = g_strdup((const gchar*)sqlite3_column_text(stmt, 2));
            entry.timestamp = g_strdup((const gchar*)sqlite3_column_text(stmt, 3));
            entry.visit_count = sqlite3_column_int(stmt, 4);
            g_array_append_val(history, entry);
        }
    }
    sqlite3_finalize(stmt);
    return history;
}

int storage_clear_history(StorageManager *storage) {
    if (!storage || !storage->history_db) return -1;
    return sqlite3_exec(storage->history_db, "DELETE FROM history", NULL, NULL, NULL);
}

int storage_load_json(StorageManager *storage, const gchar *filename, json_object **out_json) {
    if (!storage || !filename || !out_json) return -1;
    
    gchar *path = storage_get_path(storage, filename);
    FILE *file = fopen(path, "r");
    g_free(path);
    
    if (!file) {
        *out_json = json_object_new_object();
        return 0;
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    gchar *content = g_malloc(size + 1);
    size_t read_size = fread(content, 1, size, file);
    content[read_size] = '\0';
    fclose(file);
    
    *out_json = json_tokener_parse(content);
    g_free(content);
    
    if (!*out_json) return -1;
    return 0;
}

int storage_save_json(StorageManager *storage, const gchar *filename, json_object *json) {
    if (!storage || !filename || !json) return -1;
    
    gchar *path = storage_get_path(storage, filename);
    FILE *file = fopen(path, "w");
    g_free(path);
    
    if (!file) return -1;
    
    const gchar *json_str = json_object_to_json_string(json);
    fprintf(file, "%s\n", json_str);
    fclose(file);
    
    return 0;
}

static gchar* storage_get_path(StorageManager *storage, const gchar *filename) {
    return g_build_filename(storage->data_dir, filename, NULL);
}
