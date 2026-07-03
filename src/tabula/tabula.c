#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * Tabula - lightweight spreadsheet for LeviathanOS
 * GTK3 + WebKit2GTK-4.1. Single self-contained C file.
 * ------------------------------------------------------------------ */

#define MAX_ROWS 100
#define MAX_COLS 52          /* A .. AZ (lazily labelled, sane BSS budget) */
#define MAX_CELL_LEN 256
#define MAX_PATH 512
#define MAX_GROUPS 200

#define NUM_CELLS   (MAX_ROWS * MAX_COLS)
#define CELL_ID(r,c) ((r) * MAX_COLS + (c))

typedef struct {
    /* data[r][c]    -> displayed value (numbers for computed formulas)   */
    /* formula[r][c] -> raw "=..." text so we can recalc                  */
    char data[MAX_ROWS][MAX_COLS][MAX_CELL_LEN];
    char formula[MAX_ROWS][MAX_COLS][MAX_CELL_LEN];
    int rows;
    int cols;
} SpreadsheetData;

typedef struct {
    GtkWindow *window;
    WebKitWebView *web_view;
    char current_file[MAX_PATH];
    SpreadsheetData sheet;
} TabulaApp;

/* ================================================================== *
 * Formula engine
 * ================================================================== */

static double parse_cell_value(const char *cell) {
    char *endptr;
    double val = strtod(cell, &endptr);
    return (endptr != cell) ? val : 0.0;
}

/* Convert a column label ("A", "AA", ...) to a 0-based index. */
static int col_to_index(const char *s) {
    int idx = 0;
    while (*s && isalpha((unsigned char)*s)) {
        idx = idx * 26 + (toupper((unsigned char)*s) - 'A' + 1);
        s++;
    }
    return idx - 1;
}

/* Convert a 0-based column index to its label ("A", "Z", "AA", "AZ"...).
 * Generated lazily on demand so we never keep a big label table around. */
static void index_to_col(int idx, char *out, size_t n) {
    char buf[8];
    int len = 0;
    idx += 1;                                   /* 1-based for base-26 math */
    while (idx > 0 && len < (int)sizeof(buf)) {
        int rem = (idx - 1) % 26;
        buf[len++] = (char)('A' + rem);
        idx = (idx - 1) / 26;
    }
    int o = 0;
    for (int i = len - 1; i >= 0 && o < (int)n - 1; i--)
        out[o++] = buf[i];
    out[o] = '\0';
}

/* Expand "A1:C3" style ranges into a flat list of non-empty numbers.
 * Robust: multi-letter columns, reversed ranges, bounds-checked. */
static void get_range_values(TabulaApp *app, const char *range,
                             double *values, int *count) {
    char c1[8], c2[8];
    int r1, r2;
    *count = 0;
    if (sscanf(range, "%7[A-Za-z]%d:%7[A-Za-z]%d", c1, &r1, c2, &r2) != 4)
        return;

    int sc = col_to_index(c1), ec = col_to_index(c2);
    int sr = r1 - 1, er = r2 - 1;
    if (sc > ec) { int t = sc; sc = ec; ec = t; }
    if (sr > er) { int t = sr; sr = er; er = t; }

    for (int r = sr; r <= er; r++) {
        if (r < 0 || r >= MAX_ROWS) continue;
        for (int c = sc; c <= ec; c++) {
            if (c < 0 || c >= MAX_COLS) continue;
            if (app->sheet.data[r][c][0] != '\0')
                values[(*count)++] = parse_cell_value(app->sheet.data[r][c]);
        }
    }
}

/* ---- recursive-descent expression evaluator -----------------------
 * Grammar:
 *   expr   = term (('+'|'-') term)*
 *   term   = prim (('*'|'/') prim)*
 *   prim   = number | cellref | func '(' args ')' | '(' expr ')' | unary prim
 * Supports: =A1+B2*2 , =SUM(A1:A10)/COUNT(A1:A10) , =ROUND(AVERAGE(A1:A5),2)
 * ------------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t i;
    TabulaApp *app;
    int error;
} EvalCtx;

static double parse_expr(EvalCtx *e);      /* +/- level                    */
static double parse_compare(EvalCtx *e);   /* =,<>,<,>,<=,>= (top level)    */
static double parse_power(EvalCtx *e);     /* ^ (between * / and primary)   */

