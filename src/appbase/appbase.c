#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "datac.h"

#define MAX_TABLES 100
#define MAX_TABLE_NAME 128
#define MAX_COLUMNS 50
#define MAX_COLUMN_NAME 64
#define MAX_PATH 512

typedef struct {
    char name[MAX_COLUMN_NAME];
    char type[32];  // TEXT, INTEGER, REAL, BLOB
    int is_primary_key;
    int is_not_null;
    int is_unique;
} ColumnDef;

typedef struct {
    char name[MAX_TABLE_NAME];
} TableInfo;

typedef struct {
    GtkWindow *window;
    WebKitWebView *web_view;
    char current_file[MAX_PATH];
    sqlite3 *db;
    DatacFile *datac_file;
    int is_datac;
    TableInfo tables[MAX_TABLES];
    int table_count;

    // Table designer state
    GtkWindow *designer_window;
    GtkEntry *table_name_entry;
    GtkBox *columns_box;
    int column_count;
    GtkWidget *col_name_entries[MAX_COLUMNS];
    GtkWidget *col_type_combos[MAX_COLUMNS];
    GtkWidget *col_pk_checks[MAX_COLUMNS];
    GtkWidget *col_nn_checks[MAX_COLUMNS];

    // Form generator state
    char current_table[MAX_TABLE_NAME];
    GtkWindow *form_window;
    GtkEntry *form_inputs[MAX_COLUMNS];
    ColumnDef form_columns[MAX_COLUMNS];
    int form_column_count;
    sqlite3_int64 form_edit_rowid;
    int form_is_edit;
} AppBaseApp;

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void on_query(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_open_db(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_refresh(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_csv_import(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_csv_export(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_open_form(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_new_table(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_drop_table(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_show_schema(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_delete_row(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void on_edit_row(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app);
static void update_table_list(AppBaseApp *app);
static int load_tables(AppBaseApp *app);
static int load_tables_datac(AppBaseApp *app);
static char *render_sqlite_table_view(AppBaseApp *app, const char *table);

static const char *HTML_TEMPLATE =
"<!DOCTYPE html><html><head><meta charset='UTF-8'><style>"
"*{margin:0;padding:0;box-sizing:border-box;}"
"body{font-family:'Courier New',Courier,Monaco,monospace;background:#030705;height:100vh;color:#33ff33;}"
"#container{display:flex;height:100%;gap:0;}"
"#sidebar{width:240px;background:#0a140f;overflow-y:auto;padding:12px;border-right:1px solid #33ff33;}"
"#main{flex:1;display:flex;flex-direction:column;}"
"#toolbar{background:#0a140f;padding:12px;border-bottom:1px solid #33ff33;display:flex;gap:8px;flex-wrap:wrap;}"
".btn{padding:8px 16px;background:#030705;color:#33ff33;border:1px solid #33ff33;cursor:pointer;font-weight:bold;font-size:12px;font-family:'Courier New',Courier,Monaco,monospace;text-transform:uppercase;transition:all 0.2s;}"
".btn:hover{background:#33ff33;color:#030705;box-shadow:0 0 8px #33ff33;}"
"h3{color:#33ff33;font-size:12px;margin-bottom:12px;text-transform:uppercase;font-weight:bold;border-bottom:1px solid #33ff33;padding-bottom:8px;}"
".table-link{display:flex;justify-content:space-between;align-items:center;padding:8px;margin:4px 0;background:#030705;color:#33ff33;border:1px solid #1b3a24;transition:all 0.2s;font-size:11px;font-family:'Courier New',Courier,Monaco,monospace;text-transform:uppercase;flex-wrap:wrap;gap:2px;}"
".table-link:hover{border:1px solid #33ff33;}"
".table-btn{padding:4px 8px;background:#1b3a24;color:#33ff33;border:1px solid #33ff33;cursor:pointer;font-size:10px;margin-left:4px;transition:all 0.2s;}"
".table-btn:hover{background:#33ff33;color:#030705;}"
"#content{flex:1;padding:16px;overflow-y:auto;}"
"#query-box{width:100%;padding:8px;border:1px solid #33ff33;font-family:'Courier New',Courier,Monaco,monospace;margin:12px 0;resize:vertical;background:#030705;color:#33ff33;font-size:12px;}"
"#query-box:focus{outline:none;border:1px solid #33ff33;}"
"table{width:100%;border-collapse:collapse;background:#030705;margin:12px 0;border:1px solid #1b3a24;}"
"th,td{border:1px solid #1b3a24;padding:8px;text-align:left;font-family:'Courier New',Courier,Monaco,monospace;font-size:12px;color:#33ff33;}"
"th{background:#0a140f;font-weight:bold;border-bottom:2px solid #33ff33;text-transform:uppercase;}"
"tr:nth-child(even) td{background:#050c07;}"
"tr:nth-child(odd) td{background:#030705;}"
"td{max-width:280px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
".rowcount{color:#33ff33;font-size:12px;margin:8px 0;opacity:0.8;}"
"#result{margin-top:12px;}"
"#aboutov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;align-items:center;justify-content:center;}"
"#aboutov.on{display:flex;}"
"#aboutbox{background:#0a140f;border:2px solid #33ff33;color:#33ff33;max-width:480px;margin:16px;padding:20px;font-family:'Courier New',Courier,Monaco,monospace;font-size:12px;line-height:1.7;box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#aboutbox .atitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#aboutbox .aok{margin-top:16px;text-align:right;}"
"</style></head><body>"
"<div id='container'>"
"<div id='sidebar'><h3>TABLES</h3><div id='table-list'></div></div>"
"<div id='main'>"
"<div id='toolbar'>"
"<button class='btn' onclick='openDatabase()'>OPEN DB</button>"
"<button class='btn' onclick='newTable()'>NEW TABLE</button>"
"<button class='btn' onclick='importCSV()'>IMPORT CSV</button>"
"<button class='btn' onclick='exportCSV()'>EXPORT CSV</button>"
"<button class='btn' onclick='refreshTables()'>REFRESH</button>"
"<button class='btn' onclick='showAbout()'>ABOUT</button>"
"</div>"
"<div id='content'>"
"<textarea id='query-box' placeholder='SELECT * FROM table_name' rows='3'></textarea>"
"<button class='btn' onclick='executeQuery()'>EXECUTE</button>"
"<div id='result'></div>"
"</div>"
"</div>"
"</div>"
"<script>"
"function openDatabase(){window.webkit.messageHandlers.openDb.postMessage('');}"
"function newTable(){window.webkit.messageHandlers.newTable.postMessage('');}"
"function importCSV(){window.webkit.messageHandlers.importCSV.postMessage('');}"
"function exportCSV(){window.webkit.messageHandlers.exportCSV.postMessage('');}"
"function refreshTables(){window.webkit.messageHandlers.refreshTables.postMessage('');}"
"function openForm(name){window.webkit.messageHandlers.openForm.postMessage(name);}"
"function dropTable(name){window.webkit.messageHandlers.dropTable.postMessage(name);}"
"function showSchema(name){window.webkit.messageHandlers.showSchema.postMessage(name);}"
"function deleteRow(key){window.webkit.messageHandlers.deleteRow.postMessage(key);}"
"function editRow(key){window.webkit.messageHandlers.editRow.postMessage(key);}"
"function executeQuery(){let query=document.getElementById('query-box').value;if(query.trim()){window.webkit.messageHandlers.query.postMessage(query);}}"
"function loadTable(name){document.getElementById('query-box').value='SELECT * FROM \"'+name+'\"';executeQuery();}"
"function showAbout(){document.getElementById('aboutov').classList.add('on');}"
"function closeAbout(){document.getElementById('aboutov').classList.remove('on');}"
"</script>"
"<div id='aboutov' onclick='if(event.target===this)closeAbout()'>"
"<div id='aboutbox'>"
"<div class='atitle'>AppBase &mdash; part of LeviathanOS</div>"
"Free software under the GNU General Public License, version 3.<br>"
"This program comes with ABSOLUTELY NO WARRANTY.<br>"
"Full license: /usr/share/doc/leviathanos/LICENSE<br>"
"https://www.gnu.org/licenses/gpl-3.0.html"
"<div class='aok'><button class='btn' onclick='closeAbout()'>OK</button></div>"
"</div></div>"
"</body></html>";

/* ── Small utilities ──────────────────────────────────────────────────────── */

/* Validate an SQL identifier (table/column). Accept [A-Za-z_][A-Za-z0-9_]* .
   Such identifiers are safe to wrap in double quotes without further escaping. */
static int valid_ident(const char *s) {
    if (!s || !*s) return 0;
    if (strlen(s) >= MAX_TABLE_NAME) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}

/* Turn an arbitrary header string into a valid identifier. */
static void sanitize_ident(const char *in, char *out, size_t outsz, int idx) {
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < outsz; p++)
        if (isalnum((unsigned char)*p) || *p == '_') out[j++] = *p;
    out[j] = '\0';
    if (j == 0 || !(isalpha((unsigned char)out[0]) || out[0] == '_')) {
        /* prefix underscore if it starts with a digit but is otherwise usable */
        if (j > 0 && isdigit((unsigned char)out[0]) && j + 2 < outsz) {
            memmove(out + 1, out, j + 1);
            out[0] = '_';
        } else {
            snprintf(out, outsz, "col%d", idx + 1);
        }
    }
}

/* HTML-escape (& < > " ') via GLib. Caller g_free()s the result. */
static gchar *html_escape(const char *s) {
    return g_markup_escape_text(s ? s : "", -1);
}

/* Append a string escaped for embedding inside a single-quoted JS string. */
static void append_js_escaped(GString *out, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '\\': g_string_append(out, "\\\\"); break;
            case '\'': g_string_append(out, "\\'");  break;
            case '\n': g_string_append(out, "\\n");  break;
            case '\r': g_string_append(out, "\\r");  break;
            case '\t': g_string_append(out, "\\t");  break;
            case '\v': g_string_append(out, "\\v");  break;
            default:   g_string_append_c(out, *s);
        }
    }
}

/* Set the #result div to a block of HTML (safely escaped for JS transport). */
static void set_result_html(AppBaseApp *app, const char *html) {
    GString *js = g_string_new("document.getElementById('result').innerHTML = '");
    append_js_escaped(js, html);
    g_string_append(js, "';");
    webkit_web_view_evaluate_javascript(app->web_view, js->str, -1,
                                        NULL, NULL, NULL, NULL, NULL);
    g_string_free(js, TRUE);
}

static void show_msg(AppBaseApp *app, GtkMessageType type, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GtkWidget *d = gtk_message_dialog_new(app->window, GTK_DIALOG_MODAL,
                                          type, GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static int confirm_dialog(AppBaseApp *app, const char *msg) {
    GtkWidget *d = gtk_message_dialog_new(app->window, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                          "%s", msg);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_YES;
}

/* Match "SELECT * FROM <identifier>" (optionally quoted, trailing ';').
   Returns 1 and fills out with the table name if it is a plain single-table view. */
static int simple_table_name(const char *q, char *out, size_t outsz) {
    while (isspace((unsigned char)*q)) q++;
    if (g_ascii_strncasecmp(q, "select", 6) != 0) return 0;
    q += 6;
    while (isspace((unsigned char)*q)) q++;
    if (*q != '*') return 0;
    q++;
    while (isspace((unsigned char)*q)) q++;
    if (g_ascii_strncasecmp(q, "from", 4) != 0) return 0;
    q += 4;
    while (isspace((unsigned char)*q)) q++;

    char quote = 0;
    if (*q == '"' || *q == '`' || *q == '[') { quote = *q; q++; }

    size_t j = 0;
    while (*q && j + 1 < outsz) {
        char c = *q;
        if (quote) {
            if (c == quote || (quote == '[' && c == ']')) { q++; break; }
        } else if (isspace((unsigned char)c) || c == ';') {
            break;
        }
        out[j++] = c;
        q++;
    }
    out[j] = '\0';
    while (*q) { if (!isspace((unsigned char)*q) && *q != ';') return 0; q++; }
    return valid_ident(out);
}

/* Split "table|rowid" as posted by the row action buttons. */
static int split_row_key(const char *key, char *tbl, size_t tsz, sqlite3_int64 *rid) {
    if (!key) return 0;
    const char *bar = strchr(key, '|');
    if (!bar) return 0;
    size_t n = (size_t)(bar - key);
    if (n == 0 || n >= tsz) return 0;
    memcpy(tbl, key, n);
    tbl[n] = '\0';
    char *end = NULL;
    long long v = strtoll(bar + 1, &end, 10);
    if (end == bar + 1) return 0;
    *rid = (sqlite3_int64)v;
    return valid_ident(tbl);
}

/* ── Table loading ────────────────────────────────────────────────────────── */

static int load_tables_datac(AppBaseApp *app) {
    if (!app->datac_file) return -1;
    app->table_count = 0;
    for (int i = 0; i < app->datac_file->table_n && app->table_count < MAX_TABLES; i++) {
        strncpy(app->tables[app->table_count].name,
                app->datac_file->tables[i].name, MAX_TABLE_NAME - 1);
        app->tables[app->table_count].name[MAX_TABLE_NAME - 1] = '\0';
        app->table_count++;
    }
    return 0;
}

static int load_tables(AppBaseApp *app) {
    if (!app->db) return -1;
    app->table_count = 0;
    sqlite3_stmt *stmt;
    const char *query =
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name;";
    if (sqlite3_prepare_v2(app->db, query, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    while (sqlite3_step(stmt) == SQLITE_ROW && app->table_count < MAX_TABLES) {
        const char *table_name = (const char *)sqlite3_column_text(stmt, 0);
        if (table_name) {
            strncpy(app->tables[app->table_count].name, table_name, MAX_TABLE_NAME - 1);
            app->tables[app->table_count].name[MAX_TABLE_NAME - 1] = '\0';
            app->table_count++;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* ── Query rendering (SQLite) ─────────────────────────────────────────────── */

/* Plain arbitrary query (SELECT results, or DDL/DML with change count). */
static char *render_sqlite_query(AppBaseApp *app, const char *query) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, query, -1, &stmt, NULL) != SQLITE_OK) {
        gchar *e = html_escape(sqlite3_errmsg(app->db));
        char *r = g_strdup_printf(
            "<table><tr><th>ERROR</th></tr><tr><td>%s</td></tr></table>", e);
        g_free(e);
        return r;
    }

    int ncol = sqlite3_column_count(stmt);

    if (ncol == 0) {  /* statement returns no rows (INSERT/UPDATE/CREATE/...) */
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { }
        char *r;
        if (rc == SQLITE_DONE) {
            r = g_strdup_printf("<div class='rowcount'>OK &mdash; %d row(s) affected.</div>",
                                sqlite3_changes(app->db));
        } else {
            gchar *e = html_escape(sqlite3_errmsg(app->db));
            r = g_strdup_printf("<table><tr><th>ERROR</th></tr><tr><td>%s</td></tr></table>", e);
            g_free(e);
        }
        sqlite3_finalize(stmt);
        load_tables(app);
        update_table_list(app);
        return r;
    }

    GString *h = g_string_new("<table><tr>");
    for (int i = 0; i < ncol; i++) {
        gchar *e = html_escape(sqlite3_column_name(stmt, i));
        g_string_append_printf(h, "<th>%s</th>", e);
        g_free(e);
    }
    g_string_append(h, "</tr>");

    int rows = 0, rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        g_string_append(h, "<tr>");
        for (int i = 0; i < ncol; i++) {
            const char *val = (const char *)sqlite3_column_text(stmt, i);
            if (val) {
                gchar *e = html_escape(val);
                g_string_append_printf(h, "<td>%s</td>", e);
                g_free(e);
            } else {
                g_string_append(h, "<td>NULL</td>");
            }
        }
        g_string_append(h, "</tr>");
        rows++;
    }
    g_string_append(h, "</table>");
    if (rc != SQLITE_DONE) {
        gchar *e = html_escape(sqlite3_errmsg(app->db));
        g_string_append_printf(h, "<div class='rowcount'>ERROR: %s</div>", e);
        g_free(e);
    }
    g_string_append_printf(h, "<div class='rowcount'>%d row(s)</div>", rows);
    sqlite3_finalize(stmt);
    return g_string_free(h, FALSE);
}

/* Single-table view with per-row EDIT/DELETE actions (uses rowid). */
static char *render_sqlite_table_view(AppBaseApp *app, const char *table) {
    if (!valid_ident(table)) return render_sqlite_query(app, "SELECT 0 WHERE 0");

    GString *sql = g_string_new(NULL);
    g_string_append_printf(sql, "SELECT rowid, * FROM \"%s\"", table);
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(app->db, sql->str, -1, &stmt, NULL);
    g_string_free(sql, TRUE);
    if (rc != SQLITE_OK) {
        gchar *e = html_escape(sqlite3_errmsg(app->db));
        char *r = g_strdup_printf(
            "<table><tr><th>ERROR</th></tr><tr><td>%s</td></tr></table>", e);
        g_free(e);
        return r;
    }

    int ncol = sqlite3_column_count(stmt);   /* column 0 is rowid */
    GString *h = g_string_new("<table><tr><th>ACTIONS</th>");
    for (int i = 1; i < ncol; i++) {
        gchar *e = html_escape(sqlite3_column_name(stmt, i));
        g_string_append_printf(h, "<th>%s</th>", e);
        g_free(e);
    }
    g_string_append(h, "</tr>");

    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 rid = sqlite3_column_int64(stmt, 0);
        /* table is a validated identifier, so it is safe inside the JS literal */
        g_string_append_printf(h,
            "<tr><td>"
            "<button class='table-btn' onclick=\"editRow('%s|%lld')\">EDIT</button>"
            "<button class='table-btn' onclick=\"deleteRow('%s|%lld')\">DEL</button>"
            "</td>",
            table, (long long)rid, table, (long long)rid);
        for (int i = 1; i < ncol; i++) {
            const char *val = (const char *)sqlite3_column_text(stmt, i);
            if (val) {
                gchar *e = html_escape(val);
                g_string_append_printf(h, "<td>%s</td>", e);
                g_free(e);
            } else {
                g_string_append(h, "<td>NULL</td>");
            }
        }
        g_string_append(h, "</tr>");
        rows++;
    }
    sqlite3_finalize(stmt);
    g_string_append_printf(h, "</table><div class='rowcount'>%d row(s)</div>", rows);
    return g_string_free(h, FALSE);
}

/* ── Query rendering (datac, read-only) ───────────────────────────────────── */

static char *execute_query_datac(AppBaseApp *app, const char *query) {
    if (!app->datac_file)
        return g_strdup("<div class='rowcount'>No file open.</div>");

    char table_name[MAX_TABLE_NAME];
    /* strip surrounding quotes if the JS helper added them */
    char raw[MAX_TABLE_NAME];
    if (sscanf(query, " SELECT * FROM %127s", raw) != 1) {
        return g_strdup(
            "<table><tr><th>Error</th></tr><tr><td>Only 'SELECT * FROM table' "
            "is supported for .datac files</td></tr></table>");
    }
    /* unquote */
    {
        char *s = raw;
        if (*s == '"' || *s == '`' || *s == '[') s++;
        size_t n = strlen(s);
        while (n > 0 && (s[n-1] == '"' || s[n-1] == '`' || s[n-1] == ']' || s[n-1] == ';'))
            s[--n] = '\0';
        strncpy(table_name, s, sizeof(table_name) - 1);
        table_name[sizeof(table_name) - 1] = '\0';
    }

    DatacTable *table = datac_table(app->datac_file, table_name);
    if (!table) {
        gchar *e = html_escape(table_name);
        char *r = g_strdup_printf(
            "<table><tr><th>Error</th></tr><tr><td>Table not found: %s</td></tr></table>", e);
        g_free(e);
        return r;
    }

    GString *h = g_string_new("<table><tr>");
    for (int i = 0; i < table->schema_n; i++) {
        gchar *e = html_escape(table->schema[i].name);
        g_string_append_printf(h, "<th>%s</th>", e);
        g_free(e);
    }
    g_string_append(h, "</tr>");

    for (int r = 0; r < table->row_n; r++) {
        g_string_append(h, "<tr>");
        for (int c = 0; c < table->schema_n; c++) {
            const char *val = datac_str(&table->rows[r], table->schema[c].name);
            if (val) {
                gchar *e = html_escape(val);
                g_string_append_printf(h, "<td>%s</td>", e);
                g_free(e);
            } else {
                g_string_append(h, "<td>NULL</td>");
            }
        }
        g_string_append(h, "</tr>");
    }
    g_string_append_printf(h, "</table><div class='rowcount'>%d row(s)</div>", table->row_n);
    return g_string_free(h, FALSE);
}

static char *execute_query(AppBaseApp *app, const char *query) {
    if (app->is_datac) return execute_query_datac(app, query);
    if (!app->db) return g_strdup("<div class='rowcount'>No database open.</div>");

    char tbl[MAX_TABLE_NAME];
    if (simple_table_name(query, tbl, sizeof(tbl)))
        return render_sqlite_table_view(app, tbl);
    return render_sqlite_query(app, query);
}

static void on_query(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m;
    JSCValue *value = webkit_javascript_result_get_js_value(r);
    if (jsc_value_is_string(value)) {
        char *message = jsc_value_to_string(value);
        if (message) {
            char *result = execute_query(app, message);
            if (result) {
                set_result_html(app, result);
                g_free(result);
            }
            g_free(message);
        }
    }
}

/* ── Sidebar table list ───────────────────────────────────────────────────── */

static void update_table_list(AppBaseApp *app) {
    GString *h = g_string_new(NULL);
    if (app->table_count == 0) {
        g_string_append(h, "<p>No tables</p>");
    } else {
        for (int i = 0; i < app->table_count; i++) {
            const char *name = app->tables[i].name;
            gchar *disp = html_escape(name);
            if (valid_ident(name)) {
                g_string_append_printf(h,
                    "<div class='table-link'>"
                    "<span onclick=\"loadTable('%s')\" style='flex:1;cursor:pointer;'>%s</span>",
                    name, disp);
                g_string_append_printf(h,
                    "<button class='table-btn' onclick=\"showSchema('%s')\">INFO</button>",
                    name);
                if (!app->is_datac) {
                    g_string_append_printf(h,
                        "<button class='table-btn' onclick=\"openForm('%s')\">FORM</button>", name);
                    g_string_append_printf(h,
                        "<button class='table-btn' onclick=\"dropTable('%s')\">DROP</button>", name);
                }
                g_string_append(h, "</div>");
            } else {
                /* non-identifier names: display only, no action buttons */
                g_string_append_printf(h, "<div class='table-link'><span>%s</span></div>", disp);
            }
            g_free(disp);
        }
    }

    GString *js = g_string_new("document.getElementById('table-list').innerHTML = '");
    append_js_escaped(js, h->str);
    g_string_append(js, "';");
    webkit_web_view_evaluate_javascript(app->web_view, js->str, -1,
                                        NULL, NULL, NULL, NULL, NULL);
    g_string_free(js, TRUE);
    g_string_free(h, TRUE);
}

/* ── Schema view ──────────────────────────────────────────────────────────── */

static void on_show_schema(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m;
    JSCValue *value = webkit_javascript_result_get_js_value(r);
    char *name = jsc_value_to_string(value);
    if (!name) return;

    if (app->is_datac) {
        DatacTable *t = datac_table(app->datac_file, name);
        if (!t) { show_msg(app, GTK_MESSAGE_ERROR, "Table not found."); g_free(name); return; }
        GString *h = g_string_new(NULL);
        gchar *tn = html_escape(name);
        g_string_append_printf(h, "<div class='rowcount'>SCHEMA: %s</div>", tn);
        g_free(tn);
        g_string_append(h, "<table><tr><th>NAME</th><th>TYPE</th><th>PRIMARY</th><th>NULLABLE</th></tr>");
        for (int i = 0; i < t->schema_n; i++) {
            gchar *cn = html_escape(t->schema[i].name);
            g_string_append_printf(h, "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
                cn, datac_type_name(t->schema[i].type),
                t->schema[i].is_primary ? "yes" : "",
                t->schema[i].is_nullable ? "yes" : "");
            g_free(cn);
        }
        g_string_append(h, "</table>");
        set_result_html(app, h->str);
        g_string_free(h, TRUE);
        g_free(name);
        return;
    }

    if (!app->db || !valid_ident(name)) { g_free(name); return; }

    GString *sql = g_string_new(NULL);
    g_string_append_printf(sql, "PRAGMA table_info(\"%s\")", name);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql->str, -1, &stmt, NULL) != SQLITE_OK) {
        show_msg(app, GTK_MESSAGE_ERROR, "%s", sqlite3_errmsg(app->db));
        g_string_free(sql, TRUE);
        g_free(name);
        return;
    }
    g_string_free(sql, TRUE);

    GString *h = g_string_new(NULL);
    gchar *tn = html_escape(name);
    g_string_append_printf(h, "<div class='rowcount'>SCHEMA: %s</div>", tn);
    g_free(tn);
    g_string_append(h, "<table><tr><th>NAME</th><th>TYPE</th><th>NOT NULL</th>"
                       "<th>DEFAULT</th><th>PK</th></tr>");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cname = (const char *)sqlite3_column_text(stmt, 1);
        const char *ctype = (const char *)sqlite3_column_text(stmt, 2);
        int notnull       = sqlite3_column_int(stmt, 3);
        const char *dflt  = (const char *)sqlite3_column_text(stmt, 4);
        int pk            = sqlite3_column_int(stmt, 5);
        gchar *cn = html_escape(cname ? cname : "");
        gchar *ct = html_escape(ctype ? ctype : "");
        gchar *cd = html_escape(dflt ? dflt : "");
        g_string_append_printf(h, "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
            cn, ct, notnull ? "yes" : "", cd, pk ? "yes" : "");
        g_free(cn); g_free(ct); g_free(cd);
    }
    g_string_append(h, "</table>");
    sqlite3_finalize(stmt);
    set_result_html(app, h->str);
    g_string_free(h, TRUE);
    g_free(name);
}

/* ── Drop table ───────────────────────────────────────────────────────────── */

static void on_drop_table(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m;
    if (!app->db) return;
    JSCValue *value = webkit_javascript_result_get_js_value(r);
    char *name = jsc_value_to_string(value);
    if (!name) return;
    if (!valid_ident(name)) { show_msg(app, GTK_MESSAGE_ERROR, "Invalid table name."); g_free(name); return; }

    if (confirm_dialog(app, "Drop this table and all its data?")) {
        GString *sql = g_string_new(NULL);
        g_string_append_printf(sql, "DROP TABLE \"%s\"", name);
        char *err = NULL;
        if (sqlite3_exec(app->db, sql->str, NULL, NULL, &err) != SQLITE_OK) {
            show_msg(app, GTK_MESSAGE_ERROR, "Drop failed: %s", err ? err : "unknown");
        } else {
            set_result_html(app, "<div class='rowcount'>Table dropped.</div>");
            load_tables(app);
            update_table_list(app);
        }
        if (err) sqlite3_free(err);
        g_string_free(sql, TRUE);
    }
    g_free(name);
}

/* ── Delete row ───────────────────────────────────────────────────────────── */

static void on_delete_row(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m;
    if (!app->db) return;
    JSCValue *value = webkit_javascript_result_get_js_value(r);
    char *key = jsc_value_to_string(value);
    char tbl[MAX_TABLE_NAME];
    sqlite3_int64 rid;
    if (key && split_row_key(key, tbl, sizeof(tbl), &rid)) {
        if (confirm_dialog(app, "Delete this row?")) {
            GString *sql = g_string_new(NULL);
            g_string_append_printf(sql, "DELETE FROM \"%s\" WHERE rowid=?", tbl);
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(app->db, sql->str, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, rid);
                int rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                if (rc != SQLITE_DONE) {
                    show_msg(app, GTK_MESSAGE_ERROR, "Delete failed: %s", sqlite3_errmsg(app->db));
                } else {
                    char *html = render_sqlite_table_view(app, tbl);
                    set_result_html(app, html);
                    g_free(html);
                }
            } else {
                show_msg(app, GTK_MESSAGE_ERROR, "Delete failed: %s", sqlite3_errmsg(app->db));
            }
            g_string_free(sql, TRUE);
        }
    }
    g_free(key);
}

/* ── Record form (insert / edit), fully parameterized ─────────────────────── */

static void on_form_submit(GtkButton *button, AppBaseApp *app) {
    (void)button;
    if (!app->db || app->form_column_count == 0) return;
    if (!valid_ident(app->current_table)) {
        show_msg(app, GTK_MESSAGE_ERROR, "Invalid table name.");
        return;
    }
    /* validate column identifiers before building SQL */
    for (int i = 0; i < app->form_column_count; i++) {
        if (!valid_ident(app->form_columns[i].name)) {
            show_msg(app, GTK_MESSAGE_ERROR, "Column '%s' is not a valid identifier.",
                     app->form_columns[i].name);
            return;
        }
    }

    GString *sql = g_string_new(NULL);
    if (app->form_is_edit) {
        g_string_append_printf(sql, "UPDATE \"%s\" SET ", app->current_table);
        for (int i = 0; i < app->form_column_count; i++) {
            if (i) g_string_append(sql, ", ");
            g_string_append_printf(sql, "\"%s\"=?", app->form_columns[i].name);
        }
        g_string_append(sql, " WHERE rowid=?");
    } else {
        g_string_append_printf(sql, "INSERT INTO \"%s\" (", app->current_table);
        for (int i = 0; i < app->form_column_count; i++) {
            if (i) g_string_append(sql, ", ");
            g_string_append_printf(sql, "\"%s\"", app->form_columns[i].name);
        }
        g_string_append(sql, ") VALUES (");
        for (int i = 0; i < app->form_column_count; i++)
            g_string_append(sql, i ? ", ?" : "?");
        g_string_append(sql, ")");
    }

    sqlite3_stmt *stmt;
    int prep = sqlite3_prepare_v2(app->db, sql->str, -1, &stmt, NULL);
    g_string_free(sql, TRUE);
    if (prep != SQLITE_OK) {
        show_msg(app, GTK_MESSAGE_ERROR, "%s", sqlite3_errmsg(app->db));
        return;
    }

    for (int i = 0; i < app->form_column_count; i++) {
        const char *val = gtk_entry_get_text(app->form_inputs[i]);
        if (!val || val[0] == '\0')
            sqlite3_bind_null(stmt, i + 1);
        else
            sqlite3_bind_text(stmt, i + 1, val, -1, SQLITE_TRANSIENT);
    }
    if (app->form_is_edit)
        sqlite3_bind_int64(stmt, app->form_column_count + 1, app->form_edit_rowid);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        show_msg(app, GTK_MESSAGE_ERROR, "%s", sqlite3_errmsg(app->db));
        return;
    }

    if (app->form_is_edit) {
        if (app->form_window) { gtk_widget_destroy(GTK_WIDGET(app->form_window)); app->form_window = NULL; }
    } else {
        for (int i = 0; i < app->form_column_count; i++)
            gtk_entry_set_text(app->form_inputs[i], "");
        show_msg(app, GTK_MESSAGE_INFO, "Record inserted.");
    }

    char *html = render_sqlite_table_view(app, app->current_table);
    set_result_html(app, html);
    g_free(html);
    load_tables(app);
    update_table_list(app);
}

static void open_record_form(AppBaseApp *app, const char *table,
                             sqlite3_int64 rowid, int is_edit) {
    if (!app->db) return;
    if (!valid_ident(table)) { show_msg(app, GTK_MESSAGE_ERROR, "Invalid table name."); return; }

    strncpy(app->current_table, table, MAX_TABLE_NAME - 1);
    app->current_table[MAX_TABLE_NAME - 1] = '\0';
    app->form_is_edit = is_edit;
    app->form_edit_rowid = rowid;
    app->form_column_count = 0;

    /* schema */
    GString *psql = g_string_new(NULL);
    g_string_append_printf(psql, "PRAGMA table_info(\"%s\")", table);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, psql->str, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && app->form_column_count < MAX_COLUMNS) {
            const char *nm = (const char *)sqlite3_column_text(stmt, 1);
            const char *ty = (const char *)sqlite3_column_text(stmt, 2);
            ColumnDef *c = &app->form_columns[app->form_column_count++];
            strncpy(c->name, nm ? nm : "", MAX_COLUMN_NAME - 1); c->name[MAX_COLUMN_NAME - 1] = '\0';
            strncpy(c->type, ty ? ty : "", sizeof(c->type) - 1); c->type[sizeof(c->type) - 1] = '\0';
        }
        sqlite3_finalize(stmt);
    }
    g_string_free(psql, TRUE);

    if (app->form_column_count == 0) {
        show_msg(app, GTK_MESSAGE_ERROR, "Table has no columns or does not exist.");
        return;
    }

    /* current values for edit mode */
    char *cur[MAX_COLUMNS] = {0};
    if (is_edit) {
        GString *q = g_string_new(NULL);
        g_string_append_printf(q, "SELECT * FROM \"%s\" WHERE rowid=?", table);
        sqlite3_stmt *s2;
        if (sqlite3_prepare_v2(app->db, q->str, -1, &s2, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(s2, 1, rowid);
            if (sqlite3_step(s2) == SQLITE_ROW) {
                int nc = sqlite3_column_count(s2);
                for (int i = 0; i < nc && i < app->form_column_count; i++) {
                    const char *v = (const char *)sqlite3_column_text(s2, i);
                    cur[i] = v ? g_strdup(v) : NULL;
                }
            }
            sqlite3_finalize(s2);
        }
        g_string_free(q, TRUE);
    }

    app->form_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_transient_for(app->form_window, app->window);
    gtk_window_set_title(app->form_window, is_edit ? "Edit Record" : app->current_table);
    gtk_window_set_default_size(app->form_window, 420, 500);

    GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(app->form_window), GTK_WIDGET(vbox));

    for (int i = 0; i < app->form_column_count; i++) {
        GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
        char lbl[160];
        snprintf(lbl, sizeof(lbl), "%s (%s)", app->form_columns[i].name, app->form_columns[i].type);
        GtkLabel *label = GTK_LABEL(gtk_label_new(lbl));
        gtk_box_pack_start(hbox, GTK_WIDGET(label), FALSE, FALSE, 0);

        app->form_inputs[i] = GTK_ENTRY(gtk_entry_new());
        if (is_edit && cur[i]) gtk_entry_set_text(app->form_inputs[i], cur[i]);
        gtk_box_pack_start(hbox, GTK_WIDGET(app->form_inputs[i]), TRUE, TRUE, 0);
        gtk_box_pack_start(vbox, GTK_WIDGET(hbox), FALSE, FALSE, 0);
    }

    GtkButton *submit = GTK_BUTTON(gtk_button_new_with_label(
        is_edit ? "UPDATE RECORD" : "INSERT NEW RECORD"));
    g_signal_connect(submit, "clicked", G_CALLBACK(on_form_submit), app);
    gtk_box_pack_start(vbox, GTK_WIDGET(submit), FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(app->form_window));

    for (int i = 0; i < MAX_COLUMNS; i++) g_free(cur[i]);
}

static void on_open_form(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m;
    if (!app->db) return;
    JSCValue *value = webkit_javascript_result_get_js_value(r);
    char *table_name = jsc_value_to_string(value);
    if (table_name) {
        open_record_form(app, table_name, -1, 0);
        g_free(table_name);
    }
}

static void on_edit_row(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m;
    if (!app->db) return;
    JSCValue *value = webkit_javascript_result_get_js_value(r);
    char *key = jsc_value_to_string(value);
    char tbl[MAX_TABLE_NAME];
    sqlite3_int64 rid;
    if (key && split_row_key(key, tbl, sizeof(tbl), &rid))
        open_record_form(app, tbl, rid, 1);
    g_free(key);
}

/* ── File open ────────────────────────────────────────────────────────────── */

static void on_file_open_response(GtkDialog *dialog, gint response, AppBaseApp *app) {
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        char *filename = gtk_file_chooser_get_filename(chooser);
        if (filename) {
            const char *ext = strrchr(filename, '.');
            if (ext && strcmp(ext, ".datac") == 0) {
                DatacFile *f = datac_open(filename);
                if (f) {
                    if (app->datac_file) datac_close(app->datac_file);
                    if (app->db) { sqlite3_close(app->db); app->db = NULL; }
                    app->datac_file = f;
                    app->is_datac = 1;
                    strncpy(app->current_file, filename, MAX_PATH - 1);
                    app->current_file[MAX_PATH - 1] = '\0';
                    load_tables_datac(app);
                    update_table_list(app);
                } else {
                    show_msg(app, GTK_MESSAGE_ERROR, "Failed to open .datac file.");
                }
            } else {
                sqlite3 *newdb = NULL;
                if (sqlite3_open(filename, &newdb) == SQLITE_OK) {
                    if (app->datac_file) { datac_close(app->datac_file); app->datac_file = NULL; }
                    if (app->db) sqlite3_close(app->db);
                    app->db = newdb;
                    app->is_datac = 0;
                    strncpy(app->current_file, filename, MAX_PATH - 1);
                    app->current_file[MAX_PATH - 1] = '\0';
                    load_tables(app);
                    update_table_list(app);
                } else {
                    show_msg(app, GTK_MESSAGE_ERROR, "Failed to open database: %s",
                             newdb ? sqlite3_errmsg(newdb) : "unknown");
                    if (newdb) sqlite3_close(newdb);
                }
            }
            g_free(filename);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void on_open_db(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m; (void)r;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open Database", app->window, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Supported");
    gtk_file_filter_add_pattern(all_filter, "*.datac");
    gtk_file_filter_add_pattern(all_filter, "*.db");
    gtk_file_filter_add_pattern(all_filter, "*.sqlite");
    gtk_file_filter_add_pattern(all_filter, "*.sqlite3");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

    GtkFileFilter *datac_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(datac_filter, "DataC Files");
    gtk_file_filter_add_pattern(datac_filter, "*.datac");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), datac_filter);

    GtkFileFilter *sqlite_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(sqlite_filter, "SQLite Databases");
    gtk_file_filter_add_pattern(sqlite_filter, "*.db");
    gtk_file_filter_add_pattern(sqlite_filter, "*.sqlite");
    gtk_file_filter_add_pattern(sqlite_filter, "*.sqlite3");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), sqlite_filter);

    g_signal_connect(dialog, "response", G_CALLBACK(on_file_open_response), app);
    gtk_widget_show(dialog);
}

