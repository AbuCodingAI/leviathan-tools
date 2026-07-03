/**
 * Infinitas Browser - User Protocol Store Implementation
 */

#include "protocol.h"
#include <string.h>

#define SCHEMA \
    "CREATE TABLE IF NOT EXISTS protocols(" \
    "  name TEXT PRIMARY KEY," \
    "  type TEXT NOT NULL," \
    "  target TEXT NOT NULL," \
    "  created_at TEXT DEFAULT (datetime('now'))" \
    ");"

/* Default seeds loaded on first run */
static const struct { const gchar *name; const gchar *type; const gchar *target; }
DEFAULTS[] = {
    { "chat",    "reroute", "https://chatgpt.com"   },
    { "cpp",     "reroute", "https://cpp.sh"         },
    { "newtab",  "mask",    "infinitas://tab"        },
};

/* ── init ────────────────────────────────────────────────────────────────── */

ProtocolStore* protocol_store_new(const gchar *data_dir) {
    ProtocolStore *ps = g_new0(ProtocolStore, 1);
    ps->data_dir = g_strdup(data_dir);

    gchar *db_path = g_build_filename(data_dir, "protocols.db", NULL);
    gboolean is_new = !g_file_test(db_path, G_FILE_TEST_EXISTS);

    if (sqlite3_open(db_path, &ps->db) != SQLITE_OK) {
        g_printerr("[PROTOCOL] Failed to open db: %s\n", db_path);
        g_free(db_path);
        g_free(ps->data_dir);
        g_free(ps);
        return NULL;
    }
    g_free(db_path);

    sqlite3_exec(ps->db, SCHEMA, NULL, NULL, NULL);

    /* Seed defaults on first run */
    if (is_new) {
        for (gsize i = 0; i < G_N_ELEMENTS(DEFAULTS); i++) {
            protocol_add(ps, DEFAULTS[i].name,
                         protocol_type_from_string(DEFAULTS[i].type),
                         DEFAULTS[i].target);
        }
    }
    return ps;
}

void protocol_store_free(ProtocolStore *ps) {
    if (!ps) return;
    if (ps->db) sqlite3_close(ps->db);
    g_free(ps->data_dir);
    g_free(ps);
}

/* ── type helpers ────────────────────────────────────────────────────────── */

const gchar* protocol_type_name(ProtocolType t) {
    switch (t) {
        case PROTOCOL_REROUTE:  return "reroute";
        case PROTOCOL_MASK:     return "mask";
        case PROTOCOL_OPEN_APP: return "open_app";
    }
    return "reroute";
}

ProtocolType protocol_type_from_string(const gchar *s) {
    if (!s) return PROTOCOL_REROUTE;
    if (g_str_equal(s, "mask"))     return PROTOCOL_MASK;
    if (g_str_equal(s, "open_app")) return PROTOCOL_OPEN_APP;
    return PROTOCOL_REROUTE;
}

/* ── CRUD ────────────────────────────────────────────────────────────────── */

void protocol_add(ProtocolStore *ps, const gchar *name,
                  ProtocolType type, const gchar *target) {
    if (!ps || !ps->db || !name || !target) return;
    const gchar *sql =
        "INSERT OR REPLACE INTO protocols(name,type,target) VALUES(?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ps->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name,                       -1, NULL);
        sqlite3_bind_text(stmt, 2, protocol_type_name(type),   -1, NULL);
        sqlite3_bind_text(stmt, 3, target,                     -1, NULL);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void protocol_delete(ProtocolStore *ps, const gchar *name) {
    if (!ps || !ps->db || !name) return;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ps->db,
            "DELETE FROM protocols WHERE name=?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, NULL);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

InfinitasProtocol* protocol_lookup(ProtocolStore *ps, const gchar *name) {
    if (!ps || !ps->db || !name) return NULL;
    sqlite3_stmt *stmt;
    InfinitasProtocol *p = NULL;
    if (sqlite3_prepare_v2(ps->db,
            "SELECT name,type,target FROM protocols WHERE name=?;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            p = g_new0(InfinitasProtocol, 1);
            p->name   = g_strdup((const gchar*)sqlite3_column_text(stmt, 0));
            p->type   = protocol_type_from_string(
                            (const gchar*)sqlite3_column_text(stmt, 1));
            p->target = g_strdup((const gchar*)sqlite3_column_text(stmt, 2));
        }
        sqlite3_finalize(stmt);
    }
    return p;
}

GPtrArray* protocol_list(ProtocolStore *ps) {
    GPtrArray *arr = g_ptr_array_new();
    if (!ps || !ps->db) return arr;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ps->db,
            "SELECT name,type,target FROM protocols ORDER BY name;",
            -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            InfinitasProtocol *p = g_new0(InfinitasProtocol, 1);
            p->name   = g_strdup((const gchar*)sqlite3_column_text(stmt, 0));
            p->type   = protocol_type_from_string(
                            (const gchar*)sqlite3_column_text(stmt, 1));
            p->target = g_strdup((const gchar*)sqlite3_column_text(stmt, 2));
            g_ptr_array_add(arr, p);
        }
        sqlite3_finalize(stmt);
    }
    return arr;
}

void protocol_list_free(GPtrArray *list) {
    if (!list) return;
    for (guint i = 0; i < list->len; i++) {
        InfinitasProtocol *p = g_ptr_array_index(list, i);
        g_free(p->name); g_free(p->target); g_free(p);
    }
    g_ptr_array_free(list, TRUE);
}
