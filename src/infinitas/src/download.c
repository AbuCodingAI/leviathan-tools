/**
 * Infinitas Browser - Download Manager Implementation (WebKit 6.0 / GTK4)
 */

#include "download.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

static void on_download_progress(WebKitDownload *download, GParamSpec *pspec, DownloadManager *dm);
static void on_download_finished(WebKitDownload *download, DownloadManager *dm);
static void on_download_failed(WebKitDownload *download, GError *error, DownloadManager *dm);
static gboolean on_decide_destination(WebKitDownload *download, gchar *suggested_filename, DownloadManager *dm);
static void download_refresh_row(DownloadManager *dm, Download *dl);

static sqlite3* dm_db(void) {
    static sqlite3 *db = NULL;
    if (db) return db;
    gchar *dir = g_build_filename(g_get_home_dir(), ".infinitas", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "downloads.db", NULL);
    if (sqlite3_open(path, &db) != SQLITE_OK) { db = NULL; }
    g_free(dir); g_free(path);
    if (db) {
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS downloads("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "url TEXT, filename TEXT, filepath TEXT,"
            "total_bytes INTEGER, status INTEGER, ts INTEGER);",
            NULL, NULL, NULL);
    }
    return db;
}

DownloadManager* download_manager_new(void) {
    DownloadManager *dm = g_new0(DownloadManager, 1);
    dm->downloads = g_ptr_array_new_with_free_func(g_free);
    dm->db = dm_db();
    return dm;
}

void download_manager_free(DownloadManager *dm) {
    if (!dm) return;
    g_ptr_array_free(dm->downloads, TRUE);
    if (dm->window) gtk_window_destroy(GTK_WINDOW(dm->window));
    g_free(dm);
}

static gchar* format_bytes(guint64 bytes) {
    const gchar *units[] = { "B", "KB", "MB", "GB" };
    gdouble size = bytes;
    gint unit = 0;
    while (size >= 1024 && unit < 3) { size /= 1024; unit++; }
    return g_strdup_printf("%.1f %s", size, units[unit]);
}

static const gchar* status_text(DownloadStatus s) {
    switch (s) {
        case DOWNLOAD_RUNNING:   return "Downloading";
        case DOWNLOAD_COMPLETED: return "Completed";
        case DOWNLOAD_PAUSED:    return "Paused";
        case DOWNLOAD_CANCELLED: return "Cancelled";
        case DOWNLOAD_FAILED:    return "Failed";
    }
    return "";
}

/* ── row widget ──────────────────────────────────────────────────────────── */

static void on_open_file_clicked(GtkButton *b, Download *dl) {
    (void)b; download_manager_open_file(dl);
}
static void on_open_folder_clicked(GtkButton *b, Download *dl) {
    (void)b; download_manager_open_folder(dl);
}
static void on_cancel_clicked(GtkButton *b, Download *dl) {
    (void)b;
    if (dl->status == DOWNLOAD_RUNNING && dl->webkit_download) {
        webkit_download_cancel(dl->webkit_download);
    }
}

static void download_build_row(DownloadManager *dm, Download *dl) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);

    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(info, TRUE);

    GtkWidget *name = gtk_label_new(dl->filename);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(info), name);

    GtkWidget *progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(info), progress);

    GtkWidget *status = gtk_label_new(status_text(dl->status));
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_widget_add_css_class(status, "dim-label");
    gtk_box_append(GTK_BOX(info), status);

    gtk_box_append(GTK_BOX(row), info);

    GtkWidget *open_btn = gtk_button_new_with_label("Open");
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open_file_clicked), dl);
    gtk_box_append(GTK_BOX(row), open_btn);

    GtkWidget *folder_btn = gtk_button_new_with_label("Folder");
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_open_folder_clicked), dl);
    gtk_box_append(GTK_BOX(row), folder_btn);

    GtkWidget *cancel_btn = gtk_button_new_with_label("\xc3\x97");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), dl);
    gtk_box_append(GTK_BOX(row), cancel_btn);

    /* stash widgets for live updates */
    g_object_set_data(G_OBJECT(row), "progress", progress);
    g_object_set_data(G_OBJECT(row), "status", status);
    g_object_set_data(G_OBJECT(row), "open", open_btn);
    g_object_set_data(G_OBJECT(row), "folder", folder_btn);
    g_object_set_data(G_OBJECT(row), "cancel", cancel_btn);
    dl->row_widget = row;

    gtk_widget_set_visible(open_btn, dl->status == DOWNLOAD_COMPLETED);
    gtk_widget_set_visible(folder_btn, dl->status == DOWNLOAD_COMPLETED);

    if (dm->list_box)
        gtk_list_box_prepend(GTK_LIST_BOX(dm->list_box), row);
}