static void on_refresh(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m; (void)r;
    if (app->is_datac) load_tables_datac(app);
    else if (app->db)  load_tables(app);
    update_table_list(app);
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)widget; (void)event; (void)data;
    return FALSE;
}

/* ── Table designer ───────────────────────────────────────────────────────── */

static void on_add_column(GtkButton *button, AppBaseApp *app) {
    (void)button;
    if (app->column_count >= MAX_COLUMNS) return;
    int idx = app->column_count++;

    GtkBox *row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));

    GtkEntry *name_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(name_entry, "column");
    gtk_box_pack_start(row, GTK_WIDGET(name_entry), TRUE, TRUE, 0);

    GtkComboBoxText *type_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append(type_combo, "TEXT", "TEXT");
    gtk_combo_box_text_append(type_combo, "INTEGER", "INTEGER");
    gtk_combo_box_text_append(type_combo, "REAL", "REAL");
    gtk_combo_box_text_append(type_combo, "BLOB", "BLOB");
    gtk_combo_box_set_active(GTK_COMBO_BOX(type_combo), 0);
    gtk_box_pack_start(row, GTK_WIDGET(type_combo), FALSE, FALSE, 0);

    GtkCheckButton *pk = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("PK"));
    gtk_box_pack_start(row, GTK_WIDGET(pk), FALSE, FALSE, 0);

    GtkCheckButton *nn = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("NOT NULL"));
    gtk_box_pack_start(row, GTK_WIDGET(nn), FALSE, FALSE, 0);

    app->col_name_entries[idx] = GTK_WIDGET(name_entry);
    app->col_type_combos[idx]  = GTK_WIDGET(type_combo);
    app->col_pk_checks[idx]    = GTK_WIDGET(pk);
    app->col_nn_checks[idx]    = GTK_WIDGET(nn);

    gtk_box_pack_start(app->columns_box, GTK_WIDGET(row), FALSE, FALSE, 0);
    gtk_widget_show_all(GTK_WIDGET(row));
}

