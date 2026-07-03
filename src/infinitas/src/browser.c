/**
 * Infinitas Browser - Browser Core Implementation
 */

#include "browser.h"
#include "ui.h"
#include "infinitas_scheme.h"
#include "infinitas_plugin_api.h"
#include "adblock.h"
#include <gdk/gdkkeysyms.h>
#include <libsoup/soup.h>
#include <string.h>

#define BROWSER_TITLE "Infinitas"

/* ── forward declarations ────────────────────────────────────────────────── */

static void renderer_free_func(gpointer r, gpointer unused);

typedef struct {
    InfinitasBrowser *browser;
    WebRenderer      *renderer;
} TabSignalData;

static void on_view_uri_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d);
static void on_view_title_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d);
static void on_view_progress(WebKitWebView *v, GParamSpec *p, TabSignalData *d);
static void on_view_loading(WebKitWebView *v, GParamSpec *p, TabSignalData *d);
static void on_nav_state_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d);
static void on_close_btn_clicked(GtkButton *button, InfinitasBrowser *browser);
static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent ev,
                                const gchar *uri, GError *err, TabSignalData *d);
static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint num, InfinitasBrowser *b);
static gboolean hibernation_tick(gpointer data);
static void browser_find_hide(InfinitasBrowser *b);
static gboolean browser_key_pressed(GtkEventControllerKey *c, guint kv, guint kc,
                                     GdkModifierType state, InfinitasBrowser *b);

static void browser_setup_toolbar(InfinitasBrowser *browser);
static void browser_setup_tabs(InfinitasBrowser *browser);
static void browser_setup_splitter(InfinitasBrowser *browser);
static void browser_apply_css(void);

/* ── tab groups (Opera-style) ────────────────────────────────────────────── */
typedef struct {
    gchar   *id;         /* stable, session-persisted identifier            */
    gchar   *name;       /* user-visible group name                         */
    gchar   *color;      /* one of the GRP_COLORS css class names           */
    gboolean collapsed;  /* collapsed => members hidden behind one chip     */
} TabGroup;

static const char *GRP_COLORS[] = {
    "grp-blue", "grp-red", "grp-green", "grp-amber",
    "grp-purple", "grp-pink", "grp-cyan", "grp-gray"
};
#define GRP_NCOLORS (int)(sizeof(GRP_COLORS)/sizeof(GRP_COLORS[0]))

static void   browser_apply_groups(InfinitasBrowser *b);
static void   group_free(gpointer p);
static TabGroup *group_find(InfinitasBrowser *b, const char *id);
static const char *tab_get_group(GtkWidget *page);
static void   tab_set_group(GtkWidget *page, const char *id);

/* ── CSS theme ───────────────────────────────────────────────────────────── */

static const gchar BROWSER_CSS[] =
    "window { background: #0f0f13; }"
    ".nav-bar { background: #1a1a24; padding: 5px 8px; border-bottom: 1px solid #2a2a3a; }"
    ".nav-bar button { background: transparent; border: none; border-radius: 8px;"
    "  color: #e2e8f0; min-width: 34px; min-height: 34px; padding: 0; }"
    ".nav-bar button:hover { background: #2a2a3a; }"
    ".nav-bar button:disabled { opacity: 0.3; }"
    ".nav-bar button:disabled:hover { background: transparent; }"
    ".addr-box { background: #0f0f13; border: 1px solid #2a2a3a; border-radius: 20px;"
    "  padding: 0 10px; }"
    ".addr-box:focus-within { border-color: #7c6af7; }"
    ".addr-box entry { background: transparent; border: none; box-shadow: none;"
    "  color: #e2e8f0; min-height: 32px; }"
    ".addr-box entry:focus { outline: none; box-shadow: none; }"
    ".lock-https { color: #4ade80; font-size: 13px; }"
    ".lock-http  { color: #94a3b8; font-size: 13px; }"
    ".account-signed-in { background: #7c6af7 !important; color: #fff !important;"
    "  border-radius: 17px !important; font-weight: 700; font-size: 14px; }"
    "progressbar trough { background: transparent; min-height: 3px; }"
    "progressbar progress { background: #7c6af7; min-height: 3px; border-radius: 0; }"
    "notebook { background: #0f0f13; }"
    "notebook > header { background: #1a1a24; border-bottom: 1px solid #2a2a3a; }"
    "notebook > header > tabs > tab { background: transparent; color: #64748b;"
    "  padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }"
    "notebook > header > tabs > tab:checked { color: #e2e8f0; border-bottom-color: #7c6af7; }"
    "notebook > header > tabs > tab:hover:not(:checked) { color: #94a3b8; }"
    /* tab-group chip styling */
    ".grp-indicator { margin-right: 3px; }"
    ".grp-dot { font-size: 11px; }"
    ".grp-name { font-size: 11px; font-weight: 700; margin-left: 2px; }"
    ".grp-blue   { color: #3b82f6; }"
    ".grp-red    { color: #ef4444; }"
    ".grp-green  { color: #22c55e; }"
    ".grp-amber  { color: #f59e0b; }"
    ".grp-purple { color: #a855f7; }"
    ".grp-pink   { color: #ec4899; }"
    ".grp-cyan   { color: #06b6d4; }"
    ".grp-gray   { color: #94a3b8; }";

/* ── free helpers ────────────────────────────────────────────────────────── */

static void renderer_free_func(gpointer r, gpointer unused) {
    (void)unused;
    renderer_free((WebRenderer *)r);
}

/* plugin browser API wrappers (C functions with the right signatures) */
static void api_navigate(const gchar *url, gpointer b) {
    browser_navigate((InfinitasBrowser*)b, url);
}
static void api_open_tab(const gchar *url, gpointer b) {
    browser_create_tab((InfinitasBrowser*)b, url);
}
static gchar* api_get_url(gpointer b) {
    return browser_get_current_url((InfinitasBrowser*)b);
}
static gchar* api_get_title(gpointer b) {
    return browser_get_current_title((InfinitasBrowser*)b);
}
static void api_eval_js(const gchar *js, gpointer b) {
    WebRenderer *r = browser_get_active_renderer((InfinitasBrowser*)b);
    if (r) webkit_web_view_evaluate_javascript(r->view, js, -1,
                                               NULL, NULL, NULL, NULL, NULL);
}
static void api_register_scheme_path(const gchar *prefix,
                                      gchar* (*handler)(const gchar*, gpointer),
                                      gpointer userdata, gpointer b) {
    (void)b;
    infinitas_register_plugin_path(prefix, handler, userdata);
}

/* ── tab signal callbacks ────────────────────────────────────────────────── */

static void sync_nav_buttons(InfinitasBrowser *b, WebKitWebView *v) {
    gtk_widget_set_sensitive(b->back_btn,    webkit_web_view_can_go_back(v));
    gtk_widget_set_sensitive(b->forward_btn, webkit_web_view_can_go_forward(v));
}

static void on_view_uri_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d) {
    (void)p;
    const gchar *uri = webkit_web_view_get_uri(v);
    g_free(d->renderer->current_url);
    d->renderer->current_url = g_strdup(uri ? uri : "");

    if (browser_get_active_renderer(d->browser) != d->renderer) return;

    /* address bar */
    if (d->browser->address_bar)
        gtk_editable_set_text(GTK_EDITABLE(d->browser->address_bar), uri ? uri : "");

    /* HTTPS lock */
    if (d->browser->lock_label) {
        gboolean is_https = uri && g_str_has_prefix(uri, "https://");
        gboolean is_internal = uri && g_str_has_prefix(uri, "infinitas://");
        if (is_internal || !uri || !*uri) {
            gtk_widget_set_visible(d->browser->lock_label, FALSE);
        } else {
            gtk_widget_set_visible(d->browser->lock_label, TRUE);
            gtk_label_set_text(GTK_LABEL(d->browser->lock_label),
                               is_https ? "\xf0\x9f\x94\x92" : "\xe2\x84\xb9");
            if (is_https) {
                gtk_widget_remove_css_class(d->browser->lock_label, "lock-http");
                gtk_widget_add_css_class(d->browser->lock_label, "lock-https");
            } else {
                gtk_widget_remove_css_class(d->browser->lock_label, "lock-https");
                gtk_widget_add_css_class(d->browser->lock_label, "lock-http");
            }
        }
    }

    /* record history (skip internal pages) */
    if (uri && *uri && !g_str_has_prefix(uri, "infinitas://")) {
        const gchar *title = d->renderer->page_title ? d->renderer->page_title : uri;
        history_add(d->browser->history, title, uri);
    }

    sync_nav_buttons(d->browser, v);
    browser_update_account_btn(d->browser);
}

