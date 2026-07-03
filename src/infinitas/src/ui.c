/**
 * Infinitas Browser - UI Components Implementation
 */

#include "ui.h"
#include "infinitas_scheme.h"
#include "renderer.h"

/* ── toolbar callbacks ───────────────────────────────────────────────────── */

static void on_back_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b; browser_go_back(browser);
}
static void on_forward_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b; browser_go_forward(browser);
}
static void on_refresh_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b; browser_reload(browser);
}
static void on_home_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b; browser_go_home(browser);
}
static void on_address_bar_activate(GtkEntry *entry, InfinitasBrowser *browser) {
    browser_navigate(browser, gtk_editable_get_text(GTK_EDITABLE(entry)));
}
static void on_bookmark_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b; browser_add_bookmark(browser);
}
static void on_downloads_clicked(GtkButton *button, InfinitasBrowser *browser) {
    (void)button;
    if (browser->download_manager) {
        download_manager_show_window(browser->download_manager, browser->window);
    }
}

static void on_menu_clicked(GtkButton *button, InfinitasBrowser *browser) {
    GtkWidget *pop = ui_create_menu(browser);
    gtk_widget_set_parent(pop, GTK_WIDGET(button));
    gtk_popover_popup(GTK_POPOVER(pop));
}
static void on_account_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(browser);
    if (r) renderer_load_url(r, "https://accounts.google.com");
}

/* ── menu item callbacks ─────────────────────────────────────────────────── */

static void mi_new_tab(GtkButton *b, InfinitasBrowser *br) {
    (void)b; browser_create_tab(br, "infinitas://tab");
}
static void mi_bookmarks(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "infinitas://bookmarks");
}
static void mi_history(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "infinitas://history");
}
static void mi_offline(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "infinitas://offline");
}
static void mi_passwords(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "infinitas://passwords");
}
static void mi_settings(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "infinitas://settings");
}
static void mi_fonts(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "infinitas://fonts");
}
static void mi_fullscreen(GtkButton *b, InfinitasBrowser *br) {
    (void)b; browser_toggle_fullscreen(br);
}
static void mi_zoom_in(GtkButton *b, InfinitasBrowser *br) {
    (void)b; WebRenderer *r = browser_get_active_renderer(br);
    if (r && r->view) { double z = webkit_web_view_get_zoom_level(r->view) + 0.1;
        if (z > 3.0) z = 3.0; webkit_web_view_set_zoom_level(r->view, z); }
}
static void mi_zoom_out(GtkButton *b, InfinitasBrowser *br) {
    (void)b; WebRenderer *r = browser_get_active_renderer(br);
    if (r && r->view) { double z = webkit_web_view_get_zoom_level(r->view) - 0.1;
        if (z < 0.3) z = 0.3; webkit_web_view_set_zoom_level(r->view, z); }
}
static void mi_zoom_reset(GtkButton *b, InfinitasBrowser *br) {
    (void)b; WebRenderer *r = browser_get_active_renderer(br);
    if (r && r->view) webkit_web_view_set_zoom_level(r->view, 1.0);
}
static void mi_mute(GtkButton *b, InfinitasBrowser *br) {
    (void)b; WebRenderer *r = browser_get_active_renderer(br);
    if (r && r->view) webkit_web_view_set_is_muted(r->view,
        !webkit_web_view_get_is_muted(r->view));
}
static void mi_running(GtkButton *b, InfinitasBrowser *br) {
    (void)b; browser_create_tab(br, "infinitas://running");
}
static void mi_find(GtkButton *b, InfinitasBrowser *br) {
    (void)b; browser_find_show(br);
}
static void mi_close_tab(GtkButton *b, InfinitasBrowser *br) {
    (void)b; browser_close_tab(br, browser_get_active_tab(br));
}
static void mi_account(GtkButton *b, InfinitasBrowser *br) {
    (void)b;
    WebRenderer *r = browser_get_active_renderer(br);
    if (r) renderer_load_url(r, "https://accounts.google.com");
}

/* ── toolbar ─────────────────────────────────────────────────────────────── */

