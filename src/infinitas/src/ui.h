/**
 * Infinitas Browser - UI Components Header
 */

#ifndef UI_H
#define UI_H

#include <gtk/gtk.h>
#include "browser.h"

GtkWidget* ui_create_toolbar(InfinitasBrowser *browser);
GtkWidget* ui_create_menu(InfinitasBrowser *browser);
void ui_menu_add_item(GtkWidget *menu, const gchar *label, GCallback callback);
gchar* ui_get_error_page(const gchar *url, const gchar *error_type, const gchar *message);

#endif
