/* ---------------------------------------------------------------- десятичные числа произвольной точности
   Значение = m / 10^s (m — int или большое int, s >= 0). Тип называется float: с precision(n) все дробные числа такие,
   иначе float — обычный double, а десятичные получаются из литералов 0.1d, 5d и функции dec(). */
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_AND, OP_OR, OP_IN, OP_IS, OP_NC, OP_NEG, OP_NOT, OP_ASSIGN, OP_MUTASSIGN, OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR };
struct Dec { Value m; int s; };

static Value V_decv(Dec *d) { Value v = V_none(); v.t = T_DEC; v.u.dec = d; return v; }
static Value dec_new(VM *vm, Value m, int s) { Dec *d = (Dec *)amalloc(vm, sizeof(Dec)); d->m = m; d->s = s; return V_decv(d); }
static int dec_p(VM *vm) { return vm->prec > 0 ? vm->prec : 30; }

/* 10^k как значение-целое: кэш в обычной памяти (не в арене), общий для потоков */
#define P10_MAX 4200
static Big *P10C[P10_MAX];
static Value pow10v(VM *vm, int k) {
    if (k < 0 || k >= P10_MAX) rt(vm, 0, "precision is too large");
    if (k < 19) { int64_t r = 1; for (int i = 0; i < k; i++) r *= 10; return V_int(r); }
    Big *c = __atomic_load_n(&P10C[k], __ATOMIC_ACQUIRE);
    if (!c) {
        Value v = big_arith(vm, 4, V_int(10), V_int(k), 0);
        Big *src = big_of(vm, v);
        c = (Big *)malloc(sizeof(Big) + sizeof(uint32_t) * (size_t)(src->n + 1));
        c->n = src->n; c->sign = 1; memcpy(c->d, src->d, sizeof(uint32_t) * (size_t)(src->n + 1));
        __atomic_store_n(&P10C[k], c, __ATOMIC_RELEASE);
    }
    return big_norm(c);   /* big_norm не копирует: значение указывает на кэш; операции над ним результат в арене */
}
static int vsign(Value v) { return v.t == T_INT ? (v.u.i < 0 ? -1 : v.u.i > 0 ? 1 : 0) : (v.u.big->n == 0 ? 0 : v.u.big->sign); }
static Value vneg(VM *vm, Value v) { return big_arith(vm, 1, V_int(0), v, 0); }
static Value vabs(VM *vm, Value v) { return vsign(v) < 0 ? vneg(vm, v) : v; }
static Value vmul(VM *vm, Value a, Value b) { return big_arith(vm, 2, a, b, 0); }
static Value vadd(VM *vm, Value a, Value b) { return big_arith(vm, 0, a, b, 0); }
static Value vsub(VM *vm, Value a, Value b) { return big_arith(vm, 1, a, b, 0); }
static int vcmp(VM *vm, Value a, Value b) {
    if (a.t == T_INT && b.t == T_INT) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i ? 1 : 0;
    return big_cmp(vm, a, b);
}
/* деление модулей с усечением: q, r для |a| / |b| (b != 0) */
static void vdivmod_abs(VM *vm, Value a, Value b, Value *q, Value *r) {
    Big *x = big_of(vm, vabs(vm, a)), *y = big_of(vm, vabs(vm, b)), *qq, *rr;
    mag_divmod(vm, x, y, &qq, &rr);
    qq->sign = 1; rr->sign = 1;
    *q = big_norm(qq); *r = big_norm(rr);
}