GtkWidget* ui_create_toolbar(InfinitasBrowser *browser) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer, "nav-bar");

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(outer), row);

    /* nav buttons */
    browser->back_btn = gtk_button_new_with_label("\xe2\x86\x90");
    gtk_widget_set_tooltip_text(browser->back_btn, "Go back (Alt+Left)");
    gtk_widget_set_sensitive(browser->back_btn, FALSE);
    g_signal_connect(browser->back_btn, "clicked", G_CALLBACK(on_back_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->back_btn);

    browser->forward_btn = gtk_button_new_with_label("\xe2\x86\x92");
    gtk_widget_set_tooltip_text(browser->forward_btn, "Go forward (Alt+Right)");
    gtk_widget_set_sensitive(browser->forward_btn, FALSE);
    g_signal_connect(browser->forward_btn, "clicked", G_CALLBACK(on_forward_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->forward_btn);

    browser->refresh_btn = gtk_button_new_with_label("\xe2\x86\xbb");
    gtk_widget_set_tooltip_text(browser->refresh_btn, "Reload / Stop (F5)");
    g_signal_connect(browser->refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->refresh_btn);

    browser->home_btn = gtk_button_new_with_label("\xe2\x8c\x82");
    gtk_widget_set_tooltip_text(browser->home_btn, "Home");
    g_signal_connect(browser->home_btn, "clicked", G_CALLBACK(on_home_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->home_btn);

    gtk_box_append(GTK_BOX(row), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* address bar wrapper */
    GtkWidget *addr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(addr_box, "addr-box");
    gtk_widget_set_hexpand(addr_box, TRUE);

    browser->lock_label = gtk_label_new("");
    gtk_widget_set_visible(browser->lock_label, FALSE);
    gtk_widget_add_css_class(browser->lock_label, "lock-http");
    gtk_box_append(GTK_BOX(addr_box), browser->lock_label);

    browser->address_bar = gtk_entry_new();
    gtk_widget_set_hexpand(browser->address_bar, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(browser->address_bar),
                                   "Search or enter address\xe2\x80\xa6");
    g_signal_connect(browser->address_bar, "activate",
                     G_CALLBACK(on_address_bar_activate), browser);
    gtk_box_append(GTK_BOX(addr_box), browser->address_bar);
    gtk_box_append(GTK_BOX(row), addr_box);

    browser->bookmark_btn = gtk_button_new_with_label("\xe2\xad\x90");
    gtk_widget_set_tooltip_text(browser->bookmark_btn, "Bookmark (Ctrl+D)");
    g_signal_connect(browser->bookmark_btn, "clicked",
                     G_CALLBACK(on_bookmark_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->bookmark_btn);

    browser->account_btn = gtk_button_new_with_label("Sign in");
    gtk_widget_set_tooltip_text(browser->account_btn, "Sign in to Infinitas");
    g_signal_connect(browser->account_btn, "clicked", G_CALLBACK(on_account_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->account_btn);

    GtkButton *downloads_btn = GTK_BUTTON(gtk_button_new_with_label("\xe2\xac\x87"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(downloads_btn), "Downloads");
    g_signal_connect(downloads_btn, "clicked", G_CALLBACK(on_downloads_clicked), browser);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(downloads_btn));

    browser->menu_btn = gtk_button_new_with_label("\xe2\x98\xb0");
    gtk_widget_set_tooltip_text(browser->menu_btn, "Menu");
    g_signal_connect(browser->menu_btn, "clicked", G_CALLBACK(on_menu_clicked), browser);
    gtk_box_append(GTK_BOX(row), browser->menu_btn);

    /* progress bar — 3px thin, hidden by default */
    browser->progress_bar = gtk_progress_bar_new();
    gtk_widget_set_visible(browser->progress_bar, FALSE);
    gtk_box_append(GTK_BOX(outer), browser->progress_bar);

    return outer;
}

/* ── menu popover ────────────────────────────────────────────────────────── */

static GtkWidget* menu_btn(const gchar *label, GCallback cb, InfinitasBrowser *br) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_halign(b, GTK_ALIGN_FILL);
    if (cb) g_signal_connect(b, "clicked", cb, br);
    return b;
}

GtkWidget* ui_create_menu(InfinitasBrowser *browser) {
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 180, -1);
    gtk_popover_set_child(GTK_POPOVER(pop), box);

    gtk_box_append(GTK_BOX(box), menu_btn("New Tab",          G_CALLBACK(mi_new_tab),    browser));
    gtk_box_append(GTK_BOX(box), menu_btn("Close Tab",        G_CALLBACK(mi_close_tab),  browser));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), menu_btn("Bookmarks",        G_CALLBACK(mi_bookmarks),  browser));
    gtk_box_append(GTK_BOX(box), menu_btn("History",          G_CALLBACK(mi_history),    browser));
    gtk_box_append(GTK_BOX(box), menu_btn("Offline Pages",    G_CALLBACK(mi_offline),    browser));
    gtk_box_append(GTK_BOX(box), menu_btn("Passwords",        G_CALLBACK(mi_passwords),  browser));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), menu_btn("Font Manager",     G_CALLBACK(mi_fonts),      browser));
    gtk_box_append(GTK_BOX(box), menu_btn("Settings",         G_CALLBACK(mi_settings),   browser));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), menu_btn("Fullscreen",       G_CALLBACK(mi_fullscreen), browser));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* Zoom row: [ − ] [ Reset ] [ + ] */
    GtkWidget *zoomrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *zout = gtk_button_new_with_label("\xe2\x88\x92");   /* − */
    GtkWidget *zrst = gtk_button_new_with_label("Reset");
    GtkWidget *zin  = gtk_button_new_with_label("+");
    gtk_widget_set_hexpand(zout, TRUE); gtk_button_set_has_frame(GTK_BUTTON(zout), FALSE);
    gtk_widget_set_hexpand(zrst, TRUE); gtk_button_set_has_frame(GTK_BUTTON(zrst), FALSE);
    gtk_widget_set_hexpand(zin,  TRUE); gtk_button_set_has_frame(GTK_BUTTON(zin),  FALSE);
    g_signal_connect(zout, "clicked", G_CALLBACK(mi_zoom_out),   browser);
    g_signal_connect(zrst, "clicked", G_CALLBACK(mi_zoom_reset), browser);
    g_signal_connect(zin,  "clicked", G_CALLBACK(mi_zoom_in),    browser);
    gtk_box_append(GTK_BOX(zoomrow), zout);
    gtk_box_append(GTK_BOX(zoomrow), zrst);
    gtk_box_append(GTK_BOX(zoomrow), zin);
    gtk_box_append(GTK_BOX(box), zoomrow);

    gtk_box_append(GTK_BOX(box), menu_btn("Find in Page",     G_CALLBACK(mi_find),       browser));
    gtk_box_append(GTK_BOX(box), menu_btn("Mute Tab",         G_CALLBACK(mi_mute),       browser));
    gtk_box_append(GTK_BOX(box), menu_btn("What's Running",   G_CALLBACK(mi_running),    browser));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), menu_btn("Account",          G_CALLBACK(mi_account),    browser));

    return pop;
}

