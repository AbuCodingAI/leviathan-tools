/**
 * Infinitas Browser - Main Entry Point
 */

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <curl/curl.h>
#include "browser.h"

typedef struct {
    gchar *url;
    gchar *pwa_name;
    gboolean pwa_mode;
} BrowserArgs;

static BrowserArgs g_args = { NULL, NULL, FALSE };

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    InfinitasBrowser *browser = browser_new(app, g_args.pwa_mode);
    
    if (g_args.pwa_mode) {
        /* Set window title to PWA name if provided */
        if (g_args.pwa_name && *g_args.pwa_name) {
            gtk_window_set_title(GTK_WINDOW(browser->window), g_args.pwa_name);
        }
    }
    
    if (g_args.url)
        browser_navigate(browser, g_args.url);
    browser_show(browser);
}

int main(int argc, char *argv[]) {
    /* Disable bubblewrap sandbox - required on systems with restricted user namespaces */
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Parse arguments BEFORE GApplication sees it */
    int new_argc = 1;
    char **new_argv = g_malloc(sizeof(char*) * (argc + 1));
    new_argv[0] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--pwa=")) {
            /* Extract PWA name if present (format: --pwa=Name:URL) */
            gchar *value = argv[i] + strlen("--pwa=");
            gchar *colon = strchr(value, ':');
            if (colon) {
                g_args.pwa_name = g_strndup(value, colon - value);
                g_args.url = g_strdup(colon + 1);
            } else {
                g_args.pwa_name = g_strdup("PWA");
                g_args.url = g_strdup(value);
            }
            g_args.pwa_mode = TRUE;
            /* Don't pass --pwa= to GApplication */
        } else if (g_str_has_prefix(argv[i], "--app=")) {
            g_args.url = argv[i] + strlen("--app=");
            g_args.pwa_mode = TRUE;
            /* Don't pass --app= to GApplication */
        } else if (g_str_has_prefix(argv[i], "http://") ||
                   g_str_has_prefix(argv[i], "https://") ||
                   g_str_has_prefix(argv[i], "infinitas://")) {
            g_args.url = argv[i];
        } else {
            /* Pass other arguments through */
            new_argv[new_argc++] = argv[i];
        }
    }
    new_argv[new_argc] = NULL;

    GtkApplication *app = gtk_application_new("org.infinitas.browser",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), new_argc, new_argv);
    g_object_unref(app);
    g_free(new_argv);

    curl_global_cleanup();
    g_free(g_args.url);
    g_free(g_args.pwa_name);
    return status;
}