/* Human-readable duration: 45s / 12m 30s / 2h 15m / 3 days / 2 weeks. */
static gchar* format_duration(guint64 s) {
    if (s < 60)     return g_strdup_printf("%llus", (unsigned long long)s);
    if (s < 3600)   return g_strdup_printf("%llum %llus", (unsigned long long)(s/60), (unsigned long long)(s%60));
    if (s < 86400)  return g_strdup_printf("%lluh %llum", (unsigned long long)(s/3600), (unsigned long long)((s%3600)/60));
    if (s < 604800) return g_strdup_printf("%llu days", (unsigned long long)(s/86400));
    return g_strdup_printf("%llu weeks", (unsigned long long)(s/604800));
}

static void download_refresh_row(DownloadManager *dm, Download *dl) {
    (void)dm;
    if (!dl->row_widget) return;
    GtkWidget *progress = g_object_get_data(G_OBJECT(dl->row_widget), "progress");
    GtkWidget *status   = g_object_get_data(G_OBJECT(dl->row_widget), "status");
    GtkWidget *open_btn = g_object_get_data(G_OBJECT(dl->row_widget), "open");
    GtkWidget *folder_btn = g_object_get_data(G_OBJECT(dl->row_widget), "folder");
    GtkWidget *cancel_btn = g_object_get_data(G_OBJECT(dl->row_widget), "cancel");

    gdouble frac = dl->total_bytes > 0
        ? (gdouble)dl->received_bytes / dl->total_bytes : 0.0;
    if (dl->status == DOWNLOAD_COMPLETED) frac = 1.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), frac);

    gchar *recv = format_bytes(dl->received_bytes);
    gchar *tot  = format_bytes(dl->total_bytes);
    gchar *txt;
    if (dl->status == DOWNLOAD_RUNNING) {
        int pct = dl->total_bytes > 0 ? (int)(frac * 100) : 0;
        guint64 now = (guint64)time(NULL);
        guint64 elapsed = now > dl->start_time ? now - dl->start_time : 1;
        double speed = (double)dl->received_bytes / (double)elapsed;   /* bytes/s */
        gchar *spd = format_bytes((guint64)speed);
        /* ETA scales with real speed: 3 MB/s vs 5 KB/s = seconds vs weeks. */
        if (dl->total_bytes > dl->received_bytes && speed > 1.0) {
            guint64 eta = (guint64)((double)(dl->total_bytes - dl->received_bytes) / speed);
            gchar *e = format_duration(eta);
            txt = g_strdup_printf("%d%%  ·  %s / %s  ·  %s/s  ·  %s left",
                                  pct, recv, tot, spd, e);
            g_free(e);
        } else {
            txt = g_strdup_printf("%d%%  ·  %s / %s  ·  %s/s", pct, recv, tot, spd);
        }
        g_free(spd);
    } else {
        txt = g_strdup_printf("%s — %s", tot, status_text(dl->status));
    }
    gtk_label_set_text(GTK_LABEL(status), txt);
    g_free(txt); g_free(recv); g_free(tot);

    gboolean done = (dl->status == DOWNLOAD_COMPLETED);
    gtk_widget_set_visible(open_btn, done);
    gtk_widget_set_visible(folder_btn, done);
    gtk_widget_set_visible(cancel_btn, dl->status == DOWNLOAD_RUNNING);
}

/* ── webkit signal handlers ──────────────────────────────────────────────── */