static void skipws(EvalCtx *e) {
    while (e->s[e->i] == ' ' || e->s[e->i] == '\t') e->i++;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double apply_func(const char *name, double *v, int n) {
    char u[32];
    int j = 0;
    while (name[j] && j < 31) { u[j] = toupper((unsigned char)name[j]); j++; }
    u[j] = '\0';

    if (strcmp(u, "SUM") == 0) {
        double s = 0; for (int i = 0; i < n; i++) s += v[i]; return s;
    } else if (strcmp(u, "AVERAGE") == 0 || strcmp(u, "MEAN") == 0) {
        if (n == 0) return 0;                 /* guard div-by-zero */
        double s = 0; for (int i = 0; i < n; i++) s += v[i]; return s / n;
    } else if (strcmp(u, "PRODUCT") == 0) {
        if (n == 0) return 0;
        double p = 1; for (int i = 0; i < n; i++) p *= v[i]; return p;
    } else if (strcmp(u, "COUNT") == 0) {
        return n;
    } else if (strcmp(u, "MIN") == 0) {
        if (n == 0) return 0;
        double m = v[0]; for (int i = 1; i < n; i++) if (v[i] < m) m = v[i]; return m;
    } else if (strcmp(u, "MAX") == 0) {
        if (n == 0) return 0;
        double m = v[0]; for (int i = 1; i < n; i++) if (v[i] > m) m = v[i]; return m;
    } else if (strcmp(u, "MEDIAN") == 0) {
        if (n == 0) return 0;
        double *tmp = malloc(sizeof(double) * n);
        if (!tmp) return 0;
        memcpy(tmp, v, sizeof(double) * n);
        qsort(tmp, n, sizeof(double), cmp_double);
        double med = (n % 2) ? tmp[n / 2]
                             : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
        free(tmp);
        return med;
    } else if (strcmp(u, "STDEV") == 0 || strcmp(u, "STDEVP") == 0) {
        int pop = (strcmp(u, "STDEVP") == 0);
        if (n < (pop ? 1 : 2)) return 0;
        double mean = 0; for (int i = 0; i < n; i++) mean += v[i]; mean /= n;
        double ss = 0; for (int i = 0; i < n; i++) ss += (v[i]-mean)*(v[i]-mean);
        return sqrt(ss / (pop ? n : n - 1));
    } else if (strcmp(u, "ABS") == 0) {
        return n ? fabs(v[0]) : 0;
    } else if (strcmp(u, "ROUND") == 0) {
        if (n == 0) return 0;
        int nd = (n > 1) ? (int)v[1] : 0;
        double f = pow(10.0, nd);
        return round(v[0] * f) / f;
    } else if (strcmp(u, "ROUNDUP") == 0) {
        if (n == 0) return 0;
        double f = pow(10.0, (n > 1) ? (int)v[1] : 0);
        return (v[0] < 0 ? -ceil(-v[0] * f) : ceil(v[0] * f)) / f;
    } else if (strcmp(u, "ROUNDDOWN") == 0 || strcmp(u, "TRUNC") == 0) {
        if (n == 0) return 0;
        double f = pow(10.0, (n > 1) ? (int)v[1] : 0);
        return (v[0] < 0 ? -floor(-v[0] * f) : floor(v[0] * f)) / f;
    /* ---- logic ---- */
    } else if (strcmp(u, "IF") == 0) {
        if (n >= 3) return v[0] != 0 ? v[1] : v[2];
        if (n == 2) return v[0] != 0 ? v[1] : 0;
        return 0;
    } else if (strcmp(u, "AND") == 0) {
        for (int i = 0; i < n; i++) { if (v[i] == 0) return 0; }
        return n ? 1 : 0;
    } else if (strcmp(u, "OR") == 0) {
        for (int i = 0; i < n; i++) { if (v[i] != 0) return 1; }
        return 0;
    } else if (strcmp(u, "NOT") == 0) {
        return (n && v[0] != 0) ? 0 : 1;
    } else if (strcmp(u, "COUNTA") == 0) {
        return n;                            /* non-empty cells passed in */
    /* ---- math ---- */
    } else if (strcmp(u, "SQRT") == 0) {
        return (n && v[0] >= 0) ? sqrt(v[0]) : 0;
    } else if (strcmp(u, "POWER") == 0 || strcmp(u, "POW") == 0) {
        return n >= 2 ? pow(v[0], v[1]) : 0;
    } else if (strcmp(u, "MOD") == 0) {
        return (n >= 2 && v[1] != 0) ? fmod(v[0], v[1]) : 0;
    } else if (strcmp(u, "INT") == 0 || strcmp(u, "FLOOR") == 0) {
        return n ? floor(v[0]) : 0;
    } else if (strcmp(u, "CEILING") == 0 || strcmp(u, "CEIL") == 0) {
        return n ? ceil(v[0]) : 0;
    } else if (strcmp(u, "EXP") == 0) {
        return n ? exp(v[0]) : 0;
    } else if (strcmp(u, "LN") == 0) {
        return (n && v[0] > 0) ? log(v[0]) : 0;
    } else if (strcmp(u, "LOG10") == 0) {
        return (n && v[0] > 0) ? log10(v[0]) : 0;
    } else if (strcmp(u, "LOG") == 0) {
        if (!n || v[0] <= 0) return 0;
        if (n >= 2 && v[1] > 0 && v[1] != 1) return log(v[0]) / log(v[1]);
        return log10(v[0]);
    } else if (strcmp(u, "PI") == 0) {
        return M_PI;
    } else if (strcmp(u, "SIGN") == 0) {
        return n ? ((v[0] > 0) - (v[0] < 0)) : 0;
    }
    return 0;   /* unknown function */
}

/* Attempt to consume a range (A1:C3) as a function argument. */
static int try_range(EvalCtx *e, double *vals, int *count) {
    skipws(e);
    char c1[8], c2[8];
    int r1, r2, consumed = 0;
    if (sscanf(e->s + e->i, "%7[A-Za-z]%d:%7[A-Za-z]%d%n",
               c1, &r1, c2, &r2, &consumed) == 4) {
        char after = e->s[e->i + consumed];
        if (after == ',' || after == ')' || after == '\0') {
            char rng[40];
            snprintf(rng, sizeof(rng), "%s%d:%s%d", c1, r1, c2, r2);
            double tmp[MAX_ROWS * MAX_COLS];
            int tc = 0;
            get_range_values(e->app, rng, tmp, &tc);
            for (int k = 0; k < tc; k++) vals[(*count)++] = tmp[k];
            e->i += consumed;
            return 1;
        }
    }
    return 0;
}

static double eval_function(EvalCtx *e, const char *name) {
    static double vals[MAX_ROWS * MAX_COLS];   /* static: avoid huge stack */
    int count = 0;
    skipws(e);
    if (e->s[e->i] != ')') {
        do {
            if (!try_range(e, vals, &count)) {
                double v = parse_compare(e);   /* args may be A1>5, IF(...), etc. */
                if (count < MAX_ROWS * MAX_COLS) vals[count++] = v;
            }
            skipws(e);
        } while (e->s[e->i] == ',' && (e->i++, 1));
    }
    skipws(e);
    if (e->s[e->i] == ')') e->i++; else e->error = 1;
    return apply_func(name, vals, count);
}

static double parse_prim(EvalCtx *e) {
    skipws(e);
    char ch = e->s[e->i];

    if (ch == '(') {
        e->i++;
        double v = parse_compare(e);
        skipws(e);
        if (e->s[e->i] == ')') e->i++; else e->error = 1;
        return v;
    }
    if (ch == '+') { e->i++; return parse_power(e); }
    if (ch == '-') { e->i++; return -parse_power(e); }

    if (isalpha((unsigned char)ch)) {
        char name[32];
        int n = 0;
        while (isalpha((unsigned char)e->s[e->i]) && n < 31)
            name[n++] = e->s[e->i++];
        name[n] = '\0';

        size_t after_name = e->i;
        skipws(e);
        if (e->s[e->i] == '(') { e->i++; return eval_function(e, name); }

        e->i = after_name;                     /* cell reference? */
        if (isdigit((unsigned char)e->s[e->i])) {
            int row = 0;
            while (isdigit((unsigned char)e->s[e->i]))
                row = row * 10 + (e->s[e->i++] - '0');
            int col = col_to_index(name);
            int r = row - 1;
            if (r >= 0 && r < MAX_ROWS && col >= 0 && col < MAX_COLS)
                return parse_cell_value(e->app->sheet.data[r][col]);
            return 0;
        }
        e->error = 1;
        return 0;
    }

    if (isdigit((unsigned char)ch) || ch == '.') {
        char *end;
        double v = strtod(e->s + e->i, &end);
        e->i = (size_t)(end - e->s);
        return v;
    }

    e->error = 1;
    return 0;
}

/* exponent: right-associative so 2^3^2 = 2^(3^2). */
static double parse_power(EvalCtx *e) {
    double base = parse_prim(e);
    skipws(e);
    if (e->s[e->i] == '^') { e->i++; return pow(base, parse_power(e)); }
    return base;
}

static double parse_term(EvalCtx *e) {
    double v = parse_power(e);
    for (;;) {
        skipws(e);
        char c = e->s[e->i];
        if (c == '*') { e->i++; v *= parse_power(e); }
        else if (c == '/') {
            e->i++;
            double d = parse_power(e);
            v = (d != 0.0) ? v / d : 0.0;       /* guard div-by-zero */
        } else break;
    }
    return v;
}

static double parse_expr(EvalCtx *e) {
    double v = parse_term(e);
    for (;;) {
        skipws(e);
        char c = e->s[e->i];
        if (c == '+') { e->i++; v += parse_term(e); }
        else if (c == '-') { e->i++; v -= parse_term(e); }
        else break;
    }
    return v;
}

/* Comparisons sit above +/-, produce 1.0 (true) / 0.0 (false), and are the
 * building block for IF/AND/OR. Single, non-associative: A1>B1, A1<=10, x<>0. */
static double parse_compare(EvalCtx *e) {
    double v = parse_expr(e);
    skipws(e);
    char c = e->s[e->i];
    if (c == '=' || c == '<' || c == '>') {
        e->i++;
        char c2 = e->s[e->i];
        if (c == '<' && c2 == '=') { e->i++; return parse_expr(e) >= v ? 1 : 0; }
        if (c == '<' && c2 == '>') { e->i++; return parse_expr(e) != v ? 1 : 0; }
        if (c == '>' && c2 == '=') { e->i++; return v >= parse_expr(e) ? 1 : 0; }
        double r = parse_expr(e);
        if (c == '=') return v == r ? 1 : 0;
        if (c == '<') return v <  r ? 1 : 0;
        return v > r ? 1 : 0;
    }
    return v;
}

static double eval_formula(TabulaApp *app, const char *formula) {
    const char *p = formula;
    while (*p == ' ') p++;
    if (*p == '=') p++;
    EvalCtx e = { p, 0, app, 0 };
    return parse_compare(&e);
    /* sanity: "=1+2*3"=>7 ; "=SUM(A1:A3)" over 1,2,3 =>6 ; "=IF(A1>10,1,0)" */
}

static void fmt_num(double v, char *out, size_t n) {
    if (isnan(v) || isinf(v)) { snprintf(out, n, "0"); return; }
    snprintf(out, n, "%.10g", v);
}

/* ================================================================== *
 * Dependency-graph recalc (topological order + cycle detection)
 * ================================================================== */

#define CYCLE_TEXT "#CYCLE!"

static int g_state[NUM_CELLS];   /* 0 unvisited, 1 visiting (on stack), 2 done */
static int g_cyclic[NUM_CELLS];  /* 1 => cell is on / depends on a cycle       */

static void add_ref(int r, int c, int *ids, int *n, int max) {
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) return;
    if (*n < max) ids[(*n)++] = CELL_ID(r, c);
}

