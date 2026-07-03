#include "datac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void ltrim(char *s) {
    int i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void rtrim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void trim(char *s) { ltrim(s); rtrim(s); }

/* Strip ';' comments, respecting quoted strings. */
static void strip_comment(char *line) {
    int in_quote = 0;
    char qc = 0;
    for (int i = 0; line[i]; i++) {
        if (!in_quote && (line[i] == '"' || line[i] == '\'')) {
            in_quote = 1; qc = line[i];
        } else if (in_quote && line[i] == qc) {
            in_quote = 0;
        } else if (!in_quote && line[i] == ';') {
            line[i] = '\0';
            return;
        }
    }
}

/* ── Schema helpers ──────────────────────────────────────────────────────── */

static DatacType type_from_str(const char *s) {
    if (strcmp(s, "int")      == 0) return DATAC_INT;
    if (strcmp(s, "float")    == 0) return DATAC_FLOAT;
    if (strcmp(s, "bool")     == 0) return DATAC_BOOL;
    if (strcmp(s, "date")     == 0) return DATAC_DATE;
    if (strcmp(s, "datetime") == 0) return DATAC_DATETIME;
    if (strcmp(s, "deltat")   == 0) return DATAC_DELTAT;
    return DATAC_STRING;
}

/* Growable schema list used during parsing. */
typedef struct { DatacField *fields; int n, cap; } SchemaBuilder;

static void sb_add(SchemaBuilder *sb, const char *name,
                   DatacType type, int is_primary) {
    if (sb->n >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 8;
        sb->fields = realloc(sb->fields, (size_t)sb->cap * sizeof(DatacField));
    }
    DatacField *f = &sb->fields[sb->n++];
    strncpy(f->name, name, 63); f->name[63] = '\0';
    f->type       = type;
    f->is_primary = is_primary;
}

static void sb_reset(SchemaBuilder *sb) {
    free(sb->fields);
    sb->fields = NULL;
    sb->n = sb->cap = 0;
}

/* ── Row parsing ─────────────────────────────────────────────────────────── */

static DatacRow parse_row(const char *line, int max_fields) {
    DatacRow row = {0};
    row.keys = malloc((size_t)max_fields * sizeof(char *));
    row.vals = malloc((size_t)max_fields * sizeof(char *));
    if (!row.keys || !row.vals) return row;

    const char *p = line;

    while (*p && row.n < max_fields) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        /* key up to ':' */
        const char *key_start = p;
        while (*p && *p != ':') p++;
        if (!*p) break;

        int key_len = (int)(p - key_start);
        char *key = malloc((size_t)key_len + 1);
        if (!key) break;
        snprintf(key, (size_t)key_len + 1, "%.*s", key_len, key_start);
        trim(key);
        p++;  /* skip ':' */

        while (*p == ' ' || *p == '\t') p++;

        /* value — quoted or unquoted */
        char *val;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *val_start = p;
            while (*p && *p != q) p++;
            int val_len = (int)(p - val_start);
            val = malloc((size_t)val_len + 1);
            if (!val) { free(key); break; }
            snprintf(val, (size_t)val_len + 1, "%.*s", val_len, val_start);
            if (*p) p++;
        } else {
            const char *val_start = p;
            while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
            int val_len = (int)(p - val_start);
            val = malloc((size_t)val_len + 1);
            if (!val) { free(key); break; }
            snprintf(val, (size_t)val_len + 1, "%.*s", val_len, val_start);
            rtrim(val);
        }

        /* nil → field is absent (datac_null returns 1) */
        if (val[0] && (val[0]=='n'||val[0]=='N') &&
            (val[1]=='i'||val[1]=='I') &&
            (val[2]=='l'||val[2]=='L') && val[3]=='\0') {
            free(key); free(val);
            continue;
        }

        row.keys[row.n] = key;
        row.vals[row.n] = val;
        row.n++;
    }

    return row;
}

/* ── Internal: free one table's rows and schema (not the struct itself) ─── */

static void _free_table_contents(DatacTable *t) {
    for (int i = 0; i < t->row_n; i++) {
        DatacRow *r = &t->rows[i];
        for (int j = 0; j < r->n; j++) {
            free(r->keys[j]);
            free(r->vals[j]);
        }
        free(r->keys);
        free(r->vals);
    }
    free(t->rows);
    free(t->schema);
}

/* ── datac_open ──────────────────────────────────────────────────────────── */

