// calc.c - a scientific calculator: type an expression, Enter evaluates it. A live preview shows
// the result while typing; Up / Down bring back earlier lines.
//
// Grammar (recursive descent, doubles):
//   line    := name '=' sum | sum
//   sum     := term (('+' | '-') term)*
//   term    := unary (('*' | '/' | '%') unary | <implicit *> unary)*
//   unary   := ('-' | '+') unary | power
//   power   := postfix ('^' unary)?           right-associative, -2^2 = -4
//   postfix := primary '!'*
//   primary := number | name | name '(' sum (',' sum)* ')' | '(' sum ')'
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pmlapp.h"

#define INPUT_MAX   120
#define HISTORY_MAX 48
#define VARS_MAX    16
#define NAME_MAX    10

typedef struct {
    char expr[INPUT_MAX + 1];
    char result[48];
    bool error;
} entry_t;

typedef struct {
    char name[NAME_MAX + 1];
    double value;
} var_t;

typedef enum { BASE_DEC, BASE_HEX, BASE_BIN, BASE_OCT } base_t;
static const char *const BASE_NAMES[] = {"DEC", "HEX", "BIN", "OCT"};

static entry_t s_hist[HISTORY_MAX];
static int s_hist_n;
static var_t s_vars[VARS_MAX];
static int s_vars_n;
static double s_ans;
static bool s_degrees;
static base_t s_base;

static char s_input[INPUT_MAX + 1];
static int s_cur;
static int s_recall = -1; // history entry shown in the input line, -1 = a new line

// ---- evaluation ---------------------------------------------------------------------------------
typedef struct {
    const char *p;
    const char *err;
    bool dry; // a preview: don't assign
} parser_t;

static double sum(parser_t *ps);
static double unary(parser_t *ps);

static void skip(parser_t *ps)
{
    while (*ps->p == ' ') {
        ps->p++;
    }
}

static bool accept(parser_t *ps, char c)
{
    skip(ps);
    if (*ps->p == c) {
        ps->p++;
        return true;
    }
    return false;
}

static double fail(parser_t *ps, const char *why)
{
    if (!ps->err) {
        ps->err = why;
    }
    return NAN;
}

static bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static int read_name(parser_t *ps, char *out)
{
    int n = 0;
    while (is_name_char(*ps->p)) {
        if (n < NAME_MAX) {
            out[n++] = (char)(*ps->p >= 'A' && *ps->p <= 'Z' ? *ps->p + 32 : *ps->p);
        }
        ps->p++;
    }
    out[n] = '\0';
    return n;
}

static var_t *find_var(const char *name)
{
    for (int i = 0; i < s_vars_n; i++) {
        if (strcmp(s_vars[i].name, name) == 0) {
            return &s_vars[i];
        }
    }
    return NULL;
}

static double to_rad(double x)
{
    return s_degrees ? x * (M_PI / 180.0) : x;
}

static double from_rad(double x)
{
    return s_degrees ? x * (180.0 / M_PI) : x;
}

static double factorial(parser_t *ps, double x)
{
    if (x < 0 || x != floor(x)) {
        return tgamma(x + 1);
    }
    if (x > 170) {
        return fail(ps, "Too large");
    }
    double r = 1;
    for (int i = 2; i <= (int)x; i++) {
        r *= i;
    }
    return r;
}

