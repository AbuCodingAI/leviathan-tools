/**
 * OLaunch — LeviathanOS quick launcher (C/GTK4 "spotlight").
 *
 * A launcher that ANSWERS, not just opens. As you type it shows a live,
 * ranked result list:
 *   1. Inline calculator   — "2+2*10", "sqrt(144)", "50*1.08"  → Enter copies
 *   2. Unit conversions     — "10 km to mi", "72 f to c"        → Enter copies
 *   3. Installed apps       — scans .desktop files              → Enter launches
 *   4. URL / web-search      — the original resolution as fallback
 *
 * Up/Down move the selection, Enter activates it, Esc closes.
 * Calc/units use a self-contained evaluator (expr.h / units.h) — no shelling out.
 */

#include <gtk/gtk.h>
#include <string.h>
#include <ctype.h>

#include "expr.h"
#include "units.h"

/* ------------------------------------------------------------------ model */

typedef enum { ACT_CALC, ACT_CONVERT, ACT_APP, ACT_URL } ActType;

typedef struct {
    ActType type;
    char   *payload;   /* calc/convert: text to copy; app: exec argv line; url: url */
} Result;

typedef struct {
    char *name;
    char *exec;        /* cleaned Exec line (field codes stripped) */
} AppEntry;

typedef struct {
    GtkWindow  *win;
    GtkWidget  *entry;
    GtkListBox *list;
    AppEntry   *apps;
    guint       n_apps;
} AppUI;

/* ------------------------------------------------------------- app scanning */

/* Strip trailing/embedded desktop field codes (%U %f %i ...) from an Exec line. */
static char *clean_exec(const char *exec) {
    GString *out = g_string_new("");
    for (const char *p = exec; *p; p++) {
        if (*p == '%') {
            p++;
            if (*p == '%') g_string_append_c(out, '%');
            /* otherwise drop the field code entirely */
            if (!*p) break;
        } else {
            g_string_append_c(out, *p);
        }
    }
    char *s = g_string_free(out, FALSE);
    return g_strstrip(s);
}

static void scan_dir(const char *dir, GHashTable *seen, GPtrArray *apps) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *fname;
    while ((fname = g_dir_read_name(d))) {
        if (!g_str_has_suffix(fname, ".desktop")) continue;
        if (g_hash_table_contains(seen, fname)) continue;   /* dir precedence */
        g_hash_table_add(seen, g_strdup(fname));

        char *path = g_build_filename(dir, fname, NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
            char *type    = g_key_file_get_string(kf, "Desktop Entry", "Type", NULL);
            gboolean nodisp = g_key_file_get_boolean(kf, "Desktop Entry", "NoDisplay", NULL);
            gboolean hidden = g_key_file_get_boolean(kf, "Desktop Entry", "Hidden", NULL);
            char *name    = g_key_file_get_string(kf, "Desktop Entry", "Name", NULL);
            char *exec    = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);

            if (name && exec && !nodisp && !hidden &&
                (!type || g_strcmp0(type, "Application") == 0)) {
                AppEntry *a = g_new0(AppEntry, 1);
                a->name = g_strdup(name);
                a->exec = clean_exec(exec);
                g_ptr_array_add(apps, a);
            }
            g_free(type); g_free(name); g_free(exec);
        }
        g_key_file_free(kf);
        g_free(path);
    }
    g_dir_close(d);
}

static AppEntry *load_apps(guint *out_n) {
    GPtrArray  *apps = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *user_dir = g_build_filename(g_get_home_dir(), ".local", "share", "applications", NULL);
    scan_dir(user_dir, seen, apps);                 /* user overrides system */
    scan_dir("/usr/local/share/applications", seen, apps);
    scan_dir("/usr/share/applications", seen, apps);
    g_free(user_dir);
    g_hash_table_destroy(seen);

    *out_n = apps->len;
    AppEntry *arr = g_new0(AppEntry, apps->len ? apps->len : 1);
    for (guint i = 0; i < apps->len; i++) {
        AppEntry *a = g_ptr_array_index(apps, i);
        arr[i] = *a;
        g_free(a);
    }
    g_ptr_array_free(apps, TRUE);
    return arr;
}

