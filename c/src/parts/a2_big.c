
/* ---------------------------------------------------------------- большие целые (int без ограничения) */
/* Модуль хранится по основанию 1e9, младший разряд первым. Всё, что помещается в int64, хранится как T_INT. */
static void rt(VM *vm, int line, const char *fmt, ...);
#define BIG_BASE 1000000000u
struct Big { int sign; int n; uint32_t d[1]; };

static Big *big_alloc(VM *vm, int n) {
    Big *b = (Big *)amalloc(vm, sizeof(Big) + sizeof(uint32_t) * (size_t)(n + 1));
    b->n = n; b->sign = 1; memset(b->d, 0, sizeof(uint32_t) * (size_t)(n + 1));
    return b;
}
static void big_trim(Big *b) { while (b->n > 0 && b->d[b->n - 1] == 0) b->n--; if (b->n == 0) b->sign = 1; }

/* модуль int64 -> Big */
static Big *big_from_i64(VM *vm, int64_t v) {
    Big *b = big_alloc(vm, 3);
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    b->sign = v < 0 ? -1 : 1;
    for (int i = 0; i < 3 && m; i++) { b->d[i] = (uint32_t)(m % BIG_BASE); m /= BIG_BASE; }
    big_trim(b); return b;
}
static Big *big_of(VM *vm, Value v) { return v.t == T_BIG ? v.u.big : big_from_i64(vm, v.u.i); }

static Value V_bigval(Big *b) { Value v = V_none(); v.t = T_BIG; v.u.big = b; return v; }
/* убрать лишние нули и вернуть T_INT, если число помещается в int64 */
static Value big_norm(Big *b) {
    big_trim(b);
    if (b->n <= 3) {
        static const uint32_t MAXD[3] = { 854775807u, 223372036u, 9u };
        int fits = 1;
        if (b->n == 3) {
            uint32_t lim[3]; memcpy(lim, MAXD, sizeof lim); if (b->sign < 0) lim[0] += 1;
            for (int i = 2; i >= 0; i--) { if (b->d[i] != lim[i]) { fits = b->d[i] < lim[i]; break; } }
        }
        if (fits) {
            uint64_t m = 0;
            for (int i = b->n - 1; i >= 0; i--) m = m * BIG_BASE + b->d[i];
            return V_int(b->sign < 0 ? (int64_t)((uint64_t)0 - m) : (int64_t)m);
        }
    }
    return V_bigval(b);
}
static int mag_cmp(const Big *a, const Big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static Big *mag_add(VM *vm, const Big *a, const Big *b) {
    int n = (a->n > b->n ? a->n : b->n) + 1;
    Big *r = big_alloc(vm, n); uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = c + (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)(s % BIG_BASE); c = s / BIG_BASE;
    }
    big_trim(r); return r;
}
static Big *mag_sub(VM *vm, const Big *a, const Big *b) { /* |a| >= |b| */
    Big *r = big_alloc(vm, a->n); int64_t c = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t s = (int64_t)a->d[i] - c - (i < b->n ? (int64_t)b->d[i] : 0);
        if (s < 0) { s += BIG_BASE; c = 1; } else c = 0;
        r->d[i] = (uint32_t)s;
    }
    big_trim(r); return r;
}
static Big *mag_mul(VM *vm, const Big *a, const Big *b) {
    if (!a->n || !b->n) return big_alloc(vm, 0);
    Big *r = big_alloc(vm, a->n + b->n);
    for (int i = 0; i < a->n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < b->n || c; j++) {
            uint64_t cur = r->d[i + j] + c + (j < b->n ? (uint64_t)a->d[i] * b->d[j] : 0);
            r->d[i + j] = (uint32_t)(cur % BIG_BASE); c = cur / BIG_BASE;
        }
    }
    big_trim(r); return r;
}
static Big *mag_mul_small(VM *vm, const Big *a, uint32_t k) {
    Big *r = big_alloc(vm, a->n + 1); uint64_t c = 0;
    for (int i = 0; i < a->n || c; i++) {
        uint64_t cur = c + (i < a->n ? (uint64_t)a->d[i] * k : 0);
        r->d[i] = (uint32_t)(cur % BIG_BASE); c = cur / BIG_BASE;
    }
    big_trim(r); return r;
}
/* деление модулей: q = |a| / |b|, r = |a| % |b| (b != 0) */
static void mag_divmod(VM *vm, const Big *a, const Big *b, Big **q, Big **r) {
    if (mag_cmp(a, b) < 0) { *q = big_alloc(vm, 0); Big *c = big_alloc(vm, a->n); memcpy(c->d, a->d, sizeof(uint32_t) * (size_t)a->n); big_trim(c); *r = c; return; }
    Big *Q = big_alloc(vm, a->n);
    Big *R = big_alloc(vm, 0);
    for (int i = a->n - 1; i >= 0; i--) {
        /* R = R * BASE + a[i] */
        Big *t = big_alloc(vm, R->n + 1);
        for (int k = 0; k < R->n; k++) t->d[k + 1] = R->d[k];
        t->d[0] = a->d[i]; big_trim(t); R = t;
        uint32_t lo = 0, hi = BIG_BASE - 1;
        if (mag_cmp(R, b) < 0) hi = 0;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo + 1) / 2;
            Big *pr = mag_mul_small(vm, b, mid);
            if (mag_cmp(pr, R) <= 0) lo = mid; else hi = mid - 1;
        }
        Q->d[i] = lo;
        if (lo) R = mag_sub(vm, R, mag_mul_small(vm, b, lo));
    }
    big_trim(Q); *q = Q; *r = R;
}

