/**
 * Infinitas Browser - History Manager Header
 */

#ifndef HISTORY_H
#define HISTORY_H

#include <sqlite3.h>
#include <glib.h>
#include "storage.h"

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
    sqlite3 *db;
} HistoryManager;

HistoryManager* history_new(const gchar *data_dir);
void history_free(HistoryManager *history);
int history_open(HistoryManager *history);
int history_close(HistoryManager *history);
int history_init(HistoryManager *history);
HistoryEntry* history_add(HistoryManager *history, const gchar *title, const gchar *url);
GPtrArray* history_get_all(HistoryManager *history, gint limit);
GPtrArray* history_search(HistoryManager *history, const gchar *query);
int history_clear(HistoryManager *history);
int history_delete_entry(HistoryManager *history, gint id);

#endif
