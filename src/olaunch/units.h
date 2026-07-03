/**
 * units.h — tiny built-in unit converter for OLaunch.
 *
 * Parses "<n> <unit> to <unit>" / "<n> <unit> in <unit>" and converts using a
 * small static table. Length, mass, volume, time, speed convert through a base
 * factor; temperature is handled specially. Pure C (no deps beyond libc).
 */
#ifndef OLAUNCH_UNITS_H
#define OLAUNCH_UNITS_H

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

typedef enum {
    UCAT_LENGTH, UCAT_MASS, UCAT_VOLUME, UCAT_TIME, UCAT_SPEED, UCAT_TEMP
} UnitCat;

typedef struct {
    const char *name;   /* lowercase alias */
    UnitCat     cat;
    double      factor; /* value * factor = base unit (ignored for temp) */
} UnitDef;

static const UnitDef olaunch_units[] = {
    /* length — base metre */
    { "m",      UCAT_LENGTH, 1.0 },
    { "meter",  UCAT_LENGTH, 1.0 },
    { "meters", UCAT_LENGTH, 1.0 },
    { "metre",  UCAT_LENGTH, 1.0 },
    { "km",     UCAT_LENGTH, 1000.0 },
    { "cm",     UCAT_LENGTH, 0.01 },
    { "mm",     UCAT_LENGTH, 0.001 },
    { "mi",     UCAT_LENGTH, 1609.344 },
    { "mile",   UCAT_LENGTH, 1609.344 },
    { "miles",  UCAT_LENGTH, 1609.344 },
    { "yd",     UCAT_LENGTH, 0.9144 },
    { "yard",   UCAT_LENGTH, 0.9144 },
    { "ft",     UCAT_LENGTH, 0.3048 },
    { "foot",   UCAT_LENGTH, 0.3048 },
    { "feet",   UCAT_LENGTH, 0.3048 },
    { "in",     UCAT_LENGTH, 0.0254 },
    { "inch",   UCAT_LENGTH, 0.0254 },
    { "nmi",    UCAT_LENGTH, 1852.0 },

    /* mass — base gram */
    { "g",      UCAT_MASS, 1.0 },
    { "gram",   UCAT_MASS, 1.0 },
    { "grams",  UCAT_MASS, 1.0 },
    { "kg",     UCAT_MASS, 1000.0 },
    { "mg",     UCAT_MASS, 0.001 },
    { "t",      UCAT_MASS, 1000000.0 },
    { "tonne",  UCAT_MASS, 1000000.0 },
    { "lb",     UCAT_MASS, 453.59237 },
    { "lbs",    UCAT_MASS, 453.59237 },
    { "pound",  UCAT_MASS, 453.59237 },
    { "oz",     UCAT_MASS, 28.349523 },

    /* volume — base litre */
    { "l",      UCAT_VOLUME, 1.0 },
    { "liter",  UCAT_VOLUME, 1.0 },
    { "litre",  UCAT_VOLUME, 1.0 },
    { "ml",     UCAT_VOLUME, 0.001 },
    { "gal",    UCAT_VOLUME, 3.785411784 },
    { "gallon", UCAT_VOLUME, 3.785411784 },
    { "qt",     UCAT_VOLUME, 0.946352946 },
    { "pt",     UCAT_VOLUME, 0.473176473 },
    { "cup",    UCAT_VOLUME, 0.2365882365 },

    /* time — base second */
    { "s",      UCAT_TIME, 1.0 },
    { "sec",    UCAT_TIME, 1.0 },
    { "min",    UCAT_TIME, 60.0 },
    { "h",      UCAT_TIME, 3600.0 },
    { "hr",     UCAT_TIME, 3600.0 },
    { "hour",   UCAT_TIME, 3600.0 },
    { "day",    UCAT_TIME, 86400.0 },
    { "week",   UCAT_TIME, 604800.0 },

    /* speed — base m/s */
    { "mps",    UCAT_SPEED, 1.0 },
    { "kmh",    UCAT_SPEED, 0.277777778 },
    { "kph",    UCAT_SPEED, 0.277777778 },
    { "mph",    UCAT_SPEED, 0.44704 },
    { "knot",   UCAT_SPEED, 0.514444 },
    { "knots",  UCAT_SPEED, 0.514444 },

    /* temperature — factor unused, handled specially */
    { "c",       UCAT_TEMP, 0.0 },
    { "celsius", UCAT_TEMP, 0.0 },
    { "f",       UCAT_TEMP, 0.0 },
    { "fahrenheit", UCAT_TEMP, 0.0 },
    { "k",       UCAT_TEMP, 0.0 },
    { "kelvin",  UCAT_TEMP, 0.0 },
};