static void on_create_table(GtkButton *button, AppBaseApp *app) {
    (void)button;
    if (!app->db) return;
    const char *table_name = gtk_entry_get_text(app->table_name_entry);
    if (!valid_ident(table_name)) {
        show_msg(app, GTK_MESSAGE_ERROR,
                 "Table name must start with a letter or underscore and contain only "
                 "letters, digits and underscores.");
        return;
    }
    if (app->column_count == 0) {
        show_msg(app, GTK_MESSAGE_ERROR, "Add at least one column.");
        return;
    }

    GString *sql = g_string_new(NULL);
    g_string_append_printf(sql, "CREATE TABLE \"%s\" (", table_name);

    for (int i = 0; i < app->column_count; i++) {
        const char *cname = gtk_entry_get_text(GTK_ENTRY(app->col_name_entries[i]));
        if (!valid_ident(cname)) {
            show_msg(app, GTK_MESSAGE_ERROR, "Column '%s' is not a valid identifier.", cname);
            g_string_free(sql, TRUE);
            return;
        }
        gchar *ctype = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->col_type_combos[i]));
        const char *type = ctype ? ctype : "TEXT";
        /* type comes from a fixed dropdown, so it is a known-safe keyword */
        if (i) g_string_append(sql, ", ");
        g_string_append_printf(sql, "\"%s\" %s", cname, type);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->col_pk_checks[i])))
            g_string_append(sql, " PRIMARY KEY");
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->col_nn_checks[i])))
            g_string_append(sql, " NOT NULL");
        g_free(ctype);
    }
    g_string_append(sql, ")");

    char *err = NULL;
    if (sqlite3_exec(app->db, sql->str, NULL, NULL, &err) == SQLITE_OK) {
        gtk_widget_destroy(GTK_WIDGET(app->designer_window));
        app->designer_window = NULL;
        app->column_count = 0;
        load_tables(app);
        update_table_list(app);
    } else {
        show_msg(app, GTK_MESSAGE_ERROR, "Create failed: %s", err ? err : "unknown");
    }
    if (err) sqlite3_free(err);
    g_string_free(sql, TRUE);
}