DatacFile *datac_open(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    DatacFile *file = calloc(1, sizeof(DatacFile));
    if (!file) { fclose(fp); return NULL; }

    SchemaBuilder sb = {0};
    DatacTable    current;
    int           in_block = 0;
    int           row_cap  = 0;
    char          line[4096];

    while (fgets(line, sizeof(line), fp)) {
        strip_comment(line);
        trim(line);
        if (!line[0]) continue;

        if (!in_block) {
            char type_buf[32], name_buf[64], sub_buf[64];

            /* "class TYPE property: NAME" */
            if (sscanf(line, "class %31s property: %63s",
                       type_buf, name_buf) == 2) {
                sb_add(&sb, name_buf, type_from_str(type_buf), 1);
                continue;
            }

            /* "TYPE sub.SUB: NAME" */
            if (sscanf(line, "%31s sub.%63[^:]: %63s",
                       type_buf, sub_buf, name_buf) == 3) {
                sb_add(&sb, name_buf, type_from_str(type_buf), 0);
                continue;
            }

            /* "set inclusion pk.field to nil" — nullable field */
            {
                char pk_buf[64], field_buf[64];
                if (sscanf(line, "set inclusion %63[^.].%63s",
                           pk_buf, field_buf) == 2 && strstr(line, " to nil")) {
                    for (int si = 0; si < sb.n; si++) {
                        if (strcmp(sb.fields[si].name, field_buf) == 0) {
                            sb.fields[si].is_nullable = 1;
                            break;
                        }
                    }
                    continue;
                }
            }

            /* "tablename {" */
            char *brace = strchr(line, '{');
            if (brace) {
                *brace = '\0';
                trim(line);
                if (line[0]) {
                    memset(&current, 0, sizeof(current));
                    snprintf(current.name, 64, "%.63s", line);
                    /* transfer schema ownership */
                    current.schema   = sb.fields;
                    current.schema_n = sb.n;
                    sb.fields = NULL; sb.n = sb.cap = 0;
                    /* init rows */
                    row_cap = 64;
                    current.rows = malloc((size_t)row_cap * sizeof(DatacRow));
                    in_block = 1;
                }
                continue;
            }

        } else {
            if (line[0] == '}') {
                in_block = 0;
                /* shrink rows */
                if (current.row_n > 0)
                    current.rows = realloc(current.rows,
                                          (size_t)current.row_n * sizeof(DatacRow));
                else
                    free(current.rows), current.rows = NULL;

                /* append table (struct copy) */
                file->tables = realloc(file->tables,
                                       (size_t)(file->table_n + 1) * sizeof(DatacTable));
                file->tables[file->table_n++] = current;
                row_cap = 0;
                continue;
            }

            if (current.row_n >= row_cap) {
                row_cap *= 2;
                current.rows = realloc(current.rows,
                                       (size_t)row_cap * sizeof(DatacRow));
            }
            current.rows[current.row_n++] =
                parse_row(line, current.schema_n ? current.schema_n : 64);
        }
    }

    fclose(fp);
    sb_reset(&sb);  /* cleanup if file ended mid-schema */
    return file;
}

/* ── datac_close ─────────────────────────────────────────────────────────── */

void datac_close(DatacFile *file) {
    if (!file) return;
    for (int i = 0; i < file->table_n; i++)
        _free_table_contents(&file->tables[i]);
    free(file->tables);
    free(file);
}

/* ── datac_table ─────────────────────────────────────────────────────────── */

DatacTable *datac_table(DatacFile *file, const char *name) {
    if (!file) return NULL;
    for (int i = 0; i < file->table_n; i++)
        if (strcmp(file->tables[i].name, name) == 0)
            return &file->tables[i];
    return NULL;
}

/* ── datac_load (convenience: first table, caller owns) ─────────────────── */

DatacTable *datac_load(const char *path) {
    DatacFile *file = datac_open(path);
    if (!file || file->table_n == 0) {
        datac_close(file);
        return NULL;
    }

    /* Allocate standalone table and transfer ownership of first table's data */
    DatacTable *t = malloc(sizeof(DatacTable));
    if (!t) { datac_close(file); return NULL; }
    *t = file->tables[0];

    /* Free remaining tables (not the first — its pointers are now owned by t) */
    for (int i = 1; i < file->table_n; i++)
        _free_table_contents(&file->tables[i]);
    free(file->tables);
    free(file);

    return t;
}

