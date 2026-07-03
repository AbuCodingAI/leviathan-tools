/**
 * Infinitas Browser - Password Manager Implementation
 *
 * Storage: credentials are encrypted with a per-profile 256-bit key using a
 * SHA256-CTR stream cipher authenticated with HMAC-SHA256 (the same proven
 * scheme used by offstore.c), then indexed in SQLite at
 * ~/.infinitas/passwords.db. Passwords are NEVER written in plaintext.
 *
 * Why encrypted local store and not libsecret: libsecret's runtime library is
 * present on Leviathan, but the -dev package (headers + libsecret-1.pc) is not
 * installed in this build environment, so linking against it via pkg-config
 * would break the clean build. The encrypted store keeps passwords off-disk in
 * cleartext while adding no new build dependency.
 *
 * HARD CONSTRAINT: credit-card data is refused. The injected JS flags card
 * fields (autocomplete="cc-*", names/ids containing card/credit/cvv/cvc, or
 * Luhn-valid 13-19 digit values) and this file re-checks with
 * passwords_is_card_number() before ever storing anything.
 */

#include "passwords.h"
#include "browser.h"
#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define PW_KEY_SIZE  32
#define PW_IV_SIZE   16
#define PW_MAC_SIZE  32
#define PW_MAGIC     "INFP"
#define PW_VERSION   ((guchar)0x01)

struct PasswordStore {
    gchar   *data_dir;
    sqlite3 *db;
    guchar   key[PW_KEY_SIZE];
    gboolean key_loaded;
};

#define PW_SCHEMA \
    "CREATE TABLE IF NOT EXISTS credentials(" \
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  origin TEXT NOT NULL," \
    "  username TEXT NOT NULL," \
    "  iv BLOB NOT NULL," \
    "  cipher BLOB NOT NULL," \
    "  mac BLOB NOT NULL," \
    "  updated_at TEXT DEFAULT (datetime('now'))," \
    "  UNIQUE(origin, username)" \
    ");"

/* ── init / teardown ─────────────────────────────────────────────────────── */

PasswordStore* passwords_new(const gchar *data_dir) {
    PasswordStore *s = g_new0(PasswordStore, 1);
    s->data_dir = g_strdup(data_dir ? data_dir : ".infinitas");
    g_mkdir_with_parents(s->data_dir, 0700);

    gchar *db_path = g_build_filename(s->data_dir, "passwords.db", NULL);
    if (sqlite3_open(db_path, &s->db) == SQLITE_OK)
        sqlite3_exec(s->db, PW_SCHEMA, NULL, NULL, NULL);
    g_free(db_path);
    return s;
}

void passwords_free(PasswordStore *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    g_free(s->data_dir);
    g_free(s);
}

/* ── key management ──────────────────────────────────────────────────────── */

static gboolean ensure_key(PasswordStore *s) {
    if (s->key_loaded) return TRUE;
    gchar *kp = g_build_filename(s->data_dir, "passwords.key", NULL);

    if (g_file_test(kp, G_FILE_TEST_EXISTS)) {
        FILE *f = fopen(kp, "rb");
        if (f) {
            gboolean ok = (fread(s->key, 1, PW_KEY_SIZE, f) == PW_KEY_SIZE);
            fclose(f);
            if (ok) { s->key_loaded = TRUE; g_free(kp); return TRUE; }
        }
    }
    for (int i = 0; i < PW_KEY_SIZE; i += 4) {
        guint32 r = g_random_int();
        memcpy(s->key + i, &r, 4);
    }
    FILE *f = fopen(kp, "wb");
    if (f) {
        fwrite(s->key, 1, PW_KEY_SIZE, f);
        fclose(f);
        chmod(kp, 0600);
    }
    g_free(kp);
    s->key_loaded = TRUE;
    return TRUE;
}

/* ── SHA256-CTR stream cipher + HMAC (mirrors offstore.c) ────────────────── */