static void on_new_table(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m; (void)r;
    if (!app->db) {
        show_msg(app, GTK_MESSAGE_WARNING, "Open or create a SQLite database first.");
        return;
    }
    app->column_count = 0;

    app->designer_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_transient_for(app->designer_window, app->window);
    gtk_window_set_title(app->designer_window, "Create New Table");
    gtk_window_set_default_size(app->designer_window, 560, 420);

    GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(app->designer_window), GTK_WIDGET(vbox));

    GtkBox *name_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
    GtkLabel *name_label = GTK_LABEL(gtk_label_new("Table Name:"));
    app->table_name_entry = GTK_ENTRY(gtk_entry_new());
    gtk_box_pack_start(name_box, GTK_WIDGET(name_label), FALSE, FALSE, 0);
    gtk_box_pack_start(name_box, GTK_WIDGET(app->table_name_entry), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox, GTK_WIDGET(name_box), FALSE, FALSE, 0);

    GtkLabel *cols_label = GTK_LABEL(gtk_label_new("Columns:"));
    gtk_box_pack_start(vbox, GTK_WIDGET(cols_label), FALSE, FALSE, 0);

    GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    app->columns_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(app->columns_box));
    gtk_box_pack_start(vbox, GTK_WIDGET(scroll), TRUE, TRUE, 0);

    GtkButton *add_col_btn = GTK_BUTTON(gtk_button_new_with_label("+ Add Column"));
    g_signal_connect(add_col_btn, "clicked", G_CALLBACK(on_add_column), app);
    gtk_box_pack_start(vbox, GTK_WIDGET(add_col_btn), FALSE, FALSE, 0);

    GtkButton *create_btn = GTK_BUTTON(gtk_button_new_with_label("CREATE TABLE"));
    g_signal_connect(create_btn, "clicked", G_CALLBACK(on_create_table), app);
    gtk_box_pack_start(vbox, GTK_WIDGET(create_btn), FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(app->designer_window));
    on_add_column(NULL, app);  /* start with one column row */
}