/* Scan a formula for the cells it references, expanding A1:C3 ranges.
 * Writes encoded cell ids into `ids` (capacity `max`), returns the count.
 * A bare letter-run followed by digits is a cell ref; letters followed by
 * '(' are function names and are skipped. A letter-run preceded by a digit
 * or '.' (e.g. the 'e' in 1e5) is treated as part of a number, not a ref. */
static int extract_refs(const char *f, int *ids, int max) {
    int n = 0;
    const char *p = f;
    char prev = '\0';
    while (*p) {
        if (isalpha((unsigned char)*p) && prev != '.' &&
            !isdigit((unsigned char)prev)) {
            char col[8]; int ci = 0;
            while (isalpha((unsigned char)*p) && ci < 7) col[ci++] = *p++;
            col[ci] = '\0';
            if (isdigit((unsigned char)*p)) {
                int row = 0;
                while (isdigit((unsigned char)*p)) row = row * 10 + (*p++ - '0');
                int c1 = col_to_index(col), r1 = row - 1;
                if (*p == ':' && isalpha((unsigned char)p[1])) {
                    p++;                                   /* consume ':' */
                    char col2[8]; int c2i = 0;
                    while (isalpha((unsigned char)*p) && c2i < 7) col2[c2i++] = *p++;
                    col2[c2i] = '\0';
                    int row2 = 0;
                    while (isdigit((unsigned char)*p)) row2 = row2 * 10 + (*p++ - '0');
                    int c2 = col_to_index(col2), r2 = row2 - 1;
                    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
                    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
                    for (int r = r1; r <= r2; r++)
                        for (int c = c1; c <= c2; c++)
                            add_ref(r, c, ids, &n, max);
                } else {
                    add_ref(r1, c1, ids, &n, max);
                }
                prev = '0';
                continue;
            }
            prev = 'A';       /* function name / bareword: p already advanced */
            continue;
        }
        prev = *p;
        p++;
    }
    return n;
}

/* Depth-first evaluate: recurses into dependencies first so each cell is
 * computed after everything it reads. A back-edge to a cell still on the
 * stack marks a cycle, and the cyclic flag propagates to dependents so any
 * cell that (transitively) sits on a cycle shows CYCLE_TEXT. */
static void dfs_recalc(TabulaApp *app, int r, int c) {
    int id = CELL_ID(r, c);
    g_state[id] = 1;

    int *ids = malloc(sizeof(int) * NUM_CELLS);
    int nd = ids ? extract_refs(app->sheet.formula[r][c], ids, NUM_CELLS) : 0;
    for (int k = 0; k < nd; k++) {
        int dep = ids[k];
        int dr = dep / MAX_COLS, dc = dep % MAX_COLS;
        if (app->sheet.formula[dr][dc][0] != '=') continue;  /* only formula nodes */
        if (g_state[dep] == 1) {
            g_cyclic[id] = 1;                                 /* back-edge: cycle */
        } else {
            if (g_state[dep] == 0) dfs_recalc(app, dr, dc);
            if (g_cyclic[dep]) g_cyclic[id] = 1;              /* propagate error  */
        }
    }
    free(ids);

    if (g_cyclic[id]) {
        g_strlcpy(app->sheet.data[r][c], CYCLE_TEXT, MAX_CELL_LEN);
    } else {
        char out[64];
        fmt_num(eval_formula(app, app->sheet.formula[r][c]), out, sizeof(out));
        g_strlcpy(app->sheet.data[r][c], out, MAX_CELL_LEN);
    }
    g_state[id] = 2;
}

/* Recompute every formula cell in dependency order. Deterministic and
 * cycle-safe; plain-value edits are picked up because dependents re-read
 * the freshly written data[][]. */
static void recompute_all(TabulaApp *app) {
    memset(g_state, 0, sizeof(g_state));
    memset(g_cyclic, 0, sizeof(g_cyclic));
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++)
            if (app->sheet.formula[r][c][0] == '=' && g_state[CELL_ID(r, c)] == 0)
                dfs_recalc(app, r, c);
}

/* ================================================================== *
 * Escaping helpers
 * ================================================================== */

static void append_escaped_html(GString *g, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&': g_string_append(g, "&amp;"); break;
            case '<': g_string_append(g, "&lt;");  break;
            case '>': g_string_append(g, "&gt;");  break;
            default:  g_string_append_c(g, *s);     break;
        }
    }
}

/* Escape for use inside a single-quoted HTML attribute (data-f='...'). */
static void append_escaped_attr(GString *g, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&':  g_string_append(g, "&amp;");  break;
            case '<':  g_string_append(g, "&lt;");   break;
            case '>':  g_string_append(g, "&gt;");   break;
            case '\'': g_string_append(g, "&#39;");  break;
            case '"':  g_string_append(g, "&quot;"); break;
            default:   g_string_append_c(g, *s);      break;
        }
    }
}

static char *js_escape(const char *s) {
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        switch (*s) {
            case '\\': g_string_append(g, "\\\\"); break;
            case '\'': g_string_append(g, "\\'");  break;
            case '\n': g_string_append(g, "\\n");  break;
            case '\r': g_string_append(g, "\\r");  break;
            default:   g_string_append_c(g, *s);    break;
        }
    }
    return g_string_free(g, FALSE);
}

/* ================================================================== *
 * HTML generation (dynamic - never overflows)
 * ================================================================== */

