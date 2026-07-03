/**
 * Infinitas Browser - Bookmarks Manager Implementation
 */

#include "bookmarks.h"
#include "storage.h"
#include <time.h>
#include <string.h>

#define MAX_BOOKMARKS 1000

static void bookmark_free(gpointer data);
static gchar* format_timestamp(void);

BookmarksManager* bookmarks_new(const gchar *data_dir) {
    BookmarksManager *bookmarks = g_new0(BookmarksManager, 1);
    bookmarks->data_dir = g_strdup(data_dir ? data_dir : ".infinitas");
    bookmarks->bookmarks = json_object_new_array();
    bookmarks->dirty = FALSE;
    bookmarks_load(bookmarks);
    return bookmarks;
}

void bookmarks_free(BookmarksManager *bookmarks) {
    if (!bookmarks) return;
    if (bookmarks->data_dir) g_free(bookmarks->data_dir);
    if (bookmarks->bookmarks) json_object_put(bookmarks->bookmarks);
    g_free(bookmarks);
}

int bookmarks_load(BookmarksManager *bookmarks) {
    if (!bookmarks) return -1;
    StorageManager *storage = storage_new(bookmarks->data_dir);
    int rc = storage_load_json(storage, BOOKMARKS_FILE, &bookmarks->bookmarks);
    storage_free(storage);
    
    if (!bookmarks->bookmarks ||
        !json_object_is_type(bookmarks->bookmarks, json_type_array)) {
        if (bookmarks->bookmarks) json_object_put(bookmarks->bookmarks);
        bookmarks->bookmarks = json_object_new_array();
    }
    return rc;
}

int bookmarks_save(BookmarksManager *bookmarks) {
    if (!bookmarks) return -1;
    StorageManager *storage = storage_new(bookmarks->data_dir);
    int rc = storage_save_json(storage, BOOKMARKS_FILE, bookmarks->bookmarks);
    storage_free(storage);
    if (rc == 0) bookmarks->dirty = FALSE;
    return rc;
}

Bookmark* bookmarks_add(BookmarksManager *bookmarks, const gchar *title, const gchar *url) {
    if (!bookmarks || !bookmarks->bookmarks || !title || !url) return NULL;
    
    gint count = (gint)json_object_array_length(bookmarks->bookmarks);
    if (count >= MAX_BOOKMARKS) return NULL;
    
    json_object *bookmark = json_object_new_object();
    json_object_object_add(bookmark, "id", json_object_new_int(count + 1));
    json_object_object_add(bookmark, "title", json_object_new_string(title));
    json_object_object_add(bookmark, "url", json_object_new_string(url));
    json_object_object_add(bookmark, "created", json_object_new_string(format_timestamp()));
    
    json_object_array_add(bookmarks->bookmarks, bookmark);
    bookmarks->dirty = TRUE;
    bookmarks_save(bookmarks);
    
    Bookmark *result = g_new0(Bookmark, 1);
    result->id = count + 1;
    result->title = g_strdup(title);
    result->url = g_strdup(url);
    result->created = g_strdup(format_timestamp());
    return result;
}

gboolean bookmarks_remove(BookmarksManager *bookmarks, gint id) {
    if (!bookmarks || !bookmarks->bookmarks) return FALSE;
    
    gint count = (gint)json_object_array_length(bookmarks->bookmarks);
    for (gint i = 0; i < count; i++) {
        json_object *bookmark = json_object_array_get_idx(bookmarks->bookmarks, i);
        json_object *id_obj = json_object_object_get(bookmark, "id");
        
        if (id_obj && (gint)json_object_get_int(id_obj) == id) {
            json_object_array_del_idx(bookmarks->bookmarks, i, 1);
            bookmarks->dirty = TRUE;
            bookmarks_save(bookmarks);
            return TRUE;
        }
    }
    return FALSE;
}

GPtrArray* bookmarks_get_all(BookmarksManager *bookmarks) {
    if (!bookmarks || !bookmarks->bookmarks) return NULL;
    
    GPtrArray *result = g_ptr_array_new_with_free_func(bookmark_free);
    gint count = (gint)json_object_array_length(bookmarks->bookmarks);
    
    for (gint i = 0; i < count; i++) {
        json_object *bookmark = json_object_array_get_idx(bookmarks->bookmarks, i);
        Bookmark *b = g_new0(Bookmark, 1);
        
        json_object *id_obj = json_object_object_get(bookmark, "id");
        json_object *title_obj = json_object_object_get(bookmark, "title");
        json_object *url_obj = json_object_object_get(bookmark, "url");
        json_object *created_obj = json_object_object_get(bookmark, "created");
        
        b->id = id_obj ? (gint)json_object_get_int(id_obj) : 0;
        b->title = title_obj ? g_strdup(json_object_get_string(title_obj)) : NULL;
        b->url = url_obj ? g_strdup(json_object_get_string(url_obj)) : NULL;
        b->created = created_obj ? g_strdup(json_object_get_string(created_obj)) : NULL;
        
        g_ptr_array_add(result, b);
    }
    return result;
}

GPtrArray* bookmarks_search(BookmarksManager *bookmarks, const gchar *query) {
    if (!bookmarks || !bookmarks->bookmarks || !query) return NULL;
    
    GPtrArray *result = g_ptr_array_new_with_free_func(bookmark_free);
    gchar *lower_query = g_ascii_strdown(query, -1);
    gint count = (gint)json_object_array_length(bookmarks->bookmarks);
    
    for (gint i = 0; i < count; i++) {
        json_object *bookmark = json_object_array_get_idx(bookmarks->bookmarks, i);
        json_object *title_obj = json_object_object_get(bookmark, "title");
        json_object *url_obj = json_object_object_get(bookmark, "url");
        
        gboolean match = FALSE;
        if (title_obj) {
            gchar *lower_title = g_ascii_strdown(json_object_get_string(title_obj), -1);
            if (strstr(lower_title, lower_query)) match = TRUE;
            g_free(lower_title);
        }
        
        if (!match && url_obj) {
            gchar *lower_url = g_ascii_strdown(json_object_get_string(url_obj), -1);
            if (strstr(lower_url, lower_query)) match = TRUE;
            g_free(lower_url);
        }
        
        if (match) {
            Bookmark *b = g_new0(Bookmark, 1);
            b->id = (gint)json_object_get_int(json_object_object_get(bookmark, "id"));
            b->title = title_obj ? g_strdup(json_object_get_string(title_obj)) : NULL;
            b->url = url_obj ? g_strdup(json_object_get_string(url_obj)) : NULL;
            b->created = NULL;
            g_ptr_array_add(result, b);
        }
    }
    
    g_free(lower_query);
    return result;
}

static void bookmark_free(gpointer data) {
    Bookmark *b = (Bookmark*)data;
    if (b) {
        if (b->title) g_free(b->title);
        if (b->url) g_free(b->url);
        if (b->created) g_free(b->created);
        g_free(b);
    }
}

static gchar* format_timestamp(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    gchar *timestamp = g_malloc(20);
    strftime(timestamp, 20, "%Y-%m-%dT%H:%M:%S", tm);
    return timestamp;
}