/* ── CSV parsing (RFC-4180-ish) ───────────────────────────────────────────── */

/* Read one CSV record from f into fields (GPtrArray of g_strdup'd strings).
   Handles quoted fields, embedded commas/newlines, and "" escapes.
   Returns 1 if a record was read, 0 at EOF with no data. */
static int csv_read_record(FILE *f, GPtrArray *fields) {
    int c = fgetc(f);
    if (c == EOF) return 0;

    GString *field = g_string_new(NULL);
    int in_quotes = 0;
    int got_any = 0;

    while (c != EOF) {
        got_any = 1;
        if (in_quotes) {
            if (c == '"') {
                int n = fgetc(f);
                if (n == '"') { g_string_append_c(field, '"'); }
                else { in_quotes = 0; if (n != EOF) ungetc(n, f); }
            } else {
                g_string_append_c(field, (char)c);
            }
        } else {
            if (c == '"') {
                in_quotes = 1;
            } else if (c == ',') {
                g_ptr_array_add(fields, g_strdup(field->str));
                g_string_set_size(field, 0);
            } else if (c == '\r') {
                /* swallow, handle CRLF at LF */
            } else if (c == '\n') {
                break;
            } else {
                g_string_append_c(field, (char)c);
            }
        }
        c = fgetc(f);
    }

    g_ptr_array_add(fields, g_strdup(field->str));
    g_string_free(field, TRUE);
    return got_any;
}

