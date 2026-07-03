/**
 * Infinitas Browser - HTTP Handler Implementation
 */

#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CACHE_MAX_AGE 300

static void cache_entry_free(gpointer data);
static HTTPResponse* http_response_new(void);
static void http_response_free(HTTPResponse *response);

HTTPHandler* http_new(void) {
    HTTPHandler *http = g_new0(HTTPHandler, 1);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    http->curl = curl_easy_init();
    
    if (!http->curl) {
        g_printerr("Failed to initialize curl\n");
        http_free(http);
        return NULL;
    }
    
    http->multi = curl_multi_init();
    http->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cache_entry_free);
    http->cache_size = 0;
    http->max_cache_size = 50;
    http->user_agent = g_strdup("Infinitas Browser/1.0");
    
    curl_easy_setopt(http->curl, CURLOPT_USERAGENT, http->user_agent);
    curl_easy_setopt(http->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(http->curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(http->curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(http->curl, CURLOPT_CONNECTTIMEOUT, 5L);
    
    return http;
}

void http_free(HTTPHandler *http) {
    if (!http) return;
    
    if (http->cache) {
        g_hash_table_destroy(http->cache);
    }
    if (http->user_agent) {
        g_free(http->user_agent);
    }
    if (http->multi) {
        curl_multi_cleanup(http->multi);
    }
    if (http->curl) {
        curl_easy_cleanup(http->curl);
    }
    curl_global_cleanup();
    g_free(http);
}

HTTPResponse* http_fetch(HTTPHandler *http, const gchar *url, gint timeout) {
    if (!http || !url) return NULL;
    
    gchar *cache_key = http_cache_key(url);
    HTTPResponse *response = NULL;
    
    if (http->cache) {
        response = g_hash_table_lookup(http->cache, cache_key);
        if (response && !response->error) {
            g_free(cache_key);
            return response;
        }
    }
    
    response = http_response_new();
    response->content = NULL;
    response->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    CURL *curl = http->curl;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response->content);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response->headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res == CURLE_OK) {
        response->success = TRUE;
        
        long status_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        response->status_code = (gint)status_code;
        
        gchar *content_type = NULL;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
        if (content_type) {
            response->content_type = g_strdup(content_type);
        }
        
        gchar *final_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
        if (final_url) {
            response->url = g_strdup(final_url);
        } else {
            response->url = g_strdup(url);
        }
        
        if (http->cache_size < http->max_cache_size) {
            g_hash_table_insert(http->cache, g_strdup(url), response);
            http->cache_size++;
        }
    } else {
        response->success = FALSE;
        response->error = TRUE;
        response->content = g_strdup(curl_easy_strerror(res));
    }
    
    g_free(cache_key);
    return response;
}

void http_clear_cache(HTTPHandler *http) {
    if (!http || !http->cache) return;
    g_hash_table_remove_all(http->cache);
    http->cache_size = 0;
}

gchar* http_normalize_url(const gchar *url) {
    if (!url) return NULL;
    if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
        return g_strdup(url);
    }
    return g_strdup_printf("https://%s", url);
}

gchar* http_cache_key(const gchar *url) {
    if (!url) return NULL;
    return g_strdup(url);
}

gsize http_write_callback(void *contents, size_t size, size_t nmemb, gchar **user_data) {
    size_t total_size = size * nmemb;
    if (!user_data || !contents) return 0;
    
    gchar *str = *user_data;
    if (!str) {
        str = g_strndup(contents, total_size);
    } else {
        gsize current_len = strlen(str);
        str = g_realloc(str, current_len + total_size + 1);
        memcpy(str + current_len, contents, total_size);
        str[current_len + total_size] = '\0';
    }
    *user_data = str;
    return total_size;
}

GHashTable* http_parse_headers(const gchar *header_data) {
    GHashTable *headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!header_data) return headers;
    
    gchar **lines = g_strsplit(header_data, "\n", 0);
    for (gint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (strlen(line) == 0) continue;
        
        gchar **parts = g_strsplit(line, ":", 2);
        if (parts && parts[0] && parts[1]) {
            gchar *key = g_strstrip(parts[0]);
            gchar *value = g_strstrip(parts[1]);
            g_hash_table_insert(headers, g_strdup(key), g_strdup(value));
        }
        g_strfreev(parts);
    }
    g_strfreev(lines);
    return headers;
}

static void cache_entry_free(gpointer data) {
    HTTPResponse *response = (HTTPResponse*)data;
    if (response) {
        http_response_free(response);
    }
}

static HTTPResponse* http_response_new(void) {
    return g_new0(HTTPResponse, 1);
}

static void http_response_free(HTTPResponse *response) {
    if (!response) return;
    if (response->content) g_free(response->content);
    if (response->content_type) g_free(response->content_type);
    if (response->url) g_free(response->url);
    if (response->headers) g_hash_table_destroy(response->headers);
    g_free(response);
}