/* ---------------------------------------------------------------- actions */

static void launch_exec(const char *cmdline) {
    if (!cmdline || !*cmdline) return;
    int argc = 0; char **argv = NULL;
    if (g_shell_parse_argv(cmdline, &argc, &argv, NULL)) {
        g_spawn_async(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL);
        g_strfreev(argv);
    }
}

static void open_url(const char *url) {
    if (!url || !*url) return;
    gchar *browser = g_find_program_in_path("infinitas");
    const gchar *launcher = browser ? browser : "xdg-open";
    gchar *argv[] = { (gchar *)launcher, (gchar *)url, NULL };
    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    g_free(browser);
}

/* URL-encode a query for ?q= (spaces → '+', reserved → %XX). */
static gchar *encode_query(const char *s) {
    GString *out = g_string_new("");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == ' ')                       g_string_append_c(out, '+');
        else if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
                                             g_string_append_c(out, *p);
        else                                 g_string_append_printf(out, "%%%02X", *p);
    }
    return g_string_free(out, FALSE);
}

/* The original resolution logic, but returning the URL instead of opening it. */
static gchar *resolve_url(const char *raw, char **label_out) {
    gchar *lowered = g_ascii_strdown(raw, -1);
    gchar *input   = g_strstrip(lowered);
    gchar *url = NULL, *label = NULL;

    if (*input == '\0') {
        /* nothing */
    } else if (strcmp(input, "browser") == 0) {
        url = g_strdup("https://google.com");
        label = g_strdup("Open google.com");
    } else if (strchr(input, ' ')) {
        gchar *q = encode_query(input);
        url = g_strconcat("https://www.google.com/search?q=", q, NULL);
        label = g_strconcat("Search the web for “", input, "”", NULL);
        g_free(q);
    } else if (g_str_has_prefix(input, "http://") || g_str_has_prefix(input, "https://")) {
        url = g_strdup(input);
        label = g_strconcat("Open ", input, NULL);
    } else if (strchr(input, '.')) {
        url = g_strconcat("https://", input, NULL);
        label = g_strconcat("Open ", input, NULL);
    } else {
        url = g_strconcat("https://", input, ".com", NULL);
        label = g_strconcat("Open ", input, ".com", NULL);
    }

    g_free(lowered);
    if (label_out) *label_out = label; else g_free(label);
    return url;
}

/* ------------------------------------------------------------- formatting */

/* Format a double: integers without a decimal point, else trimmed to ~10 sig. */
static char *fmt_number(double v) {
    if (fabs(v) < 1e15 && fabs(v - round(v)) < 1e-9)
        return g_strdup_printf("%.0f", v);
    char buf[64];
    g_snprintf(buf, sizeof buf, "%.10g", v);
    return g_strdup(buf);
}

/* ---------------------------------------------------------------- results */

static void free_result(gpointer p) {
    Result *r = p;
    if (!r) return;
    g_free(r->payload);
    g_free(r);
}