static void on_view_title_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d) {
    (void)p;
    const gchar *title = webkit_web_view_get_title(v);
    g_free(d->renderer->page_title);
    d->renderer->page_title = g_strdup(title ? title : "New Tab");

    /* update window title if active */
    if (browser_get_active_renderer(d->browser) == d->renderer) {
        gchar *wt = g_strdup_printf("%s — Infinitas", d->renderer->page_title);
        gtk_window_set_title(GTK_WINDOW(d->browser->window), wt);
        g_free(wt);
    }

    /* update tab label */
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(d->browser->tab_widget));
    for (gint i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(d->browser->tab_widget), i);
        if (page != renderer_get_widget(d->renderer)) continue;
        GtkWidget *tb  = gtk_notebook_get_tab_label(GTK_NOTEBOOK(d->browser->tab_widget), page);
        GtkWidget *lbl = g_object_get_data(G_OBJECT(tb), "label");
        if (lbl) gtk_label_set_text(GTK_LABEL(lbl), d->renderer->page_title);
        break;
    }
}

static void on_view_favicon_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d) {
    (void)p;
    GdkTexture *icon = webkit_web_view_get_favicon(v);
    if (!icon) return;
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(d->browser->tab_widget));
    for (gint i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(d->browser->tab_widget), i);
        if (page != renderer_get_widget(d->renderer)) continue;
        GtkWidget *tb  = gtk_notebook_get_tab_label(GTK_NOTEBOOK(d->browser->tab_widget), page);
        GtkWidget *fav = g_object_get_data(G_OBJECT(tb), "favicon");
        if (fav) gtk_image_set_from_paintable(GTK_IMAGE(fav), GDK_PAINTABLE(icon));
        break;
    }
}

static void on_view_progress(WebKitWebView *v, GParamSpec *p, TabSignalData *d) {
    (void)p;
    if (browser_get_active_renderer(d->browser) != d->renderer) return;
    double prog = webkit_web_view_get_estimated_load_progress(v);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->browser->progress_bar), prog);
}

static void on_view_loading(WebKitWebView *v, GParamSpec *p, TabSignalData *d) {
    (void)p;
    if (browser_get_active_renderer(d->browser) != d->renderer) return;
    gboolean loading = webkit_web_view_is_loading(v);
    gtk_widget_set_visible(d->browser->progress_bar, loading);
    if (!loading) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->browser->progress_bar), 0.0);
    /* toggle refresh ↻ / stop × */
    gtk_button_set_label(GTK_BUTTON(d->browser->refresh_btn),
                         loading ? "\xc3\x97" : "\xe2\x86\xbb");
}

static void on_nav_state_changed(WebKitWebView *v, GParamSpec *p, TabSignalData *d) {
    (void)p;
    if (browser_get_active_renderer(d->browser) != d->renderer) return;
    sync_nav_buttons(d->browser, v);
}

static gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, InfinitasBrowser *browser) {
    (void)view; (void)browser;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;

    WebKitResponsePolicyDecision *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);

    /* If WebKit can't display this content inline, download it instead */
    if (!webkit_response_policy_decision_is_mime_type_supported(response)) {
        webkit_policy_decision_download(decision);
        return TRUE;
    }
    return FALSE;
}

static void on_download_started(WebKitNetworkSession *session, WebKitDownload *download, InfinitasBrowser *browser) {
    (void)session;
    download_manager_add(browser->download_manager, download);
}

/* ── permission requests (deny-by-default, explicit consent) ─────────────────
 * WebKit denies camera/mic/geolocation by default with no handler, which
 * silently breaks Meet/Maps. We prompt the user; the request is only granted
 * on an explicit "Allow" click. Closing the dialog = deny. */

static void perm_allow_cb(GtkButton *b, gpointer data) {
    WebKitPermissionRequest *req = data;
    webkit_permission_request_allow(req);
    g_object_set_data(G_OBJECT(req), "decided", GINT_TO_POINTER(1));
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}

static void perm_deny_cb(GtkButton *b, gpointer data) {
    WebKitPermissionRequest *req = data;
    webkit_permission_request_deny(req);
    g_object_set_data(G_OBJECT(req), "decided", GINT_TO_POINTER(1));
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}

/* If the dialog closes without a choice, default to DENY, then release our ref. */
static void perm_win_destroy_cb(GtkWidget *w, gpointer data) {
    (void)w;
    WebKitPermissionRequest *req = data;
    if (!g_object_get_data(G_OBJECT(req), "decided"))
        webkit_permission_request_deny(req);
    g_object_unref(req);
}

static gboolean on_permission_request(WebKitWebView *view, WebKitPermissionRequest *req,
                                      InfinitasBrowser *browser) {
    (void)view;
    const char *cap = NULL;

    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req)) {
        WebKitUserMediaPermissionRequest *um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
        gboolean vid = webkit_user_media_permission_is_for_video_device(um);
        gboolean aud = webkit_user_media_permission_is_for_audio_device(um);
        if (vid && aud)   cap = "use your camera and microphone";
        else if (vid)     cap = "use your camera";
        else if (aud)     cap = "use your microphone";
        else              cap = "access a media device";
    } else if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req)) {
        cap = "access your location";
    } else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req)) {
        cap = "show notifications";
    } else if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(req)) {
        cap = "read your clipboard";
    } else {
        /* Unknown / dangerous capability → deny by default, no prompt. */
        webkit_permission_request_deny(req);
        return TRUE;
    }

    /* Identify the requesting site (host of the active page). */
    WebRenderer *r = browser_get_active_renderer(browser);
    gchar *host = NULL;
    if (r && r->current_url) {
        GUri *uri = g_uri_parse(r->current_url, G_URI_FLAGS_NONE, NULL);
        if (uri) { host = g_strdup(g_uri_get_host(uri)); g_uri_unref(uri); }
    }
    if (!host || !*host) { g_free(host); host = g_strdup("This site"); }

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Permission Request");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(browser->window));
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    gtk_window_set_child(GTK_WINDOW(win), box);

    gchar *msg = g_strdup_printf("<b>%s</b> wants to %s.", host, cap);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), msg);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
    gtk_box_append(GTK_BOX(box), label);
    g_free(msg); g_free(host);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *deny = gtk_button_new_with_label("Deny");
    GtkWidget *allow = gtk_button_new_with_label("Allow");
    gtk_widget_add_css_class(allow, "suggested-action");
    gtk_box_append(GTK_BOX(btns), deny);
    gtk_box_append(GTK_BOX(btns), allow);
    gtk_box_append(GTK_BOX(box), btns);

    /* Keep the request alive until the user decides; default-deny on close. */
    g_object_ref(req);
    g_signal_connect(allow, "clicked", G_CALLBACK(perm_allow_cb), req);
    g_signal_connect(deny, "clicked", G_CALLBACK(perm_deny_cb), req);
    g_signal_connect(win, "destroy", G_CALLBACK(perm_win_destroy_cb), req);

    gtk_widget_grab_focus(deny);   /* safe default is focused */
    gtk_window_present(GTK_WINDOW(win));
    return TRUE;                    /* we handle the decision asynchronously */
}

/* Handle window.open / target=_blank: open the URL as a real tab instead of
 * silently dropping it. This is also what lets downloads from "_blank" links
 * (e.g. ISO mirror buttons) actually fire — the navigation has to proceed. */
static GtkWidget* on_view_create(WebKitWebView *view, WebKitNavigationAction *action,
                                 InfinitasBrowser *browser) {
    (void)view;
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const gchar *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    if (uri && *uri) {
        browser_create_tab(browser, uri);
    }
    return NULL;  /* we handled it ourselves */
}