void ui_menu_add_item(GtkWidget *menu, const gchar *label, GCallback callback) {
    if (!menu || !label) return;
    GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(menu));
    GtkWidget *b   = gtk_button_new_with_label(label);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    if (callback) g_signal_connect(b, "clicked", callback, NULL);
    gtk_box_append(GTK_BOX(box), b);
}

/* ── error page ──────────────────────────────────────────────────────────── */

gchar* ui_get_error_page(const gchar *url, const gchar *error_type, const gchar *message) {
    if (!url || !error_type) return NULL;
    const gchar *title = "Error", *desc = "An error occurred.";
    if (!strcmp(error_type, "not_found"))       { title = "Page Not Found";    desc = "The address could not be found."; }
    else if (!strcmp(error_type, "connection_error")) { title = "No Connection"; desc = "Check your internet connection."; }
    else if (!strcmp(error_type, "timeout"))    { title = "Timed Out";         desc = "The server took too long to respond."; }
    else if (!strcmp(error_type, "invalid_url")){ title = "Invalid URL";       desc = "That doesn't look like a valid address."; }
    if (message && *message) desc = message;
    return g_strdup_printf(
        "<!DOCTYPE html><html><head><title>%s</title>"
        "<style>body{font-family:system-ui;background:#0f0f13;color:#e2e8f0;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
        ".box{text-align:center;max-width:480px}"
        "h1{font-size:28px;margin-bottom:12px;color:#f87171}"
        "p{color:#94a3b8;line-height:1.6}"
        ".url{font-family:monospace;font-size:13px;color:#64748b;margin-top:16px;word-break:break-all}"
        "button{margin-top:24px;background:#7c6af7;color:#fff;border:none;padding:10px 24px;"
        "border-radius:8px;font-size:15px;cursor:pointer}</style></head>"
        "<body><div class='box'><h1>%s</h1><p>%s</p>"
        "<div class='url'>%s</div>"
        "<button onclick='history.back()'>Go Back</button></div></body></html>",
        title, title, desc, url);
}
