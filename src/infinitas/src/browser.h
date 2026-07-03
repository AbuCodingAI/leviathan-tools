/**
 * Infinitas Browser - Browser Core Header
 */

#ifndef BROWSER_H
#define BROWSER_H

#include <gtk/gtk.h>
#include <gio/gio.h>
#include "renderer.h"
#include "settings.h"
#include "bookmarks.h"
#include "history.h"
#include "search.h"
#include "offstore.h"
#include "plugin.h"
#include "extension.h"
#include "protocol.h"
#include "download.h"
#include "passwords.h"

#define BROWSER_WINDOW_WIDTH 1200
#define BROWSER_WINDOW_HEIGHT 800
#define MAX_TABS 100

typedef struct {
    GtkWidget *window;
    GtkWidget *toolbar;
    GtkWidget *address_bar;
    GtkWidget *tab_widget;
    GtkWidget *splitter;
    GtkWidget *find_bar;
    GtkWidget *find_entry;

    GPtrArray *tabs;
    GPtrArray *tab_titles;
    GPtrArray *groups;      /* Opera-style tab groups (TabGroup*, see browser.c) */
    gboolean loading_session; /* suppress group apply/prune during restore */
    gint active_tab;
    
    SettingsManager *settings;
    BookmarksManager *bookmarks;
    HistoryManager *history;
    SearchEngine *search;
    
    gboolean pwa_mode;  /* PWA isolated mode */

    DownloadManager *download_manager;

    GtkWidget *back_btn;
    GtkWidget *forward_btn;
    GtkWidget *refresh_btn;
    GtkWidget *home_btn;
    GtkWidget *bookmark_btn;
    GtkWidget *menu_btn;
    GtkWidget    *progress_bar;
    GtkWidget    *lock_label;
    GtkWidget    *account_btn;
    OfflineStore     *offstore;
    PluginManager    *plugins;
    ExtensionManager *extensions;
    ProtocolStore    *protocols;
    PasswordStore    *passwords;

    /* ONE persistent network session shared by every WebView (see browser.c).
     * This is what makes sign-ins survive a full quit+relaunch. */
    WebKitNetworkSession *session;
} InfinitasBrowser;

InfinitasBrowser* browser_new(GtkApplication *app, gboolean pwa_mode);
void browser_free(InfinitasBrowser *browser);
void browser_show(InfinitasBrowser *browser);

WebRenderer* browser_create_tab(InfinitasBrowser *browser, const gchar *url);
void browser_close_tab(InfinitasBrowser *browser, gint index);
void browser_switch_tab(InfinitasBrowser *browser, gint index);
gint browser_get_active_tab(InfinitasBrowser *browser);
WebRenderer* browser_get_active_renderer(InfinitasBrowser *browser);

void browser_navigate(InfinitasBrowser *browser, const gchar *url);
void browser_go_home(InfinitasBrowser *browser);
void browser_reload(InfinitasBrowser *browser);
void browser_go_back(InfinitasBrowser *browser);
void browser_go_forward(InfinitasBrowser *browser);

void browser_toggle_fullscreen(InfinitasBrowser *browser);
void browser_find_show(InfinitasBrowser *browser);   /* open the find-in-page bar */

void browser_add_bookmark(InfinitasBrowser *browser);
gchar* browser_get_current_url(InfinitasBrowser *browser);
gchar* browser_get_current_title(InfinitasBrowser *browser);
void browser_update_account_btn(InfinitasBrowser *browser);


#endif