static void on_csv_import(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m; (void)r;
    if (!app->db) {
        show_msg(app, GTK_MESSAGE_WARNING, "Open or create a SQLite database first.");
        return;
    }

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Import CSV", app->window, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "CSV Files");
    gtk_file_filter_add_pattern(filter, "*.csv");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        return;
    }
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_widget_destroy(dialog);
    if (!filename) return;

    FILE *f = fopen(filename, "r");
    if (!f) { show_msg(app, GTK_MESSAGE_ERROR, "Cannot open file."); g_free(filename); return; }

    /* ask for a table name */
    GtkWidget *td = gtk_dialog_new_with_buttons(
        "Import to Table", app->window, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Import", GTK_RESPONSE_ACCEPT, NULL);
    GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(td)));
    gtk_box_pack_start(content, gtk_label_new("Table name:"), FALSE, FALSE, 5);
    GtkEntry *tn_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(tn_entry, "imported_data");
    gtk_box_pack_start(content, GTK_WIDGET(tn_entry), FALSE, FALSE, 5);
    gtk_widget_show_all(td);

    if (gtk_dialog_run(GTK_DIALOG(td)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(td);
        fclose(f);
        g_free(filename);
        return;
    }
    char tname[MAX_TABLE_NAME];
    strncpy(tname, gtk_entry_get_text(tn_entry), sizeof(tname) - 1);
    tname[sizeof(tname) - 1] = '\0';
    gtk_widget_destroy(td);

    if (!valid_ident(tname)) {
        show_msg(app, GTK_MESSAGE_ERROR, "Invalid table name.");
        fclose(f); g_free(filename); return;
    }

    /* header row → column identifiers */
    GPtrArray *header = g_ptr_array_new_with_free_func(g_free);
    if (!csv_read_record(f, header) || header->len == 0) {
        show_msg(app, GTK_MESSAGE_ERROR, "CSV file is empty.");
        g_ptr_array_free(header, TRUE);
        fclose(f); g_free(filename); return;
    }

    int ncol = (int)header->len;
    char **cols = g_malloc0((size_t)ncol * sizeof(char *));
    for (int i = 0; i < ncol; i++) {
        char safe[MAX_COLUMN_NAME];
        sanitize_ident((const char *)g_ptr_array_index(header, i), safe, sizeof(safe), i);
        cols[i] = g_strdup(safe);
    }
    g_ptr_array_free(header, TRUE);

    /* CREATE TABLE */
    GString *csql = g_string_new(NULL);
    g_string_append_printf(csql, "CREATE TABLE \"%s\" (", tname);
    for (int i = 0; i < ncol; i++) {
        if (i) g_string_append(csql, ", ");
        g_string_append_printf(csql, "\"%s\" TEXT", cols[i]);
    }
    g_string_append(csql, ")");
    char *err = NULL;
    if (sqlite3_exec(app->db, csql->str, NULL, NULL, &err) != SQLITE_OK) {
        show_msg(app, GTK_MESSAGE_ERROR, "Create table failed: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        g_string_free(csql, TRUE);
        for (int i = 0; i < ncol; i++) g_free(cols[i]);
        g_free(cols);
        fclose(f); g_free(filename); return;
    }
    g_string_free(csql, TRUE);

    /* prepared INSERT */
    GString *isql = g_string_new(NULL);
    g_string_append_printf(isql, "INSERT INTO \"%s\" VALUES (", tname);
    for (int i = 0; i < ncol; i++) g_string_append(isql, i ? ", ?" : "?");
    g_string_append(isql, ")");
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, isql->str, -1, &stmt, NULL);
    g_string_free(isql, TRUE);

    int inserted = 0;
    if (rc == SQLITE_OK) {
        sqlite3_exec(app->db, "BEGIN", NULL, NULL, NULL);
        GPtrArray *rec = g_ptr_array_new_with_free_func(g_free);
        while (csv_read_record(f, rec)) {
            /* skip a fully-empty trailing line */
            if (rec->len == 1 && ((char *)g_ptr_array_index(rec, 0))[0] == '\0') {
                g_ptr_array_set_size(rec, 0);
                continue;
            }
            for (int i = 0; i < ncol; i++) {
                if (i < (int)rec->len) {
                    const char *v = (const char *)g_ptr_array_index(rec, i);
                    if (v && v[0] != '\0')
                        sqlite3_bind_text(stmt, i + 1, v, -1, SQLITE_TRANSIENT);
                    else
                        sqlite3_bind_null(stmt, i + 1);
                } else {
                    sqlite3_bind_null(stmt, i + 1);
                }
            }
            if (sqlite3_step(stmt) == SQLITE_DONE) inserted++;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            g_ptr_array_set_size(rec, 0);
        }
        g_ptr_array_free(rec, TRUE);
        sqlite3_exec(app->db, "COMMIT", NULL, NULL, NULL);
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < ncol; i++) g_free(cols[i]);
    g_free(cols);
    fclose(f);
    g_free(filename);

    load_tables(app);
    update_table_list(app);
    show_msg(app, GTK_MESSAGE_INFO, "Imported %d row(s) into \"%s\".", inserted, tname);
}

