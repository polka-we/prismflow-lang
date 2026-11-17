
/* ---------------------------------------------------------------- встроенные функции */
static void out_write(VM *vm, const char *s, size_t n) { if (vm->out) vm->out(s, n, vm->outud); }

static Value nat_print(VM *vm, Value *a, int n) {
    SB sb; sb_init(vm, &sb);
    for (int i = 0; i < n; i++) { if (i) sb_c(&sb, ' '); Str *s = to_str(vm, a[i], 1); sb_add(&sb, s->s, s->len); }
    sb_c(&sb, '\n');
    out_write(vm, sb.b, (size_t)sb.n);
    return V_none();
}
static Value mk_stream(VM *vm, int kind, Value up, Value f, int line, int stateful, int arrchain);
static Value mk_range(VM *vm, int64_t s, int64_t e, int64_t st, int hasE);
static Value nat_range(VM *vm, Value *a, int n) {
    int64_t v[3] = { 0, 0, 1 }; int hasE = 1;
    for (int i = 0; i < n && i < 3; i++) {
        if (i == 1 && n >= 2 && a[1].t == T_NONE) continue;
        if (a[i].t != T_INT) rt(vm, 0, "range: expected int, got %s%s", type_of(a[i]), a[i].t == T_FLT ? "" : "");
    }
    if (n == 1) v[1] = a[0].u.i;
    else if (n == 2 || n == 3) { v[0] = a[0].u.i; if (a[1].t == T_NONE) hasE = 0; else v[1] = a[1].u.i; if (n == 3) v[2] = a[2].u.i; }
    else rt(vm, 0, "range takes 1 to 3 arguments");
    if (v[2] == 0) rt(vm, 0, "range: step cannot be 0");
    return mk_range(vm, v[0], v[1], v[2], hasE);
}
static Value nat_len(VM *vm, Value *a, int n) {
    if (n < 1) rt(vm, 0, "len: missing argument");
    Value v = a[0];
    if (v.t == T_STR) return V_int(u8_count(v.u.s->s, v.u.s->len));
    if (v.t == T_ARR) return V_int(v.u.a->n);
    if (v.t == T_STREAM) return V_int(stream_materialize(vm, v).u.a->n);
    if (v.t == T_OBJ) return V_int(v.u.o->n);
    rt(vm, 0, "len: not supported for %s", type_of(v));
    return V_none();
}
static Value nat_str(VM *vm, Value *a, int n) { return V_str(to_str(vm, n ? a[0] : V_none(), 1)); }
static Value nat_type(VM *vm, Value *a, int n) { return V_str(cstr(vm, type_of(n ? a[0] : V_none()))); }
static Value nat_error(VM *vm, Value *a, int n) {
    Value v = V_none(); v.t = T_ERR; v.u.s = n ? to_str(vm, a[0], 1) : cstr(vm, "error"); return v;
}
static Value to_int_checked(VM *vm, double x) {
    if (!isfinite(x)) { char b[64]; fmt_float(x, b, sizeof b); char m[120]; snprintf(m, sizeof m, "cannot convert %s to int", b); return V_err(vm, m); }
    if (x >= 9.3e18 || x <= -9.3e18) rt(vm, 0, "integer out of range");
    return V_int((int64_t)x);
}
static Value m1(VM *vm, Value *a, int n, const char *name, int kind) {
    if (n < 1 || !is_num(a[0])) rt(vm, 0, "%s: expected number, got %s", name, type_of(n ? a[0] : V_none()));
    Value v = a[0];
    if (v.t == T_DEC || (kind == 1 && vm->prec > 0 && v.t != T_BIG)) {   /* десятичные: abs, sqrt, floor, ceil, round без потери точности */
        Value d = to_dec(vm, v);
        if (d.t == T_DEC) switch (kind) {
            case 0: return dec_abs(vm, d);
            case 1: return dec_sqrt(vm, d);
            case 2: return dec_to_int(vm, d, 0);
            case 3: return dec_to_int(vm, d, 1);
            default: return dec_to_int(vm, d, 3);
        }
    }
    switch (kind) {
        case 0:
            if (v.t == T_BIG) { Big *b = big_neg(vm, v.u.big); if (v.u.big->sign > 0) return v; return big_norm(b); }
            if (v.t == T_INT) { if (v.u.i == INT64_MIN) return big_norm(big_neg(vm, big_from_i64(vm, v.u.i))); return V_int(v.u.i < 0 ? -v.u.i : v.u.i); }
            return V_flt(fabs(v.u.f));
        case 1: return V_flt(sqrt(as_f(v)));
        case 2: return v.t != T_FLT ? v : to_int_checked(vm, floor(v.u.f));
        case 3: return v.t != T_FLT ? v : to_int_checked(vm, ceil(v.u.f));
        default: return v.t != T_FLT ? v : to_int_checked(vm, floor(v.u.f + 0.5));
    }
}
static Value nat_abs(VM *vm, Value *a, int n) { return m1(vm, a, n, "abs", 0); }
static Value nat_sqrt(VM *vm, Value *a, int n) { return m1(vm, a, n, "sqrt", 1); }
static Value nat_floor(VM *vm, Value *a, int n) { return m1(vm, a, n, "floor", 2); }
static Value nat_ceil(VM *vm, Value *a, int n) { return m1(vm, a, n, "ceil", 3); }
static Value minmax(VM *vm, Value *a, int n, int sign, const char *name) {
    Value *xs = a; int cnt = n;
    if (n == 1 && a[0].t == T_ARR) { xs = a[0].u.a->v; cnt = a[0].u.a->n; }
    if (!cnt) return V_none();
    for (int i = 0; i < cnt; i++) if (!is_num(xs[i])) rt(vm, 0, "%s: expected number, got %s", name, type_of(xs[i]));
    Value m = xs[0];
    for (int i = 1; i < cnt; i++) {
        int c = (xs[i].t == T_DEC || m.t == T_DEC) ? dec_cmp_values(xs[i], m) : (xs[i].t == T_INT && m.t == T_INT) ? (xs[i].u.i < m.u.i ? -1 : xs[i].u.i > m.u.i ? 1 : 0) : (xs[i].t != T_FLT && m.t != T_FLT) ? big_cmp(vm, xs[i], m) : (as_f(xs[i]) < as_f(m) ? -1 : as_f(xs[i]) > as_f(m) ? 1 : 0);
        if (sign * c > 0) m = xs[i];
    }
    return m;
}
static Value nat_min(VM *vm, Value *a, int n) { return minmax(vm, a, n, -1, "min"); }
static Value nat_max(VM *vm, Value *a, int n) { return minmax(vm, a, n, 1, "max"); }
static Value nat_div(VM *vm, Value *a, int n) {
    if (n != 2 || !is_num(a[0]) || !is_num(a[1])) rt(vm, 0, "div(a, b) requires two numbers");
    if ((a[0].t == T_BIG || a[1].t == T_BIG) && a[0].t != T_FLT && a[1].t != T_FLT) return big_arith(vm, 5, a[0], a[1], 0);
    if (a[0].t == T_INT && a[1].t == T_INT) {
        int64_t x = a[0].u.i, y = a[1].u.i;
        if (y == 0) return V_err(vm, "division by zero");
        if (y == -1) { if (x == INT64_MIN) return big_norm(big_neg(vm, big_from_i64(vm, x))); return V_int(-x); }
        int64_t q = x / y; if ((x % y != 0) && ((x < 0) != (y < 0))) q -= 1;
        return V_int(q);
    }
    if (as_f(a[1]) == 0) return V_err(vm, "division by zero");
    return to_int_checked(vm, floor(as_f(a[0]) / as_f(a[1])));
}
static Str *trim_str(VM *vm, Str *s) {
    int a = 0, b = s->len;
    while (a < b && isspace((unsigned char)s->s[a])) a++;
    while (b > a && isspace((unsigned char)s->s[b - 1])) b--;
    return mkstr(vm, s->s + a, b - a);
}
static Value parse_int_str(VM *vm, Str *s0) {
    Str *s = trim_str(vm, s0); int i = 0;
    if (i < s->len && (s->s[i] == '+' || s->s[i] == '-')) i++;
    int ok = i < s->len;
    for (int j = i; j < s->len; j++) if (!isdigit((unsigned char)s->s[j])) ok = 0;
    if (!ok) { char m[300]; snprintf(m, sizeof m, "\"%.200s\" is not a valid int", s0->s); return V_err(vm, m); }
    if (s->len - i > 18) return big_parse(vm, s->s + i, s->len - i, s->s[0] == '-');
    return V_int((int64_t)strtoll(cz(vm, s), NULL, 10));
}
static Value parse_flt_str(VM *vm, Str *s0) {
    Str *s = trim_str(vm, s0); int i = 0, dig = 0, dot = 0, ok = 1;
    if (i < s->len && (s->s[i] == '+' || s->s[i] == '-')) i++;
    for (; i < s->len; i++) { if (isdigit((unsigned char)s->s[i])) dig++; else if (s->s[i] == '.' && !dot) dot = 1; else ok = 0; }
    if (!ok || !dig) { char m[300]; snprintf(m, sizeof m, "\"%.200s\" is not a valid float", s0->s); return V_err(vm, m); }
    if (vm->prec > 0) { Value d = dec_parse_str(vm, s->s, s->len); if (d.t == T_DEC) return d; }
    return V_flt(strtod(cz(vm, s), NULL));
}
static Value parse_int_base(VM *vm, Str *s0, Value bv);
static Value nat_int(VM *vm, Value *a, int n) {
    if (n < 1) rt(vm, 0, "int(): missing argument");
    Value v = a[0];
    if (v.t == T_INT) return v;
    if (v.t == T_FLT) return to_int_checked(vm, trunc(v.u.f));
    if (v.t == T_DEC) return dec_to_int(vm, v, 2);
    if (v.t == T_STR) return n > 1 ? parse_int_base(vm, v.u.s, a[1]) : parse_int_str(vm, v.u.s);
    if (v.t == T_BOOL) return V_int(v.u.b);
    rt(vm, 0, "int(): cannot convert %s", type_of(v));
    return V_none();
}
static Value nat_float(VM *vm, Value *a, int n) {
    if (n < 1) rt(vm, 0, "float(): missing argument");
    Value v = a[0];
    if (is_num(v)) { if (vm->prec > 0) { Value d = to_dec(vm, v); if (d.t == T_DEC) return d; } return V_flt(as_f(v)); }
    if (v.t == T_STR) return parse_flt_str(vm, v.u.s);
    rt(vm, 0, "float(): cannot convert %s", type_of(v));
    return V_none();
}
static Value nat_random(VM *vm, Value *a, int n) {
    (void)a; (void)n;
    uint64_t x = vm->rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; vm->rng = x;
    return V_flt((double)(x >> 11) / 9007199254740992.0);
}
static int read_line(VM *vm, Str **out) {
    if (vm->wid) rt(vm, vm->curline, "request.* cannot be used inside ::par");
    if (!vm->in) return 0;
    char buf[65536];
    int k = vm->in(buf, sizeof buf, vm->inud);
    if (k < 0) return 0;
    if (k > (int)sizeof buf) k = (int)sizeof buf;
    while (k > 0 && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) k--;
    *out = mkstr(vm, buf, k);
    return 1;
}
static Value nat_req_line(VM *vm, Value *a, int n) { (void)a; (void)n; Str *s; return read_line(vm, &s) ? V_str(s) : V_none(); }
/* виды потоков */
static Value mk_stream(VM *vm, int kind, Value up, Value f, int line, int stateful, int arrchain);
static Value nat_req_lines(VM *vm, Value *a, int n) { (void)a; (void)n; return mk_stream(vm, SK_LINES, V_none(), V_none(), 0, 1, 0); }
static Value nat_req_words(VM *vm, Value *a, int n) { (void)a; (void)n; return mk_stream(vm, SK_WORDS, V_none(), V_none(), 0, 1, 0); }
static Value nat_req_ints(VM *vm, Value *a, int n) { (void)a; (void)n; return mk_stream(vm, SK_INTS, V_none(), V_none(), 0, 1, 0); }