/* ---- текстовая форма без арены (для вывода, преобразования в double и сравнения) ---- */
typedef struct { int neg; char *dig; int n; int s; } DT;   /* цифры без знака, s знаков после запятой */
static void dt_free(DT *d) { free(d->dig); d->dig = NULL; }
static void dt_norm(DT *d) {   /* убрать ведущие нули целой части и хвостовые нули дробной */
    int lead = 0; while (lead < d->n - d->s - 1 && d->dig[lead] == '0') lead++;
    if (lead) { memmove(d->dig, d->dig + lead, (size_t)(d->n - lead)); d->n -= lead; }
    while (d->s > 0 && d->n > 1 && d->dig[d->n - 1] == '0') { d->n--; d->s--; }
    if (d->n == 1 && d->dig[0] == '0') d->neg = 0;
    d->dig[d->n] = 0;
}
static int dt_parse(const char *t, int len, DT *d) {   /* [+-]цифры[.цифры][e[+-]цифры]; 0 при ошибке */
    int i = 0, neg = 0;
    while (i < len && isspace((unsigned char)t[i])) i++;
    while (len > i && isspace((unsigned char)t[len - 1])) len--;
    if (i < len && (t[i] == '+' || t[i] == '-')) { neg = t[i] == '-'; i++; }
    char *dig = (char *)malloc((size_t)(len - i) + 8); int n = 0, s = 0, seen = 0, dot = 0;
    for (; i < len; i++) {
        if (isdigit((unsigned char)t[i])) { dig[n++] = t[i]; seen = 1; if (dot) s++; }
        else if (t[i] == '.' && !dot) dot = 1;
        else break;
    }
    if (!seen) { free(dig); return 0; }
    long ex = 0;
    if (i < len && (t[i] == 'e' || t[i] == 'E')) {
        i++; int en = 0; if (i < len && (t[i] == '+' || t[i] == '-')) { en = t[i] == '-'; i++; }
        if (i >= len) { free(dig); return 0; }
        for (; i < len && isdigit((unsigned char)t[i]); i++) { ex = ex * 10 + (t[i] - '0'); if (ex > 100000) { free(dig); return 0; } }
        if (en) ex = -ex;
    }
    if (i != len) { free(dig); return 0; }
    long sc = (long)s - ex;
    if (sc < 0) { dig = (char *)realloc(dig, (size_t)(n - sc) + 2); for (long k = 0; k < -sc; k++) dig[n++] = '0'; sc = 0; }
    d->neg = neg; d->dig = dig; d->n = n; d->s = (int)sc; dig[n] = 0;
    return 1;
}
static void m_digits(Value m, DT *d) {   /* цифры мантиссы (модуль) */
    if (m.t == T_INT) {
        char buf[32]; uint64_t u = m.u.i < 0 ? (uint64_t)0 - (uint64_t)m.u.i : (uint64_t)m.u.i;
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)u);
        d->n = (int)strlen(buf); d->dig = (char *)malloc((size_t)d->n + 2); memcpy(d->dig, buf, (size_t)d->n + 1); d->neg = m.u.i < 0;
        return;
    }
    const Big *b = m.u.big;
    d->dig = (char *)malloc((size_t)b->n * 9 + 4); int k = 0;
    k += snprintf(d->dig + k, 12, "%u", b->d[b->n - 1]);
    for (int i = b->n - 2; i >= 0; i--) k += snprintf(d->dig + k, 12, "%09u", b->d[i]);
    d->n = k; d->neg = b->sign < 0;
}
static void dec_dt(Value v, DT *d) {   /* T_DEC -> DT, нормализованный: цифр не меньше s+1 */
    m_digits(v.u.dec->m, d); d->s = v.u.dec->s;
    if (d->n <= d->s) {
        int pad = d->s - d->n + 1;
        d->dig = (char *)realloc(d->dig, (size_t)(d->n + pad) + 2);
        memmove(d->dig + pad, d->dig, (size_t)d->n); memset(d->dig, '0', (size_t)pad); d->n += pad;
    }
    d->dig[d->n] = 0; dt_norm(d);
}
/* число любого вида -> DT; 0 для не конечных значений */
static int num_dt(Value v, DT *d) {
    switch (v.t) {
        case T_DEC: dec_dt(v, d); return 1;
        case T_INT: case T_BIG: m_digits(v, d); d->s = 0; dt_norm(d); return 1;
        case T_BOOL: m_digits(V_int(v.u.b), d); d->s = 0; return 1;
        case T_FLT: { if (!isfinite(v.u.f)) return 0; char buf[64]; fmt_float(v.u.f, buf, sizeof buf); if (!dt_parse(buf, (int)strlen(buf), d)) return 0; dt_norm(d); return 1; }
        default: return 0;
    }
}
static int dt_cmp(const DT *a, const DT *b) {
    int za = a->n == 1 && a->dig[0] == '0', zb = b->n == 1 && b->dig[0] == '0';
    if (za && zb) return 0;
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int ia = a->n - a->s, ib = b->n - b->s, c = 0;
    if (ia != ib) c = ia < ib ? -1 : 1;
    else {
        int m = a->n > b->n ? a->n : b->n;
        for (int k = 0; k < m && !c; k++) { char x = k < a->n ? a->dig[k] : '0', y = k < b->n ? b->dig[k] : '0'; if (x != y) c = x < y ? -1 : 1; }
    }
    return a->neg ? -c : c;
}
/* сравнение двух чисел, из которых хотя бы одно — decimal; 2 — если сравнить нельзя (nan/inf) */
static int dec_cmp_values(Value a, Value b) {
    DT x, y;
    if (!num_dt(a, &x)) return 2;
    if (!num_dt(b, &y)) { dt_free(&x); return 2; }
    int c = dt_cmp(&x, &y); dt_free(&x); dt_free(&y); return c;
}
/* текст значения: целое выглядит как 12.0, хвостовые нули дроби убраны */
static char *dec_text(Value v) {
    DT d; dec_dt(v, &d);
    int il = d.n - d.s; char *o = (char *)malloc((size_t)d.n + 6); int k = 0;
    if (d.neg) o[k++] = '-';
    memcpy(o + k, d.dig, (size_t)il); k += il; o[k++] = '.';
    if (d.s) { memcpy(o + k, d.dig + il, (size_t)d.s); k += d.s; } else o[k++] = '0';
    o[k] = 0; dt_free(&d); return o;
}
static void dec_repr(SB *sb, Value v) { char *t = dec_text(v); sb_cs(sb, t); free(t); }
static double dec_to_double(Value v) { char *t = dec_text(v); double r = strtod(t, NULL); free(t); return r; }