/* ── datac_free ──────────────────────────────────────────────────────────── */

void datac_free(DatacTable *tbl) {
    if (!tbl) return;
    _free_table_contents(tbl);
    free(tbl);
}

/* ── Field access ────────────────────────────────────────────────────────── */

const char *datac_str(const DatacRow *row, const char *field) {
    for (int i = 0; i < row->n; i++)
        if (strcmp(row->keys[i], field) == 0)
            return row->vals[i];
    return NULL;
}

long datac_int(const DatacRow *row, const char *field) {
    const char *s = datac_str(row, field);
    return s ? atol(s) : 0;
}

double datac_float(const DatacRow *row, const char *field) {
    const char *s = datac_str(row, field);
    return s ? atof(s) : 0.0;
}

int datac_bool(const DatacRow *row, const char *field) {
    const char *s = datac_str(row, field);
    return (s && strcmp(s, "true") == 0) ? 1 : 0;
}

int datac_null(const DatacRow *row, const char *field) {
    return datac_str(row, field) == NULL;
}

/* ── Date / time accessors ───────────────────────────────────────────────── */

void datac_date(const DatacRow *row, const char *field,
                int *day, int *month, int *year) {
    const char *s = datac_str(row, field);
    int d = 0, mo = 0, y = 0;
    if (s) sscanf(s, "%d.%d.%d", &d, &mo, &y);
    if (day)   *day   = d;
    if (month) *month = mo;
    if (year)  *year  = 2000 + y;
}

void datac_datetime(const DatacRow *row, const char *field,
                    int *day, int *month, int *year,
                    int *hour, int *min) {
    const char *s = datac_str(row, field);
    int d = 0, mo = 0, y = 0, h = 0, mi = 0;
    if (s) sscanf(s, "%d.%d.%d %d:%d", &d, &mo, &y, &h, &mi);
    if (day)   *day   = d;
    if (month) *month = mo;
    if (year)  *year  = 2000 + y;
    if (hour)  *hour  = h;
    if (min)   *min   = mi;
}

void datac_deltat(const DatacRow *row, const char *field,
                  int *hours, int *min, int *sec) {
    const char *s = datac_str(row, field);
    int h = 0, m = 0, sc = 0;
    if (s) sscanf(s, "%d:%d:%d", &h, &m, &sc);
    if (hours) *hours = h;
    if (min)   *min   = m;
    if (sec)   *sec   = sc;
}

long datac_deltat_seconds(const DatacRow *row, const char *field) {
    int h = 0, m = 0, s = 0;
    datac_deltat(row, field, &h, &m, &s);
    return (long)h * 3600 + (long)m * 60 + s;
}

/* ── Querying ────────────────────────────────────────────────────────────── */

int datac_find_str(const DatacTable *tbl, const char *field,
                   const char *val, DatacRow **out, int max_out) {
    int n = 0;
    for (int i = 0; i < tbl->row_n && n < max_out; i++) {
        const char *v = datac_str(&tbl->rows[i], field);
        if (v && strcmp(v, val) == 0)
            out[n++] = &tbl->rows[i];
    }
    return n;
}

int datac_find_int(const DatacTable *tbl, const char *field,
                   long val, DatacRow **out, int max_out) {
    int n = 0;
    for (int i = 0; i < tbl->row_n && n < max_out; i++)
        if (datac_int(&tbl->rows[i], field) == val)
            out[n++] = &tbl->rows[i];
    return n;
}

int datac_find_float(const DatacTable *tbl, const char *field,
                     double val, DatacRow **out, int max_out) {
    int n = 0;
    for (int i = 0; i < tbl->row_n && n < max_out; i++)
        if (datac_float(&tbl->rows[i], field) == val)
            out[n++] = &tbl->rows[i];
    return n;
}

int datac_find_bool(const DatacTable *tbl, const char *field,
                    int val, DatacRow **out, int max_out) {
    int n = 0;
    for (int i = 0; i < tbl->row_n && n < max_out; i++)
        if (datac_bool(&tbl->rows[i], field) == val)
            out[n++] = &tbl->rows[i];
    return n;
}

/* ── Schema introspection ────────────────────────────────────────────────── */

const DatacField *datac_schema_get(const DatacTable *tbl, const char *field) {
    for (int i = 0; i < tbl->schema_n; i++)
        if (strcmp(tbl->schema[i].name, field) == 0)
            return &tbl->schema[i];
    return NULL;
}

