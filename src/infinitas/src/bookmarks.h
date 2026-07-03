/**
 * Infinitas Browser - Bookmarks Manager Header
 */

#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#include <json-c/json.h>
#include <glib.h>

#define BOOKMARKS_FILE "bookmarks.json"

typedef struct {
    gchar *data_dir;
    json_object *bookmarks;
    gboolean dirty;
} BookmarksManager;

typedef struct {
    gint id;
    gchar *title;
    gchar *url;
    gchar *created;
} Bookmark;

BookmarksManager* bookmarks_new(const gchar *data_dir);
void bookmarks_free(BookmarksManager *bookmarks);
int bookmarks_load(BookmarksManager *bookmarks);
int bookmarks_save(BookmarksManager *bookmarks);
Bookmark* bookmarks_add(BookmarksManager *bookmarks, const gchar *title, const gchar *url);
gboolean bookmarks_remove(BookmarksManager *bookmarks, gint id);
GPtrArray* bookmarks_get_all(BookmarksManager *bookmarks);
GPtrArray* bookmarks_search(BookmarksManager *bookmarks, const gchar *query);

#endif