static void add_row(AppUI *ui, ActType type, const char *icon,
                    const char *primary, const char *secondary, const char *payload) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(hbox, 8);
    gtk_widget_set_margin_bottom(hbox, 8);
    gtk_widget_set_margin_start(hbox, 12);
    gtk_widget_set_margin_end(hbox, 12);

    GtkWidget *ic = gtk_label_new(icon);
    gtk_widget_add_css_class(ic, "olaunch-icon");
    gtk_box_append(GTK_BOX(hbox), ic);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *plabel = gtk_label_new(NULL);
    char *pesc = g_markup_escape_text(primary, -1);
    char *pmarkup = g_strconcat("<span weight='bold'>", pesc, "</span>", NULL);
    gtk_label_set_markup(GTK_LABEL(plabel), pmarkup);
    gtk_label_set_xalign(GTK_LABEL(plabel), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(plabel), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vbox), plabel);
    g_free(pesc); g_free(pmarkup);

    if (secondary && *secondary) {
        GtkWidget *slabel = gtk_label_new(secondary);
        gtk_label_set_xalign(GTK_LABEL(slabel), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(slabel), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(slabel, "olaunch-sub");
        gtk_box_append(GTK_BOX(vbox), slabel);
    }
    gtk_box_append(GTK_BOX(hbox), vbox);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    Result *res = g_new0(Result, 1);
    res->type = type;
    res->payload = g_strdup(payload);
    g_object_set_data_full(G_OBJECT(row), "result", res, free_result);

    gtk_list_box_append(ui->list, row);
}

/* Rank app matches: prefix > word-boundary > substring; case-insensitive. */
static int app_score(const char *name_lc, const char *q_lc) {
    const char *hit = strstr(name_lc, q_lc);
    if (!hit) return 0;
    if (hit == name_lc) return 3;                       /* prefix */
    if (hit[-1] == ' ' || hit[-1] == '-') return 2;     /* word start */
    return 1;                                           /* anywhere */
}

typedef struct { AppEntry *app; int score; } Match;

static gint match_cmp(gconstpointer a, gconstpointer b) {
    const Match *ma = a, *mb = b;
    if (ma->score != mb->score) return mb->score - ma->score;
    return g_ascii_strcasecmp(ma->app->name, mb->app->name);
}

static void clear_list(GtkListBox *list) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL)
        gtk_list_box_remove(list, child);
}

static void rebuild(AppUI *ui) {
    const char *raw = gtk_editable_get_text(GTK_EDITABLE(ui->entry));
    clear_list(ui->list);

    char *trimmed = g_strstrip(g_strdup(raw));
    if (*trimmed == '\0') { g_free(trimmed); return; }

    /* 1. calculator */
    double v;
    if (expr_has_digit(trimmed) && expr_eval(trimmed, &v)) {
        char *val = fmt_number(v);
        char *primary = g_strconcat("= ", val, NULL);
        add_row(ui, ACT_CALC, "=", primary, "Enter copies to clipboard", val);
        g_free(primary); g_free(val);
    }

    /* 2. unit conversion */
    double cv; char cu[16];
    if (olaunch_convert(trimmed, &cv, cu, sizeof cu)) {
        char *val = fmt_number(cv);
        char *primary = g_strconcat(val, " ", cu, NULL);
        add_row(ui, ACT_CONVERT, "⇄", primary, "Unit conversion — Enter copies", val);
        g_free(primary); g_free(val);
    }

    /* 3. installed apps */
    if (strlen(trimmed) >= 1) {
        char *q_lc = g_ascii_strdown(trimmed, -1);
        GArray *matches = g_array_new(FALSE, FALSE, sizeof(Match));
        for (guint i = 0; i < ui->n_apps; i++) {
            char *name_lc = g_ascii_strdown(ui->apps[i].name, -1);
            int sc = app_score(name_lc, q_lc);
            g_free(name_lc);
            if (sc) { Match m = { &ui->apps[i], sc }; g_array_append_val(matches, m); }
        }
        g_array_sort(matches, match_cmp);
        guint shown = MIN(matches->len, 6u);
        for (guint i = 0; i < shown; i++) {
            Match *m = &g_array_index(matches, Match, i);
            add_row(ui, ACT_APP, "▶", m->app->name, m->app->exec, m->app->exec);
        }
        g_array_free(matches, TRUE);
        g_free(q_lc);
    }

    /* 4. URL / search fallback (always available) */
    char *label = NULL;
    char *url = resolve_url(raw, &label);
    if (url) {
        add_row(ui, ACT_URL, "🌐", label ? label : url, url, url);
        g_free(url);
    }
    g_free(label);
    g_free(trimmed);

    /* select the first row so Enter has a target */
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(ui->list, 0);
    if (first) gtk_list_box_select_row(ui->list, first);
}

