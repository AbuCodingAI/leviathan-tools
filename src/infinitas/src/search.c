/**
 * Infinitas Browser - Search Engine Implementation
 */

#include "search.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

SearchEngine* search_new(const gchar *default_engine) {
    SearchEngine *search = g_new0(SearchEngine, 1);
    search->default_engine = g_strdup(default_engine ? default_engine : "google");
    search->engines = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    g_hash_table_insert(search->engines, g_strdup("google"), g_strdup(SEARCH_ENGINE_GOOGLE));
    
    return search;
}

void search_free(SearchEngine *search) {
    if (!search) return;
    if (search->default_engine) g_free(search->default_engine);
    if (search->engines) g_hash_table_destroy(search->engines);
    g_free(search);
}

gchar* search_url_encode(const gchar *text) {
    if (!text) return NULL;
    
    GString *encoded = g_string_new(NULL);
    for (const gchar *p = text; *p; p++) {
        guint8 c = (guint8)*p;
        if (c == ' ') {
            g_string_append_c(encoded, '+');
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            g_string_append_c(encoded, c);
        } else {
            g_string_append_printf(encoded, "%%%02X", c);
        }
    }
    return g_string_free(encoded, FALSE);
}

gboolean search_is_search_query(const gchar *input) {
    if (!input || *input == '\0') return FALSE;
    if (strstr(input, "://")) return FALSE;
    if (strchr(input, '.'))    return FALSE;
    return TRUE;
}

gboolean search_looks_like_domain(const gchar *text) {
    if (!text) return FALSE;
    if (!strchr(text, '.')) return FALSE;
    if (strchr(text, ' ')) return FALSE;
    if (strlen(text) <= 3) return FALSE;
    
    const gchar *p = text;
    gboolean has_letter = FALSE;
    while (*p) {
        if (isalpha(*p)) has_letter = TRUE;
        else if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_') return FALSE;
        p++;
    }
    return has_letter;
}

gchar* search_get_search_url(SearchEngine *search, const gchar *query, const gchar *engine) {
    if (!search || !query) return NULL;
    if (!engine) engine = search->default_engine;
    
    gchar *engine_url = g_hash_table_lookup(search->engines, engine);
    if (!engine_url) engine_url = g_hash_table_lookup(search->engines, "google");
    
    gchar *encoded_query = search_url_encode(query);
    gchar *url = g_strdup_printf(engine_url, encoded_query);
    
    g_free(encoded_query);
    return url;
}

gboolean search_is_ip_address(const gchar *text) {
    if (!text) return FALSE;
    gchar **parts = g_strsplit(text, ".", -1);
    if (g_strv_length(parts) != 4) { g_strfreev(parts); return FALSE; }
    gboolean ok = TRUE;
    for (int i = 0; i < 4; i++) {
        if (!parts[i] || !*parts[i]) { ok = FALSE; break; }
        for (const gchar *p = parts[i]; *p; p++) {
            if (!g_ascii_isdigit(*p)) { ok = FALSE; break; }
        }
        if (!ok) break;
        int n = atoi(parts[i]);
        if (n < 0 || n > 255) { ok = FALSE; break; }
    }
    g_strfreev(parts);
    return ok;
}

gchar* search_process_input(SearchEngine *search, const gchar *input_text) {
    if (!search || !input_text) return NULL;

    gchar *trimmed = g_strstrip(g_strdup(input_text));

    /* bare IPv4 → ping page */
    if (search_is_ip_address(trimmed)) {
        gchar *url = g_strdup_printf("infinitas://ping/%s", trimmed);
        g_free(trimmed);
        return url;
    }

    if (search_is_search_query(trimmed)) {
        gchar *url = search_get_search_url(search, trimmed, NULL);
        g_free(trimmed);
        return url;
    } else {
        gchar *url;
        if (strstr(trimmed, "://")) {
            url = g_strdup(trimmed);
        } else {
            url = g_strdup_printf("https://%s", trimmed);
        }
        g_free(trimmed);
        return url;
    }
}

gboolean search_is_valid_domain(const gchar *text) {
    if (!text) return FALSE;
    return search_looks_like_domain(text);
}