static const char *HTML_HEAD =
"<!DOCTYPE html><html><head><meta charset='UTF-8'><style>"
"*{margin:0;padding:0;box-sizing:border-box;}"
"body{font-family:'Courier New',Courier,Monaco,monospace;background:#030705;padding:16px;height:100vh;overflow:auto;color:#33ff33;}"
"#container{display:flex;flex-direction:column;gap:16px;}"
"#toolbar{display:flex;gap:8px;background:#0a140f;padding:8px;border:1px solid #33ff33;flex-wrap:wrap;align-items:center;}"
".btn{padding:8px 16px;background:#030705;color:#33ff33;border:1px solid #33ff33;cursor:pointer;font-size:12px;font-weight:bold;font-family:'Courier New',Courier,Monaco,monospace;text-transform:uppercase;transition:all 0.2s;}"
".btn:hover{background:#33ff33;color:#030705;box-shadow:0 0 8px #33ff33;}"
"#search-box{padding:6px 10px;background:#030705;color:#33ff33;border:1px solid #33ff33;font-family:'Courier New',Courier,Monaco,monospace;font-size:12px;}"
"#fbar-row{display:flex;align-items:stretch;gap:8px;background:#0a140f;padding:6px 8px;border:1px solid #33ff33;}"
"#fbar-addr{min-width:48px;padding:6px 8px;background:#030705;border:1px solid #1b3a24;text-align:center;font-weight:bold;}"
"#formula-bar{flex:1;padding:6px 10px;background:#030705;color:#33ff33;border:1px solid #33ff33;font-family:'Courier New',Courier,Monaco,monospace;font-size:13px;}"
"table{border-collapse:collapse;background:#030705;border:1px solid #1b3a24;}"
"th,td{border:1px solid #1b3a24;padding:6px 8px;text-align:left;color:#33ff33;font-family:'Courier New',Courier,Monaco,monospace;font-size:12px;min-width:64px;}"
"th{background:#0a140f;font-weight:bold;border-bottom:2px solid #33ff33;text-align:center;}"
"th.rowhdr{position:sticky;left:0;min-width:32px;}"
"td{cursor:cell;transition:background 0.15s;}"
"td:focus{background:#33ff33;color:#030705;outline:none;}"
".highlight{background:#1b3a24;}"
"#genie-result table{margin-top:8px;}"
"#genie-result th{border-bottom:2px solid #33ff33;}"
"#aboutov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;align-items:center;justify-content:center;}"
"#aboutov.on{display:flex;}"
"#aboutbox{background:#0a140f;border:2px solid #33ff33;color:#33ff33;max-width:480px;margin:16px;padding:20px;font-family:'Courier New',Courier,Monaco,monospace;font-size:12px;line-height:1.7;box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#aboutbox .atitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#aboutbox .aok{margin-top:16px;text-align:right;}"
"</style></head><body>"
"<div id='container'><div id='toolbar'>"
"<button class='btn' onclick='openFile()'>OPEN</button>"
"<button class='btn' onclick='saveSheet()'>SAVE</button>"
"<button class='btn' onclick='exportCsv()'>EXPORT</button>"
"<button class='btn' onclick='newSheet()'>NEW</button>"
"<button class='btn' onclick='recalc()'>RECALC</button>"
"<button class='btn' onclick='launchGenie()'>GENIE</button>"
"<button class='btn' onclick='launchChart()'>CHART</button>"
"<button class='btn' onclick='addRow()'>+ROW</button>"
"<button class='btn' onclick='addCol()'>+COL</button>"
"<button class='btn' onclick='showAbout()'>ABOUT</button>"
"<input id='search-box' type='text' placeholder='Ctrl+F Search...' onkeyup='searchCells(this.value)'>"
"</div>"
"<div id='fbar-row'>"
"<span id='fbar-addr'>A1</span>"
"<input id='formula-bar' type='text' placeholder='Raw formula or value of the selected cell...'>"
"</div>"
"<table id='grid'>";

static const char *SCRIPT_FMT =
"</table><div id='chart-panel'></div><div id='genie-result'></div></div>"
"<div id='aboutov' onclick='if(event.target===this)closeAbout()'>"
"<div id='aboutbox'>"
"<div class='atitle'>Tabula &mdash; part of LeviathanOS</div>"
"Free software under the GNU General Public License, version 3.<br>"
"This program comes with ABSOLUTELY NO WARRANTY.<br>"
"Full license: /usr/share/doc/leviathanos/LICENSE<br>"
"https://www.gnu.org/licenses/gpl-3.0.html"
"<div class='aok'><button class='btn' onclick='closeAbout()'>OK</button></div>"
"</div></div>"
"<script>"
"function showAbout(){document.getElementById('aboutov').classList.add('on');}"
"function closeAbout(){document.getElementById('aboutov').classList.remove('on');}"
"var ROWS=%d, COLS=%d;"
"var activeR=0, activeC=0;"
"function colLabel(c){var s='';c=c+1;while(c>0){var m=(c-1)%%26;s=String.fromCharCode(65+m)+s;c=Math.floor((c-1)/26);}return s;}"
"function cellAt(r,c){return document.querySelector('td[data-r=\"'+r+'\"][data-c=\"'+c+'\"]');}"
"function focusCell(r,c){var el=cellAt(r,c);if(el){el.focus();}}"
"function updateFbar(t){activeR=parseInt(t.dataset.r);activeC=parseInt(t.dataset.c);"
"document.getElementById('fbar-addr').innerText=colLabel(activeC)+(activeR+1);"
"var f=t.getAttribute('data-f');"
"document.getElementById('formula-bar').value=(f&&f.length)?f:t.innerText;}"
"function bind(cell){"
"cell.addEventListener('focus',function(e){updateFbar(e.target);});"
"cell.addEventListener('blur',function(e){var t=e.target;"
"window.webkit.messageHandlers.cellEdit.postMessage({r:parseInt(t.dataset.r),c:parseInt(t.dataset.c),v:t.innerText});});"
"cell.addEventListener('keydown',function(e){var t=e.target;var r=parseInt(t.dataset.r),c=parseInt(t.dataset.c);"
"if(e.key==='Enter'){e.preventDefault();t.blur();focusCell(r+1,c);}"
"else if(e.key==='Tab'){e.preventDefault();t.blur();focusCell(r,e.shiftKey?c-1:c+1);}"
"else if(e.ctrlKey&&(e.key==='f'||e.key==='F')){e.preventDefault();document.getElementById('search-box').focus();}"
"});}"
"document.querySelectorAll('td').forEach(bind);"
"var fbar=document.getElementById('formula-bar');"
"fbar.addEventListener('keydown',function(e){"
"if(e.key==='Enter'){e.preventDefault();"
"window.webkit.messageHandlers.fbarEdit.postMessage({r:activeR,c:activeC,v:fbar.value});"
"focusCell(activeR,activeC);}});"
"document.addEventListener('keydown',function(e){if(e.ctrlKey&&(e.key==='r'||e.key==='R')){e.preventDefault();recalc();}});"
"function openFile(){window.webkit.messageHandlers.openFile.postMessage('');}"
"function flush(){if(document.activeElement&&document.activeElement.tagName==='TD')document.activeElement.blur();}"
"function saveSheet(){flush();window.webkit.messageHandlers.save.postMessage('');}"
"function exportCsv(){flush();window.webkit.messageHandlers.exportCsv.postMessage('');}"
"function newSheet(){if(confirm('Clear the entire spreadsheet?')){window.webkit.messageHandlers.newSheet.postMessage('');}}"
"function recalc(){flush();window.webkit.messageHandlers.recalc.postMessage('');}"
"function launchGenie(){flush();window.webkit.messageHandlers.genie.postMessage('');}"
"function launchChart(){flush();window.webkit.messageHandlers.chart.postMessage('');}"
"function addRow(){flush();window.webkit.messageHandlers.addRow.postMessage('');}"
"function addCol(){flush();window.webkit.messageHandlers.addCol.postMessage('');}"
/* renderChart(labels[],values[],type) — canvas bar/line/pie, injected from C */
"function renderChart(labels,values,type){var p=document.getElementById('chart-panel');"
"p.innerHTML='';if(!values.length){p.innerHTML='<p style=\"color:#33ff33\">No numeric data in that range.</p>';return;}"
"var cv=document.createElement('canvas');cv.width=680;cv.height=360;"
"cv.style.border='1px solid #33ff33';cv.style.background='#030705';cv.style.marginTop='8px';p.appendChild(cv);"
"var x=cv.getContext('2d'),W=cv.width,H=cv.height,pad=44,n=values.length;"
"var cols=['#33ff33','#2bd4d4','#d4d42b','#d42bd4','#2b6bd4','#d4802b','#8fff8f','#ff6b6b'];"
"x.font='11px monospace';x.fillStyle='#8fbf8f';"
"if(type==='pie'){var tot=0;for(var i=0;i<n;i++)tot+=Math.abs(values[i]);"
"var a=-Math.PI/2,cx=W*0.36,cy=H/2,rd=Math.min(W*0.6,H)/2-pad;"
"for(var i=0;i<n;i++){var sl=Math.abs(values[i])/(tot||1)*2*Math.PI;x.fillStyle=cols[i%cols.length];"
"x.beginPath();x.moveTo(cx,cy);x.arc(cx,cy,rd,a,a+sl);x.closePath();x.fill();a+=sl;"
"x.fillStyle=cols[i%cols.length];x.fillRect(W*0.72,30+i*20,12,12);"
"x.fillStyle='#cfe';x.fillText((labels[i]||('#'+(i+1)))+' ('+values[i]+')',W*0.72+18,40+i*20);}return;}"
"var mx=Math.max.apply(null,values.concat([0])),mn=Math.min.apply(null,values.concat([0]));"
"if(mx===mn)mx=mn+1;"
"function X(i){return pad+(i+0.5)*((W-2*pad)/n);}"
"function Y(v){return H-pad-((v-mn)/(mx-mn))*(H-2*pad);}"
"x.strokeStyle='#1b3a24';x.beginPath();x.moveTo(pad,Y(0));x.lineTo(W-pad,Y(0));"
"x.moveTo(pad,pad);x.lineTo(pad,H-pad);x.stroke();"
"var bw=(W-2*pad)/n*0.6;"
"for(var i=0;i<n;i++){var px=X(i),py=Y(values[i]);"
"if(type==='line'){x.fillStyle='#33ff33';x.fillRect(px-2,py-2,4,4);"
"if(i>0){x.strokeStyle='#33ff33';x.beginPath();x.moveTo(X(i-1),Y(values[i-1]));x.lineTo(px,py);x.stroke();}}"
"else{x.fillStyle=cols[i%cols.length];x.fillRect(px-bw/2,Math.min(py,Y(0)),bw,Math.abs(py-Y(0)));}"
"x.save();x.fillStyle='#8fbf8f';x.translate(px,H-pad+4);x.textAlign='center';"
"x.fillText((labels[i]||('#'+(i+1))).substring(0,9),0,8);x.restore();}}"
"function searchCells(q){document.querySelectorAll('td').forEach(function(cell){"
"if(q&&cell.innerText.toLowerCase().indexOf(q.toLowerCase())!==-1){cell.classList.add('highlight');}"
"else{cell.classList.remove('highlight');}});}"
"</script></body></html>";

