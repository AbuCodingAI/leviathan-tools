/**
 * Infinitas Browser - EasyList -> WebKit content-blocker converter ("EzList")
 * See ezlist.h for the supported/unsupported rule matrix.  glib-only, no WebKit.
 */

#include "ezlist.h"
#include <string.h>

/* ── JSON helpers ────────────────────────────────────────────────────────── */

static void json_escape_append(GString *o, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  g_string_append(o, "\\\""); break;
            case '\\': g_string_append(o, "\\\\"); break;
            case '\n': g_string_append(o, "\\n");  break;
            case '\r': g_string_append(o, "\\r");  break;
            case '\t': g_string_append(o, "\\t");  break;
            default:
                if (c < 0x20) g_string_append_printf(o, "\\u%04x", c);
                else          g_string_append_c(o, (char)c);
        }
    }
}

/* Append a JSON array of strings: ["a","b",...] */
static void json_str_array(GString *o, GPtrArray *items) {
    g_string_append_c(o, '[');
    for (guint i = 0; i < items->len; i++) {
        if (i) g_string_append_c(o, ',');
        g_string_append_c(o, '"');
        json_escape_append(o, g_ptr_array_index(items, i));
        g_string_append_c(o, '"');
    }
    g_string_append_c(o, ']');
}

/* ── pattern -> url-filter regex ─────────────────────────────────────────── */
/* Produces a regex from the WebKit content-blocker supported subset only:
 * ^ $ . * ? + ( ) [ ] [^ ] \x — no {m,n}, no backreferences. */
static gchar *pattern_to_regex(const char *p) {
    GString *o = g_string_new(NULL);
    size_t len = strlen(p);
    size_t i = 0, end = len;
    gboolean anchor_end = FALSE;

    if (len >= 2 && p[0] == '|' && p[1] == '|') {
        /* domain anchor: scheme + optional subdomains */
        g_string_append(o, "^https?://([^/]*\\.)?");
        i = 2;
    } else if (len >= 1 && p[0] == '|') {
        g_string_append(o, "^");
        i = 1;
    }
    if (end > i && p[end - 1] == '|') { anchor_end = TRUE; end--; }

    for (; i < end; i++) {
        char c = p[i];
        if (c == '*') {
            g_string_append(o, ".*");
        } else if (c == '^') {
            /* EasyList separator: anything that isn't a URL "word" char */
            g_string_append(o, "[^a-zA-Z0-9_.%-]");
        } else if (c == '.' || c == '?' || c == '+' || c == '(' || c == ')' ||
                   c == '{' || c == '}' || c == '[' || c == ']' || c == '|' ||
                   c == '\\' || c == '$' || c == '/') {
            g_string_append_c(o, '\\');
            g_string_append_c(o, c);
        } else {
            g_string_append_c(o, c);
        }
    }
    if (anchor_end) g_string_append_c(o, '$');
    if (o->len == 0) g_string_append(o, ".*");
    return g_string_free(o, FALSE);
}

/* ── options ─────────────────────────────────────────────────────────────── */

typedef struct {
    GPtrArray *if_domains;      /* char* like "*example.com"   */
    GPtrArray *unless_domains;
    GPtrArray *resource_types;  /* char* unique WebKit types   */
    const char *load_type;      /* "third-party"/"first-party"/NULL */
    gboolean case_sensitive;
    gboolean unsupported;       /* option WebKit can't express -> skip rule */
} RuleOpts;

static void opts_init(RuleOpts *o) {
    o->if_domains     = g_ptr_array_new_with_free_func(g_free);
    o->unless_domains = g_ptr_array_new_with_free_func(g_free);
    o->resource_types = g_ptr_array_new_with_free_func(g_free);
    o->load_type      = NULL;
    o->case_sensitive = FALSE;
    o->unsupported    = FALSE;
}
static void opts_clear(RuleOpts *o) {
    g_ptr_array_free(o->if_domains, TRUE);
    g_ptr_array_free(o->unless_domains, TRUE);
    g_ptr_array_free(o->resource_types, TRUE);
}

