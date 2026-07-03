/**
 * Infinitas Browser - Web Renderer Implementation
 */

#include "renderer.h"
#include <string.h>

/* Floating "Install as App" button injected into every http/https page */
static const gchar PWA_INSTALL_SCRIPT[] =
    "(function(){"
    "if(location.protocol!=='http:'&&location.protocol!=='https:')return;"
    "if(document.getElementById('_inf_pwa_btn_'))return;"
    "var b=document.createElement('button');"
    "b.id='_inf_pwa_btn_';"
    "b.textContent='\xe2\x8a\x95 Install App';"
    "b.style.cssText='position:fixed;bottom:18px;right:18px;z-index:2147483647;"
    "background:#7c6af7;color:#fff;border:none;border-radius:22px;"
    "padding:9px 18px;font-size:13px;font-family:system-ui;font-weight:600;"
    "cursor:pointer;box-shadow:0 4px 16px rgba(124,106,247,.45);"
    "transition:opacity .2s,transform .2s;opacity:.85';"
    "b.onmouseenter=function(){this.style.opacity='1';this.style.transform='scale(1.04)';};"
    "b.onmouseleave=function(){this.style.opacity='.85';this.style.transform='';};"
    "b.onclick=function(){"
    "var ic=document.querySelector(\"link[rel~='icon']\");"
    "var iu=(ic&&ic.href)?ic.href:(location.origin+'/favicon.ico');"
    "location.href='infinitas://install-pwa?url='+encodeURIComponent(location.href)"
    "+'&title='+encodeURIComponent(document.title||location.hostname)"
    "+'&icon='+encodeURIComponent(iu);};"
    "document.body.appendChild(b);"
    "})();";

WebRenderer* renderer_new(gboolean pwa_mode, WebKitNetworkSession *session) {
    WebRenderer *renderer = g_new0(WebRenderer, 1);
    /* Bind every WebView to the ONE shared persistent network session so that
     * cookies, localStorage, IndexedDB and service-worker data are all stored
     * in the same on-disk profile and survive quit+relaunch. Passing NULL would
     * fall back to the default session; we always pass the shared one. */
    if (session)
        renderer->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                                      "network-session", session, NULL));
    else
        renderer->view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    renderer->widget = GTK_WIDGET(renderer->view);
    renderer->current_url = NULL;
    renderer->page_title = NULL;
    renderer->last_active = g_get_monotonic_time();
    renderer->hibernated = FALSE;
    renderer->hibernated_url = NULL;

    /* Enable the Web Inspector (dev tools) */
    WebKitSettings *settings = webkit_web_view_get_settings(renderer->view);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    /* Render on the CPU, not the GPU. Old/Intel GPUs with flaky Mesa drivers
     * (Leviathan's target hardware) intermittently crash WebKit's compositor;
     * software compositing is stable and plenty fast for these machines. */
    webkit_settings_set_hardware_acceleration_policy(settings,
        WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);

    /* Present as Safari — NOT Chrome. Infinitas is WebKit (Safari's engine),
     * so a Safari UA makes sites serve WebKit-compatible code that actually
     * renders here, while still passing "real browser" checks. A Chrome UA
     * makes sites (esp. Google) serve Chrome-only code that crashes WebKit. */
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15");

    /* Inject PWA install button on every http/https page (but not in PWA mode) */
    if (!pwa_mode) {
        WebKitUserContentManager *mgr =
            webkit_web_view_get_user_content_manager(renderer->view);
        WebKitUserScript *script = webkit_user_script_new(
            PWA_INSTALL_SCRIPT,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
            NULL, NULL);
        webkit_user_content_manager_add_script(mgr, script);
        webkit_user_script_unref(script);
    }

    return renderer;
}

void renderer_free(WebRenderer *renderer) {
    if (!renderer) return;
    g_free(renderer->current_url);
    g_free(renderer->page_title);
    g_free(renderer->hibernated_url);
    /* widget is managed by its parent container */
    g_free(renderer);
}