/* Write one CSV field with proper escaping. */
static void csv_write_field(FILE *f, const char *val) {
    if (!val) return;
    int need_quote = 0;
    for (const char *p = val; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { need_quote = 1; break; }
    if (!need_quote) { fputs(val, f); return; }
    fputc('"', f);
    for (const char *p = val; *p; p++) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void on_csv_export(WebKitUserContentManager *m, WebKitJavascriptResult *r, AppBaseApp *app) {
    (void)m; (void)r;
    if (!app->db || app->table_count == 0) {
        show_msg(app, GTK_MESSAGE_WARNING, "No table to export.");
        return;
    }

    /* export the currently-selected table, else the first one */
    const char *table = valid_ident(app->current_table) ? app->current_table
                                                         : app->tables[0].name;
    if (!valid_ident(table)) { show_msg(app, GTK_MESSAGE_ERROR, "Invalid table name."); return; }

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Export to CSV", app->window, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    {
        char suggested[MAX_TABLE_NAME + 8];
        snprintf(suggested, sizeof(suggested), "%s.csv", table);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested);
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GString *sql = g_string_new(NULL);
        g_string_append_printf(sql, "SELECT * FROM \"%s\"", table);
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(app->db, sql->str, -1, &stmt, NULL) == SQLITE_OK) {
            FILE *f = fopen(filename, "w");
            if (f) {
                int cols = sqlite3_column_count(stmt);
                for (int i = 0; i < cols; i++) {
                    if (i) fputc(',', f);
                    csv_write_field(f, sqlite3_column_name(stmt, i));
                }
                fputc('\n', f);
                int rows = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    for (int i = 0; i < cols; i++) {
                        if (i) fputc(',', f);
                        csv_write_field(f, (const char *)sqlite3_column_text(stmt, i));
                    }
                    fputc('\n', f);
                    rows++;
                }
                fclose(f);
                show_msg(app, GTK_MESSAGE_INFO, "Exported %d row(s) from \"%s\".", rows, table);
            } else {
                show_msg(app, GTK_MESSAGE_ERROR, "Cannot write file.");
            }
            sqlite3_finalize(stmt);
        } else {
            show_msg(app, GTK_MESSAGE_ERROR, "%s", sqlite3_errmsg(app->db));
        }
        g_string_free(sql, TRUE);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

