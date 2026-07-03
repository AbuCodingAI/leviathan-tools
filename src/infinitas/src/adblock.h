/**
 * Infinitas Browser - Native ad/tracker blocker (WebKit content filters)
 *
 * Compiles a JSON blocklist (WebKit content-blocker format) into a native
 * filter that WebKit enforces in the network layer — faster than JS blockers.
 */

#ifndef ADBLOCK_H
#define ADBLOCK_H

#include <webkit/webkit.h>

/* Kick off (async) compilation of the blocklist. Call once at startup.
 * rules_path: path to adblock-rules.json. Safe to call with a missing file
 * (blocking is simply disabled). */
void adblock_init(const gchar *rules_path);

/* Enable/disable blocking at runtime (persists for the session). */
void adblock_set_enabled(gboolean enabled);
gboolean adblock_is_enabled(void);

/* Register a tab's content manager so the filter applies to it. Handles the
 * case where compilation hasn't finished yet (applies once it's ready). */
void adblock_register_manager(WebKitUserContentManager *manager);

#endif