/* ---------------------------------------------------------------- математика и текст */
static Value nat_round2(VM *vm, Value *a, int n) {
    if (n < 2) return m1(vm, a, n, "round", 4);
    if (a[0].t == T_DEC) {
        if (a[1].t != T_INT || a[1].u.i < 0 || a[1].u.i > 1000) rt(vm, 0, "round(x, n): n must be an int in 0..1000");
        return dec_round_to(vm, a[0], (int)a[1].u.i);
    }
    if (!is_num(a[0]) || a[1].t != T_INT || a[1].u.i < 0 || a[1].u.i > 15) rt(vm, 0, "round(x, n): x must be a number and n an int in 0..15");
    if (a[0].t != T_FLT) return a[0];
    double x = a[0].u.f, p = pow(10, (double)a[1].u.i);
    if (!isfinite(x)) return a[0];
    double r = floor(fabs(x) * p + 0.5) / p;
    return V_flt(x < 0 ? -r : (x > 0 ? r : x));
}
#define MATHF(NAME, EXPR) static Value nat_##NAME(VM *vm, Value *a, int n) { \
    if (n != 1 || !is_num(a[0])) rt(vm, 0, #NAME ": expected one number, got %s", n == 1 ? type_of(a[0]) : "wrong argument count"); \
    double x = as_f(a[0]); return V_flt(EXPR); }