/* ---------------------------------------------------------------- activate */

static void activate_row(AppUI *ui, GtkListBoxRow *row) {
    if (!row) return;
    Result *r = g_object_get_data(G_OBJECT(row), "result");
    if (!r) return;

    switch (r->type) {
    case ACT_CALC:
    case ACT_CONVERT: {
        GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(ui->win));
        gdk_clipboard_set(cb, G_TYPE_STRING, r->payload);
        /* brief confirmation; keep window open */
        GtkWidget *child = gtk_list_box_row_get_child(row);
        (void)child;
        gtk_window_set_title(ui->win, "OLaunch — copied");
        break;
    }
    case ACT_APP:
        launch_exec(r->payload);
        gtk_window_close(ui->win);
        break;
    case ACT_URL:
        open_url(r->payload);
        gtk_window_close(ui->win);
        break;
    }
}

static void on_entry_changed(GtkEditable *editable, gpointer data) {
    (void)editable;
    rebuild((AppUI *)data);
}

static void on_entry_activate(GtkWidget *w, gpointer data) {
    (void)w;
    AppUI *ui = data;
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(ui->list);
    if (!sel) sel = gtk_list_box_get_row_at_index(ui->list, 0);
    activate_row(ui, sel);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    (void)list;
    activate_row((AppUI *)data, row);
}

/* Up/Down move the selection while the entry keeps keyboard focus; Esc closes. */
static gboolean on_key(GtkEventControllerKey *ctrl, guint keyval,
                       guint keycode, GdkModifierType state, gpointer data) {
    (void)ctrl; (void)keycode; (void)state;
    AppUI *ui = data;

    if (keyval == GDK_KEY_Escape) { gtk_window_close(ui->win); return TRUE; }

    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(ui->list);
        int idx = sel ? gtk_list_box_row_get_index(sel) : -1;
        idx += (keyval == GDK_KEY_Down) ? 1 : -1;
        if (idx < 0) idx = 0;
        GtkListBoxRow *next = gtk_list_box_get_row_at_index(ui->list, idx);
        if (next) { gtk_list_box_select_row(ui->list, next); }
        return TRUE;
    }
    return FALSE;
}

/* --------------------------------------------------------------------- UI */

static void apply_css(void) {
    static const char *css =
        "entry { font-size: 20px; padding: 10px; }"
        ".olaunch-icon { font-size: 18px; min-width: 24px; }"
        ".olaunch-sub { opacity: 0.6; font-size: 12px; }"
        "row:selected { background: alpha(@accent_bg_color, 0.9); }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    apply_css();

    AppUI *ui = g_new0(AppUI, 1);
    ui->apps = load_apps(&ui->n_apps);

    GtkWidget *win = gtk_application_window_new(app);
    ui->win = GTK_WINDOW(win);
    gtk_window_set_title(GTK_WINDOW(win), "OLaunch");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 420);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *entry = gtk_entry_new();
    ui->entry = entry;
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
        "calc · app · 10 km to mi · site or search…");
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(box), scroll);

    GtkWidget *list = gtk_list_box_new();
    ui->list = GTK_LIST_BOX(list);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);

    g_signal_connect(entry, "changed",  G_CALLBACK(on_entry_changed),  ui);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), ui);
    g_signal_connect(list,  "row-activated", G_CALLBACK(on_row_activated), ui);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), ui);
    gtk_widget_add_controller(entry, kc);

    /* free UI + app table when the window goes away */
    g_object_set_data_full(G_OBJECT(win), "ui", ui, g_free);

    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("org.leviathanos.olaunch",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