static char *build_html(TabulaApp *app) {
    GString *g = g_string_new(NULL);
    g_string_append(g, HTML_HEAD);

    /* column header row (labels generated lazily: A..Z, AA..AZ) */
    g_string_append(g, "<tr><th class='rowhdr'></th>");
    for (int c = 0; c < app->sheet.cols; c++) {
        char lbl[8];
        index_to_col(c, lbl, sizeof(lbl));
        g_string_append_printf(g, "<th>%s</th>", lbl);
    }
    g_string_append(g, "</tr>");

    /* data rows: cell shows computed value; data-f carries the raw formula */
    for (int r = 0; r < app->sheet.rows; r++) {
        g_string_append_printf(g, "<tr><th class='rowhdr'>%d</th>", r + 1);
        for (int c = 0; c < app->sheet.cols; c++) {
            g_string_append_printf(g,
                "<td contenteditable='true' data-r='%d' data-c='%d' data-f='", r, c);
            append_escaped_attr(g, app->sheet.formula[r][c]);
            g_string_append(g, "'>");
            append_escaped_html(g, app->sheet.data[r][c]);
            g_string_append(g, "</td>");
        }
        g_string_append(g, "</tr>");
    }

    g_string_append_printf(g, SCRIPT_FMT, app->sheet.rows, app->sheet.cols);
    return g_string_free(g, FALSE);
}

static void reload_grid(TabulaApp *app) {
    char *html = build_html(app);
    webkit_web_view_load_html(app->web_view, html, NULL);
    g_free(html);
}

/* Push a single cell's computed value AND raw formula back into the DOM,
 * keeping data-f (used by the formula bar) in sync with C state. */
static void push_cell(TabulaApp *app, int r, int c) {
    char *disp = js_escape(app->sheet.data[r][c]);
    char *frm  = js_escape(app->sheet.formula[r][c]);
    GString *js = g_string_new(NULL);
    g_string_append_printf(js,
        "var el=document.querySelector('td[data-r=\"%d\"][data-c=\"%d\"]');"
        "if(el){el.innerText='%s';el.setAttribute('data-f','%s');}",
        r, c, disp, frm);
    webkit_web_view_evaluate_javascript(app->web_view, js->str, -1,
                                        NULL, NULL, NULL, NULL, NULL);
    g_string_free(js, TRUE);
    g_free(disp);
    g_free(frm);
}

/* Recompute the whole sheet, then refresh the display of every formula cell
 * (plus the just-edited cell) so the grid mirrors C state exactly. */
static void recalc_and_push(TabulaApp *app, int er, int ec) {
    recompute_all(app);
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++)
            if (app->sheet.formula[r][c][0] == '=')
                push_cell(app, r, c);
    if (er >= 0 && ec >= 0) push_cell(app, er, ec);
}

/* ================================================================== *
 * CSV load / save
 * ================================================================== */

static int field_needs_quote(const char *s) {
    return strpbrk(s, ",\"\n\r") != NULL;
}

static void write_csv(TabulaApp *app, const char *path) {
    int maxr = -1, maxc = -1;
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++)
            if (app->sheet.data[r][c][0]) {
                if (r > maxr) maxr = r;
                if (c > maxc) maxc = c;
            }

    FILE *f = fopen(path, "w");
    if (!f) { g_print("Tabula: cannot write %s\n", path); return; }

    for (int r = 0; r <= maxr; r++) {
        for (int c = 0; c <= maxc; c++) {
            if (c) fputc(',', f);
            const char *s = app->sheet.data[r][c];
            if (field_needs_quote(s)) {
                fputc('"', f);
                for (const char *p = s; *p; p++) {
                    if (*p == '"') fputc('"', f);
                    fputc(*p, f);
                }
                fputc('"', f);
            } else {
                fputs(s, f);
            }
        }
        fputc('\n', f);
    }
    fclose(f);
    g_print("Tabula: saved %s\n", path);
}