MATHF(sin, sin(x)) MATHF(cos, cos(x)) MATHF(tan, tan(x)) MATHF(atan, atan(x)) MATHF(exp, exp(x)) MATHF(log2, log2(x)) MATHF(log10, log10(x))
static Value nat_log(VM *vm, Value *a, int n) {
    if (n < 1 || n > 2 || !is_num(a[0]) || (n == 2 && !is_num(a[1]))) rt(vm, 0, "log(x) or log(x, base) requires numbers");
    return V_flt(n == 1 ? log(as_f(a[0])) : log(as_f(a[0])) / log(as_f(a[1])));
}
static Value nat_atan2(VM *vm, Value *a, int n) {
    if (n != 2 || !is_num(a[0]) || !is_num(a[1])) rt(vm, 0, "atan2(y, x) requires two numbers");
    return V_flt(atan2(as_f(a[0]), as_f(a[1])));
}
static int is_intv(Value v) { return v.t == T_INT || v.t == T_BIG; }
static Value nat_pow(VM *vm, Value *a, int n) {
    if ((n != 2 && n != 3) || !is_num(a[0]) || !is_num(a[1]) || (n == 3 && !is_num(a[2]))) rt(vm, 0, "pow(a, b) or pow(a, b, mod) requires numbers");
    if (n == 2) return arith(vm, OP_POW, a[0], a[1], 0);
    if (!is_intv(a[0]) || !is_intv(a[1]) || !is_intv(a[2])) rt(vm, 0, "pow(a, b, mod) requires ints");
    if (a[0].t != T_INT || a[1].t != T_INT || a[2].t != T_INT) rt(vm, 0, "pow(a, b, mod) is not supported for integers beyond 64 bits");
    int64_t m = a[2].u.i, e = a[1].u.i;
    if (m == 0) return V_err(vm, "division by zero");
    if (e < 0) rt(vm, 0, "pow(a, b, mod): b must be >= 0");
    __int128 mm = m, b = ((__int128)a[0].u.i % mm + mm) % mm, r = 1;
    while (e > 0) { if (e & 1) r = (r * b) % mm; e >>= 1; b = (b * b) % mm; }
    return V_int((int64_t)(((r % mm) + mm) % mm));
}
static int sgn_neg(VM *vm, Value v) { return v.t == T_BIG ? big_cmp(vm, v, V_int(0)) < 0 : v.u.i < 0; }
static Value nat_gcd(VM *vm, Value *a, int n) {
    if (n != 2 || !is_intv(a[0]) || !is_intv(a[1])) rt(vm, 0, "gcd(a, b) requires two ints");
    Value x = a[0], y = a[1];
    if (sgn_neg(vm, x)) x = arith(vm, OP_MUL, x, V_int(-1), 0);
    if (sgn_neg(vm, y)) y = arith(vm, OP_MUL, y, V_int(-1), 0);
    while (!val_eq(y, V_int(0))) { Value t = arith(vm, OP_MOD, x, y, 0); x = y; y = t; }
    return x;
}
static Value nat_divmod(VM *vm, Value *a, int n) {
    if (n != 2 || !is_intv(a[0]) || !is_intv(a[1])) rt(vm, 0, "divmod(a, b) requires two ints");
    Value q = nat_div(vm, a, 2);
    if (q.t == T_ERR) return q;
    Arr *r = arr_new(vm, 2); arr_push(vm, r, q); arr_push(vm, r, arith(vm, OP_MOD, a[0], a[1], 0));
    return V_arr(r);
}
static Value nat_ord(VM *vm, Value *a, int n) {
    if (n < 1 || a[0].t != T_STR || u8_count(a[0].u.s->s, a[0].u.s->len) != 1) rt(vm, 0, "ord(c) requires a one-character string");
    int adv; return V_int((int64_t)u8_dec(a[0].u.s->s, &adv));
}
static Value nat_chr(VM *vm, Value *a, int n) {
    if (n < 1 || a[0].t != T_INT || a[0].u.i < 0 || a[0].u.i > 0x10FFFF || (a[0].u.i >= 0xD800 && a[0].u.i <= 0xDFFF)) rt(vm, 0, "chr(n) requires a valid code point");
    char buf[8]; int k = u8_enc((uint32_t)a[0].u.i, buf); return V_str(mkstr(vm, buf, k));
}
static Value pi_value(void) { return V_flt(3.141592653589793); }
static Value to_str_base(VM *vm, Value v, Value b, int line) {
    if (b.t != T_INT || b.u.i < 2 || b.u.i > 36) rt(vm, line, "to_string(base): base must be int in 2..36");
    if (v.t == T_BIG) rt(vm, line, "to_string(base) is not supported for integers beyond 64 bits");
    char buf[80]; int i = 79; buf[i] = 0; int neg = v.u.i < 0; uint64_t u = neg ? (uint64_t)0 - (uint64_t)v.u.i : (uint64_t)v.u.i;
    if (!u) buf[--i] = '0';
    while (u) { int d = (int)(u % (uint64_t)b.u.i); buf[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (uint64_t)b.u.i; }
    if (neg) buf[--i] = '-';
    return V_str(cstr(vm, buf + i));
}
static Value parse_int_base(VM *vm, Str *s0, Value bv) {
    if (bv.t != T_INT || bv.u.i < 2 || bv.u.i > 36) rt(vm, 0, "int(s, base): base must be int in 2..36");
    Str *s = trim_str(vm, s0); int i = 0, neg = 0, base = (int)bv.u.i;
    if (i < s->len && (s->s[i] == '+' || s->s[i] == '-')) { neg = s->s[i] == '-'; i++; }
    int ok = i < s->len; Value r = V_int(0);
    for (int j = i; ok && j < s->len; j++) {
        int c = tolower((unsigned char)s->s[j]), d = isdigit(c) ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : 99;
        if (d >= base) { ok = 0; break; }
        r = arith(vm, OP_ADD, arith(vm, OP_MUL, r, V_int(base), 0), V_int(d), 0);
    }
    if (!ok) { SB sb; sb_init(vm, &sb); sb_add(&sb, "\"", 1); sb_add(&sb, s0->s, s0->len); sb_add(&sb, "\" is not a valid int", 20); return V_err(vm, sb_str(&sb)->s); }
    return neg ? arith(vm, OP_MUL, r, V_int(-1), 0) : r;
}

static Value nat_sum(VM *vm, Value *a, int n);


/* ---------------------------------------------------------------- модули: from("name") */
static Mod *mod_find(VM *vm, const char *nm) {
    for (int i = 0; i < vm->nmods; i++) if (!strcmp(vm->mods[i].name->s, nm)) return &vm->mods[i];
    return NULL;
}
static Mod *mod_add(VM *vm, const char *nm, const char *src, size_t len) {
    if (vm->nmods == vm->capmods) {
        int nc = vm->capmods ? vm->capmods * 2 : 8;
        Mod *nm2 = (Mod *)amalloc(vm, sizeof(Mod) * (size_t)nc);
        if (vm->nmods) memcpy(nm2, vm->mods, sizeof(Mod) * (size_t)vm->nmods);
        vm->mods = nm2; vm->capmods = nc;
    }
    Mod *m = &vm->mods[vm->nmods++];
    m->name = mkstr(vm, nm, (int)strlen(nm));
    m->src = (char *)amalloc(vm, len + 1); memcpy(m->src, src, len); m->src[len] = 0;
    m->exp = NULL; m->state = 0;
    return m;
}
static Mod *mod_from_file(VM *vm, const char *nm) {
    static const char *ext[] = { ".prism", ".pr", NULL };
    if (strchr(nm, '/') || strchr(nm, '\\') || strstr(nm, "..")) return NULL;
    for (int d = 0; d < 2; d++) {
        if (d == 0 && !vm->moddir) continue;
        for (int e = 0; ext[e]; e++) {
            char path[1024];
            if (d == 0) snprintf(path, sizeof path, "%s/%s%s", vm->moddir, nm, ext[e]);
            else snprintf(path, sizeof path, "%s%s", nm, ext[e]);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            size_t cap = 4096, len = 0; char *b = (char *)malloc(cap);
            for (;;) {
                size_t k = fread(b + len, 1, cap - len, f); len += k;
                if (len < cap) break;
                cap *= 2; b = (char *)realloc(b, cap);
            }
            fclose(f);
            Mod *m = mod_add(vm, nm, b, len); free(b);
            return m;
        }
    }
    return NULL;
}
static Value nat_from(VM *vm, Value *a, int n) {
    esc_all(vm);
    if (vm->wid) rt(vm, vm->curline, "from() cannot be used inside ::par");
    if (n != 1 || a[0].t != T_STR) rt(vm, 0, "from(name) requires a string module name");
    char nm[256]; snprintf(nm, sizeof nm, "%s", a[0].u.s->s);
    size_t L = strlen(nm);
    if (L > 6 && !strcmp(nm + L - 6, ".prism")) nm[L - 6] = 0;
    else if (L > 3 && !strcmp(nm + L - 3, ".pr")) nm[L - 3] = 0;
    Mod *m = mod_find(vm, nm);
    if (!m) m = mod_from_file(vm, nm);
    if (!m) {
        SB sb; sb_init(vm, &sb);
        for (int i = 0; i < vm->nmods; i++) { if (i) sb_cs(&sb, ", "); sb_cs(&sb, vm->mods[i].name->s); }
        rt(vm, vm->curline, "module \"%s\" not found (available: %s)", nm, sb.b ? sb.b : "");
    }
    if (m->state == 2) return V_obj(m->exp);
    if (m->state == 1) rt(vm, vm->curline, "circular import of module \"%s\"", nm);
    m->state = 1;
    Env *env = env_new(vm, vm->globals, 0);
    int save_hw = vm->prog_hw;
    jmp_buf jb, *saved = vm->jbp;
    vm->jbp = &jb;
    if (setjmp(jb)) {
        vm->jbp = saved;
        m->state = 0;
        char tmp[sizeof vm->err]; const char *e = vm->err; const char *at = strstr(e, " (line ");
        if (!at) at = strchr(e, ':');
        if (at && !strstr(e, " in module \"")) {
            int pre = (int)(at - e);
            snprintf(tmp, sizeof tmp, "%.*s in module \"%s\"%s", pre, e, nm, at);
            memcpy(vm->err, tmp, sizeof vm->err);
        }
        longjmp(*vm->jbp, 1);
    }
    int pn = 0;
    Node **prog = parse_program(vm, m->src, (int)strlen(m->src), &pn);
    vm->prog_hw = save_hw;
    exec_block(vm, prog, pn, env);
    if (vm->ctl == 3) vm->ctl = 0;
    vm->jbp = saved;
    Obj *o = obj_new(vm);
    for (int i = 0; i < env->n; i++) if (env->vars[i].name->s[0] != '_') obj_set(vm, o, env->vars[i].name, env->vars[i].v);
    m->exp = o; m->state = 2;
    return V_obj(o);
}

static int reg_nat(VM *vm, const char *name, NatFn fn) {
    int id = NNATS;
    for (int i = 0; i < NNATS; i++) if (NATNAMES[i] && !strcmp(NATNAMES[i], name) && NATS[i] == fn) return i;
    if (NNATS >= MAX_NATS) return 0;
    NATS[NNATS] = fn; NATNAMES[NNATS] = name; NNATS++;
    (void)vm;
    return id;
}
static Value nat_dec(VM *vm, Value *a, int n) {   /* dec(x) / dec(x, digits) */
    if (n < 1 || n > 2) rt(vm, 0, "dec(x) or dec(x, digits) takes one or two arguments");
    Value d;
    if (a[0].t == T_STR) {
        d = dec_parse_str(vm, a[0].u.s->s, a[0].u.s->len);
        if (d.t != T_DEC) { char m[300]; snprintf(m, sizeof m, "\"%.200s\" is not a valid number", a[0].u.s->s); return V_err(vm, m); }
    } else if (is_num(a[0]) || a[0].t == T_BOOL) d = to_dec(vm, a[0]);
    else rt(vm, 0, "dec(): cannot convert %s", type_of(a[0]));
    if (d.t != T_DEC) return d;
    if (n == 2) {
        if (a[1].t != T_INT || a[1].u.i < 0 || a[1].u.i > 1000) rt(vm, 0, "dec(x, digits): digits must be an int in 0..1000");
        d = dec_round_to(vm, d, (int)a[1].u.i);
    }
    return d;
}
static Value nat_precision(VM *vm, Value *a, int n) {   /* precision(n) — знаков после запятой у всех дробных; 0 — double; precision() — текущее */
    if (n == 0) return V_int(vm->prec);
    if (n != 1 || a[0].t != T_INT || a[0].u.i < 0 || a[0].u.i > 1000) rt(vm, 0, "precision(n): n must be an int in 0..1000");
    vm->prec = (int)a[0].u.i; return V_none();
}
static void setup_globals(VM *vm) {
    Env *g = vm->globals;
    static const struct { const char *n; NatFn f; } T[] = {
        {"print", nat_print}, {"range", nat_range}, {"len", nat_len}, {"str", nat_str}, {"type", nat_type}, {"error", nat_error},
        {"abs", nat_abs}, {"sqrt", nat_sqrt}, {"floor", nat_floor}, {"ceil", nat_ceil}, {"round", nat_round2},
        {"min", nat_min}, {"max", nat_max}, {"div", nat_div}, {"int", nat_int}, {"float", nat_float}, {"dec", nat_dec}, {"precision", nat_precision}, {"random", nat_random},
        {"sin", nat_sin}, {"cos", nat_cos}, {"tan", nat_tan}, {"atan", nat_atan}, {"exp", nat_exp}, {"log2", nat_log2}, {"log10", nat_log10}, {"log", nat_log},
        {"atan2", nat_atan2}, {"pow", nat_pow}, {"gcd", nat_gcd}, {"divmod", nat_divmod}, {"sum", nat_sum}, {"from", nat_from}, {"ord", nat_ord}, {"chr", nat_chr}, {NULL, NULL}};
    for (int i = 0; T[i].n; i++) env_def(vm, g, S(vm, T[i].n), V_nat(reg_nat(vm, T[i].n, T[i].f)), 0);
    Obj *rq = obj_new(vm);
    obj_set(vm, rq, S(vm, "line"), V_nat(reg_nat(vm, "line", nat_req_line)));
    obj_set(vm, rq, S(vm, "lines"), V_nat(reg_nat(vm, "lines", nat_req_lines)));
    obj_set(vm, rq, S(vm, "words"), V_nat(reg_nat(vm, "words", nat_req_words)));
    obj_set(vm, rq, S(vm, "ints"), V_nat(reg_nat(vm, "ints", nat_req_ints)));
    env_def(vm, g, S(vm, "request"), V_obj(rq), 0);
    env_def(vm, g, S(vm, "pi"), pi_value(), 0);
    setup_time(vm);
    setup_io(vm);
}

/* ---------------------------------------------------------------- вызов функций-значений из методов */
static Value call1(VM *vm, Value f, Value x, int line) { Value a[1]; a[0] = x; return call_fn(vm, f, a, 1, 0, line, NULL); }
static int pred(VM *vm, Value f, Value x, int line) {
    Value r = call1(vm, f, x, line);
    if (r.t != T_BOOL) {
        SB sb; sb_init(vm, &sb); repr_to(&sb, r, 0);
        rt(vm, line, "predicate must return bool, got %s %s", type_of(r), sb.b ? sb.b : "");
    }
    return r.u.b;
}
static Value nat_sum(VM *vm, Value *a, int n) {
    if (n != 1 || (a[0].t != T_ARR && a[0].t != T_STREAM)) rt(vm, 0, "sum(xs) requires an array or stream, got %s", n == 1 ? type_of(a[0]) : "wrong argument count");
    Value s = V_int(0), x; Iter *it = iter_new(vm, a[0], 0);
    while (iter_next(vm, it, &x)) s = add_values(vm, s, x, 0);
    return s;
}
static int cmp_key(VM *vm, Value key, int haskey, Value a, Value b, int line) {
    Value x = haskey ? call1(vm, key, a, line) : a, y = haskey ? call1(vm, key, b, line) : b;
    int c = order_vals(vm, x, y);
    if (c != 2) return c;
    rt(vm, line, "cannot compare %s and %s while sorting", type_of(x), type_of(y));
    return 0;
}
static Value sort_arr(VM *vm, Arr *src, Value key, int haskey, int line) {
    int n = src->n;
    Value *a = (Value *)amalloc(vm, sizeof(Value) * (size_t)(n + 1)), *b = (Value *)amalloc(vm, sizeof(Value) * (size_t)(n + 1));
    memcpy(a, src->v, sizeof(Value) * (size_t)n);
    for (int w = 1; w < n; w *= 2) {
        for (int i = 0; i < n; i += 2 * w) {
            int m = i + w < n ? i + w : n, e = i + 2 * w < n ? i + 2 * w : n, p = i, q = m, k = i;
            while (p < m && q < e) b[k++] = cmp_key(vm, key, haskey, a[q], a[p], line) < 0 ? a[q++] : a[p++];
            while (p < m) b[k++] = a[p++];
            while (q < e) b[k++] = a[q++];
        }
        Value *t = a; a = b; b = t;
    }
    Arr *r = arr_new(vm, n);
    for (int i = 0; i < n; i++) arr_push(vm, r, a[i]);
    return V_arr(r);
}

/* ---------------------------------------------------------------- потоки (ленивые, поэлементные) */
struct Stream { int kind, line, stateful, arrchain, hasE; Value up, f, other; int64_t k, s, e, st; };
/* вызов лямбды с одним параметром и выражением в теле; окружение переиспользуется между элементами */
static Value call1r(VM *vm, Iter *it, Value x) {
    if (!it->fk) {   /* один раз на итератор: подходит ли лямбда под быстрый путь */
        it->fk = 2;
        Value fv = it->f;
        if (fv.t == T_FN) {
            Fn *f = fv.u.fn;
            if (!f->multi && !f->target && !f->cl[0].async) {
                Clause *c = &f->cl[0];
                if (!c->simple) clause_classify(c);
                if (c->simple == 1 && c->np == 1 && !c->isBlock) { it->fc = c; it->fenv = f->env; it->fk = 1; }
            }
        }
    }
    if (it->fk == 1) {
        Clause *c = it->fc;
        Env *env = it->renv; it->renv = NULL;
        if (!env || env->captured || env->parent != it->fenv) env = env_new(vm, it->fenv, 0);
        else { for (int q = env->vars != env->inl ? 0 : 1; q < env->n; q++) env->vars[q].name->ep++; env->n = 1; if (env->vars != env->inl) { env->vars = env->inl; env->cap = 6; } }
        Var *v0 = &env->inl[0]; v0->name = c->params[0]->name; v0->v = x; v0->mut = 1; v0->param = 1; v0->frozen = 0;
        env->n = 1;
        if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
        vm->inl = 0;
        Value r = ev(vm, c->expr, env);
        vm->depth--;
        it->renv = env;
        return r;
    }
    return call1(vm, it->f, x, it->line);
}
static int predr(VM *vm, Iter *it, Value x) {
    int mk = mk_push(vm);
    Value r = call1r(vm, it, x);
    if (mk) mk_pop(vm);   /* результат — bool, мусор предиката освобождается */
    if (vm->ctl) return 0;   /* next/stop внутри предиката: обрабатывает вызывающий */
    if (r.t != T_BOOL) {
        SB sb; sb_init(vm, &sb); repr_to(&sb, r, 0);
        rt(vm, it->line, "predicate must return bool, got %s %s", type_of(r), sb.b ? sb.b : "");
    }
    return r.u.b;
}


static Value mk_stream(VM *vm, int kind, Value up, Value f, int line, int stateful, int arrchain) {
    Stream *s = (Stream *)amalloc(vm, sizeof(Stream));
    s->kind = kind; s->up = up; s->f = f; s->line = line; s->stateful = stateful; s->arrchain = arrchain; s->hasE = 1;
    s->other = V_none();
    Value v = V_none(); v.t = T_STREAM; v.u.sm = s; return v;
}
static Value mk_range(VM *vm, int64_t s, int64_t e, int64_t st, int hasE) {
    Value v = mk_stream(vm, SK_RANGE, V_none(), V_none(), 0, 0, 0);
    v.u.sm->s = s; v.u.sm->e = e; v.u.sm->st = st; v.u.sm->hasE = hasE; return v;
}
static int stream_stateful(Value v) { return v.t == T_STREAM && v.u.sm->stateful; }
static int stream_arrchain(Value v) { return v.t == T_STREAM && v.u.sm->arrchain; }

static Arr *stream_to_arr(VM *vm, Value v);
static Iter *iter_new(VM *vm, Value v, int line) {
    Iter *it = (Iter *)amalloc(vm, sizeof(Iter));
    it->line = line;
    if (v.t == T_STR) { Value t; iter_of(vm, v, &t, line); v = t; }
    if (v.t == T_ARR) { it->kind = SK_ARR; it->a = v.u.a; return it; }
    if (v.t != T_STREAM) rt(vm, line, "cannot iterate over %s", type_of(v));
    Stream *d = v.u.sm;
    it->kind = d->kind; it->f = d->f; it->k = d->k; it->hasE = d->hasE; it->line = d->line ? d->line : line;
    switch (d->kind) {
        case SK_RANGE: it->i = d->s; it->e = d->e; it->st = d->st; break;
        case SK_LINES: case SK_WORDS: case SK_INTS: break;
        case SK_FROM: it->x = d->other; it->flag = 0; break;
        case SK_ZIP: it->up = iter_new(vm, d->up, line); it->o = iter_new(vm, d->other, line); break;
        case SK_EXPAND: case SK_EXPANDB: it->up = iter_new(vm, d->up, line); it->x = d->other; it->st = d->st; if (d->st) it->vis = obj_new(vm); break;
        case SK_SCAN: it->up = iter_new(vm, d->up, line); it->x = d->other; it->flag = (int)d->k; break;
        case SK_CYCLE: case SK_REPEAT: it->a = stream_to_arr(vm, d->up); it->i = 0; it->e = 0; break;
        default: it->up = iter_new(vm, d->up, line);
    }
    if (d->kind == SK_UNIQUE || d->kind == SK_WINDOW) it->seen = arr_new(vm, 4);
    return it;
}
static Arr *stream_to_arr(VM *vm, Value v) {
    if (v.t == T_ARR) return v.u.a;
    Arr *r = arr_new(vm, 8);
    Iter *it = iter_new(vm, v, 0); Value x;
    while (iter_next(vm, it, &x)) arr_push(vm, r, x);
    return r;
}
static Value stream_materialize(VM *vm, Value v) { return V_arr(stream_to_arr(vm, v)); }

static int range_contains(Value st, Value a, int *known) {
    Stream *d = st.u.sm;
    if (d->kind != SK_RANGE) return 0;
    *known = 1;
    if (a.t != T_INT) return 0;
    int64_t x = a.u.i;
    if (d->st > 0) return x >= d->s && (!d->hasE || x < d->e) && (x - d->s) % d->st == 0;
    return x <= d->s && (!d->hasE || x > d->e) && (d->s - x) % (-d->st) == 0;
}

static int fresh_once(VM *vm, Iter *it, Value x) {
    if (!it->st) return 1;
    SB sb; sb_init(vm, &sb); repr_to(&sb, x, 0);
    Str *k = intern(vm, sb.b ? sb.b : "", sb.n);
    if (obj_find(it->vis, k) >= 0) return 0;
    obj_set(vm, it->vis, k, V_bool(1)); return 1;
}
static int iter_next(VM *vm, Iter *it, Value *out);
__attribute__((noinline)) static int iter_next_slow(VM *vm, Iter *it, Value *out) {
    Value x;
    switch (it->kind) {
        case SK_ARR: if (it->i < it->a->n) { *out = it->a->v[it->i++]; return 1; } return 0;
        case SK_RANGE:
            if (it->done) return 0;
            if (it->st > 0 ? (it->hasE && it->i >= it->e) : (it->hasE && it->i <= it->e)) return 0;
            *out = V_int(it->i);
            if (__builtin_add_overflow(it->i, it->st, &it->i)) it->done = 1;
            return 1;
        case SK_LINES: { Str *s; if (!read_line(vm, &s)) return 0; *out = V_str(s); return 1; }
        case SK_WORDS: case SK_INTS:
            for (;;) {
                if (it->a && it->i < it->a->n) {
                    Str *w = it->a->v[it->i++].u.s;
                    if (it->kind == SK_WORDS) { *out = V_str(w); return 1; }
                    Value r = parse_int_str(vm, w);
                    if (r.t == T_ERR) rt(vm, it->line, "request.ints(): invalid integer \"%.200s\"", w->s);
                    *out = r; return 1;
                }
                Str *ln; if (!read_line(vm, &ln)) return 0;
                it->a = arr_new(vm, 8); it->i = 0;
                int p = 0;
                while (p < ln->len) {
                    while (p < ln->len && isspace((unsigned char)ln->s[p])) p++;
                    int q = p; while (q < ln->len && !isspace((unsigned char)ln->s[q])) q++;
                    if (q > p) arr_push(vm, it->a, V_str(mkstr(vm, ln->s + p, q - p)));
                    p = q;
                }
            }
        case SK_EXPAND: {
            if (!it->fr) { it->cfr = 16; it->fr = (Iter **)amalloc(vm, sizeof(Iter *) * 16); it->nd = (Value *)amalloc(vm, sizeof(Value) * 16); it->fr[0] = it->up; it->nd[0] = V_none(); it->nfr = 1; }
            while (it->nfr) {
                Iter *top = it->fr[it->nfr - 1];
                if (iter_next(vm, top, &x)) {
                    if (!fresh_once(vm, it, x)) continue;
                    Value kids = call1(vm, it->f, x, it->line);
                    if (kids.t == T_NONE) kids = V_arr(arr_new(vm, 1));
                    if (kids.t != T_ARR && kids.t != T_STREAM) {
                        SB sb; sb_init(vm, &sb); repr_to(&sb, kids, 0);
                        rt(vm, it->line, ".expand: children function must return array, stream or none, got %s %s", type_of(kids), sb.b ? sb.b : "");
                    }
                    if (it->nfr == it->cfr) {
                        Iter **nf = (Iter **)amalloc(vm, sizeof(Iter *) * (size_t)it->cfr * 2); Value *nn = (Value *)amalloc(vm, sizeof(Value) * (size_t)it->cfr * 2);
                        memcpy(nf, it->fr, sizeof(Iter *) * (size_t)it->nfr); memcpy(nn, it->nd, sizeof(Value) * (size_t)it->nfr);
                        it->fr = nf; it->nd = nn; it->cfr *= 2;
                    }
                    it->fr[it->nfr] = iter_new(vm, kids, it->line); it->nd[it->nfr] = x; it->nfr++;
                    *out = x; return 1;
                }
                int root = it->nfr == 1; Value nodev = it->nd[it->nfr - 1]; it->nfr--;
                if (!root && it->k) { *out = call1(vm, it->x, nodev, it->line); return 1; }
            }
            return 0;
        }
        case SK_EXPANDB: {
            if (!it->a) { it->a = arr_new(vm, 8); while (iter_next(vm, it->up, &x)) if (fresh_once(vm, it, x)) arr_push(vm, it->a, x); it->i = 0; }
            if (it->i >= it->a->n) return 0;
            Value cur = it->a->v[it->i++];
            Value kids = call1(vm, it->f, cur, it->line);
            if (kids.t != T_NONE) {
                if (kids.t != T_ARR && kids.t != T_STREAM) {
                    SB sb; sb_init(vm, &sb); repr_to(&sb, kids, 0);
                    rt(vm, it->line, ".expand_bfs: children function must return array, stream or none, got %s %s", type_of(kids), sb.b ? sb.b : "");
                }
                Iter *ki = iter_new(vm, kids, it->line); Value y; while (iter_next(vm, ki, &y)) if (fresh_once(vm, it, y)) arr_push(vm, it->a, y);
            }
            *out = cur; return 1;
        }
        case SK_SCAN:
            if (!iter_next(vm, it->up, &x)) return 0;
            if (!it->flag) { it->x = x; it->flag = 1; }
            else { Value args[2]; args[0] = it->x; args[1] = x; it->x = call_fn(vm, it->f, args, 2, 0, it->line, NULL); }
            *out = it->x; return 1;
        case SK_FLATMAP:
            for (;;) {
                if (it->o) { Value y; if (iter_next(vm, it->o, &y)) { *out = y; return 1; } it->o = NULL; }
                if (!iter_next(vm, it->up, &x)) return 0;
                Value r = call1(vm, it->f, x, it->line);
                if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) { it->kind = SK_ARR; it->a = arr_new(vm, 1); it->i = 0; return 0; } continue; }
                if (r.t == T_ARR || r.t == T_STREAM) it->o = iter_new(vm, r, it->line); else { *out = r; return 1; }
            }
        case SK_CYCLE: case SK_REPEAT:
            if (!it->a->n) return 0;
            if (it->i >= it->a->n) { it->i = 0; it->e++; }
            if (it->kind == SK_REPEAT && it->e >= it->k) return 0;
            *out = it->a->v[it->i++]; return 1;
        case SK_FROM:
            if (!it->flag) it->flag = 1; else it->x = call1(vm, it->f, it->x, it->line);
            *out = it->x; return 1;
        case SK_MAP:
            for (;;) {
                if (!iter_next(vm, it->up, &x)) return 0;
                Value r = call1r(vm, it, x);
                if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) { it->kind = SK_ARR; it->a = arr_new(vm, 1); it->i = 0; return 0; } continue; }
                *out = r; return 1;
            }
        case SK_FILTER:
            while (iter_next(vm, it->up, &x)) {
                int ok = predr(vm, it, x);
                if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) { it->kind = SK_ARR; it->a = arr_new(vm, 1); it->i = 0; return 0; } continue; }
                if (ok) { *out = x; return 1; }
            }
            return 0;
        case SK_TAKE: if (it->k <= 0 || it->i >= it->k) return 0; if (!iter_next(vm, it->up, &x)) return 0; it->i++; *out = x; return 1;
        case SK_SKIP: while (iter_next(vm, it->up, &x)) { if (it->i++ >= it->k) { *out = x; return 1; } } return 0;
        case SK_TAKEW:
            if (it->done) return 0;
            if (!iter_next(vm, it->up, &x)) return 0;
            if (!pred(vm, it->f, x, it->line)) { it->done = 1; return 0; }
            *out = x; return 1;
        case SK_SKIPW:
            while (iter_next(vm, it->up, &x)) { if (!it->flag && !pred(vm, it->f, x, it->line)) it->flag = 1; if (it->flag) { *out = x; return 1; } }
            return 0;
        case SK_ENUM: { if (!iter_next(vm, it->up, &x)) return 0; Arr *p = arr_new(vm, 2); arr_push(vm, p, V_int(it->i++)); arr_push(vm, p, x); *out = V_arr(p); return 1; }
        case SK_FLAT:
            for (;;) {
                if (it->a) { if (it->i < it->a->n) { *out = it->a->v[it->i++]; return 1; } it->a = NULL; }
                if (!iter_next(vm, it->up, &x)) return 0;
                if (x.t == T_ARR) { it->a = x.u.a; it->i = 0; continue; }
                *out = x; return 1;
            }
        case SK_ZIP: {
            Value p, q;
            if (!iter_next(vm, it->up, &p)) return 0;
            if (!iter_next(vm, it->o, &q)) return 0;
            Arr *r = arr_new(vm, 2); arr_push(vm, r, p); arr_push(vm, r, q); *out = V_arr(r); return 1;
        }
        case SK_PAR: return iter_next(vm, it->up, out);
        case SK_UNIQUE:
            while (iter_next(vm, it->up, &x)) {
                int seen = 0; for (int j = 0; j < it->seen->n && !seen; j++) if (val_eq(it->seen->v[j], x)) seen = 1;
                if (!seen) { arr_push(vm, it->seen, x); *out = x; return 1; }
            }
            return 0;
        case SK_CHUNK: {
            Arr *cur = arr_new(vm, (int)it->k);
            while (iter_next(vm, it->up, &x)) { arr_push(vm, cur, x); if (cur->n == it->k) { *out = V_arr(cur); return 1; } }
            if (cur->n) { *out = V_arr(cur); return 1; }
            return 0;
        }
        case SK_WINDOW:
            while (iter_next(vm, it->up, &x)) {
                arr_push(vm, it->seen, x);
                if (it->seen->n > it->k) { memmove(it->seen->v, it->seen->v + 1, sizeof(Value) * (size_t)(it->seen->n - 1)); it->seen->n--; }
                if (it->seen->n == it->k) { *out = shallow_copy(vm, V_arr(it->seen)); return 1; }
            }
            return 0;
    }
    return 0;
}
/* самые частые виды потоков — в небольшой функции без тяжёлого пролога */
static int iter_next(VM *vm, Iter *it, Value *out) {
    Value x;
    switch (it->kind) {
        case SK_ARR: if (it->i < it->a->n) { *out = it->a->v[it->i++]; return 1; } return 0;
        case SK_RANGE:
            if (it->done) return 0;
            if (it->st > 0 ? (it->hasE && it->i >= it->e) : (it->hasE && it->i <= it->e)) return 0;
            *out = V_int(it->i);
            if (__builtin_add_overflow(it->i, it->st, &it->i)) it->done = 1;
            return 1;
        case SK_MAP:
            for (;;) {
                if (!iter_next(vm, it->up, &x)) return 0;
                Value r = call1r(vm, it, x);
                if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) { it->kind = SK_ARR; it->a = arr_new(vm, 1); it->i = 0; return 0; } continue; }
                *out = r; return 1;
            }
        case SK_FILTER:
            while (iter_next(vm, it->up, &x)) {
                int ok = predr(vm, it, x);
                if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) { it->kind = SK_ARR; it->a = arr_new(vm, 1); it->i = 0; return 0; } continue; }
                if (ok) { *out = x; return 1; }
            }
            return 0;
    }
    return iter_next_slow(vm, it, out);
}