/* ── session restore ─────────────────────────────────────────────────────── */

static gchar* session_file_path(void) {
    gchar *dir = g_build_filename(g_get_home_dir(), ".infinitas", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "session", NULL);
    g_free(dir);
    return path;
}

/* strip TAB/newline so a field can't corrupt the tab-separated session file */
static gchar *sess_field(const char *s) {
    gchar *d = g_strdup(s ? s : "");
    g_strdelimit(d, "\t\n\r", ' ');
    return d;
}

/* Session format v2 (tab-separated, backward compatible: a file that does not
 * start with the header is read as the legacy one-URL-per-line format):
 *   #INFINITAS-SESSION\tv2
 *   G\t<id>\t<name>\t<color>\t<collapsed 0|1>
 *   T\t<url>\t<group-id-or-empty>                                             */
static void browser_save_session(InfinitasBrowser *browser) {
    if (!browser || browser->pwa_mode || !browser->tabs) return;
    GString *buf = g_string_new("#INFINITAS-SESSION\tv2\n");

    if (browser->groups) {
        for (guint i = 0; i < browser->groups->len; i++) {
            TabGroup *g = g_ptr_array_index(browser->groups, i);
            gchar *nm = sess_field(g->name);
            gchar *cl = sess_field(g->color);
            g_string_append_printf(buf, "G\t%s\t%s\t%s\t%d\n",
                                   g->id, nm, cl, g->collapsed ? 1 : 0);
            g_free(nm); g_free(cl);
        }
    }

    for (guint i = 0; i < browser->tabs->len; i++) {
        WebRenderer *r = g_ptr_array_index(browser->tabs, i);
        const gchar *url = r ? r->current_url : NULL;
        /* skip internal pages and blanks — only restore real sites */
        if (!(url && *url && !g_str_has_prefix(url, "infinitas://"))) continue;
        const char *gid = r ? tab_get_group(renderer_get_widget(r)) : NULL;
        gchar *u = sess_field(url);
        g_string_append_printf(buf, "T\t%s\t%s\n", u, gid ? gid : "");
        g_free(u);
    }

    gchar *path = session_file_path();
    g_file_set_contents(path, buf->str, -1, NULL);
    g_free(path);
    g_string_free(buf, TRUE);
}