static gboolean on_decide_destination(WebKitDownload *download, gchar *suggested_filename, DownloadManager *dm) {
    Download *dl = NULL;
    for (guint i = 0; i < dm->downloads->len; i++) {
        Download *d = g_ptr_array_index(dm->downloads, i);
        if (d->webkit_download == download) { dl = d; break; }
    }
    if (!dl) return FALSE;

    const gchar *fname = (suggested_filename && *suggested_filename)
        ? suggested_filename : (dl->filename ? dl->filename : "download");

    gchar *downloads_dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    g_mkdir_with_parents(downloads_dir, 0755);
    gchar *dest = g_build_filename(downloads_dir, fname, NULL);

    /* avoid clobbering: append (n) if exists */
    if (g_file_test(dest, G_FILE_TEST_EXISTS)) {
        for (int n = 1; n < 1000; n++) {
            gchar *base = g_strdup(fname);
            gchar *dot = strrchr(base, '.');
            gchar *candidate;
            if (dot) {
                *dot = '\0';
                candidate = g_strdup_printf("%s/%s (%d).%s", downloads_dir, base, n, dot + 1);
            } else {
                candidate = g_strdup_printf("%s/%s (%d)", downloads_dir, fname, n);
            }
            g_free(base);
            if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
                g_free(dest); dest = candidate; break;
            }
            g_free(candidate);
        }
    }

    g_free(dl->filename); dl->filename = g_strdup(fname);
    g_free(dl->filepath); dl->filepath = g_strdup(dest);
    g_free(dl->destination); dl->destination = g_strdup(dest);

    webkit_download_set_allow_overwrite(download, TRUE);
    webkit_download_set_destination(download, dest);

    if (dl->row_widget) {
        GtkWidget *name = NULL;
        /* update filename label (first child of info box) */
        download_refresh_row(dm, dl);
        (void)name;
    }

    g_free(downloads_dir);
    g_free(dest);
    return TRUE;
}

static void on_download_progress(WebKitDownload *download, GParamSpec *pspec, DownloadManager *dm) {
    (void)pspec;
    for (guint i = 0; i < dm->downloads->len; i++) {
        Download *dl = g_ptr_array_index(dm->downloads, i);
        if (dl->webkit_download == download) {
            dl->received_bytes = webkit_download_get_received_data_length(download);
            WebKitURIResponse *resp = webkit_download_get_response(download);
            if (resp && dl->total_bytes == 0)
                dl->total_bytes = webkit_uri_response_get_content_length(resp);
            download_refresh_row(dm, dl);
            break;
        }
    }
}

static void on_download_finished(WebKitDownload *download, DownloadManager *dm) {
    for (guint i = 0; i < dm->downloads->len; i++) {
        Download *dl = g_ptr_array_index(dm->downloads, i);
        if (dl->webkit_download == download) {
            if (dl->status != DOWNLOAD_CANCELLED && dl->status != DOWNLOAD_FAILED) {
                dl->status = DOWNLOAD_COMPLETED;
                dl->received_bytes = dl->total_bytes;
            }
            dl->end_time = (guint64)time(NULL);
            download_refresh_row(dm, dl);
            download_manager_save_history(dm);
            break;
        }
    }
}