static int64_t cnt_arg(VM *vm, Value *a, int n, const char *what, int line) {
    if (n < 1 || a[0].t != T_INT) rt(vm, line, "%s(n): n must be int, got %s", what, type_of(n ? a[0] : V_none()));
    return a[0].u.i;
}
static void need_args(VM *vm, int n, int want, const char *name, int line) {
    if (n < want) rt(vm, line, ".%s: missing arguments", name);
}
static Value chain(VM *vm, Value src, int kind, Value f, int64_t k, int line) {
    int isArr = src.t == T_ARR;
    int st = src.t == T_STREAM && src.u.sm->stateful;
    int ac = isArr || (src.t == T_STREAM && src.u.sm->arrchain);
    Value v = mk_stream(vm, kind, src, f, line, st, ac);
    v.u.sm->k = k; return v;
}

/* методы, общие для массивов и потоков; found=0 — такого метода нет */
static Value iter_method(VM *vm, Value src, Str *nm, Value *a, int n, int line, int *found) {
    const char *m = nm->s; *found = 1;
    Value x;
    if (!strcmp(m, "map")) { need_args(vm, n, 1, m, line); return chain(vm, src, SK_MAP, a[0], 0, line); }
    if (!strcmp(m, "filter")) { need_args(vm, n, 1, m, line); return chain(vm, src, SK_FILTER, a[0], 0, line); }
    if (!strcmp(m, "expand") || !strcmp(m, "expand_bfs") || !strcmp(m, "expand_once") || !strcmp(m, "expand_bfs_once")) {
        int bfs = !strncmp(m, "expand_bfs", 10), once = !strcmp(m + strlen(m) - 5, "_once");
        if (n < 1 || n > 2) rt(vm, line, ".%s: expected a function%s", m, bfs ? "" : " (and an optional exit function)");
        if (bfs && n == 2) rt(vm, line, ".%s takes one function", m);
        Value v = chain(vm, src, bfs ? SK_EXPANDB : SK_EXPAND, a[0], n == 2, line);
        v.u.sm->st = once;
        if (n == 2) v.u.sm->other = a[1];
        return v;
    }
    if (!strcmp(m, "scan")) {
        if (n < 1 || n > 2) rt(vm, line, ".scan(init, f) or .scan(f)");
        Value v = chain(vm, src, SK_SCAN, a[n - 1], n == 2, line);
        if (n == 2) v.u.sm->other = a[0];
        return v;
    }
    if (!strcmp(m, "flat_map")) { need_args(vm, n, 1, m, line); return chain(vm, src, SK_FLATMAP, a[0], 0, line); }
    if (!strcmp(m, "cycle")) return chain(vm, src, SK_CYCLE, V_none(), 0, line);
    if (!strcmp(m, "repeat")) return chain(vm, src, SK_REPEAT, V_none(), cnt_arg(vm, a, n, "repeat", line), line);
    if (!strcmp(m, "take")) return chain(vm, src, SK_TAKE, V_none(), cnt_arg(vm, a, n, "take", line), line);
    if (!strcmp(m, "skip")) return chain(vm, src, SK_SKIP, V_none(), cnt_arg(vm, a, n, "skip", line), line);
    if (!strcmp(m, "take_while")) { need_args(vm, n, 1, m, line); return chain(vm, src, SK_TAKEW, a[0], 0, line); }
    if (!strcmp(m, "skip_while")) { need_args(vm, n, 1, m, line); return chain(vm, src, SK_SKIPW, a[0], 0, line); }
    if (!strcmp(m, "enumerate")) return chain(vm, src, SK_ENUM, V_none(), 0, line);
    if (!strcmp(m, "flat")) return chain(vm, src, SK_FLAT, V_none(), 0, line);
    if (!strcmp(m, "unique")) return chain(vm, src, SK_UNIQUE, V_none(), 0, line);
    if (!strcmp(m, "zip")) { need_args(vm, n, 1, m, line); Value r = chain(vm, src, SK_ZIP, V_none(), 0, line); r.u.sm->other = a[0]; return r; }
    if (!strcmp(m, "chunk")) { int64_t k = cnt_arg(vm, a, n, "chunk", line); if (k < 1) rt(vm, line, "chunk(n): n must be >= 1"); return chain(vm, src, SK_CHUNK, V_none(), k, line); }
    if (!strcmp(m, "window")) { int64_t k = cnt_arg(vm, a, n, "window", line); if (k < 1) rt(vm, line, "window(n): n must be >= 1"); return chain(vm, src, SK_WINDOW, V_none(), k, line); }
    if (!strcmp(m, "par")) {
        if (src.t == T_STREAM && src.u.sm->stateful) rt(vm, line, ".par() cannot be used on a stateful stream");
        return chain(vm, src, SK_PAR, V_none(), 0, line); /* выполняется последовательно */
    }
    Iter *it = NULL;
#define ITER() (it = iter_new(vm, src, line))
    if (!strcmp(m, "find")) { need_args(vm, n, 1, m, line); ITER(); while (iter_next(vm, it, &x)) if (pred(vm, a[0], x, line)) return x; return V_none(); }
    if (!strcmp(m, "any")) { need_args(vm, n, 1, m, line); ITER(); while (iter_next(vm, it, &x)) if (pred(vm, a[0], x, line)) return V_bool(1); return V_bool(0); }
    if (!strcmp(m, "all")) { need_args(vm, n, 1, m, line); ITER(); while (iter_next(vm, it, &x)) if (!pred(vm, a[0], x, line)) return V_bool(0); return V_bool(1); }
    if (!strcmp(m, "partition")) {
        need_args(vm, n, 1, m, line);
        Arr *y = arr_new(vm, 8), *no = arr_new(vm, 8); ITER();
        while (iter_next(vm, it, &x)) arr_push(vm, pred(vm, a[0], x, line) ? y : no, x);
        Arr *r = arr_new(vm, 2); arr_push(vm, r, V_arr(y)); arr_push(vm, r, V_arr(no)); return V_arr(r);
    }
    if (!strcmp(m, "min_by") || !strcmp(m, "max_by")) {
        need_args(vm, n, 1, m, line);
        int mn = m[1] == 'i'; Value best = V_none(), bk = V_none(); int first = 1; ITER();
        while (iter_next(vm, it, &x)) {
            Value k = call1(vm, a[0], x, line);
            if (first) { best = x; bk = k; first = 0; continue; }
            int c = order_vals(vm, k, bk);
            if (c == 2) rt(vm, line, "cannot compare %s and %s in %s", type_of(k), type_of(bk), m);
            if (mn ? c < 0 : c > 0) { best = x; bk = k; }
        }
        return first ? V_none() : best;
    }
    if (!strcmp(m, "count")) { int64_t c = 0; ITER(); while (iter_next(vm, it, &x)) if (!n || pred(vm, a[0], x, line)) c++; return V_int(c); }
    if (!strcmp(m, "first")) { ITER(); return iter_next(vm, it, &x) ? x : V_none(); }
    if (!strcmp(m, "to_list")) { Arr *r = arr_new(vm, 8); ITER(); while (iter_next(vm, it, &x)) arr_push(vm, r, x); return V_arr(r); }
    if (!strcmp(m, "sum")) { Value s = V_int(0); ITER(); while (iter_next(vm, it, &x)) s = add_values(vm, s, x, line); return s; }
    if (!strcmp(m, "avg")) {
        Value s = V_int(0); int64_t c = 0; ITER();
        while (iter_next(vm, it, &x)) { s = add_values(vm, s, x, line); c++; }
        if (c && (s.t == T_DEC || vm->prec > 0)) return dec_arith(vm, OP_DIV, s, V_int(c), line);
        return c ? V_flt(as_f(want_num(vm, s, "avg", line)) / (double)c) : V_none();
    }
    if (!strcmp(m, "reduce")) {
        need_args(vm, n, 1, m, line);
        Value acc = V_none(), f; int started = 0;
        if (n >= 2) { acc = a[0]; f = a[1]; started = 1; } else f = a[0];
        ITER();
        while (iter_next(vm, it, &x)) {
            if (!started) { acc = x; started = 1; }
            else { Value args[2]; args[0] = acc; args[1] = x; acc = call_fn(vm, f, args, 2, 0, line, NULL); }
        }
        return started ? acc : V_none();
    }
    if (!strcmp(m, "sort")) return sort_arr(vm, stream_to_arr(vm, src), n ? a[0] : V_none(), n > 0, line);
    if (!strcmp(m, "group_by")) {
        need_args(vm, n, 1, m, line); Obj *o = obj_new(vm); ITER();
        while (iter_next(vm, it, &x)) {
            Str *k = key_str(vm, call1(vm, a[0], x, line)); k = intern(vm, k->s, k->len);
            int j = obj_find(o, k);
            if (j < 0) { obj_set(vm, o, k, V_arr(arr_new(vm, 4))); j = obj_find(o, k); }
            arr_push(vm, o->v[j].u.a, x);
        }
        return V_obj(o);
    }
#undef ITER
    *found = 0; return V_none();
}