static double call(parser_t *ps, const char *f, const double *a, int n)
{
    static const struct {
        const char *name;
        int args;
    } FUNCS[] = {
        {"sin", 1}, {"cos", 1}, {"tan", 1}, {"asin", 1}, {"acos", 1}, {"atan", 1}, {"sinh", 1},
        {"cosh", 1}, {"tanh", 1}, {"sqrt", 1}, {"cbrt", 1}, {"exp", 1}, {"ln", 1}, {"log", 1},
        {"log2", 1}, {"abs", 1}, {"floor", 1}, {"ceil", 1}, {"round", 1}, {"trunc", 1},
        {"pow", 2}, {"atan2", 2}, {"mod", 2}, {"root", 2}, {"hypot", 2}, {"ncr", 2}, {"npr", 2},
        {"min", -1}, {"max", -1},
    };
    int want = 0;
    bool known = false;
    for (size_t i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); i++) {
        if (strcmp(f, FUNCS[i].name) == 0) {
            want = FUNCS[i].args;
            known = true;
        }
    }
    if (!known) {
        return fail(ps, "Unknown function");
    }
    if ((want > 0 && n != want) || (want < 0 && n < 1)) {
        return fail(ps, "Wrong number of arguments");
    }
    double x = a[0], y = n > 1 ? a[1] : 0;
    if (!strcmp(f, "sin")) return sin(to_rad(x));
    if (!strcmp(f, "cos")) return cos(to_rad(x));
    if (!strcmp(f, "tan")) return tan(to_rad(x));
    if (!strcmp(f, "asin")) return from_rad(asin(x));
    if (!strcmp(f, "acos")) return from_rad(acos(x));
    if (!strcmp(f, "atan")) return from_rad(atan(x));
    if (!strcmp(f, "atan2")) return from_rad(atan2(x, y));
    if (!strcmp(f, "sinh")) return sinh(x);
    if (!strcmp(f, "cosh")) return cosh(x);
    if (!strcmp(f, "tanh")) return tanh(x);
    if (!strcmp(f, "sqrt")) return sqrt(x);
    if (!strcmp(f, "cbrt")) return cbrt(x);
    if (!strcmp(f, "exp")) return exp(x);
    if (!strcmp(f, "ln")) return log(x);
    if (!strcmp(f, "log")) return log10(x);
    if (!strcmp(f, "log2")) return log2(x);
    if (!strcmp(f, "abs")) return fabs(x);
    if (!strcmp(f, "floor")) return floor(x);
    if (!strcmp(f, "ceil")) return ceil(x);
    if (!strcmp(f, "round")) return round(x);
    if (!strcmp(f, "trunc")) return trunc(x);
    if (!strcmp(f, "pow")) return pow(x, y);
    if (!strcmp(f, "mod")) return y == 0 ? fail(ps, "Division by zero") : fmod(x, y);
    if (!strcmp(f, "root")) return y == 0 ? fail(ps, "Math error") : (x < 0 && fmod(y, 2) == 1) ? -pow(-x, 1 / y) : pow(x, 1 / y);
    if (!strcmp(f, "hypot")) return hypot(x, y);
    if (!strcmp(f, "ncr") || !strcmp(f, "npr")) {
        if (x < 0 || y < 0 || y > x || x != floor(x) || y != floor(y)) {
            return fail(ps, "Math error");
        }
        double r = 1;
        for (int i = 0; i < (int)y; i++) {
            r *= (x - i);
            if (f[1] == 'c') {
                r /= (i + 1);
            }
        }
        return round(r);
    }
    double r = a[0];
    for (int i = 1; i < n; i++) {
        r = f[1] == 'i' ? fmin(r, a[i]) : fmax(r, a[i]);
    }
    return r;
}

static double number(parser_t *ps)
{
    const char *p = ps->p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B' || p[1] == 'o' || p[1] == 'O')) {
        int radix = (p[1] | 32) == 'x' ? 16 : (p[1] | 32) == 'b' ? 2 : 8;
        char *end;
        unsigned long long v = strtoull(p + 2, &end, radix);
        if (end == p + 2) {
            return fail(ps, "Bad number");
        }
        ps->p = end;
        return (double)v;
    }
    char *end;
    double v = strtod(p, &end);
    if (end == p) {
        return fail(ps, "Bad number");
    }
    ps->p = end;
    return v;
}

static double primary(parser_t *ps)
{
    skip(ps);
    char c = *ps->p;
    if ((c >= '0' && c <= '9') || c == '.') {
        return number(ps);
    }
    if (accept(ps, '(')) {
        double v = sum(ps);
        if (!accept(ps, ')')) {
            return fail(ps, "Missing )");
        }
        return v;
    }
    if (is_name_start(c)) {
        char name[NAME_MAX + 1];
        read_name(ps, name);
        if (accept(ps, '(')) {
            double args[8];
            int n = 0;
            if (!accept(ps, ')')) {
                do {
                    double v = sum(ps);
                    if (n < 8) {
                        args[n++] = v;
                    }
                } while (accept(ps, ','));
                if (!accept(ps, ')')) {
                    return fail(ps, "Missing )");
                }
            }
            return call(ps, name, args, n);
        }
        if (!strcmp(name, "pi")) return M_PI;
        if (!strcmp(name, "e")) return M_E;
        if (!strcmp(name, "ans")) return s_ans;
        var_t *v = find_var(name);
        return v ? v->value : fail(ps, "Unknown name");
    }
    return fail(ps, c ? "Syntax error" : "Incomplete");
}