const char *datac_type_name(DatacType t) {
    switch (t) {
        case DATAC_INT:      return "int";
        case DATAC_FLOAT:    return "float";
        case DATAC_BOOL:     return "bool";
        case DATAC_DATE:     return "date";
        case DATAC_DATETIME: return "datetime";
        case DATAC_DELTAT:   return "deltat";
        default:             return "string";
    }
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

static char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char  *d   = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

static void _free_row(DatacRow *r) {
    for (int j = 0; j < r->n; j++) {
        free(r->keys[j]);
        free(r->vals[j]);
    }
    free(r->keys);
    free(r->vals);
}

/* ── Mutation ────────────────────────────────────────────────────────────── */

int datac_insert(DatacTable *tbl,
                 const char **keys, const char **vals, int n) {
    DatacRow *nr = realloc(tbl->rows, (size_t)(tbl->row_n + 1) * sizeof(DatacRow));
    if (!nr) return 0;
    tbl->rows = nr;

    DatacRow *r = &tbl->rows[tbl->row_n];
    r->keys = malloc((size_t)n * sizeof(char *));
    r->vals = malloc((size_t)n * sizeof(char *));
    r->n    = 0;
    if (!r->keys || !r->vals) { free(r->keys); free(r->vals); return 0; }

    for (int i = 0; i < n; i++) {
        r->keys[i] = xstrdup(keys[i]);
        r->vals[i] = xstrdup(vals[i]);
        r->n++;
    }
    tbl->row_n++;
    return 1;
}

int datac_delete_str(DatacTable *tbl, const char *field, const char *val) {
    int removed = 0, w = 0;
    for (int r = 0; r < tbl->row_n; r++) {
        const char *v = datac_str(&tbl->rows[r], field);
        if (v && strcmp(v, val) == 0) {
            _free_row(&tbl->rows[r]);
            removed++;
        } else {
            tbl->rows[w++] = tbl->rows[r];
        }
    }
    tbl->row_n = w;
    return removed;
}

int datac_delete_int(DatacTable *tbl, const char *field, long val) {
    int removed = 0, w = 0;
    for (int r = 0; r < tbl->row_n; r++) {
        if (datac_int(&tbl->rows[r], field) == val &&
            !datac_null(&tbl->rows[r], field)) {
            _free_row(&tbl->rows[r]);
            removed++;
        } else {
            tbl->rows[w++] = tbl->rows[r];
        }
    }
    tbl->row_n = w;
    return removed;
}

int datac_update_str(DatacTable *tbl,
                     const char *cond_field, const char *cond_val,
                     const char *set_field,  const char *set_val) {
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        const char *cv = datac_str(&tbl->rows[i], cond_field);
        if (!cv || strcmp(cv, cond_val) != 0) continue;
        DatacRow *r = &tbl->rows[i];
        for (int j = 0; j < r->n; j++) {
            if (strcmp(r->keys[j], set_field) == 0) {
                free(r->vals[j]);
                r->vals[j] = xstrdup(set_val);
                count++;
                goto next_str;
            }
        }
        /* field not in row yet — append it */
        {
            char **nk = realloc(r->keys, (size_t)(r->n + 1) * sizeof(char *));
            char **nv = realloc(r->vals, (size_t)(r->n + 1) * sizeof(char *));
            if (nk && nv) {
                r->keys = nk; r->vals = nv;
                r->keys[r->n] = xstrdup(set_field);
                r->vals[r->n] = xstrdup(set_val);
                r->n++; count++;
            }
        }
        next_str:;
    }
    return count;
}

int datac_update_int(DatacTable *tbl,
                     const char *cond_field, long cond_val,
                     const char *set_field,  long set_val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", set_val);
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_int(&tbl->rows[i], cond_field) != cond_val ||
            datac_null(&tbl->rows[i], cond_field)) continue;
        DatacRow *r = &tbl->rows[i];
        for (int j = 0; j < r->n; j++) {
            if (strcmp(r->keys[j], set_field) == 0) {
                free(r->vals[j]);
                r->vals[j] = xstrdup(buf);
                count++;
                goto next_int;
            }
        }
        {
            char **nk = realloc(r->keys, (size_t)(r->n + 1) * sizeof(char *));
            char **nv = realloc(r->vals, (size_t)(r->n + 1) * sizeof(char *));
            if (nk && nv) {
                r->keys = nk; r->vals = nv;
                r->keys[r->n] = xstrdup(set_field);
                r->vals[r->n] = xstrdup(buf);
                r->n++; count++;
            }
        }
        next_int:;
    }
    return count;
}