static void clear_sheet(TabulaApp *app) {
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            app->sheet.data[r][c][0] = '\0';
            app->sheet.formula[r][c][0] = '\0';
        }
}

/* Grow the visible grid so it covers all populated cells (with a little
 * headroom), never shrinking below the comfortable default. Called after a
 * load so imported files aren't clipped by the initial view size. */
#define DEFAULT_ROWS 40
#define DEFAULT_COLS 16
static void fit_dimensions(TabulaApp *app) {
    int maxr = 0, maxc = 0;
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++)
            if (app->sheet.data[r][c][0] || app->sheet.formula[r][c][0]) {
                if (r > maxr) maxr = r;
                if (c > maxc) maxc = c;
            }
    int wr = maxr + 2, wc = maxc + 2;
    if (wr < DEFAULT_ROWS) wr = DEFAULT_ROWS;
    if (wc < DEFAULT_COLS) wc = DEFAULT_COLS;
    if (wr > MAX_ROWS) wr = MAX_ROWS;
    if (wc > MAX_COLS) wc = MAX_COLS;
    if (wr > app->sheet.rows) app->sheet.rows = wr;
    if (wc > app->sheet.cols) app->sheet.cols = wc;
}

static void load_csv(TabulaApp *app, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { g_print("Tabula: cannot read %s\n", path); return; }

    clear_sheet(app);

    int r = 0, c = 0, fi = 0, inq = 0, any = 0, ch;
    char field[MAX_CELL_LEN];

    while ((ch = fgetc(f)) != EOF) {
        if (inq) {
            if (ch == '"') {
                int nx = fgetc(f);
                if (nx == '"') { if (fi < MAX_CELL_LEN - 1) field[fi++] = '"'; }
                else { inq = 0; if (nx != EOF) ungetc(nx, f); }
            } else if (fi < MAX_CELL_LEN - 1) field[fi++] = (char)ch;
        } else if (ch == '"') {
            inq = 1; any = 1;
        } else if (ch == ',') {
            field[fi] = '\0';
            if (r < MAX_ROWS && c < MAX_COLS)
                g_strlcpy(app->sheet.data[r][c], field, MAX_CELL_LEN);
            c++; fi = 0; any = 1;
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r') { int nx = fgetc(f); if (nx != '\n' && nx != EOF) ungetc(nx, f); }
            field[fi] = '\0';
            if (r < MAX_ROWS && c < MAX_COLS)
                g_strlcpy(app->sheet.data[r][c], field, MAX_CELL_LEN);
            r++; c = 0; fi = 0; any = 0;
            if (r >= MAX_ROWS) break;
        } else {
            if (fi < MAX_CELL_LEN - 1) field[fi++] = (char)ch;
            any = 1;
        }
    }
    /* last field on a file that does not end with a newline */
    if ((fi > 0 || c > 0 || any) && r < MAX_ROWS && c < MAX_COLS) {
        field[fi] = '\0';
        g_strlcpy(app->sheet.data[r][c], field, MAX_CELL_LEN);
    }
    fclose(f);
    fit_dimensions(app);
    g_print("Tabula: loaded %s\n", path);
}

/* ================================================================== *
 * Native .tab format  (round-trips raw formulas, unlike CSV)
 * ------------------------------------------------------------------
 * Line-oriented, tab-separated text:
 *   TABULA1                         <- magic header line
 *   F<TAB>r<TAB>c<TAB>=FORMULA      <- a formula cell (raw formula kept)
 *   V<TAB>r<TAB>c<TAB>VALUE         <- a plain value cell
 * Payloads are escaped so tabs/newlines/backslashes survive a round trip.
 * ================================================================== */

static void tab_escape(FILE *f, const char *s) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '\\': fputs("\\\\", f); break;
            case '\t': fputs("\\t", f);  break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            default:   fputc(*p, f);      break;
        }
    }
}

static void tab_unescape(const char *src, char *dst, size_t n) {
    size_t o = 0;
    for (const char *p = src; *p && o < n - 1; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 't': dst[o++] = '\t'; break;
                case 'n': dst[o++] = '\n'; break;
                case 'r': dst[o++] = '\r'; break;
                case '\\': dst[o++] = '\\'; break;
                default:  dst[o++] = *p;   break;
            }
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

static void write_tab(TabulaApp *app, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { g_print("Tabula: cannot write %s\n", path); return; }
    fputs("TABULA1\n", f);
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            if (app->sheet.formula[r][c][0] == '=') {
                fprintf(f, "F\t%d\t%d\t", r, c);
                tab_escape(f, app->sheet.formula[r][c]);
                fputc('\n', f);
            } else if (app->sheet.data[r][c][0]) {
                fprintf(f, "V\t%d\t%d\t", r, c);
                tab_escape(f, app->sheet.data[r][c]);
                fputc('\n', f);
            }
        }
    fclose(f);
    g_print("Tabula: saved %s\n", path);
}

static void load_tab(TabulaApp *app, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { g_print("Tabula: cannot read %s\n", path); return; }

    char line[MAX_CELL_LEN * 3 + 32];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "TABULA", 6) != 0) {
        fclose(f);
        g_print("Tabula: %s is not a native .tab file\n", path);
        return;
    }

    clear_sheet(app);
    while (fgets(line, sizeof(line), f)) {
        char type = line[0];
        if (type != 'F' && type != 'V') continue;
        /* locate the three tab separators: type \t r \t c \t payload */
        char *t1 = strchr(line, '\t');            if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');          if (!t2) continue;
        char *t3 = strchr(t2 + 1, '\t');          if (!t3) continue;
        int r = atoi(t1 + 1);
        int c = atoi(t2 + 1);
        char *payload = t3 + 1;
        payload[strcspn(payload, "\r\n")] = '\0';
        if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) continue;
        char decoded[MAX_CELL_LEN];
        tab_unescape(payload, decoded, sizeof(decoded));
        if (type == 'F')
            g_strlcpy(app->sheet.formula[r][c], decoded, MAX_CELL_LEN);
        else
            g_strlcpy(app->sheet.data[r][c], decoded, MAX_CELL_LEN);
    }
    fclose(f);
    recompute_all(app);           /* rebuild computed values from live formulas */
    fit_dimensions(app);
    g_print("Tabula: loaded %s\n", path);
}

/* ================================================================== *
 * WebKit message handlers
 * ================================================================== */

/* Apply an edit (from either the grid cell or the formula bar), then do a
 * dependency-aware recalc and refresh affected displays. */
static void apply_edit(TabulaApp *app, int r, int c, const char *text) {
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) return;
    if (text[0] == '=') {
        g_strlcpy(app->sheet.formula[r][c], text, MAX_CELL_LEN);
    } else {
        app->sheet.formula[r][c][0] = '\0';
        g_strlcpy(app->sheet.data[r][c], text, MAX_CELL_LEN);
    }
    recalc_and_push(app, r, c);
}

static void on_cell_edit(WebKitUserContentManager *manager,
                         WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    JSCValue *rv = jsc_value_object_get_property(value, "r");
    JSCValue *cv = jsc_value_object_get_property(value, "c");
    JSCValue *vv = jsc_value_object_get_property(value, "v");
    int r = jsc_value_to_int32(rv);
    int c = jsc_value_to_int32(cv);
    char *text = jsc_value_to_string(vv);

    if (text) apply_edit(app, r, c, text);

    if (text) g_free(text);
    g_object_unref(rv);
    g_object_unref(cv);
    g_object_unref(vv);
    g_object_unref(value);
}