static void stream_crypt(const guchar *key, const guchar *iv,
                          const guchar *in, guchar *out, gsize len) {
    guchar  block[32];
    gsize   bi = 32;
    guint64 ctr = 0;
    for (gsize i = 0; i < len; i++) {
        if (bi == 32) {
            GChecksum *h = g_checksum_new(G_CHECKSUM_SHA256);
            g_checksum_update(h, key, PW_KEY_SIZE);
            g_checksum_update(h, iv,  PW_IV_SIZE);
            guchar cb[8]; guint64 c = ctr;
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

static void compute_hmac(const guchar *key, const guchar *iv,
                          const guchar *lenbuf, const guchar *cipher, gsize clen,
                          guchar out[PW_MAC_SIZE]) {
    const guchar ver = PW_VERSION;
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, PW_KEY_SIZE);
    g_hmac_update(h, (const guchar*)PW_MAGIC, 4);
    g_hmac_update(h, &ver, 1);
    g_hmac_update(h, iv, PW_IV_SIZE);
    g_hmac_update(h, lenbuf, 8);
    g_hmac_update(h, cipher, clen);
    gsize sz = PW_MAC_SIZE;
    g_hmac_get_digest(h, out, &sz);
    g_hmac_unref(h);
}

/* ── credit-card detection (Luhn) ────────────────────────────────────────── */

gboolean passwords_is_card_number(const gchar *text) {
    if (!text || !*text) return FALSE;
    char digits[40];
    int n = 0, other = 0;
    for (const char *p = text; *p; p++) {
        if (g_ascii_isdigit(*p)) { if (n < 39) digits[n++] = *p; }
        else if (*p == ' ' || *p == '-') continue;
        else other++;
    }
    if (other > 0) return FALSE;          /* letters/symbols → not a bare card # */
    digits[n] = '\0';
    if (n < 13 || n > 19) return FALSE;
    int sum = 0, alt = 0;
    for (int i = n - 1; i >= 0; i--) {
        int d = digits[i] - '0';
        if (alt) { d *= 2; if (d > 9) d -= 9; }
        sum += d; alt = !alt;
    }
    return (sum % 10) == 0;
}

/* ── save / lookup ───────────────────────────────────────────────────────── */

gboolean passwords_save(PasswordStore *s, const gchar *origin,
                        const gchar *username, const gchar *password) {
    if (!s || !s->db || !origin || !*origin || !password || !*password) return FALSE;
    /* Defence in depth: never store card-like data, regardless of the field. */
    if (passwords_is_card_number(password) ||
        (username && passwords_is_card_number(username))) return FALSE;
    if (!ensure_key(s)) return FALSE;
    if (!username) username = "";

    gsize len = strlen(password);
    guchar iv[PW_IV_SIZE];
    for (int i = 0; i < PW_IV_SIZE; i += 4) {
        guint32 r = g_random_int();
        memcpy(iv + i, &r, 4);
    }
    guchar *cipher = g_malloc(len ? len : 1);
    stream_crypt(s->key, iv, (const guchar*)password, cipher, len);

    guchar lenbuf[8]; guint64 l = (guint64)len;
    for (int i = 0; i < 8; i++) { lenbuf[i] = l & 0xFF; l >>= 8; }
    guchar mac[PW_MAC_SIZE];
    compute_hmac(s->key, iv, lenbuf, cipher, len, mac);

    const gchar *sql =
        "INSERT INTO credentials(origin,username,iv,cipher,mac,updated_at)"
        " VALUES(?,?,?,?,?,datetime('now'))"
        " ON CONFLICT(origin,username) DO UPDATE SET"
        "  iv=excluded.iv, cipher=excluded.cipher, mac=excluded.mac,"
        "  updated_at=datetime('now');";
    sqlite3_stmt *st;
    gboolean ok = FALSE;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, origin,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, username, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, iv,     PW_IV_SIZE,  SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 4, cipher, (int)len,    SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 5, mac,    PW_MAC_SIZE,  SQLITE_TRANSIENT);
        ok = (sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
    }
    g_free(cipher);
    return ok;
}

static gchar* decrypt_row(PasswordStore *s, const guchar *iv,
                          const guchar *cipher, gsize clen, const guchar *mac) {
    guchar lenbuf[8]; guint64 l = (guint64)clen;
    for (int i = 0; i < 8; i++) { lenbuf[i] = l & 0xFF; l >>= 8; }
    guchar want[PW_MAC_SIZE];
    compute_hmac(s->key, iv, lenbuf, cipher, clen, want);
    guchar diff = 0;
    for (int i = 0; i < PW_MAC_SIZE; i++) diff |= want[i] ^ mac[i];
    if (diff != 0) return NULL;           /* tampered or wrong key */
    guchar *plain = g_malloc(clen + 1);
    stream_crypt(s->key, iv, cipher, plain, clen);
    plain[clen] = '\0';
    return (gchar*)plain;
}

gboolean passwords_get_for_origin(PasswordStore *s, const gchar *origin,
                                  gchar **username_out, gchar **password_out) {
    if (username_out) *username_out = NULL;
    if (password_out) *password_out = NULL;
    if (!s || !s->db || !origin || !ensure_key(s)) return FALSE;

    const gchar *sql =
        "SELECT username,iv,cipher,mac FROM credentials WHERE origin=?"
        " ORDER BY updated_at DESC LIMIT 1;";
    sqlite3_stmt *st;
    gboolean got = FALSE;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, origin, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const gchar  *uname  = (const gchar*)sqlite3_column_text(st, 0);
            const guchar *iv     = sqlite3_column_blob(st, 1);
            const guchar *cipher = sqlite3_column_blob(st, 2);
            gsize         clen   = (gsize)sqlite3_column_bytes(st, 2);
            const guchar *mac    = sqlite3_column_blob(st, 3);
            if (iv && cipher && mac &&
                sqlite3_column_bytes(st, 1) == PW_IV_SIZE &&
                sqlite3_column_bytes(st, 3) == PW_MAC_SIZE) {
                gchar *plain = decrypt_row(s, iv, cipher, clen, mac);
                if (plain) {
                    if (username_out) *username_out = g_strdup(uname ? uname : "");
                    if (password_out) *password_out = plain; else g_free(plain);
                    got = TRUE;
                }
            }
        }
        sqlite3_finalize(st);
    }
    return got;
}