/* ── Persistence ─────────────────────────────────────────────────────────── */

static void _write_field_val(FILE *fp, const char *val, DatacType type) {
    if (!val) { fprintf(fp, "NIL"); return; }
    if (type == DATAC_STRING)
        fprintf(fp, "\"%s\"", val);
    else
        fprintf(fp, "%s", val);
}

static void _write_one_table(FILE *fp, const DatacTable *tbl) {
    /* find primary key field */
    const DatacField *pk_f = NULL;
    for (int i = 0; i < tbl->schema_n; i++)
        if (tbl->schema[i].is_primary) { pk_f = &tbl->schema[i]; break; }

    /* schema preamble */
    if (pk_f)
        fprintf(fp, "class %s property: %s\n",
                datac_type_name(pk_f->type), pk_f->name);
    for (int i = 0; i < tbl->schema_n; i++) {
        const DatacField *f = &tbl->schema[i];
        if (f->is_primary) continue;
        fprintf(fp, "%s sub.%s: %s\n",
                datac_type_name(f->type),
                pk_f ? pk_f->name : "id",
                f->name);
        if (f->is_nullable)
            fprintf(fp, "set inclusion %s.%s to nil\n",
                    pk_f ? pk_f->name : "id", f->name);
    }

    fprintf(fp, "\n%s {\n", tbl->name);

    for (int r = 0; r < tbl->row_n; r++) {
        fprintf(fp, "    ");
        int first = 1;
        /* schema-ordered fields first */
        for (int s = 0; s < tbl->schema_n; s++) {
            const char *fname = tbl->schema[s].name;
            const char *val   = datac_str(&tbl->rows[r], fname);
            if (!val && tbl->schema[s].is_nullable) continue;
            if (!first) fprintf(fp, ", ");
            fprintf(fp, "%s: ", fname);
            _write_field_val(fp, val, tbl->schema[s].type);
            first = 0;
        }
        fprintf(fp, "\n");
    }

    fprintf(fp, "}\n\n");
}

int datac_save(const DatacFile *file, const char *path) {
    if (!file) return 0;
    FILE *fp = fopen(path, "w");
    if (!fp) return 0;
    for (int i = 0; i < file->table_n; i++)
        _write_one_table(fp, &file->tables[i]);
    fclose(fp);
    return 1;
}

int datac_save_table(const DatacTable *tbl, const char *path) {
    if (!tbl) return 0;
    FILE *fp = fopen(path, "w");
    if (!fp) return 0;
    _write_one_table(fp, tbl);
    fclose(fp);
    return 1;
}

/* ── Internal: set or append a field value in a row ─────────────────────── */

static void _set_field(DatacRow *r, const char *field, const char *new_val) {
    for (int j = 0; j < r->n; j++) {
        if (strcmp(r->keys[j], field) == 0) {
            free(r->vals[j]);
            r->vals[j] = xstrdup(new_val);
            return;
        }
    }
    char **nk = realloc(r->keys, (size_t)(r->n + 1) * sizeof(char *));
    char **nv = realloc(r->vals, (size_t)(r->n + 1) * sizeof(char *));
    if (nk && nv) {
        r->keys = nk; r->vals = nv;
        r->keys[r->n] = xstrdup(field);
        r->vals[r->n] = xstrdup(new_val);
        r->n++;
    }
}

/* Internal: remove field at index j from row (shifts tail left). */
static void _row_remove_field(DatacRow *r, int j) {
    free(r->keys[j]);
    free(r->vals[j]);
    for (int k = j; k < r->n - 1; k++) {
        r->keys[k] = r->keys[k + 1];
        r->vals[k] = r->vals[k + 1];
    }
    r->n--;
}

/* ── Additional find ─────────────────────────────────────────────────────── */

int datac_find_null(const DatacTable *tbl, const char *field,
                    DatacRow **out, int max_out) {
    int n = 0;
    for (int i = 0; i < tbl->row_n && n < max_out; i++)
        if (datac_null(&tbl->rows[i], field))
            out[n++] = &tbl->rows[i];
    return n;
}

/* ── Additional delete variants ──────────────────────────────────────────── */

