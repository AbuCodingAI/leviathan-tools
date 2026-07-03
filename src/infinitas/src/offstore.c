/**
 * Infinitas Browser - Offline Page Store Implementation
 */

#include "offstore.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define MAGIC   "INFX"
#define VERSION ((guchar)0x01)

/* SQLite schema */
#define SCHEMA \
    "CREATE TABLE IF NOT EXISTS offline_pages(" \
    "  url_hash TEXT PRIMARY KEY," \
    "  url      TEXT NOT NULL," \
    "  title    TEXT," \
    "  saved_at TEXT DEFAULT (datetime('now'))" \
    ");"

/* ── init / teardown ─────────────────────────────────────────────────────── */

OfflineStore* offstore_new(const gchar *data_dir) {
    OfflineStore *s = g_new0(OfflineStore, 1);
    s->data_dir    = g_strdup(data_dir ? data_dir : ".infinitas");
    s->offline_dir = g_build_filename(s->data_dir, "offline", NULL);
    g_mkdir_with_parents(s->offline_dir, 0700);

    gchar *db_path = g_build_filename(s->data_dir, "offline.db", NULL);
    if (sqlite3_open(db_path, &s->db) == SQLITE_OK)
        sqlite3_exec(s->db, SCHEMA, NULL, NULL, NULL);
    g_free(db_path);

    return s;
}

void offstore_free(OfflineStore *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    g_free(s->offline_dir);
    g_free(s->data_dir);
    g_free(s);
}

/* ── key management ──────────────────────────────────────────────────────── */

static gboolean ensure_key(OfflineStore *s) {
    if (s->key_loaded) return TRUE;

    gchar *kp = g_build_filename(s->data_dir, "offline.key", NULL);

    if (g_file_test(kp, G_FILE_TEST_EXISTS)) {
        FILE *f = fopen(kp, "rb");
        if (f) {
            gboolean ok = (fread(s->key, 1, OFFSTORE_KEY_SIZE, f) == OFFSTORE_KEY_SIZE);
            fclose(f);
            if (ok) { s->key_loaded = TRUE; g_free(kp); return TRUE; }
        }
    }

    /* generate a fresh key */
    for (int i = 0; i < OFFSTORE_KEY_SIZE; i += 4) {
        guint32 r = g_random_int();
        memcpy(s->key + i, &r, 4);
    }
    FILE *f = fopen(kp, "wb");
    if (f) {
        fwrite(s->key, 1, OFFSTORE_KEY_SIZE, f);
        fclose(f);
        chmod(kp, 0600);
    }
    g_free(kp);
    s->key_loaded = TRUE;
    return TRUE;
}

/* ── SHA256-CTR stream cipher ────────────────────────────────────────────── */

static void stream_crypt(const guchar *key, const guchar *iv,
                          const guchar *in, guchar *out, gsize len) {
    guchar   block[32];
    gsize    bi = 32; /* force first block gen */
    guint64  ctr = 0;

    for (gsize i = 0; i < len; i++) {
        if (bi == 32) {
            GChecksum *h = g_checksum_new(G_CHECKSUM_SHA256);
            g_checksum_update(h, key, OFFSTORE_KEY_SIZE);
            g_checksum_update(h, iv,  OFFSTORE_IV_SIZE);
            guchar cb[8];
            guint64 c = ctr;
            for (int j = 0; j < 8; j++) { cb[j] = c & 0xFF; c >>= 8; }
            g_checksum_update(h, cb, 8);
            gsize blen = 32;
            g_checksum_get_digest(h, block, &blen);
            g_checksum_free(h);
            ctr++; bi = 0;
        }
        out[i] = in[i] ^ block[bi++];
    }
}

/* ── URL → SHA256 hex (used as filename and DB key) ─────────────────────── */

static gchar* url_hash(const gchar *url) {
    GChecksum *h = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(h, (const guchar*)url, (gssize)strlen(url));
    gchar *hex = g_strdup(g_checksum_get_string(h));
    g_checksum_free(h);
    return hex;
}

static gchar* hash_to_path(OfflineStore *s, const gchar *hash) {
    gchar *fn = g_strdup_printf("%s.infx", hash);
    gchar *p  = g_build_filename(s->offline_dir, fn, NULL);
    g_free(fn);
    return p;
}

/* ── HMAC helper ─────────────────────────────────────────────────────────── */