static void restype_add(GPtrArray *arr, const char *t) {
    for (guint i = 0; i < arr->len; i++)
        if (g_strcmp0(g_ptr_array_index(arr, i), t) == 0) return;
    g_ptr_array_add(arr, g_strdup(t));
}

/* EasyList content type -> WebKit resource-type, or NULL if not a type token */
static const char *map_restype(const char *t) {
    if (!strcmp(t, "script"))            return "script";
    if (!strcmp(t, "image"))             return "image";
    if (!strcmp(t, "stylesheet"))        return "style-sheet";
    if (!strcmp(t, "css"))               return "style-sheet";
    if (!strcmp(t, "font"))              return "font";
    if (!strcmp(t, "media"))             return "media";
    if (!strcmp(t, "xmlhttprequest"))    return "raw";
    if (!strcmp(t, "xhr"))               return "raw";
    if (!strcmp(t, "websocket"))         return "raw";
    if (!strcmp(t, "ping"))              return "raw";
    if (!strcmp(t, "beacon"))            return "raw";
    if (!strcmp(t, "other"))             return "raw";
    if (!strcmp(t, "object"))            return "raw";
    if (!strcmp(t, "object-subrequest")) return "raw";
    if (!strcmp(t, "subdocument"))       return "document";
    if (!strcmp(t, "document"))          return "document";
    return NULL;
}

/* Options that change a rule into something other than a plain block/hide and
 * therefore can't be honestly represented — skip the whole rule. */
static gboolean is_skip_option(const char *t) {
    static const char *skip[] = {
        "redirect", "redirect-rule", "csp", "removeparam", "rewrite",
        "replace", "empty", "mp4", "badfilter", "cookie", "permissions",
        "header", "referrerpolicy", "removeheader", "hls", "jsonprune",
        "stealth", "urltransform", NULL
    };
    for (int i = 0; skip[i]; i++) if (!strcmp(t, skip[i])) return TRUE;
    return FALSE;
}

/* Parse the "$...options" portion (after the '$'). */
static void parse_options(RuleOpts *o, const char *optstr) {
    gchar **toks = g_strsplit(optstr, ",", -1);
    for (int i = 0; toks[i]; i++) {
        char *tok = g_strstrip(toks[i]);
        if (!*tok) continue;

        if (g_str_has_prefix(tok, "domain=")) {
            gchar **ds = g_strsplit(tok + 7, "|", -1);
            for (int j = 0; ds[j]; j++) {
                char *d = g_strstrip(ds[j]);
                if (!*d) continue;
                if (*d == '~')
                    g_ptr_array_add(o->unless_domains, g_strconcat("*", d + 1, NULL));
                else
                    g_ptr_array_add(o->if_domains, g_strconcat("*", d, NULL));
            }
            g_strfreev(ds);
            continue;
        }

        gboolean neg = (*tok == '~');
        char *name = neg ? tok + 1 : tok;
        /* an option may carry a value ("csp=...", "redirect=..."); the key is
         * the part before '='. Chop it off so key comparisons work. */
        char *eq = strchr(name, '=');
        if (eq) *eq = '\0';

        if (!strcmp(name, "third-party")) {
            o->load_type = neg ? "first-party" : "third-party";
        } else if (!strcmp(name, "first-party")) {
            o->load_type = neg ? "third-party" : "first-party";
        } else if (!strcmp(name, "match-case")) {
            o->case_sensitive = TRUE;
        } else {
            const char *rt = map_restype(name);
            if (rt) {
                /* negated resource types can't be expressed exactly; ignore
                 * the negation and keep the rule broad rather than dropping. */
                if (!neg) restype_add(o->resource_types, rt);
            } else if (is_skip_option(name)) {
                o->unsupported = TRUE;
            }
            /* otherwise: benign/unknown modifier (popup, important, elemhide,
             * generichide, all, ...) — ignore it, keep the rule. */
        }
    }
    g_strfreev(toks);
}

/* ── rule emitters ───────────────────────────────────────────────────────── */

