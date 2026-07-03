/**
 * Infinitas Browser - Web Renderer Header
 */

#ifndef RENDERER_H
#define RENDERER_H

#include <webkit/webkit.h>
#include <gtk/gtk.h>

typedef struct {
    WebKitWebView *view;
    GtkWidget *widget;
    gchar *current_url;
    gchar *page_title;
    gint64 last_active;      /* monotonic time this tab was last in foreground */
    gboolean hibernated;     /* true while sleeping to reclaim RAM */
    gchar *hibernated_url;   /* URL to reload on wake */
} WebRenderer;

WebRenderer* renderer_new(gboolean pwa_mode, WebKitNetworkSession *session);
void renderer_free(WebRenderer *renderer);
GtkWidget* renderer_get_widget(WebRenderer *renderer);
void renderer_toggle_inspector(WebRenderer *renderer);
WebKitWebView* renderer_get_view(WebRenderer *renderer);
void renderer_load_url(WebRenderer *renderer, const gchar *url);
void renderer_load_html(WebRenderer *renderer, const gchar *html, const gchar *base_url);
void renderer_reload(WebRenderer *renderer);
void renderer_go_back(WebRenderer *renderer);
void renderer_go_forward(WebRenderer *renderer);
gchar* renderer_get_current_url(WebRenderer *renderer);
gchar* renderer_get_page_title(WebRenderer *renderer);
void renderer_show_error(WebRenderer *renderer, const gchar *url,
                         const gchar *error_type, const gchar *message);
void renderer_apply_font(WebRenderer *renderer, const gchar *family);

#endif