static Big *big_neg(VM *vm, const Big *a) { Big *r = big_alloc(vm, a->n); memcpy(r->d, a->d, sizeof(uint32_t) * (size_t)a->n); r->sign = a->n ? -a->sign : 1; return r; }
static Big *sadd(VM *vm, const Big *a, const Big *b) {
    if (a->sign == b->sign) { Big *r = mag_add(vm, a, b); r->sign = a->sign; if (!r->n) r->sign = 1; return r; }
    int c = mag_cmp(a, b);
    if (c == 0) return big_alloc(vm, 0);
    if (c > 0) { Big *r = mag_sub(vm, a, b); r->sign = a->sign; return r; }
    Big *r = mag_sub(vm, b, a); r->sign = b->sign; return r;
}
static Big *big_mul_s(VM *vm, const Big *a, const Big *b) { Big *r = mag_mul(vm, a, b); r->sign = a->sign * b->sign; if (!r->n) r->sign = 1; return r; }

static double big_to_double(Value v) { /* через десятичную запись: корректное округление */
    const Big *b = v.u.big;
    size_t len = (size_t)b->n * 9 + 4;
    char *buf = (char *)malloc(len); if (!buf) return 0;
    size_t k = 0;
    if (b->sign < 0) buf[k++] = '-';
    k += (size_t)snprintf(buf + k, len - k, "%u", b->d[b->n - 1]);
    for (int i = b->n - 2; i >= 0; i--) k += (size_t)snprintf(buf + k, len - k, "%09u", b->d[i]);
    double r = strtod(buf, NULL); free(buf); return r;
}
static int big_eq(Value a, Value b) { return a.u.big->sign == b.u.big->sign && mag_cmp(a.u.big, b.u.big) == 0; }
static int big_cmp(VM *vm, Value a, Value b) {
    Big *x = big_of(vm, a), *y = big_of(vm, b);
    if (x->n == 0 && y->n == 0) return 0;
    if (x->sign != y->sign) return x->sign < y->sign ? -1 : 1;
    int c = mag_cmp(x, y); return x->sign < 0 ? -c : c;
}
static void big_repr(SB *sb, Value v) {
    const Big *b = v.u.big; char buf[16];
    if (b->sign < 0) sb_c(sb, '-');
    snprintf(buf, sizeof buf, "%u", b->d[b->n - 1]); sb_cs(sb, buf);
    for (int i = b->n - 2; i >= 0; i--) { snprintf(buf, sizeof buf, "%09u", b->d[i]); sb_cs(sb, buf); }
}
/* из десятичной записи (только цифры) */
static Value big_parse(VM *vm, const char *s, int len, int neg) {
    while (len > 1 && *s == '0') { s++; len--; }
    int n = (len + 8) / 9; Big *b = big_alloc(vm, n);
    for (int i = 0; i < n; i++) {
        int e = len - i * 9, st = e - 9 < 0 ? 0 : e - 9; uint32_t x = 0;
        for (int k = st; k < e; k++) x = x * 10 + (uint32_t)(s[k] - '0');
        b->d[i] = x;
    }
    b->sign = neg ? -1 : 1; return big_norm(b);
}

/* арифметика над значениями-целыми (T_INT или T_BIG). op: 0 + 1 - 2 * 3 % 4 ** 5 div */
static Value big_arith(VM *vm, int op, Value a, Value b, int line) {
    Big *x = big_of(vm, a), *y = big_of(vm, b);
    switch (op) {
        case 0: return big_norm(sadd(vm, x, y));
        case 1: return big_norm(sadd(vm, x, big_neg(vm, y)));
        case 2: return big_norm(big_mul_s(vm, x, y));
        case 3: case 5: {
            if (!y->n) return V_err(vm, "division by zero");
            Big *q, *r; mag_divmod(vm, x, y, &q, &r);
            q->sign = x->sign * y->sign; if (!q->n) q->sign = 1;
            r->sign = x->sign; if (!r->n) r->sign = 1;
            if (r->n && x->sign != y->sign) { q = sadd(vm, q, big_from_i64(vm, -1)); r = sadd(vm, r, y); }
            return big_norm(op == 3 ? r : q);
        }
        case 4: {
            if (y->sign < 0 || y->n > 1) { if (y->sign < 0) return V_flt(pow(as_f(a), as_f(b))); rt(vm, line, "exponent too large"); }
            uint32_t e = y->n ? y->d[0] : 0;
            if (x->n > 1 && e > 100000) rt(vm, line, "exponent too large");
            Big *res = big_from_i64(vm, 1), *base = x;
            while (e) { if (e & 1) res = big_mul_s(vm, res, base); e >>= 1; if (e) base = big_mul_s(vm, base, base); }
            return big_norm(res);
        }
    }
    return V_none();
}