/* методы, которым нужен готовый массив (поток сначала превращается в массив) */
static Value arr_only_method(VM *vm, Arr *arr, Str *nm, Value *a, int n, Node *callee, Env *env, int line, int *found, int fromStream) {
    const char *m = nm->s; *found = 1;
    int N = arr->n; Value *v = arr->v; Arr *r;
    if (!strcmp(m, "copy")) return shallow_copy(vm, V_arr(arr));
    if (!strcmp(m, "dcopy")) return deep_copy(vm, V_arr(arr));
    if (!strcmp(m, "len")) return V_int(N);
    if (!strcmp(m, "last")) return N ? v[N - 1] : V_none();
    if (!strcmp(m, "reverse")) { r = arr_new(vm, N); for (int i = N - 1; i >= 0; i--) arr_push(vm, r, v[i]); return V_arr(r); }
    if (!strcmp(m, "contains")) { need_args(vm, n, 1, m, line); for (int i = 0; i < N; i++) if (val_eq(v[i], a[0])) return V_bool(1); return V_bool(0); }
    if (!strcmp(m, "index_of")) { need_args(vm, n, 1, m, line); for (int i = 0; i < N; i++) if (val_eq(v[i], a[0])) return V_int(i); return V_none(); }
    if (!strcmp(m, "join")) {
        SB sb; sb_init(vm, &sb); Str *sep = n ? to_str(vm, a[0], 1) : vm->s_empty;
        for (int i = 0; i < N; i++) { if (i) sb_add(&sb, sep->s, sep->len); Str *s = to_str(vm, v[i], 1); sb_add(&sb, s->s, s->len); }
        return V_str(sb_str(&sb));
    }
    if (!strcmp(m, "min")) { Value x = V_arr(arr); return minmax(vm, &x, 1, -1, "min"); }
    if (!strcmp(m, "max")) { Value x = V_arr(arr); return minmax(vm, &x, 1, 1, "max"); }
    if (!strcmp(m, "pop")) {
        if (fromStream) rt(vm, line, ".pop requires an array stored in a variable");
        require_mutable(vm, callee ? callee->a : NULL, env, line);
        if (!N) rt(vm, line, ".pop: array is empty");
        if (n < 1) return v[--arr->n];
        if (a[0].t != T_INT) rt(vm, line, "index must be int");
        int64_t i = a[0].u.i < 0 ? N + a[0].u.i : a[0].u.i;
        if (i < 0 || i >= N) rt(vm, line, "index %lld out of range for array of length %d", (long long)a[0].u.i, N);
        Value x = v[i]; memmove(v + i, v + i + 1, sizeof(Value) * (size_t)(N - i - 1)); arr->n--; return x;
    }
    if (!strcmp(m, "remove") || !strcmp(m, "insert")) {
        if (fromStream) rt(vm, line, ".%s requires an array stored in a variable", m);
        require_mutable(vm, callee ? callee->a : NULL, env, line);
        need_args(vm, n, 1, m, line);
        if (a[0].t != T_INT) rt(vm, line, "index must be int");
        int64_t i = a[0].u.i < 0 ? N + a[0].u.i : a[0].u.i;
        if (!strcmp(m, "remove")) {
            if (i < 0 || i >= N) rt(vm, line, "index %lld out of range for array of length %d", (long long)a[0].u.i, N);
            Value x = v[i]; memmove(v + i, v + i + 1, sizeof(Value) * (size_t)(N - i - 1)); arr->n--; return x;
        }
        need_args(vm, n, 2, m, line);
        if (i < 0 || i > N) rt(vm, line, "index %lld out of range for array of length %d", (long long)a[0].u.i, N);
        arr_push(vm, arr, V_none());
        if (vm->nmk && is_ptr_val(a[1])) esc_container(vm, arr);
        memmove(arr->v + i + 1, arr->v + i, sizeof(Value) * (size_t)(N - i)); arr->v[i] = a[1];
        return V_none();
    }
    *found = 0; return V_none();
}