static void begin_rule(GString *arr, gboolean *first) {
    if (!*first) g_string_append_c(arr, ',');
    *first = FALSE;
    g_string_append(arr, "\n  {");
}

static void emit_network(GString *arr, gboolean *first,
                         const char *url_filter, RuleOpts *o, gboolean exception) {
    begin_rule(arr, first);
    g_string_append(arr, "\"trigger\":{\"url-filter\":\"");
    json_escape_append(arr, url_filter);
    g_string_append_c(arr, '"');
    if (o->case_sensitive)
        g_string_append(arr, ",\"url-filter-is-case-sensitive\":true");
    if (o->resource_types->len) {
        g_string_append(arr, ",\"resource-type\":");
        json_str_array(arr, o->resource_types);
    }
    if (o->load_type) {
        g_string_append(arr, ",\"load-type\":[\"");
        g_string_append(arr, o->load_type);
        g_string_append(arr, "\"]");
    }
    /* WebKit forbids if-domain and unless-domain together: prefer includes. */
    if (o->if_domains->len) {
        g_string_append(arr, ",\"if-domain\":");
        json_str_array(arr, o->if_domains);
    } else if (o->unless_domains->len) {
        g_string_append(arr, ",\"unless-domain\":");
        json_str_array(arr, o->unless_domains);
    }
    g_string_append(arr, "},\"action\":{\"type\":\"");
    g_string_append(arr, exception ? "ignore-previous-rules" : "block");
    g_string_append(arr, "\"}}");
}

static void emit_css(GString *arr, gboolean *first,
                     GPtrArray *if_domains, GPtrArray *unless_domains,
                     const char *selector) {
    begin_rule(arr, first);
    g_string_append(arr, "\"trigger\":{\"url-filter\":\".*\"");
    if (if_domains->len) {
        g_string_append(arr, ",\"if-domain\":");
        json_str_array(arr, if_domains);
    } else if (unless_domains->len) {
        g_string_append(arr, ",\"unless-domain\":");
        json_str_array(arr, unless_domains);
    }
    g_string_append(arr, "},\"action\":{\"type\":\"css-display-none\",\"selector\":\"");
    json_escape_append(arr, selector);
    g_string_append(arr, "\"}}");
}

/* ── element-hiding detection ────────────────────────────────────────────── */

/* selectors WebKit's content-blocker CSS engine can't handle */
static gboolean selector_unsupported(const char *sel) {
    static const char *bad[] = {
        ":-abp-", "+js(", ":style(", ":matches-css", ":has-text",
        ":contains(", ":xpath(", ":nth-ancestor", ":upward(", ":remove(",
        ":watch-attr", ":min-text-length", ":has(", NULL
    };
    for (int i = 0; bad[i]; i++) if (strstr(sel, bad[i])) return TRUE;
    return FALSE;
}

/* ── main converter ──────────────────────────────────────────────────────── */

