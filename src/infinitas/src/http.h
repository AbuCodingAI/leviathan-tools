/**
 * Infinitas Browser - HTTP Handler Header
 * 
 * libcurl wrapper for HTTP requests
 */

#ifndef HTTP_H
#define HTTP_H

#include <curl/curl.h>
#include <glib.h>

typedef struct {
    CURL *curl;
    CURLM *multi;
    GHashTable *cache;
    gint cache_size;
    gint max_cache_size;
    gchar *user_agent;
} HTTPHandler;

typedef struct {
    gboolean success;
    gchar *content;
    gint status_code;
    gchar *content_type;
    gchar *url;
    GHashTable *headers;
    gboolean error;
} HTTPResponse;

/* Public functions */
HTTPHandler* http_new(void);
void http_free(HTTPHandler *http);
HTTPResponse* http_fetch(HTTPHandler *http, const gchar *url, gint timeout);
void http_clear_cache(HTTPHandler *http);
gchar* http_normalize_url(const gchar *url);
gchar* http_cache_key(const gchar *url);

/* Helper functions */
gsize http_write_callback(void *contents, size_t size, size_t nmemb, gchar **user_data);
GHashTable* http_parse_headers(const gchar *header_data);

#endif /* HTTP_H */