static gboolean browser_restore_session(InfinitasBrowser *browser) {
    if (!browser || browser->pwa_mode) return FALSE;
    gchar *path = session_file_path();
    gchar *contents = NULL;
    gboolean any = FALSE;
    if (g_file_get_contents(path, &contents, NULL, NULL) && contents) {
        gchar **lines = g_strsplit(contents, "\n", -1);

        if (g_str_has_prefix(contents, "#INFINITAS-SESSION")) {
            /* v2: create groups first, then tabs; suppress apply until done */
            browser->loading_session = TRUE;
            for (int i = 1; lines[i]; i++) {
                if (!*lines[i]) continue;
                gchar **f = g_strsplit(lines[i], "\t", -1);
                guint nf = g_strv_length(f);
                if (f[0][0] == 'G' && nf >= 5) {
                    TabGroup *g = g_new0(TabGroup, 1);
                    g->id        = g_strdup(f[1]);
                    g->name      = g_strdup(f[2]);
                    g->color     = g_strdup(f[3]);
                    g->collapsed = (g_strcmp0(f[4], "0") != 0);
                    g_ptr_array_add(browser->groups, g);
                } else if (f[0][0] == 'T' && nf >= 2 && *f[1]) {
                    WebRenderer *r = browser_create_tab(browser, f[1]);
                    if (r && nf >= 3 && *f[2])
                        tab_set_group(renderer_get_widget(r), f[2]);
                    any = TRUE;
                }
                g_strfreev(f);
            }
            browser->loading_session = FALSE;
            browser_apply_groups(browser);
        } else {
            /* legacy: one URL per line */
            for (int i = 0; lines[i]; i++)
                if (*lines[i]) { browser_create_tab(browser, lines[i]); any = TRUE; }
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
    return any;
}

/* ── public API ──────────────────────────────────────────────────────────── */

InfinitasBrowser* browser_new(GtkApplication *app, gboolean pwa_mode) {
    browser_apply_css();

    /* NOTE: forcing gtk-application-prefer-dark-theme=TRUE makes Google serve
     * its dark-mode content, which crashes WebKit's renderer here — so we do
     * NOT force it. Dark mode follows the system GTK theme (Leviathan ships
     * WhiteSur-Dark) and each site's own dark setting instead. */

    InfinitasBrowser *browser = g_new0(InfinitasBrowser, 1);

    browser->pwa_mode = pwa_mode;

    /* ── ONE persistent network session, created up front and shared by every
     * WebView ─────────────────────────────────────────────────────────────
     * This is the fix for "sign-in doesn't persist". We build an explicitly
     * persistent session with on-disk profile + cache directories under
     * ~/.infinitas, a SQLite cookie jar, and accept-all cookies — created
     * BEFORE anything else can touch the (fragile, implicit) default session.
     * Because every tab is created against this same session, cookies AND
     * site data (localStorage / IndexedDB / service workers) all land in the
     * same on-disk profile and survive a full quit + relaunch. */
    {
        gchar *base = g_build_filename(g_get_home_dir(), ".infinitas", NULL);
        gchar *profile_dir = g_build_filename(base, "profile", NULL);
        gchar *cache_dir   = g_build_filename(base, "cache", NULL);
        g_mkdir_with_parents(profile_dir, 0700);
        g_mkdir_with_parents(cache_dir, 0700);

        browser->session = webkit_network_session_new(profile_dir, cache_dir);

        WebKitWebsiteDataManager *dm =
            webkit_network_session_get_website_data_manager(browser->session);
        if (dm) webkit_website_data_manager_set_favicons_enabled(dm, TRUE);
        /* keep HTTP auth credentials across launches too */
        webkit_network_session_set_persistent_credential_storage_enabled(
            browser->session, TRUE);

        WebKitCookieManager *cm =
            webkit_network_session_get_cookie_manager(browser->session);
        if (cm) {
            gchar *cookie_path = g_build_filename(profile_dir, "cookies.sqlite", NULL);
            /* SQLite-backed jar → login cookies persist across launches. Set
             * this BEFORE the session is used, so it actually takes effect. */
            webkit_cookie_manager_set_persistent_storage(cm, cookie_path,
                WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
            /* accept 3rd-party cookies too — Google sign-in needs them */
            webkit_cookie_manager_set_accept_policy(cm,
                WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
            g_free(cookie_path);
        }
        g_free(profile_dir); g_free(cache_dir);

        /* encrypted password store lives alongside the profile */
        browser->passwords = passwords_new(base);
        g_free(base);
    }

    browser->settings  = settings_new(".infinitas");
    settings_load(browser->settings);
    browser->bookmarks = bookmarks_new(".infinitas");
    bookmarks_load(browser->bookmarks);
    browser->history   = history_new(".infinitas");
    history_init(browser->history);
    browser->offstore  = offstore_new(".infinitas");
    browser->protocols = protocol_store_new(".infinitas");
    browser->search    = search_new(NULL);

    /* extensions */
    gchar *ext_dir = g_build_filename(g_get_home_dir(), ".infinitas", "extensions", NULL);
    browser->extensions = extension_manager_new(ext_dir);
    g_free(ext_dir);
    extension_manager_load_all(browser->extensions);
    extension_manager_register_resources(browser->extensions);

    /* plugins — build the browser API struct first */
    static InfinitasBrowserAPI s_api;
    memset(&s_api, 0, sizeof(s_api));
    s_api.browser   = browser;
    s_api.navigate             = api_navigate;
    s_api.open_tab             = api_open_tab;
    s_api.get_url              = api_get_url;
    s_api.get_title            = api_get_title;
    s_api.eval_js              = api_eval_js;
    s_api.register_scheme_path = api_register_scheme_path;

    gchar *plug_dir = g_build_filename(g_get_home_dir(), ".infinitas", "plugins", NULL);
    browser->plugins = plugin_manager_new(plug_dir);
    g_free(plug_dir);
    plugin_manager_load_all(browser->plugins, &s_api);

    browser->tabs       = g_ptr_array_new_with_free_func(NULL);
    browser->tab_titles = g_ptr_array_new_with_free_func(g_free);
    browser->groups     = g_ptr_array_new_with_free_func(group_free);

    browser->download_manager = download_manager_new();
    download_manager_load_history(browser->download_manager);

    /* Native ad/tracker blocker — find the ruleset (installed or dev paths). */
    {
        const gchar *candidates[] = {
            "/usr/share/infinitas/adblock-rules.json",
            "/usr/local/share/infinitas/adblock-rules.json",
            "data/adblock-rules.json",
            "src/infinitas/data/adblock-rules.json",
            NULL
        };
        for (int i = 0; candidates[i]; i++) {
            if (g_file_test(candidates[i], G_FILE_TEST_EXISTS)) {
                adblock_init(candidates[i]);
                break;
            }
        }
    }

    browser->window = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(browser->window), app);
    gtk_window_set_default_size(GTK_WINDOW(browser->window),
                                BROWSER_WINDOW_WIDTH, BROWSER_WINDOW_HEIGHT);
    gtk_window_set_title(GTK_WINDOW(browser->window), BROWSER_TITLE);

    /* set window icon from installed PNG */
    GdkPixbuf *icon = gdk_pixbuf_new_from_file(
        "/usr/share/icons/hicolor/1024x1024/apps/infinitas.png", NULL);
    if (!icon) {
        /* fallback: look next to the binary */
        icon = gdk_pixbuf_new_from_file("data/infinitas.png", NULL);
    }
    if (icon) {
        gtk_window_set_icon_name(GTK_WINDOW(browser->window), "infinitas");
        g_object_unref(icon);
    }

    webkit_web_context_register_uri_scheme(
        webkit_web_context_get_default(), "infinitas",
        (WebKitURISchemeRequestCallback)infinitas_scheme_handle, browser, NULL);

    browser_setup_toolbar(browser);
    browser_update_account_btn(browser);
    browser_setup_tabs(browser);
    browser_setup_splitter(browser);

    /* In PWA mode, hide toolbar and tab headers but keep renderer visible */
    if (browser->pwa_mode) {
        gtk_widget_set_visible(browser->toolbar, FALSE);
        /* Hide tab header but keep notebook widget visible so renderer shows */
        if (browser->tab_widget) {
            gtk_notebook_set_show_tabs(GTK_NOTEBOOK(browser->tab_widget), FALSE);
        }
        /* Keep titlebar with close/minimize buttons */
    }

    /* periodically hibernate idle background tabs to reclaim RAM */
    g_timeout_add_seconds(60, hibernation_tick, browser);

    /* restore previous session, or open home if nothing to restore */
    if (!browser_restore_session(browser)) {
        const gchar *home = settings_get_string(browser->settings, "home_page");
        browser_create_tab(browser, home ? home : "infinitas://tab");
    }

    /* keyboard shortcuts */
    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(browser_key_pressed), browser);
    gtk_widget_add_controller(browser->window, kc);

    g_signal_connect_swapped(browser->window, "destroy",
                              G_CALLBACK(browser_free), browser);
    return browser;
}

void browser_free(InfinitasBrowser *browser) {
    if (!browser) return;
    /* persist open tabs before tearing them down */
    browser_save_session(browser);
    g_signal_handlers_disconnect_by_func(browser->window,
                                          G_CALLBACK(browser_free), browser);
    if (browser->tabs) {
        g_ptr_array_foreach(browser->tabs, renderer_free_func, NULL);
        g_ptr_array_free(browser->tabs, TRUE);
        browser->tabs = NULL;
    }
    if (browser->download_manager) {
        download_manager_free(browser->download_manager);
        browser->download_manager = NULL;
    }
    if (browser->tab_titles) {
        g_ptr_array_free(browser->tab_titles, TRUE);
        browser->tab_titles = NULL;
    }
    if (browser->groups) {
        g_ptr_array_free(browser->groups, TRUE);
        browser->groups = NULL;
    }
    settings_free(browser->settings);
    bookmarks_free(browser->bookmarks);
    history_free(browser->history);
    offstore_free(browser->offstore);
    protocol_store_free(browser->protocols);
    plugin_manager_free(browser->plugins);
    extension_manager_free(browser->extensions);
    search_free(browser->search);
    passwords_free(browser->passwords);
    if (browser->session) g_object_unref(browser->session);
    g_free(browser);
}

void browser_show(InfinitasBrowser *browser) {
    if (!browser || !browser->window) return;
    gtk_window_present(GTK_WINDOW(browser->window));
}

static void on_google_cookies(GObject *src, GAsyncResult *res, gpointer data) {
    InfinitasBrowser *b = data;
    GList *cookies = webkit_cookie_manager_get_cookies_finish(
        WEBKIT_COOKIE_MANAGER(src), res, NULL);

    gboolean signed_in = FALSE;
    for (GList *l = cookies; l; l = l->next) {
        SoupCookie *c = l->data;
        if (g_strcmp0(soup_cookie_get_name(c), "SAPISID") == 0) {
            signed_in = TRUE;
            break;
        }
    }
    g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);

    if (!b->account_btn) return;
    if (signed_in) {
        gtk_button_set_label(GTK_BUTTON(b->account_btn), "G");
        gtk_widget_add_css_class(b->account_btn, "account-signed-in");
        gtk_widget_set_tooltip_text(b->account_btn, "Google Account");
    } else {
        gtk_button_set_label(GTK_BUTTON(b->account_btn), "Sign in");
        gtk_widget_remove_css_class(b->account_btn, "account-signed-in");
        gtk_widget_set_tooltip_text(b->account_btn, "Sign in with Google");
    }
}

void browser_update_account_btn(InfinitasBrowser *browser) {
    if (!browser || !browser->account_btn) return;
    WebKitNetworkSession *ns = browser->session
        ? browser->session : webkit_network_session_get_default();
    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(ns);
    webkit_cookie_manager_get_cookies(cm, "https://accounts.google.com",
                                       NULL, on_google_cookies, browser);
}

/* ── tab groups: model ───────────────────────────────────────────────────── */

static void group_free(gpointer p) {
    TabGroup *g = p;
    if (!g) return;
    g_free(g->id); g_free(g->name); g_free(g->color);
    g_free(g);
}

static TabGroup *group_find(InfinitasBrowser *b, const char *id) {
    if (!id || !b->groups) return NULL;
    for (guint i = 0; i < b->groups->len; i++) {
        TabGroup *g = g_ptr_array_index(b->groups, i);
        if (g_strcmp0(g->id, id) == 0) return g;
    }
    return NULL;
}

/* A group id lives on the notebook *page* widget so it follows the tab. */
static const char *tab_get_group(GtkWidget *page) {
    return page ? g_object_get_data(G_OBJECT(page), "group-id") : NULL;
}
static void tab_set_group(GtkWidget *page, const char *id) {
    if (!page) return;
    g_object_set_data_full(G_OBJECT(page), "group-id",
                           id ? g_strdup(id) : NULL, g_free);
}

static guint group_member_count(InfinitasBrowser *b, const char *id) {
    guint n = 0;
    int pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->tab_widget));
    for (int i = 0; i < pages; i++) {
        GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->tab_widget), i);
        if (g_strcmp0(tab_get_group(pg), id) == 0) n++;
    }
    return n;
}

/* index of the first (representative) member page of a group, or -1 */
static int group_rep_index(InfinitasBrowser *b, const char *id) {
    int pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->tab_widget));
    for (int i = 0; i < pages; i++) {
        GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->tab_widget), i);
        if (g_strcmp0(tab_get_group(pg), id) == 0) return i;
    }
    return -1;
}

static void groups_prune_empty(InfinitasBrowser *b) {
    if (!b->groups) return;
    for (guint i = 0; i < b->groups->len; ) {
        TabGroup *g = g_ptr_array_index(b->groups, i);
        if (group_member_count(b, g->id) == 0)
            g_ptr_array_remove_index(b->groups, i);   /* frees via free func */
        else
            i++;
    }
}

