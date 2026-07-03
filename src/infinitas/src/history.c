/**
 * Infinitas Browser - History Manager Implementation
 */

#include "history.h"
#include <string.h>
#include <stdlib.h>

static void history_entry_free(gpointer data);

HistoryManager* history_new(const gchar *data_dir) {
    HistoryManager *history = g_new0(HistoryManager, 1);
    history->data_dir = g_strdup(data_dir ? data_dir : ".infinitas");
    history->db = NULL;
    return history;
}

void history_free(HistoryManager *history) {
    if (!history) return;
    history_close(history);
    g_free(history->data_dir);
    g_free(history);
}

int history_open(HistoryManager *history) {
    if (!history) return -1;
    if (history->db) return 0;
    
    gchar *db_path = g_build_filename(history->data_dir, HISTORY_DB, NULL);
    int rc = sqlite3_open(db_path, &history->db);
    g_free(db_path);
    
    if (rc != SQLITE_OK) {
        g_printerr("Failed to open history database: %s\n", sqlite3_errmsg(history->db));
        return -1;
    }
    return 0;
}

int history_close(HistoryManager *history) {
    if (!history || !history->db) return 0;
    int rc = sqlite3_close(history->db);
    history->db = NULL;
    return rc;
}

int history_init(HistoryManager *history) {
    if (!history) return -1;
    if (history_open(history) != 0) return -1;
    
    char *error_msg = NULL;
    int rc = sqlite3_exec(history->db, HISTORY_CREATE_SQL, NULL, NULL, &error_msg);
    
    if (rc != SQLITE_OK) {
        g_printerr("Failed to create history table: %s\n", error_msg);
        sqlite3_free(error_msg);
        return -1;
    }
    return 0;
}

HistoryEntry* history_add(HistoryManager *history, const gchar *title, const gchar *url) {
    if (!history || !history->db || !title || !url) return NULL;
    
    const gchar *check_sql = "SELECT id, visit_count FROM history WHERE url = ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(history->db, check_sql, -1, &stmt, NULL);
    
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            gint id = sqlite3_column_int(stmt, 0);
            gint visit_count = sqlite3_column_int(stmt, 1) + 1;
            
            gchar *update_sql = g_strdup_printf(
                "UPDATE history SET visit_count = %d, timestamp = datetime('now') WHERE id = %d",
                visit_count, id);
            rc = sqlite3_exec(history->db, update_sql, NULL, NULL, NULL);
            g_free(update_sql);
            sqlite3_finalize(stmt);
            return NULL;
        }
        sqlite3_finalize(stmt);
    }
    
    const gchar *insert_sql = "INSERT INTO history (title, url, timestamp) VALUES (?, ?, datetime('now'))";
    rc = sqlite3_prepare_v2(history->db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    
    sqlite3_bind_text(stmt, 1, title, -1, NULL);
    sqlite3_bind_text(stmt, 2, url, -1, NULL);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        HistoryEntry *entry = g_new0(HistoryEntry, 1);
        entry->id = (gint)sqlite3_last_insert_rowid(history->db);
        entry->title = g_strdup(title);
        entry->url = g_strdup(url);
        entry->timestamp = g_strdup("now");
        entry->visit_count = 1;
        return entry;
    }
    return NULL;
}

GPtrArray* history_get_all(HistoryManager *history, gint limit) {
    if (!history || !history->db) return NULL;
    
    GPtrArray *history_array = g_ptr_array_new_with_free_func(history_entry_free);
    
    gchar *sql = g_strdup_printf(
        "SELECT id, title, url, timestamp, visit_count FROM history ORDER BY timestamp DESC LIMIT %d", limit);
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(history->db, sql, -1, &stmt, NULL);
    g_free(sql);
    
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry *entry = g_new0(HistoryEntry, 1);
            entry->id = sqlite3_column_int(stmt, 0);
            entry->title = g_strdup((const gchar*)sqlite3_column_text(stmt, 1));
            entry->url = g_strdup((const gchar*)sqlite3_column_text(stmt, 2));
            entry->timestamp = g_strdup((const gchar*)sqlite3_column_text(stmt, 3));
            entry->visit_count = sqlite3_column_int(stmt, 4);
            g_ptr_array_add(history_array, entry);
        }
    }
    sqlite3_finalize(stmt);
    return history_array;
}

GPtrArray* history_search(HistoryManager *history, const gchar *query) {
    if (!history || !history->db || !query) return NULL;
    
    GPtrArray *result = g_ptr_array_new_with_free_func(history_entry_free);
    
    gchar *search_sql = g_strdup_printf(
        "SELECT id, title, url, timestamp, visit_count FROM history "
        "WHERE title LIKE '%%%s%%' OR url LIKE '%%%s%%' ORDER BY timestamp DESC",
        query, query);
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(history->db, search_sql, -1, &stmt, NULL);
    g_free(search_sql);
    
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry *entry = g_new0(HistoryEntry, 1);
            entry->id = sqlite3_column_int(stmt, 0);
            entry->title = g_strdup((const gchar*)sqlite3_column_text(stmt, 1));
            entry->url = g_strdup((const gchar*)sqlite3_column_text(stmt, 2));
            entry->timestamp = g_strdup((const gchar*)sqlite3_column_text(stmt, 3));
            entry->visit_count = sqlite3_column_int(stmt, 4);
            g_ptr_array_add(result, entry);
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

int history_clear(HistoryManager *history) {
    if (!history || !history->db) return -1;
    return sqlite3_exec(history->db, "DELETE FROM history", NULL, NULL, NULL);
}

int history_delete_entry(HistoryManager *history, gint id) {
    if (!history || !history->db) return -1;
    
    gchar *sql = g_strdup_printf("DELETE FROM history WHERE id = %d", id);
    int rc = sqlite3_exec(history->db, sql, NULL, NULL, NULL);
    g_free(sql);
    return rc;
}

static void history_entry_free(gpointer data) {
    HistoryEntry *entry = (HistoryEntry*)data;
    if (entry) {
        if (entry->title) g_free(entry->title);
        if (entry->url) g_free(entry->url);
        if (entry->timestamp) g_free(entry->timestamp);
        g_free(entry);
    }
}