static void on_download_failed(WebKitDownload *download, GError *error, DownloadManager *dm) {
    (void)error;
    for (guint i = 0; i < dm->downloads->len; i++) {
        Download *dl = g_ptr_array_index(dm->downloads, i);
        if (dl->webkit_download == download) {
            /* cancelled also lands here */
            dl->status = (error && g_error_matches(error, WEBKIT_DOWNLOAD_ERROR,
                          WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
                          ? DOWNLOAD_CANCELLED : DOWNLOAD_FAILED;
            download_refresh_row(dm, dl);
            break;
        }
    }
}

/* ── public ──────────────────────────────────────────────────────────────── */

void download_manager_add(DownloadManager *dm, WebKitDownload *webkit_dl) {
    if (!dm || !webkit_dl) return;

    Download *dl = g_new0(Download, 1);
    dl->id = dm->downloads->len + 1;
    dl->webkit_download = webkit_dl;

    WebKitURIRequest *req = webkit_download_get_request(webkit_dl);
    dl->url = g_strdup(req ? webkit_uri_request_get_uri(req) : "");

    WebKitURIResponse *resp = webkit_download_get_response(webkit_dl);
    const gchar *sug = resp ? webkit_uri_response_get_suggested_filename(resp) : NULL;
    dl->filename = g_strdup(sug && *sug ? sug : "download");
    dl->total_bytes = resp ? webkit_uri_response_get_content_length(resp) : 0;
    dl->status = DOWNLOAD_RUNNING;
    dl->start_time = (guint64)time(NULL);

    g_ptr_array_add(dm->downloads, dl);

    g_signal_connect(webkit_dl, "decide-destination",
                     G_CALLBACK(on_decide_destination), dm);
    g_signal_connect(webkit_dl, "notify::estimated-progress",
                     G_CALLBACK(on_download_progress), dm);
    g_signal_connect(webkit_dl, "received-data",
                     G_CALLBACK(on_download_progress), dm);
    g_signal_connect(webkit_dl, "finished",
                     G_CALLBACK(on_download_finished), dm);
    g_signal_connect(webkit_dl, "failed",
                     G_CALLBACK(on_download_failed), dm);

    if (dm->list_box) download_build_row(dm, dl);
}

static void on_clear_clicked(GtkButton *b, DownloadManager *dm) {
    (void)b;
    /* remove finished rows from UI + array */
    for (guint i = 0; i < dm->downloads->len; ) {
        Download *dl = g_ptr_array_index(dm->downloads, i);
        if (dl->status != DOWNLOAD_RUNNING) {
            if (dl->row_widget && dm->list_box) {
                GtkWidget *lbrow = gtk_widget_get_parent(dl->row_widget);
                if (lbrow) gtk_list_box_remove(GTK_LIST_BOX(dm->list_box), lbrow);
            }
            g_free(dl->url); g_free(dl->filename);
            g_free(dl->filepath); g_free(dl->destination);
            g_ptr_array_remove_index(dm->downloads, i);
        } else i++;
    }
    sqlite3 *db = dm_db();
    if (db) sqlite3_exec(db, "DELETE FROM downloads;", NULL, NULL, NULL);
}

void download_manager_show_window(DownloadManager *dm, GtkWindow *parent) {
    if (!dm) return;
    if (dm->window) { gtk_window_present(GTK_WINDOW(dm->window)); return; }

    dm->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dm->window), "Downloads");
    gtk_window_set_default_size(GTK_WINDOW(dm->window), 560, 420);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(dm->window), parent);
    g_signal_connect(dm->window, "destroy", G_CALLBACK(gtk_widget_unrealize), NULL);
    g_signal_connect_swapped(dm->window, "destroy",
                             G_CALLBACK(g_nullify_pointer), &dm->window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(dm->window), vbox);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(header, 8);
    gtk_widget_set_margin_end(header, 8);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 8);
    GtkWidget *title = gtk_label_new("Downloads");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header), title);
    GtkWidget *clear = gtk_button_new_with_label("Clear finished");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_clicked), dm);
    gtk_box_append(GTK_BOX(header), clear);
    gtk_box_append(GTK_BOX(vbox), header);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(vbox), scrolled);

    dm->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dm->list_box), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dm->list_box);

    /* populate existing downloads */
    for (guint i = 0; i < dm->downloads->len; i++) {
        Download *dl = g_ptr_array_index(dm->downloads, i);
        dl->row_widget = NULL;
        download_build_row(dm, dl);
        download_refresh_row(dm, dl);
    }

    gtk_window_present(GTK_WINDOW(dm->window));
}

void download_manager_save_history(DownloadManager *dm) {
    sqlite3 *db = dm_db();
    if (!db || !dm) return;
    for (guint i = 0; i < dm->downloads->len; i++) {
        Download *dl = g_ptr_array_index(dm->downloads, i);
        if (dl->status != DOWNLOAD_COMPLETED) continue;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO downloads(url,filename,filepath,total_bytes,status,ts) "
            "VALUES(?,?,?,?,?,?);", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, dl->url, -1, NULL);
            sqlite3_bind_text(st, 2, dl->filename, -1, NULL);
            sqlite3_bind_text(st, 3, dl->filepath, -1, NULL);
            sqlite3_bind_int64(st, 4, dl->total_bytes);
            sqlite3_bind_int(st, 5, dl->status);
            sqlite3_bind_int64(st, 6, dl->end_time);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

void download_manager_load_history(DownloadManager *dm) {
    sqlite3 *db = dm_db();
    if (!db || !dm) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT url,filename,filepath,total_bytes,status FROM downloads "
        "ORDER BY ts DESC LIMIT 100;", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Download *dl = g_new0(Download, 1);
            dl->url      = g_strdup((const char*)sqlite3_column_text(st, 0));
            dl->filename = g_strdup((const char*)sqlite3_column_text(st, 1));
            dl->filepath = g_strdup((const char*)sqlite3_column_text(st, 2));
            dl->total_bytes = sqlite3_column_int64(st, 3);
            dl->received_bytes = dl->total_bytes;
            dl->status   = sqlite3_column_int(st, 4);
            dl->webkit_download = NULL;
            g_ptr_array_add(dm->downloads, dl);
        }
        sqlite3_finalize(st);
    }
}

void download_manager_open_file(Download *dl) {
    if (!dl || !dl->filepath) return;
    gchar *cmd = g_strdup_printf("xdg-open \"%s\"", dl->filepath);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

void download_manager_open_folder(Download *dl) {
    if (!dl || !dl->filepath) return;
    gchar *folder = g_path_get_dirname(dl->filepath);
    gchar *cmd = g_strdup_printf("xdg-open \"%s\"", folder);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    g_free(folder);
}