/* Re-apply group visuals (colors, chip text, collapse) across the tab strip. */
static void browser_apply_groups(InfinitasBrowser *b) {
    if (!b || !b->tab_widget || b->loading_session) return;
    groups_prune_empty(b);

    GtkNotebook *nb = GTK_NOTEBOOK(b->tab_widget);
    int pages = gtk_notebook_get_n_pages(nb);
    int active = gtk_notebook_get_current_page(nb);
    int switch_to = -1;

    for (int i = 0; i < pages; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
        GtkWidget *tab_box = gtk_notebook_get_tab_label(nb, page);
        if (!tab_box) continue;
        GtkWidget *ind  = g_object_get_data(G_OBJECT(tab_box), "grp-indicator");
        GtkWidget *name = g_object_get_data(G_OBJECT(tab_box), "grp-name");
        if (!ind || !name) continue;

        const char *gid = tab_get_group(page);
        TabGroup *g = gid ? group_find(b, gid) : NULL;
        if (gid && !g) { tab_set_group(page, NULL); gid = NULL; }  /* stale */

        /* reset palette classes on the indicator */
        for (int c = 0; c < GRP_NCOLORS; c++)
            gtk_widget_remove_css_class(ind, GRP_COLORS[c]);

        if (!g) {
            gtk_widget_set_visible(ind, FALSE);
            gtk_widget_set_visible(name, FALSE);
            gtk_widget_set_visible(tab_box, TRUE);
            continue;
        }

        gtk_widget_add_css_class(ind, g->color);
        gtk_widget_set_visible(ind, TRUE);

        gboolean is_rep = (group_rep_index(b, g->id) == i);
        if (g->collapsed) {
            if (is_rep) {
                gtk_widget_set_visible(tab_box, TRUE);
                guint n = group_member_count(b, g->id);
                gchar *t = g_strdup_printf("%s (%u) \xe2\x96\xb8", g->name, n); /* ▸ */
                gtk_label_set_text(GTK_LABEL(name), t);
                g_free(t);
                gtk_widget_set_visible(name, TRUE);
            } else {
                gtk_widget_set_visible(tab_box, FALSE);   /* hidden behind chip */
                if (i == active) switch_to = group_rep_index(b, g->id);
            }
        } else {
            gtk_widget_set_visible(tab_box, TRUE);
            if (is_rep) {
                gchar *t = g_strdup_printf("%s \xe2\x96\xbe", g->name);  /* ▾ */
                gtk_label_set_text(GTK_LABEL(name), t);
                g_free(t);
                gtk_widget_set_visible(name, TRUE);
            } else {
                gtk_widget_set_visible(name, FALSE);      /* just the color dot */
            }
        }
    }

    if (switch_to >= 0)
        gtk_notebook_set_current_page(nb, switch_to);
}

/* ── tab groups: right-click menu ────────────────────────────────────────── */

static int page_index_for_tablabel(InfinitasBrowser *b, GtkWidget *tab_box) {
    int pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->tab_widget));
    for (int i = 0; i < pages; i++) {
        GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->tab_widget), i);
        if (gtk_notebook_get_tab_label(GTK_NOTEBOOK(b->tab_widget), pg) == tab_box)
            return i;
    }
    return -1;
}

static void on_popover_closed(GtkPopover *pop, gpointer data) {
    (void)data;
    gtk_widget_unparent(GTK_WIDGET(pop));   /* created via set_parent → clean up */
}

static void on_group_menu_action(GtkButton *btn, gpointer data) {
    (void)data;
    InfinitasBrowser *b = g_object_get_data(G_OBJECT(btn), "browser");
    GtkWidget *tab_box  = g_object_get_data(G_OBJECT(btn), "tabbox");
    const char *act     = g_object_get_data(G_OBJECT(btn), "action");
    GtkWidget *pop      = g_object_get_data(G_OBJECT(btn), "popover");
    if (!b || !tab_box || !act) goto done;

    int idx = page_index_for_tablabel(b, tab_box);
    if (idx < 0) goto done;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->tab_widget), idx);

    if (g_strcmp0(act, "new") == 0) {
        TabGroup *g = g_new0(TabGroup, 1);
        g->id    = g_strdup_printf("g%08x%04x", g_random_int(),
                                   (guint)(g_get_monotonic_time() & 0xffff));
        g->name  = g_strdup_printf("Group %u", b->groups->len + 1);
        g->color = g_strdup(GRP_COLORS[b->groups->len % GRP_NCOLORS]);
        g->collapsed = FALSE;
        g_ptr_array_add(b->groups, g);
        tab_set_group(page, g->id);
    } else if (g_strcmp0(act, "add") == 0) {
        const char *gid = g_object_get_data(G_OBJECT(btn), "gid");
        tab_set_group(page, gid);
    } else if (g_strcmp0(act, "remove") == 0) {
        tab_set_group(page, NULL);
    } else if (g_strcmp0(act, "collapse") == 0) {
        TabGroup *g = group_find(b, tab_get_group(page));
        if (g) g->collapsed = !g->collapsed;
    }
    browser_apply_groups(b);

done:
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static GtkWidget *menu_row(InfinitasBrowser *b, GtkWidget *tab_box, GtkWidget *pop,
                           const char *label, const char *action, const char *gid) {
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "flat");
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(btn));
    if (GTK_IS_LABEL(child)) gtk_label_set_xalign(GTK_LABEL(child), 0.0);
    g_object_set_data(G_OBJECT(btn), "browser", b);
    g_object_set_data(G_OBJECT(btn), "tabbox", tab_box);
    g_object_set_data(G_OBJECT(btn), "popover", pop);
    g_object_set_data(G_OBJECT(btn), "action", (gpointer)action);
    if (gid) g_object_set_data_full(G_OBJECT(btn), "gid", g_strdup(gid), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_group_menu_action), NULL);
    return btn;
}

static void on_tab_right_click(GtkGestureClick *gc, int n_press,
                               double x, double y, gpointer data) {
    (void)n_press;
    GtkWidget *tab_box = data;
    InfinitasBrowser *b = g_object_get_data(G_OBJECT(tab_box), "browser");
    if (!b) return;

    int idx = page_index_for_tablabel(b, tab_box);
    GtkWidget *page = idx >= 0
        ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->tab_widget), idx) : NULL;
    const char *cur_gid = tab_get_group(page);

    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, tab_box);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), TRUE);
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", G_CALLBACK(on_popover_closed), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 4);  gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 4);    gtk_widget_set_margin_bottom(box, 4);

    gtk_box_append(GTK_BOX(box),
        menu_row(b, tab_box, pop, "Add to new group", "new", NULL));

    /* "Add to <group>" for every other existing group */
    for (guint i = 0; i < b->groups->len; i++) {
        TabGroup *g = g_ptr_array_index(b->groups, i);
        if (g_strcmp0(g->id, cur_gid) == 0) continue;
        gchar *lbl = g_strdup_printf("Add to \xe2\x80\x9c%s\xe2\x80\x9d", g->name);
        gtk_box_append(GTK_BOX(box), menu_row(b, tab_box, pop, lbl, "add", g->id));
        g_free(lbl);
    }

    if (cur_gid) {
        TabGroup *g = group_find(b, cur_gid);
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(box), menu_row(b, tab_box, pop,
            (g && g->collapsed) ? "Expand group" : "Collapse group", "collapse", NULL));
        gtk_box_append(GTK_BOX(box), menu_row(b, tab_box, pop,
            "Remove from group", "remove", NULL));
    }

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* Left-click on the colored chip toggles collapse (Opera-style). */
static void on_group_chip_click(GtkGestureClick *gc, int n_press,
                                double x, double y, gpointer data) {
    (void)n_press; (void)x; (void)y;
    GtkWidget *tab_box = data;
    InfinitasBrowser *b = g_object_get_data(G_OBJECT(tab_box), "browser");
    if (!b) return;
    int idx = page_index_for_tablabel(b, tab_box);
    if (idx < 0) return;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->tab_widget), idx);
    TabGroup *g = group_find(b, tab_get_group(page));
    if (!g) return;
    g->collapsed = !g->collapsed;
    browser_apply_groups(b);
    gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
}