gchar *ezlist_convert(const gchar *text, guint max_rules, EzlistStats *st) {
    EzlistStats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    if (max_rules == 0) max_rules = EZLIST_DEFAULT_MAX_RULES;

    GString *arr = g_string_new("[");
    gboolean first = TRUE;

    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    for (int li = 0; lines[li]; li++) {
        st->total_lines++;
        gchar *line = g_strstrip(lines[li]);       /* trims \r and spaces */

        if (!*line) { st->blank++; continue; }
        if (line[0] == '!' || line[0] == '[') { st->comments++; continue; }

        /* honest skips for cosmetic-exception / extended cosmetic filters */
        if (strstr(line, "#@#")) { st->skipped_cosmetic_exception++; continue; }
        if (strstr(line, "#?#") || strstr(line, "#$#")) { st->skipped_extended++; continue; }

        gboolean at_cap = (st->emitted >= max_rules);

        char *sep = strstr(line, "##");
        if (sep) {
            /* ── element hiding ── */
            char *selector = sep + 2;
            if (!*selector) { st->skipped_extended++; continue; }
            if (selector_unsupported(selector)) { st->skipped_extended++; continue; }

            if (at_cap) { st->dropped_over_cap++; continue; }

            RuleOpts o; opts_init(&o);
            *sep = '\0';                            /* line now = domains */
            if (*line) {
                gchar **ds = g_strsplit(line, ",", -1);
                for (int j = 0; ds[j]; j++) {
                    char *d = g_strstrip(ds[j]);
                    if (!*d) continue;
                    if (*d == '~')
                        g_ptr_array_add(o.unless_domains, g_strconcat("*", d + 1, NULL));
                    else
                        g_ptr_array_add(o.if_domains, g_strconcat("*", d, NULL));
                }
                g_strfreev(ds);
            }
            emit_css(arr, &first, o.if_domains, o.unless_domains, selector);
            st->element_hide++; st->emitted++;
            opts_clear(&o);
            continue;
        }

        /* ── network rule ── */
        char *s = line;
        gboolean exception = FALSE;
        if (s[0] == '@' && s[1] == '@') { exception = TRUE; s += 2; }

        RuleOpts o; opts_init(&o);
        char *pattern;
        char *dollar = strrchr(s, '$');
        /* Options begin at the last '$' whose next char starts an option name
         * (a letter or '~'). A literal '$' inside a URL is followed by other
         * things, so this avoids mis-parsing it as the option separator. */
        if (dollar && (g_ascii_isalpha((guchar)dollar[1]) || dollar[1] == '~')) {
            pattern = g_strndup(s, (gsize)(dollar - s));
            parse_options(&o, dollar + 1);
        } else {
            pattern = g_strdup(s);
        }

        if (o.unsupported) {
            st->skipped_unsupported_option++;
            g_free(pattern); opts_clear(&o); continue;
        }

        gchar *rx = pattern_to_regex(pattern);
        /* refuse rules that would match every URL with no domain scope —
         * a global block/allow-all would break the whole web. */
        if (g_strcmp0(rx, ".*") == 0 && o.if_domains->len == 0) {
            st->skipped_unsafe++;
            g_free(rx); g_free(pattern); opts_clear(&o); continue;
        }
        if (at_cap) {
            st->dropped_over_cap++;
            g_free(rx); g_free(pattern); opts_clear(&o); continue;
        }

        emit_network(arr, &first, rx, &o, exception);
        if (exception) st->network_exception++; else st->network_block++;
        st->emitted++;

        g_free(rx); g_free(pattern); opts_clear(&o);
    }
    g_strfreev(lines);

    g_string_append(arr, "\n]\n");
    return g_string_free(arr, FALSE);
}

gboolean ezlist_convert_file(const gchar *in_path, const gchar *out_path,
                             guint max_rules, EzlistStats *stats, GError **error) {
    gchar *text = NULL;
    gsize len = 0;
    if (!g_file_get_contents(in_path, &text, &len, error)) return FALSE;
    gchar *json = ezlist_convert(text, max_rules, stats);
    g_free(text);
    gboolean ok = g_file_set_contents(out_path, json, -1, error);
    g_free(json);
    return ok;
}

gchar *ezlist_stats_to_string(const EzlistStats *s) {
    return g_strdup_printf(
        "EzList conversion summary:\n"
        "  lines read ............ %u\n"
        "  comments/blank ........ %u / %u\n"
        "  network block rules ... %u\n"
        "  exception rules ....... %u\n"
        "  element-hiding rules .. %u\n"
        "  --> emitted to JSON ... %u\n"
        "  skipped cosmetic-exc .. %u (#@#)\n"
        "  skipped extended ...... %u (#?# #$# +js :has() ...)\n"
        "  skipped bad option .... %u (redirect/csp/removeparam/...)\n"
        "  skipped unsafe(match-all) %u\n"
        "  dropped over cap ...... %u",
        s->total_lines, s->comments, s->blank,
        s->network_block, s->network_exception, s->element_hide, s->emitted,
        s->skipped_cosmetic_exception, s->skipped_extended,
        s->skipped_unsupported_option, s->skipped_unsafe, s->dropped_over_cap);
}