/* UTF-8: найти подстроку, вернуть смещение в байтах или -1 */
static int str_find(Str *s, Str *sub) {
    if (!sub->len) return 0;
    const char *p = mem_find(s->s, s->len, sub->s, sub->len);
    return p ? (int)(p - s->s) : -1;
}
static Value str_method(VM *vm, Str *s, Str *nm, Value *a, int n, Env *env, int line, int *found) {
    const char *m = nm->s; *found = 1;
    int L = u8_count(s->s, s->len);
    Arr *r;
    if (!strcmp(m, "len")) return V_int(L);
    if (!strcmp(m, "first")) return s->len ? str_slice(vm, s, 0, 1) : V_none();
    if (!strcmp(m, "last")) return s->len ? str_slice(vm, s, L - 1, L) : V_none();
    if (!strcmp(m, "skip") || !strcmp(m, "take")) {
        if (n < 1 || a[0].t != T_INT || a[0].u.i < 0) rt(vm, line, "%s(n): n must be int >= 0", m);
        int k = a[0].u.i > L ? L : (int)a[0].u.i;
        return m[0] == 's' ? str_slice(vm, s, k, L) : str_slice(vm, s, 0, k);
    }
    if (!strcmp(m, "remove") || !strcmp(m, "insert")) {   /* строки неизменяемы: возвращают новую строку */
        int ins = m[0] == 'i';
        if (n != (ins ? 2 : 1)) rt(vm, line, ".%s takes %d argument%s", m, ins ? 2 : 1, ins ? "s (index, text)" : " (index)");
        if (a[0].t != T_INT) rt(vm, line, "index must be int");
        int64_t i = a[0].u.i < 0 ? L + a[0].u.i : a[0].u.i;
        if (i < 0 || i > L || (!ins && i >= L)) rt(vm, line, "index %lld out of range for string of length %d", (long long)a[0].u.i, L);
        int o1 = u8_off(s->s, s->len, (int)i);
        int o2 = ins ? o1 : u8_off(s->s, s->len, (int)i + 1);
        Str *t = ins ? to_str(vm, a[1], 1) : NULL;
        int tl = t ? t->len : 0;
        char *buf = (char *)malloc((size_t)s->len + (size_t)tl + 1);
        memcpy(buf, s->s, (size_t)o1); if (tl) memcpy(buf + o1, t->s, (size_t)tl);
        memcpy(buf + o1 + tl, s->s + o2, (size_t)(s->len - o2));
        Str *res = mkstr(vm, buf, s->len - (o2 - o1) + tl); free(buf);
        return V_str(res);
    }
    if (!strcmp(m, "upper") || !strcmp(m, "lower")) {
        SB sb; sb_init(vm, &sb); int up = m[0] == 'u';
        for (int i = 0; i < s->len;) { int adv; uint32_t c = u8_dec(s->s + i, &adv); char b[4]; int k = u8_enc(case_map(c, up), b); sb_add(&sb, b, k); i += adv; }
        return V_str(sb_str(&sb));
    }
    if (!strcmp(m, "json")) return json_parse_str(vm, s);
    if (!strcmp(m, "trim")) return V_str(trim_str(vm, s));
    if (!strcmp(m, "chars")) { Value t; iter_of(vm, V_str(s), &t, line); return t; }
    if (!strcmp(m, "lines")) {
        r = arr_new(vm, 4); int st = 0;
        for (int i = 0; i <= s->len; i++) if (i == s->len || s->s[i] == '\n') {
            if (i == s->len && st == i) break;
            int e = i; if (e > st && s->s[e - 1] == '\r') e--;
            arr_push(vm, r, V_str(mkstr(vm, s->s + st, e - st))); st = i + 1;
        }
        return V_arr(r);
    }
    if (!strcmp(m, "split")) {
        r = arr_new(vm, 8);
        if (!n) {
            int i = 0;
            while (i < s->len) {
                while (i < s->len && isspace((unsigned char)s->s[i])) i++;
                int st = i; while (i < s->len && !isspace((unsigned char)s->s[i])) i++;
                if (i > st) arr_push(vm, r, V_str(mkstr(vm, s->s + st, i - st)));
            }
            return V_arr(r);
        }
        Str *sep = to_str(vm, a[0], 1);
        if (!sep->len) { Value t; iter_of(vm, V_str(s), &t, line); return t; }
        int st = 0;
        for (;;) {
            const char *p = mem_find(s->s + st, s->len - st, sep->s, sep->len);
            if (!p) { arr_push(vm, r, V_str(mkstr(vm, s->s + st, s->len - st))); break; }
            arr_push(vm, r, V_str(mkstr(vm, s->s + st, (int)(p - s->s) - st))); st = (int)(p - s->s) + sep->len;
        }
        return V_arr(r);
    }
    if (!strcmp(m, "contains")) { need_args(vm, n, 1, m, line); return V_bool(str_find(s, to_str(vm, a[0], 1)) >= 0); }
    if (!strcmp(m, "starts_with")) { need_args(vm, n, 1, m, line); Str *x = to_str(vm, a[0], 1); return V_bool(x->len <= s->len && !memcmp(s->s, x->s, (size_t)x->len)); }
    if (!strcmp(m, "ends_with")) { need_args(vm, n, 1, m, line); Str *x = to_str(vm, a[0], 1); return V_bool(x->len <= s->len && !memcmp(s->s + s->len - x->len, x->s, (size_t)x->len)); }
    if (!strcmp(m, "index_of")) { need_args(vm, n, 1, m, line); int o = str_find(s, to_str(vm, a[0], 1)); return o < 0 ? V_none() : V_int(u8_count(s->s, o)); }
    if (!strcmp(m, "replace")) {
        need_args(vm, n, 2, m, line);
        Str *x = to_str(vm, a[0], 1), *y = to_str(vm, a[1], 1);
        SB sb; sb_init(vm, &sb); int st = 0;
        if (!x->len) { /* как split("").join(y): между символами */
            int i = 0;
            while (i < s->len) { int k = u8_len((unsigned char)s->s[i]); if (i) sb_add(&sb, y->s, y->len); sb_add(&sb, s->s + i, k); i += k; }
            return V_str(sb_str(&sb));
        }
        for (;;) {
            const char *p = mem_find(s->s + st, s->len - st, x->s, x->len);
            if (!p) { sb_add(&sb, s->s + st, s->len - st); break; }
            sb_add(&sb, s->s + st, (int)(p - s->s) - st); sb_add(&sb, y->s, y->len); st = (int)(p - s->s) + x->len;
        }
        return V_str(sb_str(&sb));
    }
    if (!strcmp(m, "reverse")) {
        SB sb; sb_init(vm, &sb); int i = s->len;
        while (i > 0) { int j = i - 1; while (j > 0 && ((unsigned char)s->s[j] & 0xC0) == 0x80) j--; sb_add(&sb, s->s + j, i - j); i = j; }
        return V_str(sb_str(&sb));
    }
    if (!strcmp(m, "repeat")) {
        if (n < 1 || a[0].t != T_INT || a[0].u.i < 0) rt(vm, line, "repeat(n): n must be int >= 0");
        if (a[0].u.i * (int64_t)(s->len ? s->len : 1) > 50000000) rt(vm, line, "string too large");
        SB sb; sb_init(vm, &sb); for (int64_t i = 0; i < a[0].u.i; i++) sb_add(&sb, s->s, s->len); return V_str(sb_str(&sb));
    }
    if (!strcmp(m, "pad_left") || !strcmp(m, "pad_right")) {
        if (n < 1 || a[0].t != T_INT) rt(vm, line, "%s(n, c): n must be int", m);
        Str *c = n > 1 ? (a[1].t == T_STR ? a[1].u.s : NULL) : cstr(vm, " ");
        if (!c || u8_count(c->s, c->len) != 1) rt(vm, line, "%s(n, c): c must be a one-character string", m);
        int64_t k = a[0].u.i - u8_count(s->s, s->len); if (k <= 0) return V_str(s);
        SB sb; sb_init(vm, &sb);
        if (m[4] == 'l') { for (int64_t i = 0; i < k; i++) sb_add(&sb, c->s, c->len); sb_add(&sb, s->s, s->len); }
        else { sb_add(&sb, s->s, s->len); for (int64_t i = 0; i < k; i++) sb_add(&sb, c->s, c->len); }
        return V_str(sb_str(&sb));
    }
    if (!strcmp(m, "to_int")) return parse_int_str(vm, s);
    if (!strcmp(m, "to_float")) return parse_flt_str(vm, s);
    if (!strcmp(m, "to_number")) { Value x = parse_int_str(vm, s); return x.t == T_INT ? x : parse_flt_str(vm, s); }
    if (!strcmp(m, "interpret")) {
        Parser q; memset(&q, 0, sizeof q); q.vm = vm;
        Node *nd = parseIStr(&q, s, line);
        return ev(vm, nd, env);
    }
    *found = 0; return V_none();
}