gchar* passwords_get_password(PasswordStore *s, const gchar *origin,
                              const gchar *username) {
    if (!s || !s->db || !origin || !ensure_key(s)) return NULL;
    if (!username) username = "";
    const gchar *sql =
        "SELECT iv,cipher,mac FROM credentials WHERE origin=? AND username=?"
        " LIMIT 1;";
    sqlite3_stmt *st;
    gchar *out = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, origin,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, username, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const guchar *iv     = sqlite3_column_blob(st, 0);
            const guchar *cipher = sqlite3_column_blob(st, 1);
            gsize         clen   = (gsize)sqlite3_column_bytes(st, 1);
            const guchar *mac    = sqlite3_column_blob(st, 2);
            if (iv && cipher && mac &&
                sqlite3_column_bytes(st, 0) == PW_IV_SIZE &&
                sqlite3_column_bytes(st, 2) == PW_MAC_SIZE)
                out = decrypt_row(s, iv, cipher, clen, mac);
        }
        sqlite3_finalize(st);
    }
    return out;
}

/* ── manager-view listing / deletion ─────────────────────────────────────── */

void passwords_entry_free(gpointer p) {
    PasswordEntry *e = p;
    if (!e) return;
    g_free(e->origin); g_free(e->username);
    g_free(e);
}

GPtrArray* passwords_list(PasswordStore *s) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(passwords_entry_free);
    if (!s || !s->db) return arr;
    const gchar *sql =
        "SELECT id,origin,username FROM credentials ORDER BY origin,username;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            PasswordEntry *e = g_new0(PasswordEntry, 1);
            e->id       = sqlite3_column_int(st, 0);
            e->origin   = g_strdup((const gchar*)sqlite3_column_text(st, 1));
            e->username = g_strdup((const gchar*)sqlite3_column_text(st, 2));
            g_ptr_array_add(arr, e);
        }
        sqlite3_finalize(st);
    }
    return arr;
}

gboolean passwords_delete_by_id(PasswordStore *s, gint id) {
    if (!s || !s->db) return FALSE;
    sqlite3_stmt *st;
    gboolean ok = FALSE;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM credentials WHERE id=?;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        ok = (sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
    }
    return ok;
}

/* ── WebKit integration: content script + native handler ─────────────────── */

/* Injected into every top http/https frame. Detects login forms, autofills on
 * return, and offers to save on submit — while flagging card fields so the
 * native side can refuse them. No '%' chars: passed to WebKit verbatim. */