static const UnitDef *olaunch_find_unit(const char *name) {
    for (size_t i = 0; i < sizeof(olaunch_units)/sizeof(olaunch_units[0]); i++)
        if (strcmp(olaunch_units[i].name, name) == 0)
            return &olaunch_units[i];
    return NULL;
}

static double olaunch_temp_to_c(const char *u, double v) {
    if (u[0] == 'f') return (v - 32.0) * 5.0 / 9.0;
    if (u[0] == 'k') return v - 273.15;
    return v; /* celsius */
}
static double olaunch_temp_from_c(const char *u, double c) {
    if (u[0] == 'f') return c * 9.0 / 5.0 + 32.0;
    if (u[0] == 'k') return c + 273.15;
    return c;
}

/*
 * Try to parse and convert a unit expression. On success returns 1 and fills
 * *out with the converted value plus the canonical destination unit name in
 * dst_unit (caller-provided, >=16 bytes). Returns 0 if the input is not a
 * recognised conversion.
 */
static int olaunch_convert(const char *input, double *out, char *dst_unit, size_t dst_sz) {
    if (!input) return 0;

    /* find " to " or " in " separator (case-insensitive, space-delimited) */
    char lo[128];
    size_t n = strlen(input);
    if (n == 0 || n >= sizeof(lo)) return 0;
    for (size_t i = 0; i <= n; i++) lo[i] = (char)tolower((unsigned char)input[i]);

    const char *sep = NULL; int seplen = 0;
    for (char *q = lo; *q; q++) {
        if (q[0]==' ' && (q[1]=='t'&&q[2]=='o') && q[3]==' ') { sep=q; seplen=4; break; }
        if (q[0]==' ' && (q[1]=='i'&&q[2]=='n') && q[3]==' ') { sep=q; seplen=4; break; }
    }
    if (!sep) return 0;

    /* left side: "<number> <unit>" */
    char *end = NULL;
    double val = strtod(lo, &end);
    if (end == lo) return 0;
    while (*end == ' ') end++;

    char u1[16];
    size_t u1len = (size_t)(sep - end);
    if (u1len == 0 || u1len >= sizeof(u1)) return 0;
    memcpy(u1, end, u1len); u1[u1len] = '\0';
    /* trim trailing spaces in u1 */
    while (u1len > 0 && u1[u1len-1] == ' ') u1[--u1len] = '\0';

    /* right side: unit after separator */
    const char *r = sep + seplen;
    while (*r == ' ') r++;
    char u2[16];
    size_t u2len = strlen(r);
    if (u2len == 0 || u2len >= sizeof(u2)) return 0;
    memcpy(u2, r, u2len + 1);
    while (u2len > 0 && u2[u2len-1] == ' ') u2[--u2len] = '\0';

    const UnitDef *a = olaunch_find_unit(u1);
    const UnitDef *b = olaunch_find_unit(u2);
    if (!a || !b || a->cat != b->cat) return 0;

    double result;
    if (a->cat == UCAT_TEMP) {
        result = olaunch_temp_from_c(b->name, olaunch_temp_to_c(a->name, val));
    } else {
        result = val * a->factor / b->factor;
    }

    if (out) *out = result;
    if (dst_unit && dst_sz) { strncpy(dst_unit, b->name, dst_sz - 1); dst_unit[dst_sz-1] = '\0'; }
    return 1;
}

#endif /* OLAUNCH_UNITS_H */