/* ---- создание ---- */
static Value dec_from_dt(VM *vm, const DT *d) {
    Value m = big_parse(vm, d->dig, d->n, d->neg);
    return dec_new(vm, m, d->s);
}
static Value dec_parse_str(VM *vm, const char *t, int len) {   /* none, если запись неверна */
    DT d; if (!dt_parse(t, len, &d)) return V_none();
    Value r = dec_from_dt(vm, &d); dt_free(&d); return r;
}
/* любое число -> decimal; не конечный double остаётся double */
static Value to_dec(VM *vm, Value v) {
    if (v.t == T_DEC) return v;
    if (v.t == T_INT || v.t == T_BIG) return dec_new(vm, v, 0);
    if (v.t == T_BOOL) return dec_new(vm, V_int(v.u.b), 0);
    if (v.t == T_FLT) {
        if (!isfinite(v.u.f)) return v;
        char buf[64]; fmt_float(v.u.f, buf, sizeof buf);
        return dec_parse_str(vm, buf, (int)strlen(buf));
    }
    return v;
}

/* ---- арифметика ---- */
static Value dec_round_to(VM *vm, Value d, int P) {   /* до P знаков после запятой, .5 — от нуля */
    Dec *x = d.u.dec; if (x->s <= P) return d;
    Value div = pow10v(vm, x->s - P), q, r;
    vdivmod_abs(vm, x->m, div, &q, &r);
    if (vcmp(vm, vmul(vm, V_int(2), r), div) >= 0) q = vadd(vm, q, V_int(1));
    if (vsign(x->m) < 0) q = vneg(vm, q);
    return dec_new(vm, q, P);
}
static void dec_align(VM *vm, Dec *a, Dec *b, Value *p, Value *q, int *s) {
    *s = a->s > b->s ? a->s : b->s;
    *p = a->s < *s ? vmul(vm, a->m, pow10v(vm, *s - a->s)) : a->m;
    *q = b->s < *s ? vmul(vm, b->m, pow10v(vm, *s - b->s)) : b->m;
}
static Value dec_div(VM *vm, Dec *a, Dec *b) {
    if (vsign(b->m) == 0) return V_err(vm, "division by zero");
    int P = dec_p(vm);
    Value num = vmul(vm, a->m, pow10v(vm, P + 1 + b->s)), den = vmul(vm, b->m, pow10v(vm, a->s)), q, r;
    int neg = (vsign(num) < 0) != (vsign(den) < 0);
    vdivmod_abs(vm, num, den, &q, &r);
    if (neg) q = vneg(vm, q);
    return dec_round_to(vm, dec_new(vm, q, P + 1), P);
}
static Value dec_from_double(VM *vm, double x) { return to_dec(vm, V_flt(x)); }
/* op: OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW */
static Value dec_arith_raw(VM *vm, int op, Value a, Value b, int line) {
    Value x = to_dec(vm, a), y = to_dec(vm, b);
    if (x.t != T_DEC || y.t != T_DEC) {   /* inf / nan: обычная арифметика double */
        double u = as_f(a), v = as_f(b);
        switch (op) {
            case OP_ADD: return V_flt(u + v); case OP_SUB: return V_flt(u - v); case OP_MUL: return V_flt(u * v);
            case OP_DIV: if (v == 0) return V_err(vm, "division by zero"); return V_flt(u / v);
            case OP_MOD: if (v == 0) return V_err(vm, "division by zero"); { double r = fmod(u, v); if (r != 0 && ((r < 0) != (v < 0))) r += v; return V_flt(r); }
            default: return V_flt(pow(u, v));
        }
    }
    Dec *p = x.u.dec, *q = y.u.dec; int P = dec_p(vm);
    switch (op) {
        case OP_ADD: case OP_SUB: {
            Value m1, m2; int s; dec_align(vm, p, q, &m1, &m2, &s);
            return dec_round_to(vm, dec_new(vm, op == OP_ADD ? vadd(vm, m1, m2) : vsub(vm, m1, m2), s), P);
        }
        case OP_MUL: return dec_round_to(vm, dec_new(vm, vmul(vm, p->m, q->m), p->s + q->s), P);
        case OP_DIV: return dec_div(vm, p, q);
        case OP_MOD: {
            Value m1, m2; int s; dec_align(vm, p, q, &m1, &m2, &s);
            if (vsign(m2) == 0) return V_err(vm, "division by zero");
            return dec_new(vm, big_arith(vm, 3, m1, m2, line), s);
        }
        default: {   /* ** */
            Value div = pow10v(vm, q->s), e, r;
            vdivmod_abs(vm, q->m, div, &e, &r);
            if (vsign(r) == 0) {   /* целая степень */
                if (e.t != T_INT || e.u.i > 100000) rt(vm, line, "exponent too large");
                int64_t k = e.u.i; int neg = vsign(q->m) < 0;
                Value res = dec_new(vm, V_int(1), 0), base = x;
                while (k > 0) {
                    if (k & 1) res = dec_round_to(vm, dec_new(vm, vmul(vm, res.u.dec->m, base.u.dec->m), res.u.dec->s + base.u.dec->s), P);
                    k >>= 1;
                    if (k) base = dec_round_to(vm, dec_new(vm, vmul(vm, base.u.dec->m, base.u.dec->m), base.u.dec->s * 2), P);
                }
                { Value one = dec_new(vm, V_int(1), 0); return neg ? dec_div(vm, one.u.dec, res.u.dec) : res; }
            }
            return dec_from_double(vm, pow(dec_to_double(x), dec_to_double(y)));
        }
    }
}
static Value dec_neg(VM *vm, Value d) { return dec_new(vm, vneg(vm, d.u.dec->m), d.u.dec->s); }
static Value dec_abs(VM *vm, Value d) { return vsign(d.u.dec->m) < 0 ? dec_neg(vm, d) : d; }
/* целая часть: mode 0 floor, 1 ceil, 2 trunc, 3 round (.5 от нуля); результат int */
static Value dec_to_int(VM *vm, Value d, int mode) {
    Dec *x = d.u.dec; if (x->s == 0) return x->m;
    Value div = pow10v(vm, x->s), q, r;
    vdivmod_abs(vm, x->m, div, &q, &r);
    int neg = vsign(x->m) < 0, nz = vsign(r) != 0;
    if (!nz) return neg ? vneg(vm, q) : q;
    switch (mode) {
        case 2: return neg ? vneg(vm, q) : q;
        case 0: return neg ? vneg(vm, vadd(vm, q, V_int(1))) : q;
        case 1: return neg ? vneg(vm, q) : vadd(vm, q, V_int(1));
        default: if (vcmp(vm, vmul(vm, V_int(2), r), div) >= 0) q = vadd(vm, q, V_int(1)); return neg ? vneg(vm, q) : q;
    }
}
static Value dec_sqrt_raw(VM *vm, Value d) {
    Dec *x = d.u.dec;
    if (vsign(x->m) < 0) return V_flt(NAN);
    int P = dec_p(vm), Q = P + 1;
    if (x->s > 2 * Q) d = dec_round_to(vm, d, 2 * Q), x = d.u.dec;
    Value n = vmul(vm, x->m, pow10v(vm, 2 * Q - x->s));
    Value r;
    if (vcmp(vm, n, V_int(2)) < 0) r = n;
    else {
        DT t; m_digits(n, &t); int nd = t.n; dt_free(&t);
        Value cur = pow10v(vm, (nd + 1) / 2);
        for (;;) {   /* Ньютон по целым */
            Value qv, rv; vdivmod_abs(vm, n, cur, &qv, &rv);
            Value y = big_arith(vm, 5, vadd(vm, cur, qv), V_int(2), 0);
            if (vcmp(vm, y, cur) >= 0) break;
            cur = y;
        }
        r = cur;
    }
    return dec_round_to(vm, dec_new(vm, r, Q), P);
}