static const gchar PW_CONTENT_SCRIPT[] =
    "(function(){"
    "if(location.protocol!=='http:'&&location.protocol!=='https:')return;"
    "if(window.__infPwInit)return;window.__infPwInit=1;"
    "function luhn(s){var sum=0,alt=false;for(var i=s.length-1;i>=0;i--){var d=+s[i];"
    "if(alt){d*=2;if(d>9)d-=9;}sum+=d;alt=!alt;}return sum%10===0;}"
    "function looksCard(v){v=(v||'').replace(/[ -]/g,'');return /^[0-9]{13,19}$/.test(v)&&luhn(v);}"
    "function isCardField(el){"
    "var a=((el.getAttribute&&el.getAttribute('autocomplete'))||'').toLowerCase();"
    "if(a.indexOf('cc-')===0||a==='cc-number'||a==='cc-csc'||/(^|\\s)cc-/.test(a))return true;"
    "var n=((el.name||'')+' '+(el.id||'')+' '+((el.getAttribute&&el.getAttribute('aria-label'))||'')).toLowerCase();"
    "if(/card|credit|cvv|cvc|ccnum|cardnum|creditcard/.test(n))return true;"
    "if(looksCard(el.value))return true;return false;}"
    "function pwFields(root){return root.querySelectorAll('input[type=password]');}"
    "function userFor(pw){var form=pw.form||document;var ins=form.querySelectorAll('input');var best=null;"
    "for(var i=0;i<ins.length;i++){var el=ins[i];var t=(el.type||'text').toLowerCase();"
    "if(t==='password'||t==='hidden'||t==='submit'||t==='button'||t==='checkbox'||t==='radio')continue;"
    "var ac=((el.getAttribute('autocomplete'))||'').toLowerCase();"
    "if(ac==='username'||ac==='email'||t==='email')return el;"
    "if(t==='text'||t==='tel')best=el;}return best;}"
    "function fire(el){try{el.dispatchEvent(new Event('input',{bubbles:true}));"
    "el.dispatchEvent(new Event('change',{bubbles:true}));}catch(e){}}"
    "window.__infPwFill=function(u,p){try{var pws=pwFields(document);"
    "for(var i=0;i<pws.length;i++){var pw=pws[i];if(p&&!pw.value){pw.value=p;fire(pw);}"
    "var uf=userFor(pw);if(u&&uf&&!uf.value){uf.value=u;fire(uf);}}}catch(e){}};"
    "function send(o){try{window.webkit.messageHandlers.infinitas_pw.postMessage(o);}catch(e){}}"
    "function requestFill(){send({t:'query',origin:location.origin});}"
    "function collect(scope){var pws=pwFields(scope||document);if(!pws.length)return;"
    "var pw=pws[0];var pass=pw.value;if(!pass)return;var uf=userFor(pw);var uname=uf?uf.value:'';"
    "var card=false;var form=pw.form;var ins=form?form.querySelectorAll('input'):[pw];"
    "for(var i=0;i<ins.length;i++){if(isCardField(ins[i])){card=true;break;}}"
    "if(looksCard(uname))card=true;"
    "send({t:'save',origin:location.origin,username:uname||'',password:pass,card:card});}"
    "document.addEventListener('submit',function(e){if(e.target&&e.target.tagName==='FORM')collect(e.target);},true);"
    "document.addEventListener('click',function(e){var t=e.target;if(!t)return;"
    "var ty=(t.type||'').toLowerCase();var tg=(t.tagName||'').toLowerCase();"
    "if(ty==='submit'||(tg==='button'&&ty!=='button')||/(log ?in|sign ?in|submit)/i.test(t.textContent||''))"
    "setTimeout(function(){collect(document);},60);},true);"
    "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',requestFill);else requestFill();"
    "setTimeout(requestFill,1500);"
    "})();";

typedef struct {
    PasswordStore *store;
    gpointer       browser;    /* InfinitasBrowser* */
    WebKitWebView *view;
} PwCtx;

/* pending save the user must approve */
typedef struct {
    PasswordStore *store;
    gchar *origin;
    gchar *username;
    gchar *password;
} SaveReq;

static void save_req_free(gpointer p) {
    SaveReq *r = p;
    if (!r) return;
    if (r->password) { memset(r->password, 0, strlen(r->password)); }
    g_free(r->origin); g_free(r->username); g_free(r->password);
    g_free(r);
}