/* ── UI setup ─────────────────────────────────────────────────────────────── */

static void setup_ui(AppBaseApp *app) {
    app->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(app->window, "AppBase - Database Client");
    gtk_window_set_default_size(app->window, 1200, 700);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), NULL);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    app->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_container_add(GTK_CONTAINER(app->window), GTK_WIDGET(app->web_view));

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_web_view_set_settings(app->web_view, settings);

    WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(app->web_view);
    const char *handlers[] = {
        "query", "openDb", "newTable", "importCSV", "exportCSV",
        "openForm", "refreshTables", "dropTable", "showSchema",
        "deleteRow", "editRow"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(handlers); i++)
        webkit_user_content_manager_register_script_message_handler(manager, handlers[i]);

    g_signal_connect(manager, "script-message-received::query",         G_CALLBACK(on_query), app);
    g_signal_connect(manager, "script-message-received::openDb",        G_CALLBACK(on_open_db), app);
    g_signal_connect(manager, "script-message-received::newTable",      G_CALLBACK(on_new_table), app);
    g_signal_connect(manager, "script-message-received::importCSV",     G_CALLBACK(on_csv_import), app);
    g_signal_connect(manager, "script-message-received::exportCSV",     G_CALLBACK(on_csv_export), app);
    g_signal_connect(manager, "script-message-received::openForm",      G_CALLBACK(on_open_form), app);
    g_signal_connect(manager, "script-message-received::refreshTables", G_CALLBACK(on_refresh), app);
    g_signal_connect(manager, "script-message-received::dropTable",     G_CALLBACK(on_drop_table), app);
    g_signal_connect(manager, "script-message-received::showSchema",    G_CALLBACK(on_show_schema), app);
    g_signal_connect(manager, "script-message-received::deleteRow",     G_CALLBACK(on_delete_row), app);
    g_signal_connect(manager, "script-message-received::editRow",       G_CALLBACK(on_edit_row), app);

    webkit_web_view_load_html(app->web_view, HTML_TEMPLATE, NULL);

    if (app->is_datac && app->datac_file) {
        load_tables_datac(app);
        update_table_list(app);
    } else if (app->db) {
        load_tables(app);
        update_table_list(app);
    }

    gtk_widget_show_all(GTK_WIDGET(app->window));
}

/* Open the default persistent database under the user's data dir. */
static void open_default_db(AppBaseApp *app) {
    char *dir = g_build_filename(g_get_user_data_dir(), "appbase", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "appbase.db", NULL);
    if (sqlite3_open(path, &app->db) == SQLITE_OK) {
        strncpy(app->current_file, path, MAX_PATH - 1);
        app->current_file[MAX_PATH - 1] = '\0';
    } else {
        app->db = NULL;
    }
    g_free(path);
    g_free(dir);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    AppBaseApp appbase_app = {0};

    if (argc > 1) {
        const char *ext = strrchr(argv[1], '.');
        strncpy(appbase_app.current_file, argv[1], MAX_PATH - 1);
        appbase_app.current_file[MAX_PATH - 1] = '\0';

        if (ext && strcmp(ext, ".datac") == 0) {
            appbase_app.datac_file = datac_open(argv[1]);
            if (appbase_app.datac_file) appbase_app.is_datac = 1;
            else open_default_db(&appbase_app);
        } else {
            if (sqlite3_open(argv[1], &appbase_app.db) != SQLITE_OK)
                appbase_app.db = NULL;
        }
    } else {
        open_default_db(&appbase_app);
    }

    setup_ui(&appbase_app);
    gtk_main();

    if (appbase_app.datac_file) datac_close(appbase_app.datac_file);
    if (appbase_app.db) sqlite3_close(appbase_app.db);

    return 0;
}