/* Formula bar committed (Enter): identical semantics to a grid edit. */
static void on_fbar_edit(WebKitUserContentManager *manager,
                         WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    JSCValue *rv = jsc_value_object_get_property(value, "r");
    JSCValue *cv = jsc_value_object_get_property(value, "c");
    JSCValue *vv = jsc_value_object_get_property(value, "v");
    int r = jsc_value_to_int32(rv);
    int c = jsc_value_to_int32(cv);
    char *text = jsc_value_to_string(vv);

    if (text) apply_edit(app, r, c, text);

    if (text) g_free(text);
    g_object_unref(rv);
    g_object_unref(cv);
    g_object_unref(vv);
    g_object_unref(value);
}

static void on_recalc(WebKitUserContentManager *manager,
                      WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    recalc_and_push(app, -1, -1);      /* dependency-aware, cycle-safe */
    g_object_unref(value);
}

static gboolean choose_save_path(TabulaApp *app, char *out, size_t n,
                                 const char *title, const char *default_name) {
    GtkWidget *d = gtk_file_chooser_dialog_new(title, app->window,
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), default_name);
    gboolean ok = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (fn) { strncpy(out, fn, n - 1); out[n - 1] = '\0'; g_free(fn); ok = TRUE; }
    }
    gtk_widget_destroy(d);
    return ok;
}

static void on_save(WebKitUserContentManager *manager,
                    WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    /* SAVE uses the native .tab format so formulas round-trip. */
    if (app->current_file[0] == '\0' ||
        !g_str_has_suffix(app->current_file, ".tab")) {
        if (!choose_save_path(app, app->current_file, MAX_PATH,
                              "Save Sheet (.tab)", "sheet.tab")) {
            g_object_unref(value);
            return;
        }
    }
    write_tab(app, app->current_file);
    g_object_unref(value);
}

static void on_export(WebKitUserContentManager *manager,
                      WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    char path[MAX_PATH];
    if (choose_save_path(app, path, sizeof(path), "Export CSV", "sheet.csv"))
        write_csv(app, path);
    g_object_unref(value);
}

static void on_new_sheet(WebKitUserContentManager *manager,
                         WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    clear_sheet(app);
    app->current_file[0] = '\0';
    reload_grid(app);
    g_object_unref(value);
}

static void on_file_open_clicked(WebKitUserContentManager *manager,
                                 WebKitJavascriptResult *js_result,
                                 TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open Sheet (.tab / .csv)",
        app->window, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            strncpy(app->current_file, filename, MAX_PATH - 1);
            app->current_file[MAX_PATH - 1] = '\0';
            if (g_str_has_suffix(filename, ".tab"))
                load_tab(app, filename);        /* native: restores formulas */
            else
                load_csv(app, filename);        /* CSV: values only */
            g_free(filename);
            reload_grid(app);
        }
    }
    gtk_widget_destroy(dialog);
    g_object_unref(value);
}

/* ---- Genie: a real pivot table ---------------------------------- */
static void on_genie_clicked(WebKitUserContentManager *manager,
                             WebKitJavascriptResult *js_result,
                             TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Genie - Pivot Table",
        app->window, GTK_DIALOG_MODAL,
        "_Create", GTK_RESPONSE_ACCEPT, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkEntry *group_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(group_entry, "A");
    GtkEntry *value_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(value_entry, "B");

    GtkComboBoxText *func_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    const char *funcs[] = { "COUNT", "SUM", "AVERAGE", "MIN", "MAX",
                            "MEDIAN", "PRODUCT", "STDEV" };
    for (unsigned i = 0; i < G_N_ELEMENTS(funcs); i++)
        gtk_combo_box_text_append(func_combo, funcs[i], funcs[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(func_combo), 0);

    gtk_box_pack_start(content, gtk_label_new("Group by column (e.g. A):"), FALSE, FALSE, 0);
    gtk_box_pack_start(content, GTK_WIDGET(group_entry), FALSE, FALSE, 0);
    gtk_box_pack_start(content, gtk_label_new("Value column (e.g. B):"), FALSE, FALSE, 0);
    gtk_box_pack_start(content, GTK_WIDGET(value_entry), FALSE, FALSE, 0);
    gtk_box_pack_start(content, gtk_label_new("Aggregation:"), FALSE, FALSE, 0);
    gtk_box_pack_start(content, GTK_WIDGET(func_combo), FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        int gcol = col_to_index(gtk_entry_get_text(group_entry));
        int vcol = col_to_index(gtk_entry_get_text(value_entry));
        gchar *func = gtk_combo_box_text_get_active_text(func_combo);

        char keys[MAX_GROUPS][MAX_CELL_LEN];
        int  occ[MAX_GROUPS];
        int  vcount[MAX_GROUPS];
        double *vbuf = calloc((size_t)MAX_GROUPS * MAX_ROWS, sizeof(double));
        int ng = 0;
        memset(occ, 0, sizeof(occ));
        memset(vcount, 0, sizeof(vcount));

        if (func && vbuf && gcol >= 0 && gcol < MAX_COLS) {
            for (int r = 0; r < MAX_ROWS; r++) {
                const char *k = app->sheet.data[r][gcol];
                if (!k[0]) continue;
                int gi = -1;
                for (int i = 0; i < ng; i++)
                    if (strcmp(keys[i], k) == 0) { gi = i; break; }
                if (gi < 0) {
                    if (ng >= MAX_GROUPS) continue;
                    gi = ng++;
                    strncpy(keys[gi], k, MAX_CELL_LEN - 1);
                    keys[gi][MAX_CELL_LEN - 1] = '\0';
                }
                occ[gi]++;
                if (vcol >= 0 && vcol < MAX_COLS && app->sheet.data[r][vcol][0])
                    vbuf[(size_t)gi * MAX_ROWS + vcount[gi]++] =
                        parse_cell_value(app->sheet.data[r][vcol]);
            }

            char glbl[8], vlbl[8];
            index_to_col(gcol, glbl, sizeof(glbl));
            if (vcol >= 0 && vcol < MAX_COLS)
                index_to_col(vcol, vlbl, sizeof(vlbl));
            else
                g_strlcpy(vlbl, "?", sizeof(vlbl));
            GString *tbl = g_string_new(NULL);
            g_string_append_printf(tbl,
                "<h3>Pivot: group %s, %s of %s</h3><table><tr><th>KEY</th><th>%s</th></tr>",
                glbl, func, vlbl, func);
            for (int i = 0; i < ng; i++) {
                double res = (strcmp(func, "COUNT") == 0)
                    ? occ[i]
                    : apply_func(func, &vbuf[(size_t)i * MAX_ROWS], vcount[i]);
                char num[64];
                fmt_num(res, num, sizeof(num));
                g_string_append(tbl, "<tr><td>");
                append_escaped_html(tbl, keys[i]);
                g_string_append_printf(tbl, "</td><td>%s</td></tr>", num);
            }
            g_string_append(tbl, "</table>");

            char *esc = js_escape(tbl->str);
            GString *js = g_string_new(NULL);
            g_string_append_printf(js,
                "document.getElementById('genie-result').innerHTML='%s';", esc);
            webkit_web_view_evaluate_javascript(app->web_view, js->str, -1,
                                                NULL, NULL, NULL, NULL, NULL);
            g_free(esc);
            g_string_free(js, TRUE);
            g_string_free(tbl, TRUE);
        }
        free(vbuf);
        g_free(func);
    }

    gtk_widget_destroy(dialog);
    g_object_unref(value);
}

/* ---- Chart: render a cell range as a bar / line / pie canvas ---- */
static int parse_range_bounds(const char *range,
                              int *sr, int *sc, int *er, int *ec) {
    char c1[8], c2[8]; int r1, r2;
    if (sscanf(range, "%7[A-Za-z]%d:%7[A-Za-z]%d", c1, &r1, c2, &r2) == 4) {
        *sc = col_to_index(c1); *ec = col_to_index(c2);
        *sr = r1 - 1;           *er = r2 - 1;
    } else if (sscanf(range, "%7[A-Za-z]%d", c1, &r1) == 2) {
        *sc = *ec = col_to_index(c1); *sr = *er = r1 - 1;
    } else {
        return 0;
    }
    if (*sc > *ec) { int t = *sc; *sc = *ec; *ec = t; }
    if (*sr > *er) { int t = *sr; *sr = *er; *er = t; }
    return 1;
}

static void on_chart_clicked(WebKitUserContentManager *manager,
                             WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Chart",
        app->window, GTK_DIALOG_MODAL,
        "_Create", GTK_RESPONSE_ACCEPT, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkEntry *lab = GTK_ENTRY(gtk_entry_new()); gtk_entry_set_text(lab, "A1:A6");
    GtkEntry *val = GTK_ENTRY(gtk_entry_new()); gtk_entry_set_text(val, "B1:B6");
    GtkComboBoxText *type = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append(type, "bar",  "Bar");
    gtk_combo_box_text_append(type, "line", "Line");
    gtk_combo_box_text_append(type, "pie",  "Pie");
    gtk_combo_box_set_active(GTK_COMBO_BOX(type), 0);

    gtk_box_pack_start(content, gtk_label_new("Label range (e.g. A1:A6):"), FALSE, FALSE, 0);
    gtk_box_pack_start(content, GTK_WIDGET(lab), FALSE, FALSE, 0);
    gtk_box_pack_start(content, gtk_label_new("Value range (e.g. B1:B6):"), FALSE, FALSE, 0);
    gtk_box_pack_start(content, GTK_WIDGET(val), FALSE, FALSE, 0);
    gtk_box_pack_start(content, gtk_label_new("Chart type:"), FALSE, FALSE, 0);
    gtk_box_pack_start(content, GTK_WIDGET(type), FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *ty = gtk_combo_box_text_get_active_text(type);
        GString *labs = g_string_new("["), *vals = g_string_new("[");
        int sr, sc, er, ec, first = 1;

        if (parse_range_bounds(gtk_entry_get_text(lab), &sr, &sc, &er, &ec))
            for (int r = sr; r <= er; r++)
                for (int c = sc; c <= ec; c++) {
                    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) continue;
                    if (!first) g_string_append_c(labs, ',');
                    first = 0;
                    char *esc = js_escape(app->sheet.data[r][c]);
                    g_string_append_printf(labs, "'%s'", esc);
                    g_free(esc);
                }
        g_string_append_c(labs, ']');

        first = 1;
        if (parse_range_bounds(gtk_entry_get_text(val), &sr, &sc, &er, &ec))
            for (int r = sr; r <= er; r++)
                for (int c = sc; c <= ec; c++) {
                    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) continue;
                    if (!first) g_string_append_c(vals, ',');
                    first = 0;
                    char b[64];
                    fmt_num(parse_cell_value(app->sheet.data[r][c]), b, sizeof b);
                    g_string_append(vals, b);
                }
        g_string_append_c(vals, ']');

        GString *js = g_string_new(NULL);
        g_string_append_printf(js, "renderChart(%s,%s,'%s');",
                               labs->str, vals->str, ty ? ty : "bar");
        webkit_web_view_evaluate_javascript(app->web_view, js->str, -1,
                                            NULL, NULL, NULL, NULL, NULL);
        g_string_free(js, TRUE);
        g_string_free(labs, TRUE);
        g_string_free(vals, TRUE);
        g_free(ty);
    }
    gtk_widget_destroy(dialog);
    g_object_unref(value);
}