static gchar* jv_str(JSCValue *o, const char *k) {
    JSCValue *v = jsc_value_object_get_property(o, k);
    gchar *r = NULL;
    if (v && jsc_value_is_string(v)) r = jsc_value_to_string(v);
    if (v) g_object_unref(v);
    return r;
}
static gboolean jv_bool(JSCValue *o, const char *k) {
    JSCValue *v = jsc_value_object_get_property(o, k);
    gboolean r = FALSE;
    if (v) { r = jsc_value_to_boolean(v); g_object_unref(v); }
    return r;
}

static gchar* js_quote(const char *s) {
    GString *o = g_string_new("\"");
    for (const char *p = s ? s : ""; *p; p++) {
        guchar c = (guchar)*p;
        switch (c) {
            case '\\': g_string_append(o, "\\\\"); break;
            case '"':  g_string_append(o, "\\\""); break;
            case '\n': g_string_append(o, "\\n"); break;
            case '\r': g_string_append(o, "\\r"); break;
            case '\t': g_string_append(o, "\\t"); break;
            case '<':  g_string_append(o, "\\x3C"); break;
            default:
                if (c < 0x20) g_string_append_printf(o, "\\u%04x", c);
                else          g_string_append_c(o, (gchar)c);
        }
    }
    g_string_append_c(o, '"');
    return g_string_free(o, FALSE);
}

/* Build a bare host label for the prompt, e.g. "accounts.google.com". */
static gchar* origin_host(const gchar *origin) {
    GUri *u = g_uri_parse(origin, G_URI_FLAGS_NONE, NULL);
    gchar *h = NULL;
    if (u) { const char *host = g_uri_get_host(u); if (host) h = g_strdup(host); g_uri_unref(u); }
    return (h && *h) ? h : g_strdup(origin ? origin : "this site");
}

static GtkWindow* browser_window(gpointer browser) {
    InfinitasBrowser *b = browser;
    return (b && b->window) ? GTK_WINDOW(b->window) : NULL;
}

/* ── save prompt ─────────────────────────────────────────────────────────── */

static void save_yes_cb(GtkButton *btn, gpointer data) {
    SaveReq *r = data;
    passwords_save(r->store, r->origin, r->username, r->password);
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}
static void save_no_cb(GtkButton *btn, gpointer data) {
    (void)data;
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}

static void show_save_prompt(gpointer browser, PasswordStore *store,
                             const gchar *origin, const gchar *username,
                             const gchar *password) {
    /* Skip if we already store this exact credential unchanged. */
    gchar *existing = passwords_get_password(store, origin, username);
    if (existing && g_strcmp0(existing, password) == 0) { g_free(existing); return; }
    g_free(existing);

    SaveReq *req = g_new0(SaveReq, 1);
    req->store = store;
    req->origin = g_strdup(origin);
    req->username = g_strdup(username ? username : "");
    req->password = g_strdup(password);

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Save password");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    GtkWindow *parent = browser_window(browser);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    g_object_set_data_full(G_OBJECT(win), "inf-save-req", req, save_req_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    gchar *host = origin_host(origin);
    gchar *euser = g_markup_escape_text(req->username, -1);
    gchar *ehost = g_markup_escape_text(host, -1);
    gchar *msg = g_strdup_printf(
        "<b>Save password for %s?</b>\n<span size='small'>%s</span>",
        ehost, (req->username && *req->username) ? euser : "(no username)");
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), msg);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), label);
    g_free(msg); g_free(euser); g_free(ehost); g_free(host);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *no  = gtk_button_new_with_label("Not now");
    GtkWidget *yes = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(yes, "suggested-action");
    g_signal_connect(no,  "clicked", G_CALLBACK(save_no_cb),  req);
    g_signal_connect(yes, "clicked", G_CALLBACK(save_yes_cb), req);
    gtk_box_append(GTK_BOX(btns), no);
    gtk_box_append(GTK_BOX(btns), yes);
    gtk_box_append(GTK_BOX(box), btns);

    gtk_window_present(GTK_WINDOW(win));
}

/* ── "card storage disabled" note ────────────────────────────────────────── */