int datac_delete_float(DatacTable *tbl, const char *field, double val) {
    int removed = 0, w = 0;
    for (int r = 0; r < tbl->row_n; r++) {
        if (!datac_null(&tbl->rows[r], field) &&
            datac_float(&tbl->rows[r], field) == val) {
            _free_row(&tbl->rows[r]);
            removed++;
        } else {
            tbl->rows[w++] = tbl->rows[r];
        }
    }
    tbl->row_n = w;
    return removed;
}

int datac_delete_bool(DatacTable *tbl, const char *field, int val) {
    int removed = 0, w = 0;
    for (int r = 0; r < tbl->row_n; r++) {
        if (!datac_null(&tbl->rows[r], field) &&
            datac_bool(&tbl->rows[r], field) == val) {
            _free_row(&tbl->rows[r]);
            removed++;
        } else {
            tbl->rows[w++] = tbl->rows[r];
        }
    }
    tbl->row_n = w;
    return removed;
}

int datac_delete_null(DatacTable *tbl, const char *field) {
    int removed = 0, w = 0;
    for (int r = 0; r < tbl->row_n; r++) {
        if (datac_null(&tbl->rows[r], field)) {
            _free_row(&tbl->rows[r]);
            removed++;
        } else {
            tbl->rows[w++] = tbl->rows[r];
        }
    }
    tbl->row_n = w;
    return removed;
}

/* ── Additional update variants ──────────────────────────────────────────── */

int datac_update_float(DatacTable *tbl,
                       const char *cond_field, double cond_val,
                       const char *set_field,  double set_val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", set_val);
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_float(&tbl->rows[i], cond_field) != cond_val) continue;
        _set_field(&tbl->rows[i], set_field, buf);
        count++;
    }
    return count;
}

int datac_update_bool(DatacTable *tbl,
                      const char *cond_field, int cond_val,
                      const char *set_field,  int set_val) {
    const char *set_s = set_val ? "true" : "false";
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_bool(&tbl->rows[i], cond_field) != cond_val) continue;
        _set_field(&tbl->rows[i], set_field, set_s);
        count++;
    }
    return count;
}

/* ── Set field to nil ────────────────────────────────────────────────────── */

int datac_set_nil_str(DatacTable *tbl, const char *cond_field,
                      const char *cond_val, const char *set_field) {
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        const char *cv = datac_str(&tbl->rows[i], cond_field);
        if (!cv || strcmp(cv, cond_val) != 0) continue;
        DatacRow *r = &tbl->rows[i];
        for (int j = 0; j < r->n; j++) {
            if (strcmp(r->keys[j], set_field) == 0) {
                _row_remove_field(r, j);
                count++;
                break;
            }
        }
    }
    return count;
}

int datac_set_nil_int(DatacTable *tbl, const char *cond_field,
                      long cond_val, const char *set_field) {
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_int(&tbl->rows[i], cond_field) != cond_val) continue;
        DatacRow *r = &tbl->rows[i];
        for (int j = 0; j < r->n; j++) {
            if (strcmp(r->keys[j], set_field) == 0) {
                _row_remove_field(r, j);
                count++;
                break;
            }
        }
    }
    return count;
}

/* ── Compound numeric updates ────────────────────────────────────────────── */

int datac_update_int_add(DatacTable *tbl,
                         const char *cond_field, long cond_val,
                         const char *set_field,  long delta) {
    char buf[32];
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_int(&tbl->rows[i], cond_field) != cond_val) continue;
        long cur = datac_int(&tbl->rows[i], set_field);
        snprintf(buf, sizeof(buf), "%ld", cur + delta);
        _set_field(&tbl->rows[i], set_field, buf);
        count++;
    }
    return count;
}

int datac_update_int_sub(DatacTable *tbl,
                         const char *cond_field, long cond_val,
                         const char *set_field,  long delta) {
    return datac_update_int_add(tbl, cond_field, cond_val, set_field, -delta);
}

int datac_update_int_mul(DatacTable *tbl,
                         const char *cond_field, long cond_val,
                         const char *set_field,  long factor) {
    char buf[32];
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_int(&tbl->rows[i], cond_field) != cond_val) continue;
        long cur = datac_int(&tbl->rows[i], set_field);
        snprintf(buf, sizeof(buf), "%ld", cur * factor);
        _set_field(&tbl->rows[i], set_field, buf);
        count++;
    }
    return count;
}