static Value obj_method(VM *vm, Obj *o, Str *nm, Value *a, int n, int *found) {
    const char *m = nm->s; *found = 1;
    if (!strcmp(m, "keys")) { Arr *r = arr_new(vm, o->n); for (int i = 0; i < o->n; i++) arr_push(vm, r, V_str(o->k[i])); return V_arr(r); }
    if (!strcmp(m, "values")) { Arr *r = arr_new(vm, o->n); for (int i = 0; i < o->n; i++) arr_push(vm, r, o->v[i]); return V_arr(r); }
    if (!strcmp(m, "has")) { if (n < 1) return V_bool(0); return V_bool(obj_find(o, key_str(vm, a[0])) >= 0); }
    if (!strcmp(m, "copy")) return shallow_copy(vm, V_obj(o));
    if (!strcmp(m, "dcopy")) return deep_copy(vm, V_obj(o));
    if (!strcmp(m, "len")) return V_int(o->n);
    *found = 0; return V_none();
}

/* o.f(args) для пользовательской функции f из области видимости: то же, что f(o, args) */
static Value ufcs_call(VM *vm, Value recv, Str *name, Value *a, int n, Node *callee, Env *env, int line, int *found) {
    Var *b = env_find(env, name, NULL);
    if (!b || !(b->v.t == T_FN || b->v.t == T_NAT || b->v.t == T_TYPE)) { *found = 0; return V_none(); }
    *found = 1;
    int cnt = n + 1; Value *args = (Value *)amalloc(vm, sizeof(Value) * (size_t)cnt);
    args[0] = recv; for (int i = 0; i < n; i++) args[i + 1] = a[i];
    int *mp = NULL;
    if (b->v.t == T_FN) {
        mp = (int *)amalloc(vm, sizeof(int) * (size_t)cnt);
        for (int i = 0; i < cnt; i++) mp[i] = 1;
        mp[0] = callee && callee->a ? is_mut_path(vm, callee->a, env) : 1;
    }
    return call_fn(vm, b->v, args, cnt, 0, line, mp);
}