static double postfix(parser_t *ps)
{
    double v = primary(ps);
    while (accept(ps, '!')) {
        v = factorial(ps, v);
    }
    return v;
}

static double power(parser_t *ps)
{
    double v = postfix(ps);
    if (accept(ps, '^')) {
        v = pow(v, unary(ps));
    }
    return v;
}

static double unary(parser_t *ps)
{
    if (accept(ps, '-')) {
        return -unary(ps);
    }
    if (accept(ps, '+')) {
        return unary(ps);
    }
    return power(ps);
}

static double term(parser_t *ps)
{
    double v = unary(ps);
    for (;;) {
        skip(ps);
        char c = *ps->p;
        if (c == '*' || c == '/' || c == '%') {
            ps->p++;
            double r = unary(ps);
            if ((c == '/' || c == '%') && r == 0) {
                return fail(ps, "Division by zero");
            }
            v = c == '*' ? v * r : c == '/' ? v / r : fmod(v, r);
        } else if (c == '(' || is_name_start(c) || (c >= '0' && c <= '9') || c == '.') {
            v *= unary(ps); // 2pi, 3(4+5), 2sqrt(2)
        } else {
            return v;
        }
    }
}

static double sum(parser_t *ps)
{
    double v = term(ps);
    for (;;) {
        if (accept(ps, '+')) {
            v += term(ps);
        } else if (accept(ps, '-')) {
            v -= term(ps);
        } else {
            return v;
        }
    }
}

// Evaluate a line. False (and *err) on an error.
static bool evaluate(const char *line, bool dry, double *out, const char **err)
{
    parser_t ps = {.p = line, .dry = dry};
    char name[NAME_MAX + 1] = "";
    // "name = ..." assigns
    skip(&ps);
    if (is_name_start(*ps.p)) {
        const char *save = ps.p;
        read_name(&ps, name);
        skip(&ps);
        if (*ps.p == '=') {
            ps.p++;
        } else {
            ps.p = save;
            name[0] = '\0';
        }
    }
    double v = sum(&ps);
    skip(&ps);
    if (!ps.err && *ps.p) {
        ps.err = *ps.p == ')' ? "Unmatched )" : "Syntax error";
    }
    if (!ps.err && isnan(v)) {
        ps.err = "Math error";
    }
    if (ps.err) {
        *err = ps.err;
        return false;
    }
    if (name[0] && !dry) {
        if (!strcmp(name, "pi") || !strcmp(name, "e") || !strcmp(name, "ans")) {
            *err = "Can't assign that name";
            return false;
        }
        var_t *var = find_var(name);
        if (!var && s_vars_n < VARS_MAX) {
            var = &s_vars[s_vars_n++];
            strcpy(var->name, name);
        }
        if (!var) {
            *err = "Too many variables";
            return false;
        }
        var->value = v;
    }
    *out = v;
    return true;
}

static void format(double v, char *out, size_t len)
{
    if (v == 0) {
        v = 0; // no "-0"
    }
    if (s_base != BASE_DEC && v == floor(v) && fabs(v) < 9.2e18) {
        long long n = (long long)v;
        unsigned long long u = n < 0 ? (unsigned long long)-n : (unsigned long long)n;
        char digits[72];
        int k = 0;
        int radix = s_base == BASE_HEX ? 16 : s_base == BASE_BIN ? 2 : 8;
        do {
            digits[k++] = "0123456789ABCDEF"[u % radix];
            u /= radix;
        } while (u && k < 70);
        char *o = out;
        size_t room = len;
        int w = snprintf(o, room, "%s%s", n < 0 ? "-" : "", s_base == BASE_HEX ? "0x" : s_base == BASE_BIN ? "0b" : "0o");
        o += w;
        room -= w;
        while (k > 0 && room > 1) {
            *o++ = digits[--k];
            room--;
        }
        *o = '\0';
        return;
    }
    if (isinf(v)) {
        strlcpy(out, v > 0 ? "Infinity" : "-Infinity", len);
        return;
    }
    snprintf(out, len, "%.12g", v);
}

// ---- screen -------------------------------------------------------------------------------------
#define INPUT_Y    (APP_BOTTOM - 44)
#define INPUT_COLS ((LCD_W - 16) / 7)
#define ROW_H      24