int datac_update_float_add(DatacTable *tbl,
                           const char *cond_field, double cond_val,
                           const char *set_field,  double delta) {
    char buf[64];
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_float(&tbl->rows[i], cond_field) != cond_val) continue;
        double cur = datac_float(&tbl->rows[i], set_field);
        snprintf(buf, sizeof(buf), "%g", cur + delta);
        _set_field(&tbl->rows[i], set_field, buf);
        count++;
    }
    return count;
}

int datac_update_float_mul(DatacTable *tbl,
                           const char *cond_field, double cond_val,
                           const char *set_field,  double factor) {
    char buf[64];
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_float(&tbl->rows[i], cond_field) != cond_val) continue;
        double cur = datac_float(&tbl->rows[i], set_field);
        snprintf(buf, sizeof(buf), "%g", cur * factor);
        _set_field(&tbl->rows[i], set_field, buf);
        count++;
    }
    return count;
}

int datac_update_float_div(DatacTable *tbl,
                           const char *cond_field, double cond_val,
                           const char *set_field,  double divisor) {
    if (divisor == 0.0) return 0;
    char buf[64];
    int count = 0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (datac_null(&tbl->rows[i], cond_field) ||
            datac_float(&tbl->rows[i], cond_field) != cond_val) continue;
        double cur = datac_float(&tbl->rows[i], set_field);
        snprintf(buf, sizeof(buf), "%g", cur / divisor);
        _set_field(&tbl->rows[i], set_field, buf);
        count++;
    }
    return count;
}

/* ── Aggregates ──────────────────────────────────────────────────────────── */

int datac_count(const DatacTable *tbl, const char *field) {
    int n = 0;
    for (int i = 0; i < tbl->row_n; i++)
        if (!datac_null(&tbl->rows[i], field)) n++;
    return n;
}

long datac_sum_int(const DatacTable *tbl, const char *field) {
    long sum = 0;
    for (int i = 0; i < tbl->row_n; i++)
        if (!datac_null(&tbl->rows[i], field))
            sum += datac_int(&tbl->rows[i], field);
    return sum;
}

double datac_sum_float(const DatacTable *tbl, const char *field) {
    double sum = 0.0;
    for (int i = 0; i < tbl->row_n; i++)
        if (!datac_null(&tbl->rows[i], field))
            sum += datac_float(&tbl->rows[i], field);
    return sum;
}

double datac_mean(const DatacTable *tbl, const char *field) {
    int n = 0; double sum = 0.0;
    for (int i = 0; i < tbl->row_n; i++) {
        if (!datac_null(&tbl->rows[i], field)) {
            sum += datac_float(&tbl->rows[i], field);
            n++;
        }
    }
    return n ? sum / n : 0.0;
}

long datac_min_int(const DatacTable *tbl, const char *field) {
    long min = 0; int first = 1;
    for (int i = 0; i < tbl->row_n; i++) {
        if (!datac_null(&tbl->rows[i], field)) {
            long v = datac_int(&tbl->rows[i], field);
            if (first || v < min) { min = v; first = 0; }
        }
    }
    return min;
}

long datac_max_int(const DatacTable *tbl, const char *field) {
    long max = 0; int first = 1;
    for (int i = 0; i < tbl->row_n; i++) {
        if (!datac_null(&tbl->rows[i], field)) {
            long v = datac_int(&tbl->rows[i], field);
            if (first || v > max) { max = v; first = 0; }
        }
    }
    return max;
}

double datac_min_float(const DatacTable *tbl, const char *field) {
    double min = 0.0; int first = 1;
    for (int i = 0; i < tbl->row_n; i++) {
        if (!datac_null(&tbl->rows[i], field)) {
            double v = datac_float(&tbl->rows[i], field);
            if (first || v < min) { min = v; first = 0; }
        }
    }
    return min;
}

double datac_max_float(const DatacTable *tbl, const char *field) {
    double max = 0.0; int first = 1;
    for (int i = 0; i < tbl->row_n; i++) {
        if (!datac_null(&tbl->rows[i], field)) {
            double v = datac_float(&tbl->rows[i], field);
            if (first || v > max) { max = v; first = 0; }
        }
    }
    return max;
}

const char *datac_min_str(const DatacTable *tbl, const char *field) {
    const char *min = NULL;
    for (int i = 0; i < tbl->row_n; i++) {
        const char *v = datac_str(&tbl->rows[i], field);
        if (v && (!min || strcmp(v, min) < 0)) min = v;
    }
    return min;
}

