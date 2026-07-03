/**
 * Infinitas Browser - Password Manager Header
 *
 * A small, safe password manager. Credentials are stored ENCRYPTED on disk
 * (SHA256-CTR stream cipher + HMAC-SHA256, same scheme as the offline store)
 * under ~/.infinitas/ — never in plaintext. Credit-card data is intentionally
 * refused and never stored (see passwords_is_card_number + the injected JS).
 */

#ifndef PASSWORDS_H
#define PASSWORDS_H

#include <glib.h>
#include <webkit/webkit.h>

typedef struct PasswordStore PasswordStore;

typedef struct {
    gint   id;        /* stable row id (used by the manager UI for deletion) */
    gchar *origin;    /* e.g. https://accounts.google.com */
    gchar *username;
} PasswordEntry;

PasswordStore* passwords_new(const gchar *data_dir);
void           passwords_free(PasswordStore *s);

/* Save (or update) the credential for an origin. Refuses card-like input. */
gboolean passwords_save(PasswordStore *s, const gchar *origin,
                        const gchar *username, const gchar *password);

/* Best (most recent) credential for an origin. The out-params username_out and
 * password_out are newly-allocated on success; caller frees. FALSE if none. */
gboolean passwords_get_for_origin(PasswordStore *s, const gchar *origin,
                                  gchar **username_out, gchar **password_out);

/* Decrypted password for an exact (origin, username), or NULL. Caller frees. */
gchar* passwords_get_password(PasswordStore *s, const gchar *origin,
                              const gchar *username);

/* List all stored (origin, username) pairs — NEVER exposes passwords. */
GPtrArray* passwords_list(PasswordStore *s);   /* of PasswordEntry*, free w/ passwords_entry_free */
void       passwords_entry_free(gpointer p);

gboolean passwords_delete_by_id(PasswordStore *s, gint id);

/* HARD CONSTRAINT helper: TRUE if the (digit-normalised) text looks like a
 * credit-card number — 13..19 digits and Luhn-valid. Used to block card saves. */
gboolean passwords_is_card_number(const gchar *text);

/* Wire the detector/autofill content script + native message handler onto a
 * WebView. Called once per tab. `browser` is an InfinitasBrowser* (opaque here
 * to avoid a circular include) used only for the save-prompt UI. */
void passwords_attach(PasswordStore *s, WebKitWebView *view,
                      WebKitUserContentManager *mgr, gpointer browser);

#endif
