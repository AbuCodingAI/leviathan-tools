/**
 * expr.h — tiny safe recursive-descent expression evaluator for OLaunch.
 *
 * Pure C (only libm). No allocation, no shelling out. Header-only so it can be
 * unit-tested in isolation without GTK.
 *
 * Grammar (standard precedence):
 *   expr    := term (('+' | '-') term)*
 *   term    := power (('*' | '/' | '%') power)*
 *   power   := unary ('^' power)?           // right-associative
 *   unary   := ('+' | '-') unary | primary
 *   primary := number | '(' expr ')' | ident '(' args ')' | const
 *
 * Supported functions: sqrt abs round floor ceil pow(a,b) sin cos tan log exp
 * Supported constants: pi e
 */
#ifndef OLAUNCH_EXPR_H
#define OLAUNCH_EXPR_H

#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    const char *p;
    int error;
} ExprState;

static double expr_p_expr(ExprState *s);

static void expr_skip_ws(ExprState *s) {
    while (*s->p == ' ' || *s->p == '\t') s->p++;
}

static double expr_p_primary(ExprState *s) {
    expr_skip_ws(s);
    char c = *s->p;

    if (c == '(') {
        s->p++;
        double v = expr_p_expr(s);
        expr_skip_ws(s);
        if (*s->p == ')') s->p++; else s->error = 1;
        return v;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        const char *start = s->p;
        while (isalpha((unsigned char)*s->p) || *s->p == '_') s->p++;
        size_t len = (size_t)(s->p - start);
        char name[16];
        if (len == 0 || len >= sizeof(name)) { s->error = 1; return 0; }
        for (size_t i = 0; i < len; i++) name[i] = (char)tolower((unsigned char)start[i]);
        name[len] = '\0';

        expr_skip_ws(s);

        /* constants first (no parens) */
        if (strcmp(name, "pi") == 0) return M_PI;
        if (strcmp(name, "e")  == 0) return M_E;

        if (*s->p != '(') { s->error = 1; return 0; }
        s->p++; /* consume '(' */
        double a = expr_p_expr(s);
        double b = 0.0; int have_b = 0;
        expr_skip_ws(s);
        if (*s->p == ',') { s->p++; b = expr_p_expr(s); have_b = 1; expr_skip_ws(s); }
        if (*s->p == ')') s->p++; else s->error = 1;

        if (strcmp(name, "sqrt")  == 0) return sqrt(a);
        if (strcmp(name, "abs")   == 0) return fabs(a);
        if (strcmp(name, "round") == 0) return round(a);
        if (strcmp(name, "floor") == 0) return floor(a);
        if (strcmp(name, "ceil")  == 0) return ceil(a);
        if (strcmp(name, "pow")   == 0) return have_b ? pow(a, b) : a;
        if (strcmp(name, "sin")   == 0) return sin(a);
        if (strcmp(name, "cos")   == 0) return cos(a);
        if (strcmp(name, "tan")   == 0) return tan(a);
        if (strcmp(name, "log")   == 0) return log(a);
        if (strcmp(name, "exp")   == 0) return exp(a);
        (void)b;
        s->error = 1;
        return 0;
    }

    /* numeric literal */
    char *end = NULL;
    double v = strtod(s->p, &end);
    if (end == s->p) { s->error = 1; return 0; }
    s->p = end;
    return v;
}

static double expr_p_unary(ExprState *s) {
    expr_skip_ws(s);
    if (*s->p == '-') { s->p++; return -expr_p_unary(s); }
    if (*s->p == '+') { s->p++; return  expr_p_unary(s); }
    return expr_p_primary(s);
}

static double expr_p_power(ExprState *s) {
    double base = expr_p_unary(s);
    expr_skip_ws(s);
    if (*s->p == '^') { s->p++; double e = expr_p_power(s); return pow(base, e); }
    return base;
}

static double expr_p_term(ExprState *s) {
    double v = expr_p_power(s);
    for (;;) {
        expr_skip_ws(s);
        char c = *s->p;
        if (c == '*')      { s->p++; v *= expr_p_power(s); }
        else if (c == '/') { s->p++; v /= expr_p_power(s); }
        else if (c == '%') { s->p++; double r = expr_p_power(s); v = fmod(v, r); }
        else break;
    }
    return v;
}

static double expr_p_expr(ExprState *s) {
    double v = expr_p_term(s);
    for (;;) {
        expr_skip_ws(s);
        char c = *s->p;
        if (c == '+')      { s->p++; v += expr_p_term(s); }
        else if (c == '-') { s->p++; v -= expr_p_term(s); }
        else break;
    }
    return v;
}

/*
 * Evaluate `input`. Returns 1 on success (result in *out), 0 on any parse
 * error, trailing garbage, or non-finite result.
 */
static int expr_eval(const char *input, double *out) {
    if (!input) return 0;
    ExprState s = { input, 0 };
    double v = expr_p_expr(&s);
    expr_skip_ws(&s);
    if (s.error || *s.p != '\0' || !isfinite(v)) return 0;
    if (out) *out = v;
    return 1;
}

/* True if the string contains at least one decimal digit. */
static int expr_has_digit(const char *s) {
    for (; s && *s; s++) if (isdigit((unsigned char)*s)) return 1;
    return 0;
}

#endif /* OLAUNCH_EXPR_H */
