/**
 * Infinitas Browser - EasyList -> WebKit content-blocker converter ("EzList")
 *
 * Turns EasyList / Adblock Plus filter syntax into the JSON rule format that
 * WebKit (and Safari) compile into a native WebKitUserContentFilter — the same
 * format used by src/infinitas/data/adblock-rules.json.  This runs entirely
 * offline: no network fetch.  It is used both by the build-time tool
 * tools/ezlist2json and (optionally) linked into the browser itself.
 *
 * Supported rule classes:
 *   - network block rules:      ||domain^  /path  banner_ad  *foo*  (+ options)
 *   - exceptions:               @@||domain^        -> ignore-previous-rules
 *   - element hiding:           domains##.selector -> css-display-none
 * Honestly skipped (counted + logged, never silently dropped):
 *   - cosmetic exceptions       #@#
 *   - extended/scriptlet rules  #?#  #$#  ##+js(  :has() :-abp- :style() ...
 *   - rules using options WebKit can't express (redirect, csp, removeparam ...)
 */

#ifndef EZLIST_H
#define EZLIST_H

#include <glib.h>

typedef struct {
    guint total_lines;               /* every line seen                     */
    guint comments;                  /* ! ...  and [Adblock ...] headers    */
    guint blank;                     /* empty lines                         */
    guint network_block;             /* emitted block rules                 */
    guint network_exception;         /* emitted ignore-previous-rules       */
    guint element_hide;              /* emitted css-display-none            */
    guint skipped_cosmetic_exception;/* #@#                                 */
    guint skipped_extended;          /* #?# #$# +js :has() ...              */
    guint skipped_unsupported_option;/* redirect/csp/removeparam/...        */
    guint skipped_unsafe;            /* would match everything -> refused   */
    guint dropped_over_cap;          /* exceeded WebKit rule cap            */
    guint emitted;                   /* total rules written to JSON         */
} EzlistStats;

/* WebKit refuses very large content filters; this is the historical cap. */
#define EZLIST_DEFAULT_MAX_RULES 50000u

/* Convert EasyList text -> WebKit content-blocker JSON (a JSON array string).
 * max_rules: cap on emitted rules (0 => EZLIST_DEFAULT_MAX_RULES).
 * stats: optional; zeroed then filled in.
 * Returns a newly-allocated, always-valid JSON string (caller frees). */
gchar *ezlist_convert(const gchar *text, guint max_rules, EzlistStats *stats);

/* Read in_path, convert, write out_path.  TRUE on success. */
gboolean ezlist_convert_file(const gchar *in_path, const gchar *out_path,
                             guint max_rules, EzlistStats *stats, GError **error);

/* Human-readable one-block summary of stats (caller frees). */
gchar *ezlist_stats_to_string(const EzlistStats *stats);

#endif