WebRenderer* browser_create_tab(InfinitasBrowser *browser, const gchar *url) {
    if (!browser || !browser->tabs) return NULL;

    WebRenderer *renderer = renderer_new(browser->pwa_mode, browser->session);

    /* hook signals */
    TabSignalData *sig = g_new(TabSignalData, 1);
    sig->browser  = browser;
    sig->renderer = renderer;
    g_object_set_data_full(G_OBJECT(renderer->view), "tab-signal-data", sig, g_free);
    g_signal_connect(renderer->view, "notify::uri",
                     G_CALLBACK(on_view_uri_changed), sig);
    g_signal_connect(renderer->view, "decide-policy",
                     G_CALLBACK(on_decide_policy), browser);
    g_signal_connect(renderer->view, "create",
                     G_CALLBACK(on_view_create), browser);
    g_signal_connect(renderer->view, "permission-request",
                     G_CALLBACK(on_permission_request), browser);

    WebKitNetworkSession *session = webkit_web_view_get_network_session(renderer->view);
    if (session) {
        /* connect once per session — guard against duplicate connects */
        if (!g_object_get_data(G_OBJECT(session), "infinitas-dl-hooked")) {
            g_signal_connect(session, "download-started",
                             G_CALLBACK(on_download_started), browser);
            g_object_set_data(G_OBJECT(session), "infinitas-dl-hooked", GINT_TO_POINTER(1));
        }
    }
    g_signal_connect(renderer->view, "notify::title",
                     G_CALLBACK(on_view_title_changed), sig);
    g_signal_connect(renderer->view, "notify::favicon",
                     G_CALLBACK(on_view_favicon_changed), sig);
    g_signal_connect(renderer->view, "notify::estimated-load-progress",
                     G_CALLBACK(on_view_progress), sig);
    g_signal_connect(renderer->view, "notify::is-loading",
                     G_CALLBACK(on_view_loading), sig);
    g_signal_connect(renderer->view, "notify::can-go-back",
                     G_CALLBACK(on_nav_state_changed), sig);
    g_signal_connect(renderer->view, "notify::can-go-forward",
                     G_CALLBACK(on_nav_state_changed), sig);
    g_signal_connect(renderer->view, "load-failed",
                     G_CALLBACK(on_load_failed), sig);

    /* extensions: inject bridge + content scripts */
    WebKitUserContentManager *mgr =
        webkit_web_view_get_user_content_manager(renderer->view);
    adblock_register_manager(mgr);
    extension_manager_inject(browser->extensions, renderer->view, mgr);
    extension_manager_register_handlers(browser->extensions, mgr, browser);

    /* password manager: detect login forms, autofill, offer save (blocks cards) */
    passwords_attach(browser->passwords, renderer->view, mgr, browser);

    /* apply saved font */
    const gchar *font = settings_get_string(browser->settings, "font_family");
    if (font && *font) renderer_apply_font(renderer, font);

    /* tab widget: [favicon] [title] [×] */
    GtkWidget *favicon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(favicon), 16);

    GtkWidget *tab_label  = gtk_label_new("New Tab");
    gtk_label_set_width_chars(GTK_LABEL(tab_label), 12);     /* min width so it doesn't collapse to "..." */
    gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 20);
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(tab_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(tab_label), 0.0);

    GtkWidget *close_btn = gtk_button_new_with_label("\xc3\x97");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_set_size_request(close_btn, 24, 24);

    /* group chip: [●][Name ▾] — hidden until this tab joins a group */
    GtkWidget *grp_indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(grp_indicator, "grp-indicator");
    GtkWidget *grp_dot  = gtk_label_new("\xe2\x97\x8f");  /* ● */
    gtk_widget_add_css_class(grp_dot, "grp-dot");
    GtkWidget *grp_name = gtk_label_new("");
    gtk_widget_add_css_class(grp_name, "grp-name");
    gtk_box_append(GTK_BOX(grp_indicator), grp_dot);
    gtk_box_append(GTK_BOX(grp_indicator), grp_name);
    gtk_widget_set_visible(grp_indicator, FALSE);
    gtk_widget_set_visible(grp_name, FALSE);

    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(tab_box), grp_indicator);
    gtk_box_append(GTK_BOX(tab_box), favicon);
    gtk_box_append(GTK_BOX(tab_box), tab_label);
    gtk_box_append(GTK_BOX(tab_box), close_btn);

    /* stash label + favicon so title/icon updates don't depend on child order */
    g_object_set_data(G_OBJECT(tab_box), "label", tab_label);
    g_object_set_data(G_OBJECT(tab_box), "favicon", favicon);
    g_object_set_data(G_OBJECT(tab_box), "grp-indicator", grp_indicator);
    g_object_set_data(G_OBJECT(tab_box), "grp-name", grp_name);
    g_object_set_data(G_OBJECT(tab_box), "browser", browser);

    /* right-click a tab → group menu; click the chip → collapse/expand */
    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "pressed", G_CALLBACK(on_tab_right_click), tab_box);
    gtk_widget_add_controller(tab_box, GTK_EVENT_CONTROLLER(rc));

    GtkGesture *cc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cc), GDK_BUTTON_PRIMARY);
    g_signal_connect(cc, "pressed", G_CALLBACK(on_group_chip_click), tab_box);
    gtk_widget_add_controller(grp_indicator, GTK_EVENT_CONTROLLER(cc));

    gint tab_id = (gint)browser->tabs->len;
    g_ptr_array_add(browser->tabs, renderer);
    g_ptr_array_add(browser->tab_titles, g_strdup("New Tab"));

    gtk_notebook_append_page(GTK_NOTEBOOK(browser->tab_widget),
                             renderer_get_widget(renderer), tab_box);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_btn_clicked), browser);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(browser->tab_widget), tab_id);

    browser_apply_groups(browser);   /* refresh group chips/representatives */

    if (url) renderer_load_url(renderer, url);
    return renderer;
}

void browser_close_tab(InfinitasBrowser *browser, gint index) {
    if (!browser || !browser->tabs) return;
    if ((gint)browser->tabs->len <= 1) return;
    WebRenderer *r = g_ptr_array_index(browser->tabs, index);
    gtk_notebook_remove_page(GTK_NOTEBOOK(browser->tab_widget), index);
    g_ptr_array_remove_index(browser->tabs, index);
    g_ptr_array_remove_index(browser->tab_titles, index);
    renderer_free(r);
    browser_apply_groups(browser);   /* prune emptied groups, refresh chips */
}

void browser_switch_tab(InfinitasBrowser *browser, gint index) {
    if (!browser || !browser->tab_widget) return;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(browser->tab_widget), index);
    browser->active_tab = index;
}

gint browser_get_active_tab(InfinitasBrowser *browser) {
    if (!browser || !browser->tab_widget) return -1;
    return gtk_notebook_get_current_page(GTK_NOTEBOOK(browser->tab_widget));
}

WebRenderer* browser_get_active_renderer(InfinitasBrowser *browser) {
    if (!browser || !browser->tabs) return NULL;
    gint i = browser_get_active_tab(browser);
    if (i < 0 || i >= (gint)browser->tabs->len) return NULL;
    return g_ptr_array_index(browser->tabs, i);
}

void browser_navigate(InfinitasBrowser *browser, const gchar *input) {
    if (!browser || !input) return;
    gchar *trimmed = g_strstrip(g_strdup(input));
    if (trimmed && *trimmed) {
        gchar *url = search_process_input(browser->search, trimmed);
        /* let plugins veto the navigation */
        if (url && plugin_manager_fire_navigate(browser->plugins, url)) {
            WebRenderer *r = browser_get_active_renderer(browser);
            if (r) renderer_load_url(r, url);
        }
        g_free(url);
    }
    g_free(trimmed);
}