static void compute_hmac(const guchar *key,
                          const guchar *iv,
                          const guchar *lenbuf,
                          const guchar *cipher, gsize clen,
                          guchar out[OFFSTORE_HMAC_SIZE]) {
    const guchar ver = VERSION;
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, OFFSTORE_KEY_SIZE);
    g_hmac_update(h, (const guchar*)MAGIC,  4);
    g_hmac_update(h, &ver, 1);
    g_hmac_update(h, iv,      OFFSTORE_IV_SIZE);
    g_hmac_update(h, lenbuf,  8);
    g_hmac_update(h, cipher,  clen);
    gsize sz = OFFSTORE_HMAC_SIZE;
    g_hmac_get_digest(h, out, &sz);
    g_hmac_unref(h);
}

/* ── encrypt + write ─────────────────────────────────────────────────────── */

static gboolean write_infx(OfflineStore *s, const gchar *hash,
                             const guchar *plain, gsize len) {
    /* random IV */
    guchar iv[OFFSTORE_IV_SIZE];
    for (int i = 0; i < OFFSTORE_IV_SIZE; i += 4) {
        guint32 r = g_random_int();
        memcpy(iv + i, &r, 4);
    }

    guchar *cipher = g_malloc(len);
    stream_crypt(s->key, iv, plain, cipher, len);

    /* length as 8-byte little-endian */
    guchar lenbuf[8];
    guint64 l = (guint64)len;
    for (int i = 0; i < 8; i++) { lenbuf[i] = l & 0xFF; l >>= 8; }

    guchar mac[OFFSTORE_HMAC_SIZE];
    compute_hmac(s->key, iv, lenbuf, cipher, len, mac);

    const guchar wver = VERSION;
    gchar *path = hash_to_path(s, hash);
    FILE *f = fopen(path, "wb");
    gboolean ok = FALSE;
    if (f) {
        ok  = (fwrite(MAGIC,  1, 4,                  f) == 4);
        ok &= (fwrite(&wver,  1, 1,                  f) == 1);
        ok &= (fwrite(iv,      1, OFFSTORE_IV_SIZE,   f) == (gsize)OFFSTORE_IV_SIZE);
        ok &= (fwrite(lenbuf,  1, 8,                  f) == 8);
        ok &= (fwrite(cipher,  1, len,                f) == len);
        ok &= (fwrite(mac,     1, OFFSTORE_HMAC_SIZE, f) == (gsize)OFFSTORE_HMAC_SIZE);
        fclose(f);
    }
    g_free(path);
    g_free(cipher);
    return ok;
}

/* ── decrypt + verify ────────────────────────────────────────────────────── */

gchar* offstore_retrieve(OfflineStore *s, const gchar *url) {
    if (!s || !url || !ensure_key(s)) return NULL;

    gchar *hash = url_hash(url);
    gchar *path = hash_to_path(s, hash);
    g_free(hash);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) { g_free(path); return NULL; }

    FILE *f = fopen(path, "rb");
    g_free(path);
    if (!f) return NULL;

    guchar magic[4], ver, iv[OFFSTORE_IV_SIZE], lenbuf[8];
    gboolean ok = TRUE;
    ok &= (fread(magic,  1, 4,              f) == 4 && memcmp(magic, MAGIC, 4) == 0);
    ok &= (fread(&ver,   1, 1,              f) == 1 && ver == VERSION);
    ok &= (fread(iv,     1, OFFSTORE_IV_SIZE,  f) == (gsize)OFFSTORE_IV_SIZE);
    ok &= (fread(lenbuf, 1, 8,              f) == 8);
    if (!ok) { fclose(f); return NULL; }

    guint64 orig_len = 0;
    for (int i = 7; i >= 0; i--) orig_len = (orig_len << 8) | lenbuf[i];
    if (orig_len == 0 || orig_len > 64 * 1024 * 1024) { fclose(f); return NULL; }

    guchar *cipher = g_malloc(orig_len);
    if (fread(cipher, 1, orig_len, f) != orig_len) {
        g_free(cipher); fclose(f); return NULL;
    }
    guchar stored_mac[OFFSTORE_HMAC_SIZE];
    if (fread(stored_mac, 1, OFFSTORE_HMAC_SIZE, f) != (gsize)OFFSTORE_HMAC_SIZE) {
        g_free(cipher); fclose(f); return NULL;
    }
    fclose(f);

    /* verify HMAC (constant-time) */
    guchar computed_mac[OFFSTORE_HMAC_SIZE];
    compute_hmac(s->key, iv, lenbuf, cipher, orig_len, computed_mac);
    guchar diff = 0;
    for (int i = 0; i < OFFSTORE_HMAC_SIZE; i++) diff |= stored_mac[i] ^ computed_mac[i];
    if (diff != 0) {
        g_printerr("[OFFSTORE] HMAC mismatch — file tampered or wrong key\n");
        g_free(cipher); return NULL;
    }

    guchar *plain = g_malloc(orig_len + 1);
    stream_crypt(s->key, iv, cipher, plain, orig_len);
    plain[orig_len] = '\0';
    g_free(cipher);

    g_print("[OFFSTORE] Retrieved: %s\n", url);
    return (gchar*)plain;
}