static void draw(void)
{
    char right[16];
    snprintf(right, sizeof(right), "%s %s", s_degrees ? "DEG" : "RAD", BASE_NAMES[s_base]);
    app_header("Calculator", right);
    app_clear_body();

    // history, newest at the bottom: the expression, then the result on the right
    int y = INPUT_Y - ROW_H - 2;
    for (int i = s_hist_n - 1; i >= 0 && y >= APP_TOP; i--, y -= ROW_H) {
        const entry_t *e = &s_hist[i];
        bool sel = i == s_recall;
        if (sel) {
            lcd_fill_rect(0, y, LCD_W, ROW_H, C_FIELD);
        }
        app_text_clip(6, y + 1, e->expr, (LCD_W - 12) / 6, C_DIM, FONT_SMALL);
        int rl = (int)strlen(e->result);
        int rx = LCD_W - 6 - rl * 7;
        app_text_clip(rx < 6 ? 6 : rx, y + 10, e->result, INPUT_COLS, e->error ? C_ERR : C_BRIGHT, FONT_LARGE);
    }
    if (s_hist_n == 0) {
        lcd_text(8, APP_TOP + 10, "Type an expression and press Enter.", C_DIM, FONT_SMALL);
        lcd_text(8, APP_TOP + 24, "e.g.  2^10   sqrt(2)*3   sin(30)   x=5", C_DIM, FONT_SMALL);
        lcd_text(8, APP_TOP + 38, "F1 lists the functions.", C_DIM, FONT_SMALL);
    }

    // the input line, scrolled to keep the cursor in view, and the live result under it
    lcd_fill_rect(0, INPUT_Y, LCD_W, APP_BOTTOM - INPUT_Y, C_PANEL);
    lcd_hline(0, INPUT_Y, LCD_W, C_LINE);
    lcd_fill_rect(4, INPUT_Y + 5, LCD_W - 8, 19, C_FIELD);
    look_frame(4, INPUT_Y + 5, LCD_W - 8, 19, C_ACCENT);
    int n = (int)strlen(s_input), left = s_cur >= INPUT_COLS ? s_cur - INPUT_COLS + 1 : 0;
    char vis[INPUT_COLS + 1];
    int vn = n - left < INPUT_COLS ? n - left : INPUT_COLS;
    memcpy(vis, s_input + left, vn);
    vis[vn] = '\0';
    lcd_text(8, INPUT_Y + 8, vis, C_BRIGHT, FONT_LARGE);
    lcd_fill_rect(8 + (s_cur - left) * 7, INPUT_Y + 7, 1, 15, C_ACCENT);

    double v;
    const char *err;
    if (s_input[0]) {
        char preview[56] = "= ";
        if (evaluate(s_input, true, &v, &err)) {
            format(v, preview + 2, sizeof(preview) - 2);
            int x = LCD_W - 8 - (int)strlen(preview) * 6;
            lcd_text(x < 8 ? 8 : x, INPUT_Y + 29, preview, C_OK, FONT_SMALL);
        } else if (strcmp(err, "Incomplete") != 0) {
            lcd_text(8, INPUT_Y + 29, err, C_DIM, FONT_SMALL);
        }
    }
    app_footer("F1 Help  F2 Deg/Rad  F3 Base  F4 Clear  Esc Quit");
    lcd_flush();
}

static void enter(void)
{
    if (!s_input[0]) {
        return;
    }
    if (s_hist_n == HISTORY_MAX) {
        memmove(&s_hist[0], &s_hist[1], sizeof(s_hist[0]) * (HISTORY_MAX - 1));
        s_hist_n--;
    }
    entry_t *e = &s_hist[s_hist_n++];
    strlcpy(e->expr, s_input, sizeof(e->expr));
    double v;
    const char *err;
    e->error = !evaluate(s_input, false, &v, &err);
    if (e->error) {
        strlcpy(e->result, err, sizeof(e->result));
    } else {
        s_ans = v;
        format(v, e->result, sizeof(e->result));
    }
    s_input[0] = '\0';
    s_cur = 0;
    s_recall = -1;
}

static void insert(const char *s)
{
    int n = (int)strlen(s_input), k = (int)strlen(s);
    if (n + k > INPUT_MAX) {
        return;
    }
    memmove(s_input + s_cur + k, s_input + s_cur, n - s_cur + 1);
    memcpy(s_input + s_cur, s, k);
    s_cur += k;
}