/* The window's "destroy" handler cancels this timeout, so if auto_close ever
 * runs the window is guaranteed still alive (no use-after-free). */
static gboolean auto_close(gpointer win) {
    g_object_set_data(G_OBJECT(win), "inf-note-timeout", GUINT_TO_POINTER(0));
    gtk_window_destroy(GTK_WINDOW(win));
    return G_SOURCE_REMOVE;
}
static void note_destroy_cb(GtkWidget *w, gpointer data) {
    (void)data;
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "inf-note-timeout"));
    if (id) g_source_remove(id);
}
static void note_ok_cb(GtkButton *btn, gpointer data) {
    (void)data;
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}

static void show_card_note(gpointer browser) {
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Card data not saved");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    GtkWindow *parent = browser_window(browser);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label),
        "<b>Infinitas won\xE2\x80\x99t save card data.</b>\n"
        "<span size='small'>Card storage is intentionally disabled \xE2\x80\x94 "
        "only usernames and passwords are saved.</span>");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *ok = gtk_button_new_with_label("Got it");
    gtk_widget_set_halign(ok, GTK_ALIGN_END);
    g_signal_connect(ok, "clicked", G_CALLBACK(note_ok_cb), NULL);
    gtk_box_append(GTK_BOX(box), ok);

    gtk_window_present(GTK_WINDOW(win));
    guint tid = g_timeout_add_seconds(6, auto_close, win);   /* self-dismiss */
    g_object_set_data(G_OBJECT(win), "inf-note-timeout", GUINT_TO_POINTER(tid));
    g_signal_connect(win, "destroy", G_CALLBACK(note_destroy_cb), NULL);
}

/* ── native message handler ──────────────────────────────────────────────── */

static void on_pw_msg(WebKitUserContentManager *m, JSCValue *val, gpointer user) {
    (void)m;
    PwCtx *ctx = user;
    if (!ctx || !val || !jsc_value_is_object(val)) return;

    gchar *t = jv_str(val, "t");
    if (!t) return;

    if (g_strcmp0(t, "query") == 0) {
        gchar *origin = jv_str(val, "origin");
        gchar *u = NULL, *p = NULL;
        if (origin && passwords_get_for_origin(ctx->store, origin, &u, &p)) {
            gchar *qu = js_quote(u ? u : "");
            gchar *qp = js_quote(p ? p : "");
            gchar *js = g_strdup_printf(
                "if(window.__infPwFill)window.__infPwFill(%s,%s);", qu, qp);
            webkit_web_view_evaluate_javascript(ctx->view, js, -1,
                                                NULL, NULL, NULL, NULL, NULL);
            g_free(js); g_free(qu); g_free(qp);
        }
        g_free(u); g_free(p); g_free(origin);

    } else if (g_strcmp0(t, "save") == 0) {
        gchar *origin = jv_str(val, "origin");
        gchar *uname  = jv_str(val, "username");
        gchar *pass   = jv_str(val, "password");
        gboolean card = jv_bool(val, "card");
        if (!card && pass && passwords_is_card_number(pass))  card = TRUE;
        if (!card && uname && passwords_is_card_number(uname)) card = TRUE;

        if (card) {
            show_card_note(ctx->browser);                 /* refuse, briefly explain */
        } else if (origin && pass && *pass) {
            show_save_prompt(ctx->browser, ctx->store, origin, uname, pass);
        }
        g_free(origin); g_free(uname); g_free(pass);
    }
    g_free(t);
}

void passwords_attach(PasswordStore *s, WebKitWebView *view,
                      WebKitUserContentManager *mgr, gpointer browser) {
    if (!s || !view || !mgr) return;

    PwCtx *ctx = g_new0(PwCtx, 1);
    ctx->store = s;
    ctx->browser = browser;
    ctx->view = view;
    g_object_set_data_full(G_OBJECT(view), "inf-pw-ctx", ctx, g_free);

    webkit_user_content_manager_register_script_message_handler(mgr, "infinitas_pw", NULL);
    g_signal_connect(mgr, "script-message-received::infinitas_pw",
                     G_CALLBACK(on_pw_msg), ctx);

    WebKitUserScript *script = webkit_user_script_new(
        PW_CONTENT_SCRIPT,
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(mgr, script);
    webkit_user_script_unref(script);
}