gboolean offstore_has(OfflineStore *s, const gchar *url) {
    if (!s || !url) return FALSE;
    gchar *hash = url_hash(url);
    gchar *path = hash_to_path(s, hash);
    g_free(hash);
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

/* ── SQLite index ────────────────────────────────────────────────────────── */

static void db_upsert(OfflineStore *s, const gchar *hash,
                       const gchar *url, const gchar *title) {
    if (!s->db) return;
    const gchar *sql =
        "INSERT INTO offline_pages(url_hash,url,title,saved_at)"
        " VALUES(?,?,?,datetime('now'))"
        " ON CONFLICT(url_hash) DO UPDATE SET title=excluded.title,"
        "  saved_at=datetime('now');";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hash,  -1, NULL);
        sqlite3_bind_text(stmt, 2, url,   -1, NULL);
        sqlite3_bind_text(stmt, 3, title, -1, NULL);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

GPtrArray* offstore_list(OfflineStore *s) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(offline_entry_free);
    if (!s || !s->db) return arr;

    const gchar *sql =
        "SELECT url, title, saved_at FROM offline_pages ORDER BY saved_at DESC";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OfflineEntry *e = g_new0(OfflineEntry, 1);
            e->url      = g_strdup((const gchar*)sqlite3_column_text(stmt, 0));
            e->title    = g_strdup((const gchar*)sqlite3_column_text(stmt, 1));
            e->saved_at = g_strdup((const gchar*)sqlite3_column_text(stmt, 2));
            g_ptr_array_add(arr, e);
        }
        sqlite3_finalize(stmt);
    }
    return arr;
}

void offline_entry_free(gpointer p) {
    OfflineEntry *e = p;
    if (!e) return;
    g_free(e->url); g_free(e->title); g_free(e->saved_at);
    g_free(e);
}

/* ── async capture ───────────────────────────────────────────────────────── */

typedef struct {
    OfflineStore *store;
    gchar        *url;
    gchar        *title;
} CaptureCtx;

static void on_html_ready(GObject *src, GAsyncResult *res, gpointer user_data) {
    CaptureCtx *ctx = user_data;
    GError *err = NULL;

    JSCValue *val = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(src), res, &err);

    if (err) {
        g_printerr("[OFFSTORE] JS eval failed: %s\n", err->message);
        g_error_free(err);
    } else if (val && jsc_value_is_string(val)) {
        gchar *html = jsc_value_to_string(val);
        if (html && *html) {
            ensure_key(ctx->store);
            gchar *hash = url_hash(ctx->url);
            if (write_infx(ctx->store,
                            hash,
                            (const guchar*)html,
                            strlen(html))) {
                db_upsert(ctx->store, hash, ctx->url, ctx->title);
                g_print("[OFFSTORE] Saved: %s\n", ctx->url);
            }
            g_free(hash);
            g_free(html);
        }
        g_object_unref(val);
    }

    g_free(ctx->url);
    g_free(ctx->title);
    g_free(ctx);
}

void offstore_capture(OfflineStore *s, WebKitWebView *view,
                       const gchar *url, const gchar *title) {
    if (!s || !view || !url || !*url) return;
    /* Don't cache internal or data: pages */
    if (g_str_has_prefix(url, "infinitas://") ||
        g_str_has_prefix(url, "data:") ||
        g_str_has_prefix(url, "about:")) return;

    CaptureCtx *ctx = g_new0(CaptureCtx, 1);
    ctx->store = s;
    ctx->url   = g_strdup(url);
    ctx->title = g_strdup(title ? title : url);

    webkit_web_view_evaluate_javascript(
        view,
        "document.documentElement.outerHTML",
        -1, NULL, NULL, NULL,
        on_html_ready, ctx);
}
