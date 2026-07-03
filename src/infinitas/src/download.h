/**
 * Infinitas Browser - Download Manager Header
 */

#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <glib.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <sqlite3.h>
#include <time.h>

typedef enum {
    DOWNLOAD_RUNNING,
    DOWNLOAD_COMPLETED,
    DOWNLOAD_PAUSED,
    DOWNLOAD_CANCELLED,
    DOWNLOAD_FAILED
} DownloadStatus;

typedef struct {
    guint64 id;
    gchar *url;
    gchar *filename;
    gchar *filepath;
    guint64 received_bytes;
    guint64 total_bytes;
    DownloadStatus status;
    gchar *destination;
    guint64 start_time;
    guint64 end_time;
    WebKitDownload *webkit_download;
    GtkWidget *row_widget;   /* the per-download row in the list box */
} Download;

typedef struct {
    GPtrArray *downloads;    /* array of Download* */
    GtkWidget *window;
    GtkWidget *list_box;     /* GtkListBox of download rows */
    sqlite3 *db;
} DownloadManager;

DownloadManager* download_manager_new(void);
void download_manager_free(DownloadManager *dm);
void download_manager_add(DownloadManager *dm, WebKitDownload *webkit_dl);
void download_manager_show_window(DownloadManager *dm, GtkWindow *parent);
void download_manager_save_history(DownloadManager *dm);
void download_manager_load_history(DownloadManager *dm);
void download_manager_open_file(Download *dl);
void download_manager_open_folder(Download *dl);

#endif
