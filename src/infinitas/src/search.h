/**
 * Infinitas Browser - Search Engine Header
 * 
 * Search query detection and search engine integration
 */

#ifndef SEARCH_H
#define SEARCH_H

#include <glib.h>

#define SEARCH_ENGINE_GOOGLE "https://www.google.com/search?q=%s"

typedef struct {
    gchar *default_engine;
    GHashTable *engines;
} SearchEngine;

/* Public functions */
SearchEngine* search_new(const gchar *default_engine);
void search_free(SearchEngine *search);

/* URL encoding */
gchar* search_url_encode(const gchar *text);

/* Query detection */
gboolean search_is_search_query(const gchar *input);
gboolean search_looks_like_domain(const gchar *text);
gboolean search_is_ip_address(const gchar *text);

/* URL construction */
gchar* search_get_search_url(SearchEngine *search, const gchar *query, const gchar *engine);
gchar* search_process_input(SearchEngine *search, const gchar *input_text);

#endif /* SEARCH_H */
