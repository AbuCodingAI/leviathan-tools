/**
 * Infinitas Browser - Custom Scheme Handler Header
 */

#ifndef INFINITAS_SCHEME_H
#define INFINITAS_SCHEME_H

#include <webkit/webkit.h>
#include <glib.h>

void infinitas_scheme_handle(WebKitURISchemeRequest *request, gpointer user_data);

/* Plugin-registered sub-path handlers (prefix → handler fn) */
void infinitas_register_plugin_path(const gchar *prefix,
                                     gchar* (*handler)(const gchar *subpath,
                                                        gpointer userdata),
                                     gpointer userdata);
gchar* infinitas_get_settings_page(void);
gchar* infinitas_get_fonts_page(void);
gchar* infinitas_get_apps_page(void);
gchar* infinitas_get_pwa_page(void);
gchar* infinitas_get_skribe_page(void);
gchar* infinitas_get_incognito_page(void);

#endif