void browser_go_home(InfinitasBrowser *browser) {
    if (!browser) return;
    const gchar *home = settings_get_string(browser->settings, "home_page");
    WebRenderer *r = browser_get_active_renderer(browser);
    if (r) renderer_load_url(r, home ? home : "infinitas://tab");
}

void browser_reload(InfinitasBrowser *browser) {
    WebRenderer *r = browser_get_active_renderer(browser);
    if (!r) return;
    if (webkit_web_view_is_loading(r->view))
        webkit_web_view_stop_loading(r->view);
    else
        renderer_reload(r);
}

void browser_go_back(InfinitasBrowser *browser) {
    WebRenderer *r = browser_get_active_renderer(browser);
    if (r) renderer_go_back(r);
}

void browser_go_forward(InfinitasBrowser *browser) {
    WebRenderer *r = browser_get_active_renderer(browser);
    if (r) renderer_go_forward(r);
}

void browser_toggle_fullscreen(InfinitasBrowser *browser) {
    if (!browser || !browser->window) return;
    if (gtk_window_is_fullscreen(GTK_WINDOW(browser->window)))
        gtk_window_unfullscreen(GTK_WINDOW(browser->window));
    else
        gtk_window_fullscreen(GTK_WINDOW(browser->window));
}

void browser_add_bookmark(InfinitasBrowser *browser) {
    if (!browser) return;
    WebRenderer *r = browser_get_active_renderer(browser);
    if (!r) return;
    gchar *url   = renderer_get_current_url(r);
    gchar *title = renderer_get_page_title(r);
    if (url && *url && title) {
        Bookmark *bm = bookmarks_add(browser->bookmarks, title, url);
        if (bm) {
            g_free(bm->title); g_free(bm->url); g_free(bm->created); g_free(bm);
        }
        /* encrypt and archive the page for offline access */
        offstore_capture(browser->offstore, r->view, url, title);
        g_print("[INFO] Bookmarked + offline-archived: %s\n", url);
    }
    g_free(url); g_free(title);
}

gchar* browser_get_current_url(InfinitasBrowser *browser) {
    WebRenderer *r = browser_get_active_renderer(browser);
    return r ? renderer_get_current_url(r) : NULL;
}

gchar* browser_get_current_title(InfinitasBrowser *browser) {
    WebRenderer *r = browser_get_active_renderer(browser);
    return r ? renderer_get_page_title(r) : NULL;
}

/* ── private callbacks ───────────────────────────────────────────────────── */

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent ev,
                                const gchar *uri, GError *err, TabSignalData *d) {
    (void)ev; (void)err;
    if (!uri || g_str_has_prefix(uri, "infinitas://")) return FALSE;

    gchar *cached = offstore_retrieve(d->browser->offstore, uri);
    if (!cached) return FALSE; /* no offline copy — show WebKit error page */

    /* inject an offline banner before </body> */
    const gchar *bend = strstr(cached, "</body>");
    if (!bend) bend = strstr(cached, "</BODY>");
    gchar *html;
    if (bend) {
        gchar *before = g_strndup(cached, (gsize)(bend - cached));
        html = g_strdup_printf(
            "%s"
            "<div style='position:fixed;bottom:0;left:0;right:0;"
            "background:#92400e;color:#fef3c7;padding:9px 18px;"
            "font-family:system-ui;font-size:13px;z-index:2147483647;"
            "display:flex;align-items:center;gap:10px'>"
            "<span style='font-size:18px'>&#x1F4E6;</span>"
            "<strong>Offline mode</strong> &mdash; cached version, no internet"
            "</div>"
            "%s", before, bend);
        g_free(before);
    } else {
        html = cached;
        cached = NULL;
    }

    webkit_web_view_load_html(v, html, uri);
    g_free(html);
    g_free(cached);
    return TRUE; /* error handled */
}

static void on_close_btn_clicked(GtkButton *button, InfinitasBrowser *browser) {
    GtkWidget *tab_box = gtk_widget_get_parent(GTK_WIDGET(button));
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser->tab_widget));
    for (gint i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser->tab_widget), i);
        GtkWidget *lbl  = gtk_notebook_get_tab_label(GTK_NOTEBOOK(browser->tab_widget), page);
        if (lbl == tab_box) { browser_close_tab(browser, i); return; }
    }
}

/* ── tab hibernation (reclaim RAM from idle background tabs) ──────────────── */

#define HIBERNATE_IDLE_SECONDS 600   /* sleep background tabs idle > 10 min */

/* Put a background tab to sleep: swap its heavy document for a tiny placeholder,
 * freeing the DOM/JS heap of its web process. The real URL is stored for wake. */
static void hibernate_tab(WebRenderer *r) {
    if (!r || r->hibernated || !r->view) return;
    const gchar *url = r->current_url;
    if (!url || !*url || g_str_has_prefix(url, "infinitas://")) return; /* nothing heavy to free */

    g_free(r->hibernated_url);
    r->hibernated_url = g_strdup(url);
    r->hibernated = TRUE;

    gchar *title = g_markup_escape_text(r->page_title ? r->page_title : "Sleeping", -1);
    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>\xF0\x9F\x92\xA4 %s</title>"
        "<style>body{margin:0;height:100vh;display:flex;align-items:center;justify-content:center;"
        "background:#0f0f13;color:#64748b;font-family:system-ui,sans-serif}"
        "div{text-align:center;font-size:15px}small{color:#475569}</style></head>"
        "<body><div>\xF0\x9F\x92\xA4 Tab sleeping to save memory<br>"
        "<small>Switch back to reload it</small></div></body></html>", title);
    webkit_web_view_load_html(r->view, html, r->hibernated_url);
    g_free(html); g_free(title);
}

/* Wake a sleeping tab: reload its stored URL. */
static void restore_tab(WebRenderer *r) {
    if (!r || !r->hibernated) return;
    r->hibernated = FALSE;
    r->last_active = g_get_monotonic_time();
    if (r->hibernated_url) {
        renderer_load_url(r, r->hibernated_url);
        g_free(r->hibernated_url);
        r->hibernated_url = NULL;
    }
}

static gboolean hibernation_tick(gpointer data) {
    InfinitasBrowser *b = data;
    if (!b || !b->tabs) return G_SOURCE_CONTINUE;
    gint active = browser_get_active_tab(b);
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < b->tabs->len; i++) {
        if ((gint)i == active) continue;                    /* never sleep the foreground tab */
        WebRenderer *r = g_ptr_array_index(b->tabs, i);
        if (!r || r->hibernated || !r->view) continue;
        if (webkit_web_view_is_playing_audio(r->view)) continue;  /* don't sleep audio */
        if ((now - r->last_active) / G_USEC_PER_SEC > HIBERNATE_IDLE_SECONDS)
            hibernate_tab(r);
    }
    return G_SOURCE_CONTINUE;
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint num, InfinitasBrowser *b) {
    (void)nb; (void)page;
    /* stamp the tab we're leaving so its idle clock starts now */
    if (b->active_tab >= 0 && b->tabs && b->active_tab < (gint)b->tabs->len) {
        WebRenderer *old = g_ptr_array_index(b->tabs, b->active_tab);
        if (old) old->last_active = g_get_monotonic_time();
    }
    b->active_tab = (gint)num;
    WebRenderer *r = browser_get_active_renderer(b);
    if (!r) return;
    r->last_active = g_get_monotonic_time();
    if (r->hibernated) restore_tab(r);   /* wake on return */
    if (b->address_bar) {
        const gchar *uri = r->current_url ? r->current_url : "";
        gtk_editable_set_text(GTK_EDITABLE(b->address_bar), uri);
    }
    sync_nav_buttons(b, r->view);
    gboolean loading = webkit_web_view_is_loading(r->view);
    gtk_widget_set_visible(b->progress_bar, loading);
    gtk_button_set_label(GTK_BUTTON(b->refresh_btn),
                         loading ? "\xc3\x97" : "\xe2\x86\xbb");
}