/* копия значения вне арены (для литералов, хранимых в узлах дерева) */
static Value dec_persist(Value d) {
    Dec *x = d.u.dec, *y = (Dec *)malloc(sizeof(Dec));
    y->s = x->s; y->m = x->m;
    if (x->m.t == T_BIG) {
        Big *b = x->m.u.big, *c = (Big *)malloc(sizeof(Big) + sizeof(uint32_t) * (size_t)(b->n + 1));
        c->n = b->n; c->sign = b->sign; memcpy(c->d, b->d, sizeof(uint32_t) * (size_t)(b->n + 1));
        y->m.u.big = c;
    }
    return V_decv(y);
}

/* Промежуточные вычисления над большими целыми засоряют арену; оставляем в ней только результат: копируем его, откатываем арену и кладём заново */
static Value dec_compact(VM *vm, Value r, Block *b0, size_t u0) {
    if (r.t != T_DEC || !b0 || vm->blocks != b0) return r;
    char *base = (char *)(b0 + 1) + u0;
    Dec *x = r.u.dec;
    int dec_above = (char *)x >= base, mant_above = x->m.t == T_BIG && (char *)x->m.u.big >= base;
    if (!dec_above && !mant_above) return r;
    uint32_t tmp[512]; int sign = 1, n = 0; int64_t small = 0; Value m = x->m; int s = x->s;
    if (m.t == T_BIG) {
        n = m.u.big->n; sign = m.u.big->sign;
        if (n > 500) return r;
        memcpy(tmp, m.u.big->d, sizeof(uint32_t) * (size_t)n);
    } else small = m.u.i;
    b0->used = u0;
    Value nm;
    if (n) { Big *b = big_alloc(vm, n); b->sign = sign; memcpy(b->d, tmp, sizeof(uint32_t) * (size_t)n); nm = V_bigval(b); }
    else nm = V_int(small);
    return dec_new(vm, nm, s);
}
static Value dec_arith(VM *vm, int op, Value a, Value b, int line) {
    Block *b0 = vm->blocks; size_t u0 = b0 ? b0->used : 0;
    return dec_compact(vm, dec_arith_raw(vm, op, a, b, line), b0, u0);
}
static Value dec_sqrt(VM *vm, Value d) {
    Block *b0 = vm->blocks; size_t u0 = b0 ? b0->used : 0;
    return dec_compact(vm, dec_sqrt_raw(vm, d), b0, u0);
}