static void recall(int i)
{
    s_recall = i;
    if (i < 0) {
        s_input[0] = '\0';
    } else {
        strlcpy(s_input, s_hist[i].expr, sizeof(s_input));
    }
    s_cur = (int)strlen(s_input);
}

static const char *const HELP[] = {
    "# Keys",
    "Enter      Evaluate the line",
    "Up/Down    Bring back earlier lines",
    "Left/Right Move in the line; Home/End",
    "Bksp/Del   Delete",
    "Tab        Insert \"ans\" (the last result)",
    "F2         Degrees / radians",
    "F3         Show results in dec/hex/bin/oct",
    "F4         Clear the history",
    "Esc        Clear the line; on an empty line, quit",
    "",
    "# Operators",
    "+ - * /    Arithmetic; 2pi and 3(1+2) multiply",
    "^          Power (right to left: 2^3^2 = 512)",
    "%          Remainder;  !  factorial",
    "x = 5      Store in a variable (up to 16)",
    "0x1F 0b101 Hex, binary (0o17: octal)",
    "",
    "# Functions",
    "Trig       sin cos tan asin acos atan atan2(y,x)",
    "Hyperbolic sinh cosh tanh",
    "Roots      sqrt cbrt root(x,n) hypot(a,b)",
    "Logs       ln log (base 10) log2 exp pow(a,b)",
    "Rounding   abs floor ceil round trunc mod(a,b)",
    "Counting   ncr(n,k) npr(n,k)",
    "Other      min(...) max(...)",
    "Constants  pi e ans",
};

int main(void)
{
    app_init();
    char v[8];
    if (app_setting_get("angle", v, sizeof(v))) {
        s_degrees = strcmp(v, "deg") == 0;
    } else {
        s_degrees = true;
    }
    if (app_setting_get("base", v, sizeof(v))) {
        for (int i = 0; i < 4; i++) {
            if (strcmp(v, BASE_NAMES[i]) == 0) {
                s_base = (base_t)i;
            }
        }
    }
    for (;;) {
        draw();
        int k = app_wait_key();
        int n = (int)strlen(s_input);
        switch (k) {
        case KEY_ENTER:
            enter();
            break;
        case KEY_ESC:
            if (s_input[0]) {
                s_input[0] = '\0';
                s_cur = 0;
                s_recall = -1;
            } else {
                app_exit(NULL);
            }
            break;
        case KEY_UP:
            if (s_hist_n) {
                recall(s_recall < 0 ? s_hist_n - 1 : s_recall > 0 ? s_recall - 1 : 0);
            }
            break;
        case KEY_DOWN:
            if (s_recall >= 0) {
                recall(s_recall + 1 < s_hist_n ? s_recall + 1 : -1);
            }
            break;
        case KEY_LEFT:
            if (s_cur > 0) {
                s_cur--;
            }
            break;
        case KEY_RIGHT:
            if (s_cur < n) {
                s_cur++;
            }
            break;
        case KEY_HOME:
            s_cur = 0;
            break;
        case KEY_END:
            s_cur = n;
            break;
        case KEY_BACKSPACE:
            if (s_cur > 0) {
                memmove(s_input + s_cur - 1, s_input + s_cur, n - s_cur + 1);
                s_cur--;
            }
            break;
        case KEY_DEL:
            if (s_cur < n) {
                memmove(s_input + s_cur, s_input + s_cur + 1, n - s_cur);
            }
            break;
        case KEY_TAB:
            insert("ans");
            break;
        case KEY_F1:
            app_help("Calculator help", HELP, sizeof(HELP) / sizeof(HELP[0]));
            break;
        case KEY_F2:
            s_degrees = !s_degrees;
            app_setting_set("angle", s_degrees ? "deg" : "rad");
            break;
        case KEY_F3:
            s_base = (base_t)((s_base + 1) % 4);
            app_setting_set("base", BASE_NAMES[s_base]);
            break;
        case KEY_F4:
            s_hist_n = 0;
            s_recall = -1;
            break;
        default:
            if (k >= 0x20 && k < 0x7F) {
                if (s_cur == 0 && n == 0 && strchr("+*/^%!", k) && s_hist_n && !s_hist[s_hist_n - 1].error) {
                    insert("ans"); // an operator first continues from the last result
                }
                insert((char[]){(char)k, 0});
            }
            break;
        }
    }
}