static gboolean browser_key_pressed(GtkEventControllerKey *c, guint kv, guint kc,
                                     GdkModifierType state, InfinitasBrowser *b) {
    (void)c; (void)kc;
    gboolean ctrl  = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK)   != 0;
    switch (kv) {
        case GDK_KEY_F5:    browser_reload(b);          return TRUE;
        case GDK_KEY_F11:   browser_toggle_fullscreen(b); return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:
            if (ctrl) { WebRenderer *r = browser_get_active_renderer(b);
                if (r && r->view) {
                    double z = webkit_web_view_get_zoom_level(r->view) + 0.1;
                    if (z > 3.0) z = 3.0;
                    webkit_web_view_set_zoom_level(r->view, z);
                } return TRUE; } break;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
            if (ctrl) { WebRenderer *r = browser_get_active_renderer(b);
                if (r && r->view) {
                    double z = webkit_web_view_get_zoom_level(r->view) - 0.1;
                    if (z < 0.3) z = 0.3;
                    webkit_web_view_set_zoom_level(r->view, z);
                } return TRUE; } break;
        case GDK_KEY_0:
            if (ctrl) { WebRenderer *r = browser_get_active_renderer(b);
                if (r && r->view) webkit_web_view_set_zoom_level(r->view, 1.0);
                return TRUE; } break;
        case GDK_KEY_m:
            if (ctrl) { WebRenderer *r = browser_get_active_renderer(b);
                if (r && r->view)
                    webkit_web_view_set_is_muted(r->view,
                        !webkit_web_view_get_is_muted(r->view));
                return TRUE; } break;
        case GDK_KEY_f: if (ctrl) { browser_find_show(b); return TRUE; } break;
        case GDK_KEY_Escape:
            if (b->find_bar && gtk_widget_get_visible(b->find_bar)) {
                browser_find_hide(b); return TRUE;
            } break;
        case GDK_KEY_l: if (ctrl) { gtk_widget_grab_focus(b->address_bar); return TRUE; } break;
        case GDK_KEY_t: if (ctrl) { browser_create_tab(b, "infinitas://tab"); return TRUE; } break;
        case GDK_KEY_w: if (ctrl) { browser_close_tab(b, browser_get_active_tab(b)); return TRUE; } break;
        case GDK_KEY_r: if (ctrl && shift) { browser_reload(b); return TRUE; } break;
        case GDK_KEY_F12: {
            WebRenderer *r = browser_get_active_renderer(b);
            if (r) renderer_toggle_inspector(r);
            return TRUE;
        }
        case GDK_KEY_i:
        case GDK_KEY_j: if (ctrl && shift) {
            WebRenderer *r = browser_get_active_renderer(b);
            if (r) renderer_toggle_inspector(r);
            return TRUE;
        } break;
        case GDK_KEY_d: if (ctrl) { browser_add_bookmark(b); return TRUE; } break;
        default: break;
    }
    return FALSE;
}

/* ── setup helpers ───────────────────────────────────────────────────────── */

static void browser_apply_css(void) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, BROWSER_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);
}

static void browser_setup_toolbar(InfinitasBrowser *browser) {
    browser->toolbar = ui_create_toolbar(browser);
}

static void on_new_tab_btn_clicked(GtkButton *b, InfinitasBrowser *browser) {
    (void)b;
    browser_create_tab(browser, "infinitas://tab");
}

static void browser_setup_tabs(InfinitasBrowser *browser) {
    browser->tab_widget = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(browser->tab_widget), TRUE);
    g_signal_connect(browser->tab_widget, "switch-page",
                     G_CALLBACK(on_tab_switched), browser);

    /* dedicated "+" new-tab button, pinned to the LEFT of the tab strip */
    GtkWidget *plus = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(plus, "flat");
    gtk_widget_set_tooltip_text(plus, "New tab (Ctrl+T)");
    g_signal_connect(plus, "clicked", G_CALLBACK(on_new_tab_btn_clicked), browser);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(browser->tab_widget), plus, GTK_PACK_START);

    /* Cookie persistence + favicons are configured once on the shared
     * persistent session in browser_new(); nothing to do here. */
}

/* ── find in page (Ctrl+F) ───────────────────────────────────────────────── */

static WebKitFindController* active_find_controller(InfinitasBrowser *b) {
    WebRenderer *r = browser_get_active_renderer(b);
    return (r && r->view) ? webkit_web_view_get_find_controller(r->view) : NULL;
}

static void find_do_search(InfinitasBrowser *b) {
    WebKitFindController *fc = active_find_controller(b);
    if (!fc || !b->find_entry) return;
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(b->find_entry));
    if (!text || !*text) { webkit_find_controller_search_finish(fc); return; }
    webkit_find_controller_search(fc, text,
        WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND,
        G_MAXUINT);
}

static void on_find_changed(GtkEditable *e, InfinitasBrowser *b) { (void)e; find_do_search(b); }
static void on_find_next(GtkButton *btn, InfinitasBrowser *b) {
    (void)btn; WebKitFindController *fc = active_find_controller(b);
    if (fc) webkit_find_controller_search_next(fc);
}
static void on_find_prev(GtkButton *btn, InfinitasBrowser *b) {
    (void)btn; WebKitFindController *fc = active_find_controller(b);
    if (fc) webkit_find_controller_search_previous(fc);
}
static void on_find_entry_activate(GtkEntry *e, InfinitasBrowser *b) { (void)e; on_find_next(NULL, b); }

static void browser_find_hide(InfinitasBrowser *b) {
    if (b->find_bar) gtk_widget_set_visible(b->find_bar, FALSE);
    WebKitFindController *fc = active_find_controller(b);
    if (fc) webkit_find_controller_search_finish(fc);
}
static void on_find_close(GtkButton *btn, InfinitasBrowser *b) { (void)btn; browser_find_hide(b); }

void browser_find_show(InfinitasBrowser *b) {
    if (!b || !b->find_bar) return;
    gtk_widget_set_visible(b->find_bar, TRUE);
    gtk_widget_grab_focus(b->find_entry);
}

static void browser_setup_splitter(InfinitasBrowser *browser) {
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(main_box), browser->toolbar);

    /* find-in-page bar (hidden until Ctrl+F) — sits below the toolbar */
    browser->find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(browser->find_bar, 6);
    gtk_widget_set_margin_end(browser->find_bar, 6);
    gtk_widget_set_margin_top(browser->find_bar, 4);
    gtk_widget_set_margin_bottom(browser->find_bar, 4);
    browser->find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(browser->find_entry), "Find in page\xe2\x80\xa6");
    gtk_widget_set_hexpand(browser->find_entry, TRUE);
    GtkWidget *fprev  = gtk_button_new_with_label("\xe2\x86\x91");  /* ↑ */
    GtkWidget *fnext  = gtk_button_new_with_label("\xe2\x86\x93");  /* ↓ */
    GtkWidget *fclose = gtk_button_new_with_label("\xe2\x9c\x95");  /* ✕ */
    g_signal_connect(browser->find_entry, "changed",  G_CALLBACK(on_find_changed), browser);
    g_signal_connect(browser->find_entry, "activate", G_CALLBACK(on_find_entry_activate), browser);
    g_signal_connect(fprev,  "clicked", G_CALLBACK(on_find_prev),  browser);
    g_signal_connect(fnext,  "clicked", G_CALLBACK(on_find_next),  browser);
    g_signal_connect(fclose, "clicked", G_CALLBACK(on_find_close), browser);
    gtk_box_append(GTK_BOX(browser->find_bar), browser->find_entry);
    gtk_box_append(GTK_BOX(browser->find_bar), fprev);
    gtk_box_append(GTK_BOX(browser->find_bar), fnext);
    gtk_box_append(GTK_BOX(browser->find_bar), fclose);
    gtk_widget_set_visible(browser->find_bar, FALSE);
    gtk_box_append(GTK_BOX(main_box), browser->find_bar);

    gtk_box_append(GTK_BOX(main_box), browser->tab_widget);
    gtk_widget_set_vexpand(browser->tab_widget, TRUE);

    browser->splitter = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(browser->splitter), main_box);
    gtk_widget_set_vexpand(main_box, TRUE);

    gtk_window_set_child(GTK_WINDOW(browser->window), browser->splitter);
}
