#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Types ────────────────────────────────────────────────────────────────── */

typedef enum {
    DATAC_STRING   = 0,
    DATAC_INT      = 1,
    DATAC_FLOAT    = 2,
    DATAC_BOOL     = 3,
    DATAC_DATE     = 4,  /* stored as "dd.mm.yy"         */
    DATAC_DATETIME = 5,  /* stored as "dd.mm.yy hh:mm"   */
    DATAC_DELTAT   = 6,  /* elapsed time, NOT epoch       */
} DatacType;

typedef struct {
    char      name[64];
    DatacType type;
    int       is_primary;
    int       is_nullable;  /* set inclusion pk.field to nil */
} DatacField;

typedef struct {
    char **keys;
    char **vals;   /* raw string; use typed accessors below */
    int    n;
} DatacRow;

typedef struct {
    char        name[64];
    DatacField *schema;
    int         schema_n;
    DatacRow   *rows;
    int         row_n;
} DatacTable;

/* A file can hold multiple tables. */
typedef struct {
    DatacTable *tables;
    int         table_n;
} DatacFile;

/* ── Load / free ──────────────────────────────────────────────────────────── */

DatacFile  *datac_open (const char *path);
void        datac_close(DatacFile  *file);
DatacTable *datac_table(DatacFile  *file, const char *name);

/* Convenience: load first table only. Caller owns and must datac_free() it. */
DatacTable *datac_load(const char *path);
void        datac_free(DatacTable *tbl);

/* ── Typed field access ───────────────────────────────────────────────────── */

const char *datac_str  (const DatacRow *row, const char *field);
long        datac_int  (const DatacRow *row, const char *field);
double      datac_float(const DatacRow *row, const char *field);
int         datac_bool (const DatacRow *row, const char *field);
int         datac_null (const DatacRow *row, const char *field);

void datac_date    (const DatacRow *row, const char *field,
                    int *day, int *month, int *year);
void datac_datetime(const DatacRow *row, const char *field,
                    int *day, int *month, int *year, int *hour, int *min);
void datac_deltat  (const DatacRow *row, const char *field,
                    int *hours, int *min, int *sec);
long datac_deltat_seconds(const DatacRow *row, const char *field);

/* ── Querying ─────────────────────────────────────────────────────────────── */

int datac_find_str  (const DatacTable *tbl, const char *field,
                     const char *val, DatacRow **out, int max_out);
int datac_find_int  (const DatacTable *tbl, const char *field,
                     long val, DatacRow **out, int max_out);
int datac_find_float(const DatacTable *tbl, const char *field,
                     double val, DatacRow **out, int max_out);
int datac_find_bool (const DatacTable *tbl, const char *field,
                     int val, DatacRow **out, int max_out);
int datac_find_null (const DatacTable *tbl, const char *field,
                     DatacRow **out, int max_out);

/* ── Mutation ─────────────────────────────────────────────────────────────── */

/* Append a row. keys/vals are parallel string arrays of length n.
   Returns 1 on success, 0 on allocation failure. */
int datac_insert(DatacTable *tbl,
                 const char **keys, const char **vals, int n);

/* Delete rows where field == val.  Returns count removed. */
int datac_delete_str  (DatacTable *tbl, const char *field, const char *val);
int datac_delete_int  (DatacTable *tbl, const char *field, long        val);
int datac_delete_float(DatacTable *tbl, const char *field, double      val);
int datac_delete_bool (DatacTable *tbl, const char *field, int         val);
int datac_delete_null (DatacTable *tbl, const char *field);  /* where field is nil */

/* Update set_field = set_val in every row where cond_field == cond_val.
   Returns count of rows changed. */
int datac_update_str  (DatacTable *tbl,
                       const char *cond_field, const char *cond_val,
                       const char *set_field,  const char *set_val);
int datac_update_int  (DatacTable *tbl,
                       const char *cond_field, long   cond_val,
                       const char *set_field,  long   set_val);
int datac_update_float(DatacTable *tbl,
                       const char *cond_field, double cond_val,
                       const char *set_field,  double set_val);
int datac_update_bool (DatacTable *tbl,
                       const char *cond_field, int    cond_val,
                       const char *set_field,  int    set_val);

/* Set set_field to nil (remove from row) where cond matches. */
int datac_set_nil_str(DatacTable *tbl, const char *cond_field,
                      const char *cond_val, const char *set_field);
int datac_set_nil_int(DatacTable *tbl, const char *cond_field,
                      long cond_val, const char *set_field);

/* ── Compound numeric updates (+=  -=  *=  /=) ────────────────────────────── */

int datac_update_int_add  (DatacTable *tbl,
                           const char *cond_field, long   cond_val,
                           const char *set_field,  long   delta);
int datac_update_int_sub  (DatacTable *tbl,
                           const char *cond_field, long   cond_val,
                           const char *set_field,  long   delta);
int datac_update_int_mul  (DatacTable *tbl,
                           const char *cond_field, long   cond_val,
                           const char *set_field,  long   factor);
int datac_update_float_add(DatacTable *tbl,
                           const char *cond_field, double cond_val,
                           const char *set_field,  double delta);
int datac_update_float_mul(DatacTable *tbl,
                           const char *cond_field, double cond_val,
                           const char *set_field,  double factor);
int datac_update_float_div(DatacTable *tbl,
                           const char *cond_field, double cond_val,
                           const char *set_field,  double divisor);

/* ── Aggregates ───────────────────────────────────────────────────────────── */

int         datac_count    (const DatacTable *tbl, const char *field);
long        datac_sum_int  (const DatacTable *tbl, const char *field);
double      datac_sum_float(const DatacTable *tbl, const char *field);
double      datac_mean     (const DatacTable *tbl, const char *field);
long        datac_min_int  (const DatacTable *tbl, const char *field);
long        datac_max_int  (const DatacTable *tbl, const char *field);
double      datac_min_float(const DatacTable *tbl, const char *field);
double      datac_max_float(const DatacTable *tbl, const char *field);
const char *datac_min_str  (const DatacTable *tbl, const char *field);
const char *datac_max_str  (const DatacTable *tbl, const char *field);

/* ── Sort ─────────────────────────────────────────────────────────────────── */

/* Sort rows in place. Uses schema type for numeric vs lexicographic order.
   descending=1 for Z→A / high→low; nil values sort last. */
void datac_sort(DatacTable *tbl, const char *field, int descending);

/* ── Persistence ──────────────────────────────────────────────────────────── */

/* Write all tables in a DatacFile back to disk. Returns 1 on success. */
int datac_save(const DatacFile *file, const char *path);

/* Write a single table to its own file. */
int datac_save_table(const DatacTable *tbl, const char *path);

/* ── Schema introspection ─────────────────────────────────────────────────── */

const DatacField *datac_schema_get(const DatacTable *tbl, const char *field);
const char       *datac_type_name (DatacType t);

/* ── Programmatic construction ────────────────────────────────────────────── */

/* Create an empty table. Returns heap-allocated DatacTable; caller must
   eventually datac_free() it (or hand it to datac_file_add_table). */
DatacTable *datac_create       (const char *name);
int         datac_add_field_def(DatacTable *tbl, const char *name,
                                DatacType type, int is_primary, int is_nullable);

/* Create an empty DatacFile. */
DatacFile  *datac_file_create(void);

/* Append tbl's data into file (struct copy — file takes ownership of
   schema/rows pointers). After this call: do NOT datac_free(tbl);
   just free(tbl) if it was heap-allocated by datac_create(). */
int datac_file_add_table(DatacFile *file, DatacTable *tbl);

#ifdef __cplusplus
}
#endif