const char *datac_max_str(const DatacTable *tbl, const char *field) {
    const char *max = NULL;
    for (int i = 0; i < tbl->row_n; i++) {
        const char *v = datac_str(&tbl->rows[i], field);
        if (v && (!max || strcmp(v, max) > 0)) max = v;
    }
    return max;
}

/* ── Sort ────────────────────────────────────────────────────────────────── */

static struct {
    const char *field;
    DatacType   field_type;
    int         is_numeric;
    int         descending;
} _sort_ctx;

/* Convert a stored value to a double for ordered comparison. */
static double _sort_num_val(const char *s, DatacType type) {
    if (!s) return 0.0;
    switch (type) {
        case DATAC_DATE: {
            int d = 0, mo = 0, y = 0;
            sscanf(s, "%d.%d.%d", &d, &mo, &y);
            return (2000 + y) * 10000.0 + mo * 100.0 + d;
        }
        case DATAC_DATETIME: {
            int d = 0, mo = 0, y = 0, h = 0, mi = 0;
            sscanf(s, "%d.%d.%d %d:%d", &d, &mo, &y, &h, &mi);
            return ((2000 + y) * 10000.0 + mo * 100.0 + d) * 10000.0
                   + h * 100.0 + mi;
        }
        case DATAC_DELTAT: {
            int h = 0, m = 0, sc = 0;
            sscanf(s, "%d:%d:%d", &h, &m, &sc);
            return h * 3600.0 + m * 60.0 + sc;
        }
        default:
            return atof(s);
    }
}

static int _sort_cmp(const void *a, const void *b) {
    const DatacRow *ra = (const DatacRow *)a;
    const DatacRow *rb = (const DatacRow *)b;
    const char *va = datac_str(ra, _sort_ctx.field);
    const char *vb = datac_str(rb, _sort_ctx.field);

    /* nil always sorts last regardless of direction */
    if (!va && !vb) return 0;
    if (!va) return  1;
    if (!vb) return -1;

    int cmp;
    if (_sort_ctx.is_numeric) {
        double na = _sort_num_val(va, _sort_ctx.field_type);
        double nb = _sort_num_val(vb, _sort_ctx.field_type);
        cmp = (na > nb) - (na < nb);
    } else {
        cmp = strcmp(va, vb);
    }
    return _sort_ctx.descending ? -cmp : cmp;
}

void datac_sort(DatacTable *tbl, const char *field, int descending) {
    if (!tbl || tbl->row_n <= 1) return;

    _sort_ctx.field      = field;
    _sort_ctx.descending = descending;
    _sort_ctx.is_numeric = 0;
    _sort_ctx.field_type = DATAC_STRING;

    for (int i = 0; i < tbl->schema_n; i++) {
        if (strcmp(tbl->schema[i].name, field) == 0) {
            _sort_ctx.field_type = tbl->schema[i].type;
            _sort_ctx.is_numeric = (tbl->schema[i].type != DATAC_STRING);
            break;
        }
    }

    qsort(tbl->rows, (size_t)tbl->row_n, sizeof(DatacRow), _sort_cmp);
}

/* ── Programmatic construction ────────────────────────────────────────────── */

DatacTable *datac_create(const char *name) {
    DatacTable *t = calloc(1, sizeof(DatacTable));
    if (!t) return NULL;
    snprintf(t->name, 64, "%.63s", name);
    return t;
}

int datac_add_field_def(DatacTable *tbl, const char *name,
                        DatacType type, int is_primary, int is_nullable) {
    DatacField *nf = realloc(tbl->schema,
                             (size_t)(tbl->schema_n + 1) * sizeof(DatacField));
    if (!nf) return 0;
    tbl->schema = nf;
    DatacField *f = &tbl->schema[tbl->schema_n++];
    strncpy(f->name, name, 63); f->name[63] = '\0';
    f->type        = type;
    f->is_primary  = is_primary;
    f->is_nullable = is_nullable;
    return 1;
}

DatacFile *datac_file_create(void) {
    return calloc(1, sizeof(DatacFile));
}

int datac_file_add_table(DatacFile *file, DatacTable *tbl) {
    DatacTable *nt = realloc(file->tables,
                             (size_t)(file->table_n + 1) * sizeof(DatacTable));
    if (!nt) return 0;
    file->tables = nt;
    file->tables[file->table_n++] = *tbl;  /* struct copy; file owns data */
    return 1;
}