static void on_add_row(WebKitUserContentManager *manager,
                       WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (app->sheet.rows < MAX_ROWS) { app->sheet.rows++; reload_grid(app); }
    g_object_unref(value);
}

static void on_add_col(WebKitUserContentManager *manager,
                       WebKitJavascriptResult *js_result, TabulaApp *app) {
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (app->sheet.cols < MAX_COLS) { app->sheet.cols++; reload_grid(app); }
    g_object_unref(value);
}

/* ================================================================== *
 * UI setup
 * ================================================================== */

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)widget; (void)event; (void)data;
    return FALSE;
}

static void setup_ui(TabulaApp *app) {
    app->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(app->window, "Tabula - Spreadsheet");
    gtk_window_set_default_size(app->window, 1000, 700);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), NULL);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
    gtk_container_add(GTK_CONTAINER(app->window), GTK_WIDGET(vbox));

    app->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_box_pack_start(vbox, GTK_WIDGET(app->web_view), TRUE, TRUE, 0);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_web_view_set_settings(app->web_view, settings);

    WebKitUserContentManager *manager =
        webkit_web_view_get_user_content_manager(app->web_view);

    const char *handlers[] = { "cellEdit", "fbarEdit", "save", "exportCsv",
                               "openFile", "genie", "recalc", "newSheet",
                               "chart", "addRow", "addCol" };
    for (unsigned i = 0; i < G_N_ELEMENTS(handlers); i++)
        webkit_user_content_manager_register_script_message_handler(manager, handlers[i]);

    g_signal_connect(manager, "script-message-received::cellEdit",  G_CALLBACK(on_cell_edit), app);
    g_signal_connect(manager, "script-message-received::fbarEdit",  G_CALLBACK(on_fbar_edit), app);
    g_signal_connect(manager, "script-message-received::save",      G_CALLBACK(on_save), app);
    g_signal_connect(manager, "script-message-received::exportCsv", G_CALLBACK(on_export), app);
    g_signal_connect(manager, "script-message-received::openFile",  G_CALLBACK(on_file_open_clicked), app);
    g_signal_connect(manager, "script-message-received::genie",     G_CALLBACK(on_genie_clicked), app);
    g_signal_connect(manager, "script-message-received::recalc",    G_CALLBACK(on_recalc), app);
    g_signal_connect(manager, "script-message-received::newSheet",  G_CALLBACK(on_new_sheet), app);
    g_signal_connect(manager, "script-message-received::chart",     G_CALLBACK(on_chart_clicked), app);
    g_signal_connect(manager, "script-message-received::addRow",    G_CALLBACK(on_add_row), app);
    g_signal_connect(manager, "script-message-received::addCol",    G_CALLBACK(on_add_col), app);

    reload_grid(app);
    gtk_widget_show_all(GTK_WIDGET(app->window));
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    static TabulaApp tabula_app;      /* static: sheet is large (BSS, not stack) */
    memset(&tabula_app, 0, sizeof(tabula_app));
    tabula_app.sheet.rows = DEFAULT_ROWS;   /* modest view; grows via +ROW / load */
    tabula_app.sheet.cols = DEFAULT_COLS;

    setup_ui(&tabula_app);
    gtk_main();
    return 0;
}
