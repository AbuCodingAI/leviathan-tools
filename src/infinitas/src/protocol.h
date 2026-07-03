/**
 * Infinitas Browser - User Protocol Store
 *
 * Manages infinitas:// shortcut aliases.  Three behaviours:
 *   reroute   - browser navigates away to target (address bar changes)
 *   mask      - target content served under the infinitas:// URL (address bar stays)
 *   open_app  - calls xdg-open on the target (OS-level launch)
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <glib.h>
#include <sqlite3.h>

typedef enum {
    PROTOCOL_REROUTE,
    PROTOCOL_MASK,
    PROTOCOL_OPEN_APP,
} ProtocolType;

typedef struct {
    gchar        *name;
    ProtocolType  type;
    gchar        *target;
} InfinitasProtocol;

typedef struct {
    sqlite3   *db;
    gchar     *data_dir;
} ProtocolStore;

ProtocolStore*      protocol_store_new (const gchar *data_dir);
void                protocol_store_free(ProtocolStore *ps);

/* Returns heap-allocated protocol or NULL if not found. Caller g_free()s members + struct. */
InfinitasProtocol*  protocol_lookup    (ProtocolStore *ps, const gchar *name);

/* Returns GPtrArray of InfinitasProtocol* (free with protocol_list_free) */
GPtrArray*          protocol_list      (ProtocolStore *ps);
void                protocol_list_free (GPtrArray *list);

void                protocol_add       (ProtocolStore *ps, const gchar *name,
                                        ProtocolType type, const gchar *target);
void                protocol_delete    (ProtocolStore *ps, const gchar *name);

const gchar*        protocol_type_name (ProtocolType t);
ProtocolType        protocol_type_from_string(const gchar *s);

#endif
