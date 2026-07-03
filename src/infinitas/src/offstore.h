/**
 * Infinitas Browser - Offline Page Store
 *
 * Captures pages as encrypted binary blobs (.infx files).
 * Format: magic(4) | version(1) | iv(16) | len(8) | ciphertext(N) | hmac(32)
 * Cipher: SHA256-CTR (keystream = SHA256(key ‖ iv ‖ counter))
 * Auth:   HMAC-SHA256 over the entire header+ciphertext
 */

#ifndef OFFSTORE_H
#define OFFSTORE_H

#include <glib.h>
#include <sqlite3.h>
#include <webkit/webkit.h>

#define OFFSTORE_KEY_SIZE  32
#define OFFSTORE_IV_SIZE   16
#define OFFSTORE_HMAC_SIZE 32

typedef struct {
    gchar  *data_dir;
    gchar  *offline_dir;
    sqlite3 *db;
    guchar   key[OFFSTORE_KEY_SIZE];
    gboolean key_loaded;
} OfflineStore;

typedef struct {
    gchar *url;
    gchar *title;
    gchar *saved_at;
} OfflineEntry;

OfflineStore* offstore_new(const gchar *data_dir);
void          offstore_free(OfflineStore *store);

/* Async: captures outerHTML via JS, encrypts, writes to disk, indexes in DB */
void          offstore_capture(OfflineStore *store, WebKitWebView *view,
                                const gchar *url, const gchar *title);

/* Returns heap-allocated HTML string, or NULL if not in cache */
gchar*        offstore_retrieve(OfflineStore *store, const gchar *url);

gboolean      offstore_has(OfflineStore *store, const gchar *url);

/* Returns GPtrArray of OfflineEntry* (caller frees with g_ptr_array_free) */
GPtrArray*    offstore_list(OfflineStore *store);
void          offline_entry_free(gpointer e);

#endif