static const char *const ARR_METHODS[] = { "all", "any", "avg", "chunk", "count", "cycle", "enumerate", "expand", "expand_bfs", "expand_bfs_once", "expand_once", "filter", "find", "first", "flat", "flat_map", "group_by", "map", "max_by", "min_by", "par", "partition", "reduce", "repeat", "scan", "skip", "skip_while", "sort", "sum", "take", "take_while", "to_list", "unique", "window", "zip", "contains", "copy", "dcopy", "index_of", "insert", "join", "last", "len", "max", "min", "pop", "remove", "reverse", NULL };
static const char *const STR_METHODS[] = { "chars", "contains", "ends_with", "first", "index_of", "insert", "interpret", "json", "last", "len", "lines", "lower", "pad_left", "pad_right", "remove", "repeat", "replace", "reverse", "skip", "split", "starts_with", "take", "to_float", "to_int", "to_number", "trim", "upper", NULL };
static const char *const NUM_METHODS[] = { "to_string", "to_int", "to_float", NULL };
static void no_method(VM *vm, int line, const char *what, const char *name, const char *const *list, Env *env) {
    const char *sg = suggest_list(name, list);
    (void)env;
    if (sg) rt(vm, line, "%s has no method .%s. Did you mean: .%s?", what, name, sg);
    rt(vm, line, "%s has no method .%s", what, name);
}
static int is_method_name(const char *m) {
    for (int i = 0; ARR_METHODS[i]; i++) if (!strcmp(m, ARR_METHODS[i])) return 1;
    for (int i = 0; STR_METHODS[i]; i++) if (!strcmp(m, STR_METHODS[i])) return 1;
    for (int i = 0; NUM_METHODS[i]; i++) if (!strcmp(m, NUM_METHODS[i])) return 1;
    return 0;
}
static int is_arr_only_name(const char *m) {
    static const char *names[] = { "copy", "dcopy", "len", "last", "reverse", "contains", "index_of", "join", "min", "max", "pop", "remove", "insert", NULL };
    for (int i = 0; names[i]; i++) if (!strcmp(m, names[i])) return 1;
    return 0;
}
/* Вызов метода по имени. found=0 — такого метода нет (вызывающий пробует поле объекта). */
static Value call_method(VM *vm, Value recv, Str *name, Value *a, int n, Node *callee, Env *env, int line, int *found) {
    int f = 0; Value r;
    *found = 1;
    switch (recv.t) {
        case T_NONE: rt(vm, line, "cannot access \".%s\" of none", name->s); break;
        case T_ARR: case T_STREAM:
            r = iter_method(vm, recv, name, a, n, line, &f); if (f) return r;
            if (recv.t == T_STREAM && !is_arr_only_name(name->s)) { r = ufcs_call(vm, recv, name, a, n, callee, env, line, &f); if (f) return r; }   /* функция пользователя получает поток как есть (он может быть бесконечным) */
            r = arr_only_method(vm, stream_to_arr(vm, recv), name, a, n, callee, env, line, &f, recv.t == T_STREAM); if (f) return r;
            r = ufcs_call(vm, recv, name, a, n, callee, env, line, &f); if (f) return r;
            no_method(vm, line, "array", name->s, ARR_METHODS, env); break;
        case T_STR:
            r = str_method(vm, recv.u.s, name, a, n, env, line, &f); if (f) return r;
            r = ufcs_call(vm, recv, name, a, n, callee, env, line, &f); if (f) return r;
            no_method(vm, line, "string", name->s, STR_METHODS, env); break;
        case T_INT: case T_FLT: case T_DEC:
            if (!strcmp(name->s, "to_string")) { if (n > 0 && recv.t == T_INT) return to_str_base(vm, recv, a[0], line); return V_str(to_str(vm, recv, 1)); }
            if (!strcmp(name->s, "to_int")) return recv.t == T_INT ? recv : recv.t == T_DEC ? dec_to_int(vm, recv, 2) : to_int_checked(vm, trunc(recv.u.f));
            if (!strcmp(name->s, "to_float")) { if (vm->prec > 0) { Value d = to_dec(vm, recv); if (d.t == T_DEC) return d; } return V_flt(as_f(recv)); }
            r = ufcs_call(vm, recv, name, a, n, callee, env, line, &f); if (f) return r;
            no_method(vm, line, "number", name->s, NUM_METHODS, env); break;
        case T_BOOL:
            r = ufcs_call(vm, recv, name, a, n, callee, env, line, &f); if (f) return r;
            { static const char *const none_l[] = { NULL }; no_method(vm, line, "bool", name->s, none_l, env); } break;
        case T_ERR:
            if (!strcmp(name->s, "message")) return V_str(recv.u.s);
            rt(vm, line, "error has only .message"); break;
        case T_OBJ:
            if (obj_find(recv.u.o, name) >= 0) { *found = 0; return V_none(); }
            r = obj_method(vm, recv.u.o, name, a, n, &f); if (f) return r;
            if (recv.u.o->type) { r = ufcs_call(vm, recv, name, a, n, callee, env, line, &f); if (f) return r; }
            *found = 0; return V_none();
        case T_TYPE: rt(vm, line, "type %s has no member .%s", recv.u.td->name->s, name->s); break;
        default:
            if (!strcmp(name->s, "from")) {
                Value st = mk_stream(vm, SK_FROM, V_none(), recv, line, 1, 0);
                st.u.sm->other = n ? a[0] : V_none(); return st;
            }
            rt(vm, line, "function has only .from(value)"); break;
    }
    *found = 0; return V_none();
}

/* o.name без вызова: поле объекта (иначе none); у ошибки — message */
static Value get_member(VM *vm, Value o, Str *name, Node *n, Env *env) {
    (void)env;
    int line = n ? n->line : 0;
    if (o.t == T_NONE) rt(vm, line, "cannot access \".%s\" of none", name->s);
    if (o.t == T_OBJ) { int j = obj_find(o.u.o, name); return j >= 0 ? o.u.o->v[j] : V_none(); }
    if (o.t == T_ERR && !strcmp(name->s, "message")) return V_str(o.u.s);
    if (o.t == T_ARR && !strcmp(name->s, "len")) return V_int(o.u.a->n);
    if (o.t == T_STR && !strcmp(name->s, "len")) return V_int(u8_count(o.u.s->s, o.u.s->len));
    if ((o.t == T_FN || o.t == T_NAT) && strcmp(name->s, "from")) rt(vm, line, "function has only .from(value)");
    if (o.t == T_STR || o.t == T_ARR || o.t == T_STREAM || o.t == T_INT || o.t == T_FLT || o.t == T_DEC || o.t == T_BOOL) { /* w.upper без вызова — ссылка на метод с привязанным получателем */
        Fn *b = (Fn *)amalloc(vm, sizeof(Fn));
        b->name = name; b->mname = name; b->bline = line;
        b->target = (Value *)amalloc(vm, sizeof(Value)); *b->target = o;
        Value v = V_none(); v.t = T_FN; v.u.fn = b; return v;
    }
    rt(vm, line, "%s has no field .%s", type_of(o), name->s);
    return V_none();
}