GtkWidget* renderer_get_widget(WebRenderer *renderer) {
    return renderer ? renderer->widget : NULL;
}

void renderer_toggle_inspector(WebRenderer *renderer) {
    if (!renderer || !renderer->view) return;
    WebKitWebInspector *inspector = webkit_web_view_get_inspector(renderer->view);
    if (inspector) webkit_web_inspector_show(inspector);
}

WebKitWebView* renderer_get_view(WebRenderer *renderer) {
    return renderer ? renderer->view : NULL;
}

void renderer_load_url(WebRenderer *renderer, const gchar *url) {
    if (!renderer || !url) return;
    
    if (g_str_has_prefix(url, "infinitas://")) {
        webkit_web_view_load_uri(renderer->view, url);
        return;
    }
    
    gchar *full_url;
    if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
        full_url = g_strdup(url);
    } else {
        full_url = g_strdup_printf("https://%s", url);
    }
    
    webkit_web_view_load_uri(renderer->view, full_url);
    g_free(full_url);
}

void renderer_load_html(WebRenderer *renderer, const gchar *html, const gchar *base_url) {
    if (!renderer || !html) return;
    webkit_web_view_load_html(renderer->view, html, base_url ? base_url : NULL);
}

void renderer_reload(WebRenderer *renderer) {
    if (!renderer) return;
    webkit_web_view_reload(renderer->view);
}

void renderer_go_back(WebRenderer *renderer) {
    if (!renderer) return;
    webkit_web_view_go_back(renderer->view);
}

void renderer_go_forward(WebRenderer *renderer) {
    if (!renderer) return;
    webkit_web_view_go_forward(renderer->view);
}

gchar* renderer_get_current_url(WebRenderer *renderer) {
    if (!renderer || !renderer->current_url) return NULL;
    return g_strdup(renderer->current_url);
}

gchar* renderer_get_page_title(WebRenderer *renderer) {
    if (!renderer || !renderer->page_title) return NULL;
    return g_strdup(renderer->page_title);
}

void renderer_apply_font(WebRenderer *renderer, const gchar *family) {
    if (!renderer || !renderer->view) return;

    WebKitUserContentManager *mgr =
        webkit_web_view_get_user_content_manager(renderer->view);
    webkit_user_content_manager_remove_all_style_sheets(mgr);

    if (!family || !*family) return;

    /* Persist for every future page load in this tab */
    gchar *css = g_strdup_printf(
        "* { font-family: \"%s\", sans-serif !important; }", family);
    WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new(
        css,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_STYLE_LEVEL_USER,
        NULL, NULL);
    webkit_user_content_manager_add_style_sheet(mgr, sheet);
    webkit_user_style_sheet_unref(sheet);
    g_free(css);

    /* Apply immediately to the page that is already loaded */
    gchar *js = g_strdup_printf(
        "(function(){"
        "try{"
        "var s=document.getElementById('_infinitas_font_');"
        "if(!s){"
        "  s=document.createElement('style');"
        "  s.id='_infinitas_font_';"
        "  (document.head||document.documentElement).appendChild(s);"
        "}"
        "s.textContent='*{font-family:\"%s\",sans-serif!important}';"
        "}catch(e){}})();",
        family);
    webkit_web_view_evaluate_javascript(renderer->view, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
    g_free(js);
}

void renderer_show_error(WebRenderer *renderer, const gchar *url, 
                         const gchar *error_type, const gchar *message) {
    if (!renderer || !url) return;
    
    gchar *error_html = g_strdup_printf(
        "<!DOCTYPE html><html><head><title>Error</title></head>"
        "<body><h1>Error</h1><p>URL: %s</p><p>Type: %s</p><p>Message: %s</p>"
        "<button onclick='window.history.back()'>Go Back</button></body></html>",
        url, error_type, message ? message : "Unknown error"
    );
    
    webkit_web_view_load_html(renderer->view, error_html, NULL);
    g_free(error_html);
}
