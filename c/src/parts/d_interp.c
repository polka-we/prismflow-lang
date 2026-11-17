
/* ---------------------------------------------------------------- окружения */
static Env *env_new(VM *vm, Env *parent, int par) {
    Env *e;
    if (vm->freeenv) { e = vm->freeenv; vm->freeenv = e->parent; }
    else e = (Env *)amalloc(vm, sizeof(Env));
    e->parent = parent; e->par = par; e->vars = e->inl; e->cap = 6; e->n = 0; e->captured = 0; e->owner = vm->own; e->iter = vm->iter; e->gen = ++vm->egen;
    return e;
}
static void env_release(VM *vm, Env *e) {
    if (e->captured) return;
    e->parent = vm->freeenv; vm->freeenv = e;
}
static void env_capture(Env *e) { for (; e && !__atomic_load_n(&e->captured, __ATOMIC_RELAXED); e = e->parent) __atomic_store_n(&e->captured, 1, __ATOMIC_RELAXED); }

static Var *env_own(Env *e, Str *name) {
    for (int i = e->n - 1; i >= 0; i--) if (e->vars[i].name == name) return &e->vars[i];
    return NULL;
}
static Var *env_find_o(Env *e, Str *name, int *crossed, int own) {
    int cr = 0;
    for (; e; e = e->parent) {
        for (int i = e->n - 1; i >= 0; i--) if (e->vars[i].name == name) { if (crossed) *crossed = (cr || e->owner != own) | (e->iter << 1); return &e->vars[i]; }
        if (e->par) cr = 1;
    }
    return NULL;
}
__attribute__((noinline)) static Var *env_find_slow(Env *e0, Node *n, int *crossed, int own) {
    Env *e = e0;
    Str *name = n->name; int cv = __atomic_load_n(&n->cidx, __ATOMIC_RELAXED), h = 0, cr = 0;
    int hops = (cv >> 16) - 1, cix = cv & 0xFFFF;
    for (; e; e = e->parent, h++) {
        int i = -1;
        if (h == hops && cix < e->n && e->vars[cix].name == name) i = cix;
        else for (int k = e->n - 1; k >= 0; k--) if (e->vars[k].name == name) { i = k; break; }
        if (i >= 0) {
            if (h != hops && h < 32000 && i < 65535) __atomic_store_n(&n->cidx, ((h + 1) << 16) | i, __ATOMIC_RELAXED);
            if (crossed) *crossed = (cr || e->owner != own) | (e->iter << 1);
            if (!g_par_active && (n->chit || n->cfail < 8)) {   /* кэш, который ни разу не сработал (вызовы с новым окружением каждый раз), перестаём обновлять */
                if (n->chit) n->chit = 0; else n->cfail++;
                n->cv = &e->vars[i]; n->cte = e; n->ccr = cr; n->cep = name->ep; n->cgen = e0->gen;
            }
            return &e->vars[i];
        }
        if (e->par) cr = 1;
    }
    return NULL;
}
__attribute__((noinline)) static Var *env_find_h2(Env *e, Node *n, int *crossed, int own, int cix) {
    if (e->n > 4 || !e->parent) return NULL;
    int k = 0; for (; k < e->n; k++) if (e->vars[k].name == n->name) return NULL;
    Env *pe = e->parent;
    if (cix < pe->n && pe->vars[cix].name == n->name) { if (crossed) *crossed = (e->par || pe->owner != own) | (pe->iter << 1); return &pe->vars[cix]; }
    return NULL;
}
/* Быстрый путь: окружение то же самое (gen) и с тех пор нигде не определялась новая переменная с этим именем (ep) —
   результат прошлого поиска остаётся верным. */
static inline Var *env_find_cxo(Env *e, Node *n, int *crossed, int own) {
    if (e->gen == n->cgen && n->name->ep == n->cep && !g_par_active) {
#ifdef PRISM_LOOKUP_CHECK
        { int d1; Var *chk = env_find_slow(e, n, &d1, own); if (chk != n->cv) { fprintf(stderr, "lookup cache mismatch: %s\n", n->name->s); abort(); } }
#endif
        n->chit = 1;
        if (crossed) { Env *te = n->cte; *crossed = (n->ccr || te->owner != own) | (te->iter << 1); }
        return n->cv;
    }
    { int cv = __atomic_load_n(&n->cidx, __ATOMIC_RELAXED), cix = cv & 0xFFFF;   /* имя в самом окружении: типичный случай параметров */
      if ((cv >> 16) == 1 && cix < e->n && e->vars[cix].name == n->name) { if (crossed) *crossed = (e->owner != own) | (e->iter << 1); return &e->vars[cix]; }
      if ((cv >> 16) == 2 && n->cfail >= 8) { Var *r = env_find_h2(e, n, crossed, own, cix); if (r) return r; } }
    return env_find_slow(e, n, crossed, own);
}
#define env_find_cx(e, n, c) env_find_cxo((e), (n), (c), vm->own)
#define env_find(e, nm, c) env_find_o((e), (nm), (c), vm->own)
#define env_find_c(e, n) env_find_cxo((e), (n), NULL, 0)
static Var *env_def(VM *vm, Env *e, Str *name, Value v, int mut) {
    Var *x = env_own(e, name);
    if (!x) {
        if (e->n == e->cap) {
            if (vm->nmk) esc_container(vm, e);
            int nc = e->cap * 2;
            for (int i = 0; i < e->n; i++) e->vars[i].name->ep++;   /* переменные переедут: кэшированные указатели недействительны */
            Var *nv = (Var *)amalloc(vm, sizeof(Var) * (size_t)nc);
            memcpy(nv, e->vars, sizeof(Var) * (size_t)e->n);
            e->vars = nv; e->cap = nc;
        }
        x = &e->vars[e->n++];
        name->ep++;
    }
    memset(x, 0, sizeof *x);
    x->name = name; x->v = v; x->mut = mut;
    return x;
}

/* ---------------------------------------------------------------- прототипы */
static inline Value ev(VM *vm, Node *n, Env *env);
static Value ev_call(VM *vm, Node *n, Env *env);
/* дробный литерал: double, а при precision(n) или суффиксе d — десятичное число (разбирается один раз и хранится вне арены) */
static Value ev_flt(VM *vm, Node *n, Env *env) {
    (void)env;
    if (n->name && (n->flag || vm->prec > 0)) {
        if (n->dv.t == T_DEC) return n->dv;
        Value d = dec_parse_str(vm, n->name->s, n->name->len);
        if (d.t == T_DEC) { d = dec_persist(d); n->dv = d; return d; }
    }
    return n->v;
}
static inline Value evq(VM *vm, Node *n, Env *env);
static Value call_fn(VM *vm, Value f, Value *args, int nargs, int par, int line, int *muts);
static void vm_try_compile(VM *vm, Clause *c, Env *cenv);
static Value ev_assign_v(VM *vm, Node *n, Env *env, Value val);
static Value vm_exec(VM *vm, struct VCode *code, Env *cenv, Value *args, int nargs);
static void err_not_callable(VM *vm, Node *callee, int line, Value f);
static int par_run(VM *vm, Value f, Arr *items, int64_t base, int op, int line);
static int par_default_threads(void);
static double now_us(void);
static Value exec_block(VM *vm, Node **st, int n, Env *env);
static Value do_return(VM *vm, Node *s, Env *env);
static int is_method_name(const char *m);
/* «Did you mean»: как в Python — ближайшее по расстоянию Левенштейна имя, если оно достаточно близко */
static int lev_dist(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 60 || lb > 60) return 99;
    int row[64], prev, tmp;
    for (int j = 0; j <= lb; j++) row[j] = j;
    for (int i = 1; i <= la; i++) {
        prev = row[0]; row[0] = i;
        for (int j = 1; j <= lb; j++) {
            tmp = row[j];
            int c = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = prev + c; if (row[j] + 1 < m) m = row[j] + 1; if (row[j - 1] + 1 < m) m = row[j - 1] + 1;
            row[j] = m; prev = tmp;
        }
    }
    return row[lb];
}
typedef struct { const char *best; int bd; } Sugg;
static void sugg_add(Sugg *s, const char *name, const char *cand) {
    if (!cand[0] || cand[0] == '_' || cand[0] == '$' || !strcmp(name, cand)) return;
    int la = (int)strlen(name), lc = (int)strlen(cand);
    if (la < 3) return;   /* для коротких имён подсказки только мешают */
    int d = lev_dist(name, cand);
    { int same = la == lc; for (int i = 0; same && i < la; i++) if (tolower((unsigned char)name[i]) != tolower((unsigned char)cand[i])) same = 0; if (same) d = 0; }
    if (d > (la + lc + 3) / 6) return;
    if (!s->best || d < s->bd) { s->best = cand; s->bd = d; }
}
static const char *suggest_env(Env *env, const char *name, int fnOnly) {
    Sugg s = { NULL, 99 };
    for (Env *e = env; e; e = e->parent)
        for (int i = 0; i < e->n; i++) {
            if (fnOnly && e->vars[i].v.t != T_FN && e->vars[i].v.t != T_NAT) continue;
            sugg_add(&s, name, e->vars[i].name->s);
        }
    return s.best;
}
static const char *suggest_list(const char *name, const char *const *list) {
    Sugg s = { NULL, 99 };
    for (int i = 0; list[i]; i++) sugg_add(&s, name, list[i]);
    return s.best;
}

static void drain_val(VM *vm, Value v, int line);
static Value call_method(VM *vm, Value recv, Str *name, Value *args, int nargs, Node *callee, Env *env, int line, int *found);
static Value get_member(VM *vm, Value o, Str *name, Node *n, Env *env);
static Str *S(VM *vm, const char *s) { return intern(vm, s, (int)strlen(s)); }

static const char *type_of(Value v) {
    if (v.t == T_OBJ && v.u.o->type) return v.u.o->type->name->s;
    return type_name(v);
}
static int is_callable(Value v) { return v.t == T_FN || v.t == T_NAT || v.t == T_TYPE; }
#define VM_OK(c, par) (!(c)->vmw || (!(par) && !vm->own && vm->wid == 0))   /* код с записью во внешние переменные не работает внутри ::par */
static int is_container(Value v) { return v.t == T_ARR || v.t == T_OBJ; }

static Str *key_str(VM *vm, Value v) { return to_str(vm, v, 1); }

/* ---------------------------------------------------------------- арифметика */
static Value want_num(VM *vm, Value v, const char *op, int line) {
    if (v.t == T_BOOL) return V_int(v.u.b);
    if (!is_num(v)) {
        if (v.t == T_ERR) rt(vm, line, "operator %s requires a number, got %s (%s)", op, type_of(v), v.u.s->s);
        rt(vm, line, "operator %s requires a number, got %s", op, type_of(v));
    }
    return v;
}
static Value nat_div(VM *vm, Value *a, int n);
static Value arith(VM *vm, int op, Value a, Value b, int line) {
    static const char *NM[] = { "+", "-", "*", "/", "%", "**" };
    a = want_num(vm, a, NM[op], line); b = want_num(vm, b, NM[op], line);
    if (a.t == T_DEC || b.t == T_DEC) return dec_arith(vm, op, a, b, line);
    static const int BOP[] = { 0, 1, 2, -1, 3, 4 };
    if ((a.t == T_BIG || b.t == T_BIG) && a.t != T_FLT && b.t != T_FLT) return big_arith(vm, BOP[op], a, b, line);
    if (a.t == T_INT && b.t == T_INT) {
        int64_t x = a.u.i, y = b.u.i, r;
        switch (op) {
            case OP_ADD: if (__builtin_add_overflow(x, y, &r)) return big_arith(vm, 0, a, b, line); return V_int(r);
            case OP_SUB: if (__builtin_sub_overflow(x, y, &r)) return big_arith(vm, 1, a, b, line); return V_int(r);
            case OP_MUL: if (__builtin_mul_overflow(x, y, &r)) return big_arith(vm, 2, a, b, line); return V_int(r);
            case OP_MOD: if (y == 0) return V_err(vm, "division by zero"); if (y == -1) return V_int(0); r = x % y; if (r != 0 && ((r < 0) != (y < 0))) r += y; return V_int(r);
            case OP_POW:
                if (y >= 0) {
                    int64_t res = 1, base = x; int64_t e = y;
                    while (e > 0) {
                        if (e & 1) { if (__builtin_mul_overflow(res, base, &res)) return big_arith(vm, 4, a, b, line); }
                        e >>= 1;
                        if (e && __builtin_mul_overflow(base, base, &base)) return big_arith(vm, 4, a, b, line);
                    }
                    return V_int(res);
                }
                if (vm->prec > 0) return dec_arith(vm, OP_POW, a, b, line);
                return V_flt(pow((double)x, (double)y));
        }
    }
    double x = as_f(a), y = as_f(b);
    switch (op) {
        case OP_ADD: return V_flt(x + y);
        case OP_SUB: return V_flt(x - y);
        case OP_MUL: return V_flt(x * y);
        case OP_MOD: if (y == 0) return V_err(vm, "division by zero"); { double r = fmod(x, y); if (r != 0 && ((r < 0) != (y < 0))) r += y; return V_flt(r); }
        case OP_POW: return V_flt(pow(x, y));
    }
    return V_none();
}
static Value add_values(VM *vm, Value a, Value b, int line) {
    if (a.t == T_INT && b.t == T_INT) { int64_t r; if (!__builtin_add_overflow(a.u.i, b.u.i, &r)) return V_int(r); }
    if (a.t == T_STR || b.t == T_STR) {
        Str *x = to_str(vm, a, 1), *y = to_str(vm, b, 1);
        int n = x->len + y->len;
        if (x->len >= 64) {   /* длинная строка: дозапись на месте в запас буфера, как у append в Go (старые значения не меняются: у каждого своя длина) */
            for (int k = 0; k < 4; k++) {
                if (vm->sbuf[k].end != x->s + x->len) continue;
                if ((size_t)(vm->sbuf[k].lim - vm->sbuf[k].end) > (size_t)y->len) {
                    char *e = vm->sbuf[k].end; memcpy(e, y->s, (size_t)y->len); e += y->len; *e = 0; vm->sbuf[k].end = e;
                    Str *r = (Str *)amalloc(vm, sizeof(Str)); r->len = n; r->s = x->s;
                    return V_str(r);
                }
                break;
            }
            size_t cap = (size_t)n * 2 + 16;
            char *buf = (char *)amalloc_nz(vm, cap);
            memcpy(buf, x->s, (size_t)x->len); memcpy(buf + x->len, y->s, (size_t)y->len); buf[n] = 0;
            int k = vm->sbi++ & 3; vm->sbuf[k].end = buf + n; vm->sbuf[k].lim = buf + cap - 1;
            Str *r = (Str *)amalloc(vm, sizeof(Str)); r->len = n; r->s = buf;
            return V_str(r);
        }
        Str *r = (Str *)amalloc_nz(vm, sizeof(Str) + (size_t)n + 1);
        r->len = n; r->ep = 0; r->s = (char *)(r + 1); memcpy(r->s, x->s, (size_t)x->len); memcpy(r->s + x->len, y->s, (size_t)y->len); r->s[n] = 0;
        return V_str(r);
    }
    if ((is_num(a) || a.t == T_BOOL) && (is_num(b) || b.t == T_BOOL)) return arith(vm, OP_ADD, a, b, line);
    if (a.t == T_ARR) {
        Arr *r = arr_new(vm, a.u.a->n + 1);
        for (int i = 0; i < a.u.a->n; i++) arr_push(vm, r, a.u.a->v[i]);
        arr_push(vm, r, b); return V_arr(r);
    }
    rt(vm, line, "cannot add %s and %s", type_of(a), type_of(b));
    return V_none();
}
static Value mul_values(VM *vm, Value a, Value b, int line) {
    if (is_num(a) && is_num(b)) return arith(vm, OP_MUL, a, b, line);
    Value s = V_none(); int64_t k = 0; int ok = 0;
    if ((a.t == T_STR || a.t == T_ARR) && b.t == T_INT) { s = a; k = b.u.i; ok = 1; }
    else if ((b.t == T_STR || b.t == T_ARR) && a.t == T_INT) { s = b; k = a.u.i; ok = 1; }
    if (!ok) rt(vm, line, "cannot multiply %s and %s", type_of(a), type_of(b));
    if (k < 0) rt(vm, line, "repeat count must be int >= 0");
    if (s.t == T_STR) {
        if ((double)s.u.s->len * (double)k > (double)vm->limit) rt(vm, line, "string too large");
        SB sb; sb_init(vm, &sb);
        for (int64_t i = 0; i < k; i++) sb_add(&sb, s.u.s->s, s.u.s->len);
        return V_str(sb_str(&sb));
    }
    if ((double)s.u.a->n * (double)k > 1e8) rt(vm, line, "array too large");
    Arr *r = arr_new(vm, (int)(s.u.a->n * k));
    for (int64_t i = 0; i < k; i++) for (int j = 0; j < s.u.a->n; j++) arr_push(vm, r, deep_copy(vm, s.u.a->v[j]));
    return V_arr(r);
}
/* порядок: числа, строки, массивы (по элементам; короткий префикс меньше). -1/0/1, 2 — несравнимо */
static int order_vals(VM *vm, Value a, Value b) {
    if (a.t == T_ARR && b.t == T_ARR) {
        int m = a.u.a->n < b.u.a->n ? a.u.a->n : b.u.a->n;
        for (int i = 0; i < m; i++) { int r = order_vals(vm, a.u.a->v[i], b.u.a->v[i]); if (r != 0) return r; }
        return a.u.a->n < b.u.a->n ? -1 : a.u.a->n > b.u.a->n ? 1 : 0;
    }
    if (a.t == T_STR && b.t == T_STR) {
        int m = a.u.s->len < b.u.s->len ? a.u.s->len : b.u.s->len; int c = memcmp(a.u.s->s, b.u.s->s, (size_t)m);
        if (!c) c = a.u.s->len - b.u.s->len;
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (is_num(a) && is_num(b)) {
        if (a.t == T_INT && b.t == T_INT) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i ? 1 : 0;
        if (a.t == T_DEC || b.t == T_DEC) { int c = dec_cmp_values(a, b); if (c != 2) return c; }
        else if (a.t != T_FLT && b.t != T_FLT) return big_cmp(vm, a, b);
        double x = as_f(a), y = as_f(b); return x < y ? -1 : x > y ? 1 : 0;
    }
    return 2;
}
static int cmp_values(VM *vm, int op, Value a, Value b, int line) {
    static const char *NM[] = { "<", ">", "<=", ">=" };
    if (a.t == T_ARR && b.t == T_ARR) {
        int c = order_vals(vm, a, b);
        if (c == 2) rt(vm, line, "cannot compare %s and %s with %s", type_of(a), type_of(b), NM[op - OP_LT]);
        switch (op) { case OP_LT: return c < 0; case OP_GT: return c > 0; case OP_LE: return c <= 0; default: return c >= 0; }
    }
    int ok = (is_num(a) && is_num(b)) || (a.t == T_STR && b.t == T_STR);
    if (ok && (a.t == T_DEC || b.t == T_DEC)) {
        int c = dec_cmp_values(a, b); if (c == 2) return 0;
        switch (op) { case OP_LT: return c < 0; case OP_GT: return c > 0; case OP_LE: return c <= 0; default: return c >= 0; }
    }
    if (ok && (a.t == T_BIG || b.t == T_BIG) && a.t != T_FLT && b.t != T_FLT) {
        int c = big_cmp(vm, a, b);
        switch (op) { case OP_LT: return c < 0; case OP_GT: return c > 0; case OP_LE: return c <= 0; default: return c >= 0; }
    }
    if (!ok) rt(vm, line, "cannot compare %s and %s with %s", type_of(a), type_of(b), NM[op - OP_LT]);
    int c;
    if (a.t == T_STR) { int m = a.u.s->len < b.u.s->len ? a.u.s->len : b.u.s->len; c = memcmp(a.u.s->s, b.u.s->s, (size_t)m); if (!c) c = a.u.s->len - b.u.s->len; }
    else if (a.t == T_INT && b.t == T_INT) c = a.u.i < b.u.i ? -1 : a.u.i > b.u.i ? 1 : 0;
    else { double x = as_f(a), y = as_f(b); if (x != x || y != y) return 0; c = x < y ? -1 : x > y ? 1 : 0; }
    switch (op) { case OP_LT: return c < 0; case OP_GT: return c > 0; case OP_LE: return c <= 0; default: return c >= 0; }
}
static Value str_slice(VM *vm, Str *s, int a, int b) { /* по символам */
    int oa = u8_off(s->s, s->len, a), ob = u8_off(s->s, s->len, b);
    return V_str(mkstr(vm, s->s + oa, ob - oa));
}
static Value binop(VM *vm, int op, Value a, Value b, int line) {
    switch (op) {
        case OP_ADD: return add_values(vm, a, b, line);
        case OP_MUL: return mul_values(vm, a, b, line);
        case OP_SUB:
            if (a.t == T_STR) {
                if (b.t != T_INT || b.u.i < 0) rt(vm, line, "string - k: k must be int >= 0%s", b.t == T_STR ? "" : "");
                int L = u8_count(a.u.s->s, a.u.s->len);
                if (b.u.i > L) rt(vm, line, "cannot remove %lld characters: string length is %d", (long long)b.u.i, L);
                return str_slice(vm, a.u.s, 0, L - (int)b.u.i);
            }
            if (a.t == T_ARR) {
                if (b.t != T_INT || b.u.i < 0) rt(vm, line, "array - k: k must be int >= 0");
                if (b.u.i > a.u.a->n) rt(vm, line, "cannot remove %lld elements: array length is %d", (long long)b.u.i, a.u.a->n);
                Arr *r = arr_new(vm, a.u.a->n);
                for (int i = 0; i < a.u.a->n - (int)b.u.i; i++) arr_push(vm, r, a.u.a->v[i]);
                return V_arr(r);
            }
            return arith(vm, OP_SUB, a, b, line);
        case OP_DIV: {
            a = want_num(vm, a, "/", line); b = want_num(vm, b, "/", line);
            if (a.t == T_DEC || b.t == T_DEC || vm->prec > 0) return dec_arith(vm, OP_DIV, a, b, line);
            double x = as_f(a), y = as_f(b);
            if (y == 0) return V_err(vm, "division by zero");
            return V_flt(x / y);
        }
        case OP_MOD: case OP_POW: return arith(vm, op, a, b, line);
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
            static const char *nm[] = { "&", "|", "^", "<<", ">>" };
            const char *o = nm[op - OP_BAND];
            if ((a.t != T_INT && a.t != T_BIG) || (b.t != T_INT && b.t != T_BIG)) rt(vm, line, "operator %s requires int, got %s", o, type_of((a.t != T_INT && a.t != T_BIG) ? a : b));
            if (op == OP_SHL || op == OP_SHR) {
                if (b.t == T_BIG ? big_cmp(vm, b, V_int(0)) < 0 : b.u.i < 0) rt(vm, line, "negative shift count");
                if (b.t == T_BIG || b.u.i > 65536) rt(vm, line, "shift count too large");
                int64_t k = b.u.i;
                if (op == OP_SHR) {
                    if (a.t == T_BIG) { Value ar[2]; ar[0] = a; ar[1] = arith(vm, OP_POW, V_int(2), b, line); return nat_div(vm, ar, 2); }
                    return V_int(k >= 64 ? (a.u.i < 0 ? -1 : 0) : (a.u.i >> k));
                }
                if (a.t == T_INT && k < 63) { int64_t r; if (!__builtin_mul_overflow(a.u.i, (int64_t)1 << k, &r)) return V_int(r); }
                return arith(vm, OP_MUL, a, arith(vm, OP_POW, V_int(2), b, line), line);
            }
            if (a.t == T_BIG || b.t == T_BIG) rt(vm, line, "operator %s is not supported for integers beyond 64 bits", o);
            return V_int(op == OP_BAND ? (a.u.i & b.u.i) : op == OP_BOR ? (a.u.i | b.u.i) : (a.u.i ^ b.u.i));
        }
        case OP_EQ: return V_bool(val_eq(a, b));
        case OP_NE: return V_bool(!val_eq(a, b));
        default: return V_bool(cmp_values(vm, op, a, b, line));
    }
}

/* ---------------------------------------------------------------- итерация */
typedef struct Iter Iter;
enum { SK_RANGE, SK_LINES, SK_FROM, SK_MAP, SK_FILTER, SK_TAKE, SK_SKIP, SK_TAKEW, SK_SKIPW, SK_ENUM, SK_FLAT, SK_ZIP, SK_PAR, SK_UNIQUE, SK_CHUNK, SK_WINDOW, SK_WORDS, SK_INTS, SK_EXPAND, SK_EXPANDB, SK_SCAN, SK_FLATMAP, SK_CYCLE, SK_REPEAT, SK_ARR = 100 };
struct Iter { int kind, line, done, flag, hasE; Iter *up, *o; Value f, x; int64_t i, k, e, st; Arr *a, *buf, *seen; Obj *vis; Iter **fr; Value *nd; int nfr, cfr; Env *renv; Clause *fc; Env *fenv; int fk; };
static Iter *iter_new(VM *vm, Value v, int line);
static int iter_next(VM *vm, Iter *it, Value *out);
static Value stream_materialize(VM *vm, Value v);
static int range_contains(Value st, Value a, int *known);
static Value spawn_task(VM *vm, Clause *c, Env *env, int line);
static Value await_value(VM *vm, Value v, int line);
static int stream_stateful(Value v);
static int stream_arrchain(Value v);
static void iter_of(VM *vm, Value v, Value *tmp, int line) {
    if (v.t == T_ARR) { *tmp = v; return; }
    if (v.t == T_STREAM) { *tmp = stream_materialize(vm, v); return; }
    if (v.t == T_STR) {
        Arr *a = arr_new(vm, v.u.s->len);
        for (int i = 0; i < v.u.s->len;) { int l = u8_len((unsigned char)v.u.s->s[i]); if (i + l > v.u.s->len) l = v.u.s->len - i; arr_push(vm, a, V_str(mkstr(vm, v.u.s->s + i, l))); i += l; }
        *tmp = V_arr(a); return;
    }
    rt(vm, line, "cannot iterate over %s", type_of(v));
}

/* ---------------------------------------------------------------- типы */
static int is_typename(const char *s) {
    static const char *T[] = { "number", "int", "float", "string", "bool", "none", "array", "object", "function", "error", "stream", NULL };
    for (int i = 0; T[i]; i++) if (!strcmp(s, T[i])) return 1;
    return 0;
}
static int match_typename(const char *nm, Value v) {
    if (!strcmp(nm, "number")) return is_num(v);
    if (!strcmp(nm, "object")) return v.t == T_OBJ;
    if (!strcmp(nm, "stream")) return v.t == T_STREAM;
    return !strcmp(type_of(v), nm) && !(v.t == T_OBJ && v.u.o->type);
}
static int has_type(Value v, TypeDef *td) { return v.t == T_OBJ && v.u.o->type == td; }

/* ---------------------------------------------------------------- шаблоны */
static void destructure(VM *vm, Pat *p, Value v, Env *env, int mut, int line) {
    switch (p->k) {
        case P_ID: if (strcmp(p->name->s, "_") != 0) env_def(vm, env, p->name, v, mut); return;
        case P_OBJ:
            if (v.t != T_OBJ) rt(vm, line, "cannot destructure %s as object: expected object", type_of(v));
            for (int i = 0; i < p->n; i++) {
                int j = obj_find(v.u.o, p->keys[i]);
                destructure(vm, p->items[i], j >= 0 ? v.u.o->v[j] : V_none(), env, mut, line);
            }
            return;
        case P_ARR:
            if (v.t != T_ARR) rt(vm, line, "cannot destructure %s as array: expected array", type_of(v));
            for (int i = 0; i < p->n; i++) destructure(vm, p->items[i], i < v.u.a->n ? v.u.a->v[i] : V_none(), env, mut, line);
            if (p->rest) {
                Arr *r = arr_new(vm, 4);
                for (int i = p->n; i < v.u.a->n; i++) arr_push(vm, r, v.u.a->v[i]);
                env_def(vm, env, p->rest, V_arr(r), mut);
            }
            return;
        default: rt(vm, line, "unsupported pattern");
    }
}
static int match_pat(VM *vm, Pat *p, Value v, Env *env, int line) {
    switch (p->k) {
        case P_ID: if (strcmp(p->name->s, "_") != 0) env_def(vm, env, p->name, v, 0); return 1;
        case P_TYPED: {
            Var *b = env_find(env, p->type, NULL); int ok = 0;
            if (b && b->v.t == T_TYPE) ok = has_type(v, b->v.u.td);
            else if (is_typename(p->type->s)) ok = match_typename(p->type->s, v);
            else rt(vm, line, "unknown type \"%s\" in function header", p->type->s);
            return ok && match_pat(vm, p->sub, v, env, line);
        }
        case P_LIT: return val_eq(ev(vm, p->expr, env), v);
        case P_COND: {
            Env *e2 = env_new(vm, env, 0); env_def(vm, e2, S(vm, "$subject"), v, 0);
            Value r = ev(vm, p->expr, e2); env_release(vm, e2);
            if (r.t != T_BOOL) rt(vm, line, "pattern condition must be bool, got %s", type_of(r));
            return r.u.b;
        }
        case P_OBJ:
            if (v.t != T_OBJ) return 0;
            for (int i = 0; i < p->n; i++) {
                int j = obj_find(v.u.o, p->keys[i]);
                if (j < 0) return 0;
                if (!match_pat(vm, p->items[i], v.u.o->v[j], env, line)) return 0;
            }
            return 1;
        case P_ARR:
            if (v.t != T_ARR) return 0;
            if (p->rest ? v.u.a->n < p->n : v.u.a->n != p->n) return 0;
            for (int i = 0; i < p->n; i++) if (!match_pat(vm, p->items[i], v.u.a->v[i], env, line)) return 0;
            if (p->rest) {
                Arr *r = arr_new(vm, 4);
                for (int i = p->n; i < v.u.a->n; i++) arr_push(vm, r, v.u.a->v[i]);
                env_def(vm, env, p->rest, V_arr(r), 0);
            }
            return 1;
    }
    return 0;
}
static int has_lit(Pat *p) {
    if (!p) return 0;
    if (p->k == P_LIT || p->k == P_COND || p->k == P_TYPED) return 1;
    if (p->k == P_OBJ || p->k == P_ARR) { for (int i = 0; i < p->n; i++) if (has_lit(p->items[i])) return 1; }
    return 0;
}

/* ---------------------------------------------------------------- функции */
static Value mk_fn(VM *vm, Clause *cl, int ncl, Env *env, Str *name) {
    Fn *f = (Fn *)amalloc(vm, sizeof(Fn));
    f->cl = cl; f->ncl = ncl; f->env = env; f->name = name;
    f->multi = ncl > 1;
    for (int i = 0; i < ncl && !f->multi; i++) for (int j = 0; j < cl[i].np; j++) if (has_lit(cl[i].params[j])) f->multi = 1;
    env_capture(env);
    Value v = V_none(); v.t = T_FN; v.u.fn = f; return v;
}
static Value run_clause(VM *vm, Clause *c, Env *env) {
    int lc = vm->loopcall; vm->loopcall = 0;
    Value r = c->isBlock ? exec_block(vm, c->body, c->nbody, env) : ev(vm, c->expr, env);
    if (__builtin_expect(vm->ctl == 3, 0) && !lc) { vm->ctl = 0; r = vm->retv; }   /* return из ветки: значение функции */
    return r;
}
static Value construct(VM *vm, TypeDef *td, Value *args, int n, int line) {
    if (n > td->n) rt(vm, line, "%s takes at most %d arguments, got %d", td->name->s, td->n, n);
    Obj *o = obj_new(vm); o->type = td;
    for (int i = 0; i < td->n; i++) {
        if (i < n) obj_set(vm, o, td->fields[i], args[i]);
        else if (td->defs[i]) obj_set(vm, o, td->fields[i], ev(vm, td->defs[i], td->env));
        else rt(vm, line, "missing argument \"%s\" in %s", td->fields[i]->s, td->name->s);
    }
    return V_obj(o);
}
typedef Value (*NatFn)(VM *, Value *, int);
#define MAX_NATS 128
static NatFn NATS[MAX_NATS];
static const char *NATNAMES[MAX_NATS];
static const char *nat_name(int id) { return NATNAMES[id] ? NATNAMES[id] : "builtin"; }
static int NNATS = 0;
static Value V_nat(int id) { Value v = V_none(); v.t = T_NAT; v.u.nat = id; return v; }

static void clause_classify(Clause *c) {
    int ok = c->np <= 6;
    for (int i = 0; ok && i < c->np; i++) { if (c->params[i]->k != P_ID) ok = 0; for (int j = 0; ok && j < i; j++) if (c->params[j]->name == c->params[i]->name) ok = 0; }
    __atomic_store_n(&c->simple, ok ? 1 : 2, __ATOMIC_RELAXED);
}
static Value call_fn(VM *vm, Value fv, Value *args, int nargs, int par, int line, int *muts) {
    if (fv.t == T_TYPE) return construct(vm, fv.u.td, args, nargs, line);
    if (fv.t == T_NAT) { if (line > 0) vm->curline = line; return NATS[fv.u.nat](vm, args, nargs); }
    if (fv.t != T_FN) {
        SB sb; sb_init(vm, &sb); repr_to(&sb, fv, 0);
        rt(vm, line, "expected a function, got %s %s", type_of(fv), sb.b ? sb.b : "");
    }
    Fn *f = fv.u.fn;
    if (f->target) {
        if (f->mname && f->nb == -1) {   /* несвязанный метод: первый аргумент — получатель */
            if (nargs < 1) rt(vm, line, "method .%s needs a value to call it on", f->mname->s);
            int found; Value r = call_method(vm, args[0], f->mname, args + 1, nargs - 1, NULL, NULL, line, &found);
            if (!found) rt(vm, line, "%s has no method .%s", type_of(args[0]), f->mname->s);
            return r;
        }
        if (f->mname) { int found; Value r = call_method(vm, *f->target, f->mname, args, nargs, NULL, NULL, line, &found); if (!found) rt(vm, line, "%s has no method .%s", type_of(*f->target), f->mname->s); return r; }
        if (nargs) rt(vm, f->bline, "bound call f:(...) takes no arguments, got %d", nargs);
        return call_fn(vm, *f->target, f->bargs, f->nb, 0, f->bline, NULL);
    }
    if (!f->multi && !f->cl[0].async) {
        Clause *c = &f->cl[0];
        if (!c->simple) clause_classify(c);
        if (c->simple == 1) {
            vm->inl = 0;
            if (!c->vmstate && vm->wid == 0) vm_try_compile(vm, c, f->env);
            if (c->vmstate == 1 && VM_OK(c, par)) {
                if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
                Value r = vm_exec(vm, c->code, f->env, args, nargs);
                vm->depth--;
                return r;
            }
            if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
            Env *env = env_new(vm, f->env, par);
            int np = c->np;
            for (int i = 0; i < np; i++) {
                Var *x = &env->inl[i]; Value a = i < nargs ? args[i] : V_none();
                x->name = c->params[i]->name; x->v = a; x->mut = 1; x->param = 1;
                x->frozen = muts && i < nargs && muts[i] == 0 && is_container(a);
            }
            env->n = np;
            Value result = run_clause(vm, c, env);
            env_release(vm, env);
            vm->depth--;
            return result;
        }
    }
    int inl = vm->inl; vm->inl = 0;
    if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
    Value result;
    if (!f->multi) {
        Clause *c = &f->cl[0];
        Env *env = env_new(vm, f->env, par);
        for (int i = 0; i < c->np; i++) {
            Pat *p = c->params[i]; Value a = i < nargs ? args[i] : V_none();
            if (p->k == P_ID) {
                Var *x = env_def(vm, env, p->name, a, 1); x->param = 1;
                x->frozen = muts && i < nargs && muts[i] == 0 && is_container(a);
            } else destructure(vm, p, a, env, 1, line);
        }
        if (c->async && !inl) { vm->depth--; return spawn_task(vm, c, env, line); }
        result = run_clause(vm, c, env);
        env_release(vm, env);
    } else {
        int done = 0; result = V_none();
        for (int ci = 0; ci < f->ncl && !done; ci++) {
            Clause *c = &f->cl[ci];
            Env *env = env_new(vm, f->env, par);
            int ok = 1;
            for (int i = 0; i < c->np; i++) if (!match_pat(vm, c->params[i], i < nargs ? args[i] : V_none(), env, line)) { ok = 0; break; }
            if (!ok) { env_release(vm, env); continue; }
            for (int i = 0; i < env->n; i++) { env->vars[i].mut = 1; env->vars[i].param = 1; }
            if (muts) for (int i = 0; i < c->np && i < nargs; i++) {
                Pat *p = c->params[i]; if (p->k == P_TYPED) p = p->sub;
                if (p->k == P_ID && muts[i] == 0) { Var *x = env_own(env, p->name); if (x && is_container(x->v)) x->frozen = 1; }
            }
            if (c->async && !inl) { vm->depth--; return spawn_task(vm, c, env, line); }
            result = run_clause(vm, c, env); env_release(vm, env); done = 1;
        }
        if (!done) {
            SB sb; sb_init(vm, &sb);
            for (int i = 0; i < nargs; i++) { if (i) sb_cs(&sb, ", "); repr_to(&sb, args[i], 0); }
            rt(vm, line, "no matching clause in %s for arguments (%s)", f->name ? f->name->s : "function", sb.b ? sb.b : "");
        }
    }
    vm->depth--;
    return result;
}

/* ---------------------------------------------------------------- операторы */
static void hoist(VM *vm, Node **st, int n, Env *env) {
    for (int i = 0; i < n; i++) {
        Node *s = st[i];
        if (s->type == N_FNDECL) {
            Var *ex = env_own(env, s->name);
            if (ex) {
                if (ex->v.t == T_FN && ex->v.u.fn->multi && s->flag) {
                    Fn *f = ex->v.u.fn;
                    Clause *nc = (Clause *)amalloc(vm, sizeof(Clause) * (size_t)(f->ncl + s->ncl));
                    memcpy(nc, f->cl, sizeof(Clause) * (size_t)f->ncl); memcpy(nc + f->ncl, s->cl, sizeof(Clause) * (size_t)s->ncl);
                    f->cl = nc; f->ncl += s->ncl; continue;
                }
                rt(vm, s->line, "name \"%s\" is already defined in this scope", s->name->s);
            }
            env_def(vm, env, s->name, mk_fn(vm, s->cl, s->ncl, env, s->name), 0);
        } else if (s->type == N_TYPEDECL) {
            if (env_own(env, s->name)) rt(vm, s->line, "name \"%s\" is already defined in this scope", s->name->s);
            TypeDef *td = (TypeDef *)amalloc(vm, sizeof(TypeDef));
            td->name = s->name; td->n = s->n; td->fields = s->names; td->defs = s->list; td->env = env;
            env_capture(env);
            Value v = V_none(); v.t = T_TYPE; v.u.td = td;
            env_def(vm, env, s->name, v, 0);
        }
    }
}

static Node *root_ident(Node *t) {
    while (t && (t->type == N_MEMBER || t->type == N_INDEX)) t = t->a;
    return t && t->type == N_IDENT ? t : NULL;
}
static int is_mut_path(VM *vm, Node *node, Env *env) {
    Node *r = root_ident(node);
    if (!r) return 1;
    int cr = 0; Var *f = env_find_cx(env, r, &cr);
    if (f && vm->own && (cr & 1)) return 0;   /* в параллельном ::par внешние данные только для чтения */
    return !f || (f->mut && !f->frozen);
}
static void check_writable(VM *vm, Var *f, int crossed, Str *name, int line) {
    if (!f->mut) rt(vm, line, "\"%s\" is a constant (declared with =)", name->s);
    if (crossed & 1) rt(vm, line, "cannot assign to outer variable \"%s\" inside ::par", name->s);
}
static void check_content(VM *vm, Var *f, int crossed, Str *name, int line) {
    check_writable(vm, f, crossed, name, line);
    if (f->frozen) rt(vm, line, "\"%s\" is a constant argument: its contents cannot be modified", name->s);
}
static void require_mutable(VM *vm, Node *node, Env *env, int line) {
    Node *r = root_ident(node);
    if (!r) rt(vm, line, "cannot modify a value that is not stored in a mutable variable");
    int cr = 0; Var *f = env_find(env, r->name, &cr);
    if (!f) rt(vm, line, "undefined variable \"%s\"", r->name->s);
    check_content(vm, f, cr, r->name, line);
}
static void check_alias(VM *vm, Node *n, Value val, Env *env) {
    if (n->b->type != N_IDENT || !is_container(val)) return;
    int cr = 0; Var *src = env_find(env, n->b->name, &cr);
    if (src && vm->own && (cr & 1) && n->op == 1) rt(vm, n->line, "cannot alias outer variable \"%s\" as mutable inside ::par", n->b->name->s);
    if (!src || src->param) return;
    const char *t = n->a->name->s, *s = n->b->name->s;
    if (n->op == 0 && src->mut) rt(vm, n->line, "cannot alias mutable \"%s\" as constant \"%s\"", s, t);
    if (n->op == 1 && !src->mut) rt(vm, n->line, "cannot alias constant \"%s\" as mutable \"%s\"", s, t);
}
static Value compound(VM *vm, int op, Value cur, Value val, int line) {
    if (cur.t == T_INT && val.t == T_INT) {
        int64_t r;
        if (op == 2) { if (!__builtin_add_overflow(cur.u.i, val.u.i, &r)) return V_int(r); }
        else if (op == 3) { if (!__builtin_sub_overflow(cur.u.i, val.u.i, &r)) return V_int(r); }
        else if (op == 4) { if (!__builtin_mul_overflow(cur.u.i, val.u.i, &r)) return V_int(r); }
    }
    if (op == 2) {
        if (cur.t == T_ARR) { arr_push(vm, cur.u.a, val); return cur; }
        return binop(vm, OP_ADD, cur, val, line);
    }
    if (op == 3) {
        if (cur.t == T_ARR) {
            if (val.t != T_INT || val.u.i < 0) rt(vm, line, "array -= k: k must be int >= 0");
            if (val.u.i > cur.u.a->n) rt(vm, line, "cannot remove %lld elements: array length is %d", (long long)val.u.i, cur.u.a->n);
            cur.u.a->n -= (int)val.u.i; return cur;
        }
        return binop(vm, OP_SUB, cur, val, line);
    }
    return binop(vm, op == 4 ? OP_MUL : OP_DIV, cur, val, line);
}

static Value ev_assign_slow(VM *vm, Node *n, Env *env, Value val);
static inline __attribute__((always_inline)) Value ev_assign_core(VM *vm, Node *n, Env *env, Value val);
static Value ev_assign(VM *vm, Node *n, Env *env) { Value v = evq(vm, n->b, env); if (__builtin_expect(vm->ctl, 0)) return V_none(); return ev_assign_core(vm, n, env, v); }
static Value ev_assign_v(VM *vm, Node *n, Env *env, Value val) { return ev_assign_core(vm, n, env, val); }
static inline __attribute__((always_inline)) Value ev_assign_core(VM *vm, Node *n, Env *env, Value val) {
    Node *t = n->a;
    if (t->type == N_IDENT && val.t == T_INT && n->op >= 1 && n->op <= 4) { /* x = v / x += v / x -= v / x *= v над целыми: без лишних проверок */
        int cr = 0; Var *f = env_find_cx(env, t, &cr);
        if (f && f->mut && !(cr & 1)) {
            if (n->op == 1) { f->v = val; return val; }
            if (f->v.t == T_INT) {
                int64_t r; int64_t x = f->v.u.i, y = val.u.i;
                int ok = n->op == 2 ? !__builtin_add_overflow(x, y, &r) : n->op == 3 ? !__builtin_sub_overflow(x, y, &r) : !__builtin_mul_overflow(x, y, &r);
                if (ok) { f->v.u.i = r; return f->v; }
            }
        }
    }
    if (t->type == N_INDEX && n->op == 1 && t->a->type == N_IDENT && (t->b->type == N_IDENT || t->b->type == N_INT)) { /* a[i] := v для массива в изменяемой переменной */
        int cr = 0; Var *f = env_find_cx(env, t->a, &cr);
        if (f && f->v.t == T_ARR && f->mut && !f->frozen && !(cr & 1)) {
            Value idx = evq(vm, t->b, env);
            Arr *a = f->v.u.a;
            if (idx.t == T_INT && idx.u.i >= 0 && idx.u.i < a->n) {
                if (vm->nmk && is_ptr_val(val)) esc_container(vm, a);
                a->v[idx.u.i] = val; return val;
            }
        }
    }
    if (t->type == N_MEMBER && n->op >= 1 && n->op <= 4 && t->a->type == N_IDENT) {   /* o.f = v / o.f += v для существующего поля объекта в изменяемой переменной */
        int cr = 0; Var *f = env_find_cx(env, t->a, &cr);
        if (f && f->v.t == T_OBJ && f->mut && !f->frozen && !(cr & 1)) {
            Obj *o = f->v.u.o; int j = obj_find(o, t->name);
            if (j >= 0) {
                Value nv = n->op == 1 ? val : compound(vm, n->op, o->v[j], val, n->line);
                if (vm->nmk && is_ptr_val(nv)) esc_container(vm, o);
                o->v[j] = nv; return nv;
            }
        }
    }
    return ev_assign_slow(vm, n, env, val);
}
__attribute__((noinline)) static Value ev_assign_slow(VM *vm, Node *n, Env *env, Value val) {
    Node *t = n->a; int op = n->op, line = n->line;
    if (t->type == N_IDENT) {
        Str *name = t->name;
        check_alias(vm, n, val, env);
        if (op == 0) {
            Var *own = env_own(env, name);
            if (own) rt(vm, line, own->mut ? "\"%s\" is already declared as mutable" : "\"%s\" is already declared as a constant", name->s);
            Var *outer = env_find(env, name, NULL);
            if (outer && outer->mut) rt(vm, line, "\"%s\" is a mutable variable of an outer scope", name->s);
            env_def(vm, env, name, val, 0); return val;
        }
        int cr = 0; Var *f = env_find_cx(env, t, &cr);
        if (op == 1) {
            if (!f || (f >= vm->globals->vars && f < vm->globals->vars + vm->globals->n)) { env_def(vm, env, name, val, 1); return val; }   /* встроенные имена (sum, max, comb…) можно перекрыть своей переменной */
            check_writable(vm, f, cr, name, line); if (vm->nmk && is_ptr_val(val)) esc_var(vm, cr >> 1); f->v = val; return val;
        }
        if (!f) rt(vm, line, "undefined variable \"%s\"", name->s);
        if (f->v.t == T_ARR) check_content(vm, f, cr, name, line); else check_writable(vm, f, cr, name, line);
        f->v = compound(vm, op, f->v, val, line);
        if (vm->nmk && is_ptr_val(f->v)) esc_var(vm, cr >> 1);
        return f->v;
    }
    if (op == 0) rt(vm, line, "cannot assign to a field or element with =");
    Node *root = root_ident(t);
    if (!root) rt(vm, line, "cannot assign to an expression result");
    int cr = 0; Var *f = env_find_cx(env, root, &cr);
    if (!f) rt(vm, line, "undefined variable \"%s\"", root->name->s);
    check_content(vm, f, cr, root->name, line);
    Value cont = ev(vm, t->a, env);
    if (t->type == N_MEMBER) {
        if (cont.t != T_OBJ) rt(vm, line, "cannot assign a field of %s", type_of(cont));
        int j = obj_find(cont.u.o, t->name);
        Value cur = j >= 0 ? cont.u.o->v[j] : V_none();
        Value nv = op == 1 ? val : compound(vm, op, cur, val, line);
        obj_set(vm, cont.u.o, t->name, nv);
        return nv;
    }
    Value idx = ev(vm, t->b, env);
    if (cont.t == T_ARR) {
        if (idx.t != T_INT) rt(vm, line, "array index must be int, got %s", type_of(idx));
        int64_t i = idx.u.i < 0 ? cont.u.a->n + idx.u.i : idx.u.i;
        if (i < 0 || i >= cont.u.a->n) rt(vm, line, "index %lld out of range for array of length %d", (long long)idx.u.i, cont.u.a->n);
        if (vm->nmk && is_ptr_val(val)) esc_container(vm, cont.u.a);
        cont.u.a->v[i] = op == 1 ? val : compound(vm, op, cont.u.a->v[i], val, line);
        return cont.u.a->v[i];
    }
    if (cont.t == T_OBJ) {
        Str *k = key_str(vm, idx); k = intern(vm, k->s, k->len);
        int j = obj_find(cont.u.o, k);
        Value cur = j >= 0 ? cont.u.o->v[j] : V_none();
        Value nv = op == 1 ? val : compound(vm, op, cur, val, line);
        obj_set(vm, cont.u.o, k, nv);
        return nv;
    }
    rt(vm, line, "cannot assign an element of %s", type_of(cont));
    return V_none();
}

static Value do_return(VM *vm, Node *s, Env *env) {
    Value v = s->a ? ev(vm, s->a, env) : V_none();
    if (vm->ctl) return V_none();
    vm->retv = v; vm->ctl = 3;
    if (vm->nmk && is_ptr_val(v)) esc_var(vm, 0);
    return V_none();
}
static void drain_val(VM *vm, Value v, int line) {
    if (v.t != T_STREAM) return;
    Iter *it = iter_new(vm, v, line); Value x; while (iter_next(vm, it, &x)) {}
}
/* Тело блока: значение — из return либо значение последнего выражения (если последний оператор — присваивание/распаковка/объявление, то none). */
static Value exec_block(VM *vm, Node **st, int n, Env *env) {
    if (n == 1) {   /* один оператор: без объявлений и разбора списка */
        Node *s = st[0];
        if (s->type == N_ASSIGN) { vm->curline = s->line; return ev_assign(vm, s, env); }
        if (s->type == N_RETURN) { vm->curline = s->line; if (s->flag) return do_return(vm, s, env); return s->a ? ev(vm, s->a, env) : V_none(); }
        if (s->type != N_DESTR && s->type != N_FNDECL && s->type != N_TYPEDECL) {
            vm->curline = s->line;
            return ev(vm, s, env);   /* поток как значение блока возвращается; доедает его тот, кто значение отбрасывает */
        }
    }
    hoist(vm, st, n, env);
    Value last = V_none();
    for (int i = 0; i < n; i++) {
        Node *s = st[i];
        vm->curline = s->line;
        switch (s->type) {
            case N_RETURN: { if (s->flag) return do_return(vm, s, env); return s->a ? ev(vm, s->a, env) : V_none(); }
            case N_DESTR: {
                Value v = ev(vm, s->a, env);
                destructure(vm, s->pat, v, env, s->flag, s->line);
                last = V_none(); break;
            }
            case N_FNDECL: case N_TYPEDECL: last = V_none(); break;
            default: {
                last = ev(vm, s, env);
                if (__builtin_expect(vm->ctl, 0)) return V_none();   /* next/stop: остальные операторы блока не выполняются */
                if (last.t == T_STREAM && s->type != N_ASSIGN && i < n - 1) { Iter *it = iter_new(vm, last, s->line); Value x; while (iter_next(vm, it, &x)) {} last = V_none(); }
            }
        }
    }
    return last;
}

/* ---------------------------------------------------------------- выражения */
static Value ev_index(VM *vm, Value o, Value i, int line) {
    if (o.t == T_ARR || o.t == T_STR) {
        if (i.t != T_INT) rt(vm, line, "index must be int, got %s%s", type_of(i), i.t == T_FLT ? "" : "");
        int L = o.t == T_STR ? u8_count(o.u.s->s, o.u.s->len) : o.u.a->n;
        int64_t k = i.u.i < 0 ? L + i.u.i : i.u.i;
        if (k < 0 || k >= L) return V_none();
        if (o.t == T_ARR) return o.u.a->v[k];
        return str_slice(vm, o.u.s, (int)k, (int)k + 1);
    }
    if (o.t == T_OBJ) {
        Str *k = key_str(vm, i); int j = obj_find(o.u.o, k);
        return j >= 0 ? o.u.o->v[j] : V_none();
    }
    rt(vm, line, "cannot index %s", type_of(o));
    return V_none();
}
static int slice_bound(VM *vm, Node *nd, Env *env, int dflt, int line) {
    if (!nd) return dflt;
    Value v = ev(vm, nd, env);
    if (v.t != T_INT) rt(vm, line, "slice bounds must be int, got %s", type_of(v));
    if (v.u.i > 1000000000) return 1000000000;
    if (v.u.i < -1000000000) return -1000000000;
    return (int)v.u.i;
}
static Value ev_slice(VM *vm, Node *n, Env *env) {
    Value o = ev(vm, n->a, env);
    int isS = o.t == T_STR;
    if (!isS && o.t != T_ARR) rt(vm, n->line, "slice requires an array or string, got %s", type_of(o));
    int L = isS ? u8_count(o.u.s->s, o.u.s->len) : o.u.a->n;
    int a = slice_bound(vm, n->b, env, 0, n->line), b = slice_bound(vm, n->c, env, L, n->line);
    a = a < 0 ? (L + a < 0 ? 0 : L + a) : (a > L ? L : a);
    b = b < 0 ? (L + b < 0 ? 0 : L + b) : (b > L ? L : b);
    if (a >= b) return isS ? V_str(vm->s_empty) : V_arr(arr_new(vm, 0));
    if (isS) return str_slice(vm, o.u.s, a, b);
    Arr *r = arr_new(vm, b - a);
    for (int i = a; i < b; i++) arr_push(vm, r, o.u.a->v[i]);
    return V_arr(r);
}
static int ev_in(VM *vm, Value a, Value b, int line) {
    if (b.t == T_ARR) { for (int i = 0; i < b.u.a->n; i++) if (val_eq(b.u.a->v[i], a)) return 1; return 0; }
    if (b.t == T_STR) {
        if (a.t != T_STR) rt(vm, line, "\"x in string\" requires a string on the left, got %s", type_of(a));
        return mem_find(b.u.s->s, b.u.s->len, a.u.s->s, a.u.s->len) != NULL;
    }
    if (b.t == T_OBJ) return obj_find(b.u.o, key_str(vm, a)) >= 0;
    if (b.t == T_STREAM) {
        int known = 0; int r = range_contains(b, a, &known);
        if (known) return r;
        Iter *it = iter_new(vm, b, line); Value y;
        while (iter_next(vm, it, &y)) if (val_eq(y, a)) return 1;
        return 0;
    }
    rt(vm, line, "\"in\" is not supported for %s", type_of(b));
    return 0;
}
static Value ev_is_expr(VM *vm, Node *n, Env *env) {
    for (int i = 0; i < n->n; i++) {
        Node *arm = n->list[i];
        if (arm->a) {
            Value r = evq(vm, arm->a, env);
            if (r.t != T_BOOL) rt(vm, arm->line, "is condition must be bool, got %s", type_of(r));
            if (!r.u.b) continue;
        }
        if (arm->flag) { Env *e = env_new(vm, env, 0); Value r = exec_block(vm, arm->list, arm->n, e); env_release(vm, e); return r; }
        return ev(vm, arm->b, env);
    }
    return V_none();
}
static Env *match_arm(VM *vm, Node *arm, Value subj, Env *env) {
    Env *aenv = env_new(vm, env, 0);
    env_def(vm, aenv, S(vm, "$subject"), subj, 0);
    int ok;
    if (arm->op == 2) ok = 1;
    else if (arm->op == 1) ok = match_pat(vm, arm->pat, subj, aenv, arm->line);
    else {
        Node *ex = arm->a;
        Var *tb = ex->type == N_IDENT ? env_find(env, ex->name, NULL) : NULL;
        if (tb && tb->v.t == T_TYPE) ok = has_type(subj, tb->v.u.td);
        else if (ex->type == N_IDENT && is_typename(ex->name->s)) ok = match_typename(ex->name->s, subj);
        else { Value r = ev(vm, ex, aenv); ok = r.t == T_BOOL ? r.u.b : val_eq(r, subj); }
    }
    if (!ok) { env_release(vm, aenv); return NULL; }
    return aenv;
}
static Value ev_match(VM *vm, Node *n, Env *env) {
    Value subj = ev(vm, n->a, env);
    for (int i = 0; i < n->n; i++) {
        Node *arm = n->list[i];
        Env *ae = match_arm(vm, arm, subj, env);
        if (!ae) continue;
        Value r = arm->flag ? exec_block(vm, arm->list, arm->n, ae) : ev(vm, arm->b, ae);
        env_release(vm, ae);
        return r;
    }
    return V_none();
}

/* итерация цикла ::: может освободить свою память: mk_push ставит отметку, mk_pop откатывает арену, если ничего не утекло */
static inline int mk_push(VM *vm) {
    if (vm->norewind || vm->nmk >= 8 || !vm->blocks) return 0;
    Mark *m = &vm->mk[vm->nmk++];
    m->blk = vm->blocks; m->used = vm->blocks->used; m->id = ++vm->iter_seq; m->esc = 0;
    vm->iter = m->id;
    return 1;
}
static void mk_pop_slow(VM *vm, int d);
static inline void mk_pop(VM *vm) {
    int d = --vm->nmk;
    Mark *m = &vm->mk[d];
    if (m->esc || !(vm->blocks == m->blk && m->blk->used == m->used)) mk_pop_slow(vm, d);
    vm->iter = d ? vm->mk[d - 1].id : 0;
}
__attribute__((noinline)) static void mk_pop_slow(VM *vm, int d) {
    Mark *m = &vm->mk[d];
    if (!m->esc && !(vm->blocks == m->blk && m->blk->used == m->used)) {
        vm->nmk = d + 1;                                   /* in_region смотрит на отметку d */
        Env **pp = &vm->freeenv;
        while (*pp) { if (in_region(vm, d, *pp)) *pp = (*pp)->parent; else pp = &(*pp)->parent; }
        vm->nmk = d;
        Block *mb = m->blk;
        while (vm->blocks != mb) {
            Block *b = vm->blocks; vm->blocks = b->next; vm->total -= b->cap;
            if (vm->poison) memset(b + 1, 0xDD, b->cap);
            free(b);
        }
        if (vm->poison) memset((char *)(mb + 1) + m->used, 0xDD, mb->used - m->used);
        mb->used = m->used;
        memset(vm->sbuf, 0, sizeof vm->sbuf);   /* буфер дозаписи мог лежать в освобождённой области */
    }
}
static Value ev_foreach_body(VM *vm, Node *n, Value src, Value f);
static Value ev_foreach(VM *vm, Node *n, Env *env) {
    Value src = n->a->type == N_CALL ? ev_call(vm, n->a, env) : ev(vm, n->a, env);
    Value f = ev(vm, n->b, env);
    if (!is_callable(f)) rt(vm, n->line, "expected a function after ::, got %s", type_of(f));
    if (n->flag && src.t == T_STREAM && stream_stateful(src)) rt(vm, n->line, "::par cannot be used on a stateful stream");
    if (n->flag && vm->wid == 0 && f.t == T_FN) {
        int anyasync = 0; for (int k = 0; k < f.u.fn->ncl; k++) if (f.u.fn->cl[k].async) anyasync = 1;
        if (!anyasync) {
            /* тело ::par не меняет ничего снаружи: окружения помечены владельцем, чужие только для чтения */
            int so = vm->own, st = tls_own; vm->own = 1000; tls_own = 1000;
            Value r = ev_foreach_body(vm, n, src, f);
            vm->own = so; tls_own = st;
            return r;
        }
    }
    return ev_foreach_body(vm, n, src, f);
}
static Value ev_foreach_body(VM *vm, Node *n, Value src, Value f) {
    if (n->op && src.t == T_OBJ) { /* пары (ключ, значение) */
        Obj *o = src.u.o;
        for (int i = 0; i < o->n; i++) { Value a[2]; a[0] = V_str(o->k[i]); a[1] = o->v[i]; vm->loopcall = !n->flag; Value rr = call_fn(vm, f, a, 2, n->flag, n->line, NULL); vm->loopcall = 0; drain_val(vm, rr, n->line); if (vm->ctl) { int c = vm->ctl; if (c == 3) break; vm->ctl = 0; if (c == 2) break; } }
        return V_none();
    }
    Iter *it = iter_new(vm, src, n->line); Value x; int64_t idx = 0;
    if (n->flag && vm->wid == 0 && vm->nthreads > 1 && f.t == T_FN && !f.u.fn->target) {
        int anyasync = 0; for (int k = 0; k < f.u.fn->ncl; k++) if (f.u.fn->cl[k].async) anyasync = 1;
        if (!anyasync) {
            /* первая итерация идёт обычным путём и даёт оценку стоимости; остальные — на потоках, если игра стоит свеч */
            if (!iter_next(vm, it, &x)) return V_none();
            double t0 = now_us();
            if (n->op) { Value a[2]; a[0] = V_int(0); a[1] = x; call_fn(vm, f, a, 2, 1, n->line, NULL); }
            else { Value a[1]; a[0] = x; call_fn(vm, f, a, 1, 1, n->line, NULL); }
            double dt = now_us() - t0;
            if (vm->ctl) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) rt(vm, n->line, "stop cannot be used in ::par"); }
            Arr *rest = arr_new(vm, 16); while (iter_next(vm, it, &x)) arr_push(vm, rest, x);
            if (rest->n >= 2 && dt * rest->n >= 300.0 && par_run(vm, f, rest, 1, n->op, n->line)) return V_none();
            for (int i = 0; i < rest->n; i++) {
                if (n->op) { Value a[2]; a[0] = V_int(1 + i); a[1] = rest->v[i]; call_fn(vm, f, a, 2, 1, n->line, NULL); }
                else { Value a[1]; a[0] = rest->v[i]; call_fn(vm, f, a, 1, 1, n->line, NULL); }
                if (vm->ctl) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) rt(vm, n->line, "stop cannot be used in ::par"); }
            }
            return V_none();
        }
    }
    if (f.t == T_FN && !n->op && !n->flag && !f.u.fn->multi && !f.u.fn->target && !f.u.fn->cl[0].async) {
        Clause *c = &f.u.fn->cl[0];
        if (!c->simple) clause_classify(c);
#ifdef PRISM_VM_LOOPS   /* по замерам цикл в байткоде не быстрее специализированного дерева: по умолчанию выключено */
        if (c->simple == 1 && c->np == 1 && !c->vmstate && vm->wid == 0) vm_try_compile(vm, c, f.u.fn->env);
        if (c->simple == 1 && c->np == 1 && c->vmstate == 1 && VM_OK(c, 0)) {
            /* тело цикла скомпилировано в байткод: окружение на итерацию не нужно */
            if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
            int isr = it->kind == SK_RANGE; Env *fe = f.u.fn->env; struct VCode *vc = c->code;
            for (;;) {
                if (isr) {
                    if (it->done || (it->st > 0 ? (it->hasE && it->i >= it->e) : (it->hasE && it->i <= it->e))) break;
                    x = V_int(it->i);
                    if (__builtin_add_overflow(it->i, it->st, &it->i)) it->done = 1;
                } else if (!iter_next(vm, it, &x)) break;
                vm->inl = 0;
                int mk = mk_push(vm);
                vm_exec(vm, vc, fe, &x, 1);
                if (mk) mk_pop(vm);
            }
            vm->depth--;
            return V_none();
        }
#endif
        if (c->simple == 1 && c->np == 1) {
            /* простое тело цикла: одно окружение на все итерации, пока в нём не создана замыкающая функция */
            if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
            Env *env = NULL; Str *pn = c->params[0]->name;
            int isr = it->kind == SK_RANGE;
            for (;;) {
                if (isr) { /* диапазон: значения без вызова iter_next */
                    if (it->done || (it->st > 0 ? (it->hasE && it->i >= it->e) : (it->hasE && it->i <= it->e))) break;
                    x = V_int(it->i);
                    if (__builtin_add_overflow(it->i, it->st, &it->i)) it->done = 1;
                } else if (!iter_next(vm, it, &x)) break;
                if (!env) env = env_new(vm, f.u.fn->env, 0);
                Var *v0 = &env->vars[0];
                if (env->n > 1 || env->vars != env->inl) { for (int q = env->vars != env->inl ? 0 : 1; q < env->n; q++) env->vars[q].name->ep++; }
                if (env->vars != env->inl) { env->vars = env->inl; env->cap = 6; }
                v0 = &env->inl[0]; v0->name = pn; v0->v = x; v0->mut = 1; v0->param = 1; v0->frozen = 0;
                env->n = 1; vm->inl = 0;
                int mk = mk_push(vm);
                env->iter = vm->iter;
                vm->loopcall = 1; { Value rr = run_clause(vm, c, env); if (rr.t == T_STREAM) drain_val(vm, rr, n->line); } vm->loopcall = 0;
                if (mk) mk_pop(vm);
                if (env->captured) env = NULL;
                if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; if (cc == 3) break; vm->ctl = 0; if (cc == 2) break; }
            }
            if (env) env_release(vm, env);
            vm->depth--;
            return V_none();
        }
    }
    int inl = f.t == T_FN && !f.u.fn->multi && !f.u.fn->target && f.u.fn->cl[0].async && !n->flag; /* тело ::-цикла с ожиданием — часть цикла */
    while (iter_next(vm, it, &x)) {
        vm->inl = inl;
        int mk = !inl && !n->flag && f.t == T_FN ? mk_push(vm) : 0;
        vm->loopcall = !n->flag;
        Value rr;
        if (n->op) { Value a[2]; a[0] = V_int(idx++); a[1] = x; rr = call_fn(vm, f, a, 2, n->flag, n->line, NULL); }
        else { Value a[1]; a[0] = x; rr = call_fn(vm, f, a, 1, n->flag, n->line, NULL); }
        vm->loopcall = 0;
        drain_val(vm, rr, n->line);
        if (mk) mk_pop(vm);
        if (__builtin_expect(vm->ctl, 0)) { int cc = vm->ctl; if (cc == 3) break; vm->ctl = 0; if (cc == 2) break; }
    }
    return V_none();
}
static Value ev_as(VM *vm, Node *n, Env *env) {
    Value src = ev(vm, n->a, env);
    if (src.t == T_STREAM) src = stream_materialize(vm, src);
    if (src.t != T_ARR) rt(vm, n->line, "\"as [...]\" requires an array of arrays, got %s", type_of(src));
    Arr *out = arr_new(vm, src.u.a->n);
    for (int i = 0; i < src.u.a->n; i++) {
        Value row = src.u.a->v[i];
        if (row.t != T_ARR) rt(vm, n->line, "row %d must be an array, got %s", i + 1, type_of(row));
        if (row.u.a->n != n->n) {
            SB sb; sb_init(vm, &sb);
            for (int j = 0; j < n->n; j++) { if (j) sb_cs(&sb, ", "); sb_cs(&sb, n->names[j]->s); }
            rt(vm, n->line, "row %d has %d values, schema has %d (%s)", i + 1, row.u.a->n, n->n, sb.b ? sb.b : "");
        }
        Obj *o = obj_new(vm);
        for (int j = 0; j < n->n; j++) obj_set(vm, o, n->names[j], row.u.a->v[j]);
        arr_push(vm, out, V_obj(o));
    }
    return V_arr(out);
}

static void callee_text(SB *sb, Node *c) {
    if (c->type == N_IDENT) { sb_cs(sb, "\""); sb_cs(sb, c->name->s); sb_cs(sb, "\""); return; }
    if (c->type == N_MEMBER) {
        sb_cs(sb, "\"");
        Node *o = c->a;
        if (o->type == N_IDENT) sb_cs(sb, o->name->s);
        else if (o->type == N_MEMBER && o->a->type == N_IDENT) { sb_cs(sb, o->a->name->s); sb_cs(sb, "."); sb_cs(sb, o->name->s); }
        else sb_cs(sb, "\xE2\x80\xA6");
        sb_cs(sb, "."); sb_cs(sb, c->name->s); sb_cs(sb, "\""); return;
    }
    sb_cs(sb, "the expression");
}
static void err_not_callable(VM *vm, Node *callee, int line, Value f) {
    SB sb; sb_init(vm, &sb);
    callee_text(&sb, callee);
    char buf[160]; snprintf(buf, sizeof buf, "%s", sb.b ? sb.b : "the expression");
    SB rb; sb_init(vm, &rb); repr_to(&rb, f, 0);
    if (f.t == T_NONE) rt(vm, line, "cannot call %s: it is none, not a function (variable or member is not defined, or holds none)", buf);
    rt(vm, line, "cannot call %s: it is %s %s, not a function", buf, type_of(f), rb.b ? rb.b : "");
}
static Value ev_call(VM *vm, Node *n, Env *env) {
    if (n->pure == 1 && n->a->type == N_IDENT && n->n <= 6) {   /* быстрый путь: f(выражения) — простая функция из одной ветки без асинхронности */
        Var *b = env_find_c(env, n->a);
        if (b && b->v.t == T_FN) {
            Fn *fn = b->v.u.fn; Clause *c = &fn->cl[0];
            if (!fn->multi && !fn->target && c->simple == 1 && !c->async && c->np == n->n) {
                int np = c->np; Value av[6];
                for (int i = 0; i < np; i++) av[i] = evq(vm, n->list[i], env);
                vm->curline = n->line; vm->inl = 0;
                if (!c->vmstate && vm->wid == 0) vm_try_compile(vm, c, fn->env);
                if (c->vmstate == 1 && VM_OK(c, 0)) {
                    if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
                    Value r = vm_exec(vm, c->code, fn->env, av, np);
                    vm->depth--;
                    return r;
                }
                if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
                Env *e2 = env_new(vm, fn->env, 0);
                for (int i = 0; i < np; i++) { Var *x = &e2->inl[i]; x->name = c->params[i]->name; x->v = av[i]; x->mut = 1; x->param = 1; x->frozen = 0; }
                e2->n = np;
                Value result = run_clause(vm, c, e2);
                env_release(vm, e2);
                vm->depth--;
                return result;
            }
        }
    }
    Value f; Value *args; int na = n->n;
    Value stackargs[8];
    args = na <= 8 ? stackargs : (Value *)amalloc(vm, sizeof(Value) * (size_t)na);
    if (n->a->type == N_MEMBER) {
        Node *m = n->a;
        Value recv = m->a->type == N_CALL ? ev_call(vm, m->a, env) : ev(vm, m->a, env);
        if (m->flag && recv.t == T_NONE) return V_none();
        for (int i = 0; i < na; i++) args[i] = evq(vm, n->list[i], env);
        vm->curline = n->line;
        int found = 0;
        Value r = call_method(vm, recv, m->name, args, na, m, env, n->line, &found);
        if (found) return r;
        f = get_member(vm, recv, m->name, m, env);
    } else {
        f = ev(vm, n->a, env);
        for (int i = 0; i < na; i++) args[i] = evq(vm, n->list[i], env);
    }
    vm->curline = n->line;
    int muts[16]; int *mp = NULL;
    if (f.t == T_FN && na <= 16) {
        if (!n->pure) { int pu = 1; for (int i = 0; i < na; i++) if (root_ident(n->list[i])) pu = 0; __atomic_store_n(&n->pure, pu ? 1 : 2, __ATOMIC_RELAXED); }
        if (n->pure == 2) { for (int i = 0; i < na; i++) muts[i] = is_mut_path(vm, n->list[i], env); mp = muts; }
    }
    if (f.t != T_FN && f.t != T_NAT && f.t != T_TYPE) err_not_callable(vm, n->a, n->line, f);
    return call_fn(vm, f, args, na, 0, n->line, mp);
}

static inline Value evq(VM *vm, Node *n, Env *env) {
    switch (n->type) {
        case N_INT: case N_STR: case N_BOOL: return n->v;
        case N_IDENT: { Var *b = env_find_c(env, n); if (b) return b->v; break; }
    }
    return ev(vm, n, env);
}
static Value ev_slow(VM *vm, Node *n, Env *env);
static Value ev_is_expr(VM *vm, Node *n, Env *env);
static Value ev_assign(VM *vm, Node *n, Env *env);

/* ---------------------------------------------------------------- специализированные вычислители узлов */
static Value ev_select(VM *vm, Node *n, Env *env);
static inline Value ev(VM *vm, Node *n, Env *env) {
    Value (*f)(VM *, Node *, Env *) = n->fn;
    return f ? f(vm, n, env) : ev_select(vm, n, env);
}
static Value ev_k(VM *vm, Node *n, Env *env) { (void)vm; (void)env; return n->v; }
static Value ev_id(VM *vm, Node *n, Env *env) { Var *b = env_find_c(env, n); if (b) return b->v; return ev_slow(vm, n, env); }
static Value ev_bin_rest(VM *vm, Node *n, Value a, Value b) {
    if (a.t == T_FLT && (b.t == T_FLT || b.t == T_INT)) {
        double x = a.u.f, y = b.t == T_FLT ? b.u.f : (double)b.u.i;
        if (b.t == T_FLT || (b.u.i > -9007199254740992LL && b.u.i < 9007199254740992LL)) switch (n->op) {
            case OP_ADD: return V_flt(x + y);
            case OP_SUB: return V_flt(x - y);
            case OP_MUL: return V_flt(x * y);
            case OP_LT: return V_bool(x < y);
            case OP_GT: return V_bool(x > y);
            case OP_LE: return V_bool(x <= y);
            case OP_GE: return V_bool(x >= y);
        }
    }
    return binop(vm, n->op, a, b, n->line);
}
#define BIN_FN(NAME, FAST) \
static Value NAME(VM *vm, Node *n, Env *env) { \
    Value a = evq(vm, n->a, env), b = evq(vm, n->b, env); \
    if (a.t == T_INT && b.t == T_INT) { int64_t x = a.u.i, y = b.u.i, r; (void)r; FAST } \
    return ev_bin_rest(vm, n, a, b); \
} \
static Value NAME##_c(VM *vm, Node *n, Env *env) { /* правый операнд — целая константа */ \
    Value a = evq(vm, n->a, env); \
    if (a.t == T_INT) { int64_t x = a.u.i, y = n->b->v.u.i, r; (void)r; FAST } \
    return ev_bin_rest(vm, n, a, n->b->v); \
}
BIN_FN(ev_add, if (!__builtin_add_overflow(x, y, &r)) return V_int(r);)
BIN_FN(ev_sub, if (!__builtin_sub_overflow(x, y, &r)) return V_int(r);)
BIN_FN(ev_mul, if (!__builtin_mul_overflow(x, y, &r)) return V_int(r);)
BIN_FN(ev_mod, if (y > 0) { r = x % y; if (r < 0) r += y; return V_int(r); })
BIN_FN(ev_eq, return V_bool(x == y);)
BIN_FN(ev_ne, return V_bool(x != y);)
BIN_FN(ev_lt, return V_bool(x < y);)
BIN_FN(ev_gt, return V_bool(x > y);)
BIN_FN(ev_le, return V_bool(x <= y);)
BIN_FN(ev_ge, return V_bool(x >= y);)
static Value ev_asgn(VM *vm, Node *n, Env *env) { return ev_assign(vm, n, env); }
static Value ev_callx(VM *vm, Node *n, Env *env) { Value r = ev_call(vm, n, env); if (r.t == T_STREAM && stream_arrchain(r)) return stream_materialize(vm, r); return r; }
static Value ev_isx(VM *vm, Node *n, Env *env) { return ev_is_expr(vm, n, env); }
static Value ev_generic(VM *vm, Node *n, Env *env);
static Value ev_idx(VM *vm, Node *n, Env *env) {   /* a[i]: массив и int без обхода общего ev_slow */
    Value o = evq(vm, n->a, env); Value i = evq(vm, n->b, env);
    if (o.t == T_ARR && i.t == T_INT) {
        int64_t k = i.u.i, L = o.u.a->n; if (k < 0) k += L;
        return (uint64_t)k < (uint64_t)L ? o.u.a->v[k] : V_none();
    }
    return ev_index(vm, o, i, n->line);
}
static Value ev_mem(VM *vm, Node *n, Env *env) {   /* o.name: поле объекта без вызова get_member */
    Value o = evq(vm, n->a, env);
    if (o.t == T_OBJ) { int j = obj_find(o.u.o, n->name); return j >= 0 ? o.u.o->v[j] : V_none(); }
    if (n->flag && o.t == T_NONE) return V_none();
    return get_member(vm, o, n->name, n, env);
}
static Value ev_select(VM *vm, Node *n, Env *env) {
    Value (*f)(VM *, Node *, Env *) = ev_generic;
    switch (n->type) {
        case N_INT: case N_STR: case N_BOOL: f = ev_k; break;
        case N_FLT: f = ev_flt; break;
        case N_IDENT: f = ev_id; break;
        case N_ASSIGN: f = ev_asgn; break;
        case N_CALL: f = ev_callx; break;
        case N_IS: f = ev_isx; break;
        case N_INDEX: f = ev_idx; break;
        case N_MEMBER: f = ev_mem; break;
        case N_BIN: {
            if (n->fold || n->op > OP_GE) break;
            int c = n->b->type == N_INT;
            switch (n->op) {
#define PICK(OP, F) case OP: f = c ? F##_c : F; break;
                PICK(OP_ADD, ev_add) PICK(OP_SUB, ev_sub) PICK(OP_MUL, ev_mul) PICK(OP_MOD, ev_mod)
                PICK(OP_EQ, ev_eq) PICK(OP_NE, ev_ne) PICK(OP_LT, ev_lt) PICK(OP_GT, ev_gt) PICK(OP_LE, ev_le) PICK(OP_GE, ev_ge)
#undef PICK
            }
            break;
        }
    }
    __atomic_store_n(&n->fn, f, __ATOMIC_RELAXED);
    return f(vm, n, env);
}
static Value ev_generic(VM *vm, Node *n, Env *env) {
    switch (n->type) {
        case N_INT: case N_STR: case N_BOOL: return n->v;
        case N_IDENT: { Var *b = env_find_c(env, n); if (b) return b->v; break; }
        case N_BIN: {
            int op = n->op;
            if (n->fold || op > OP_GE) break;
            Value a = evq(vm, n->a, env); Value b = evq(vm, n->b, env);
            if (a.t == T_INT && b.t == T_INT) {
                int64_t x = a.u.i, y = b.u.i, r;
                switch (op) {
                    case OP_ADD: if (!__builtin_add_overflow(x, y, &r)) return V_int(r); break;
                    case OP_SUB: if (!__builtin_sub_overflow(x, y, &r)) return V_int(r); break;
                    case OP_MUL: if (!__builtin_mul_overflow(x, y, &r)) return V_int(r); break;
                    case OP_MOD: if (y > 0) { r = x % y; if (r < 0) r += y; return V_int(r); } break;
                    case OP_EQ: return V_bool(x == y);
                    case OP_NE: return V_bool(x != y);
                    case OP_LT: return V_bool(x < y);
                    case OP_GT: return V_bool(x > y);
                    case OP_LE: return V_bool(x <= y);
                    case OP_GE: return V_bool(x >= y);
                }
            }
            else if (a.t == T_FLT && (b.t == T_FLT || b.t == T_INT)) {
                double x = a.u.f, y = b.t == T_FLT ? b.u.f : (double)b.u.i;
                if (b.t == T_FLT || (b.u.i > -9007199254740992LL && b.u.i < 9007199254740992LL)) switch (op) {
                    case OP_ADD: return V_flt(x + y);
                    case OP_SUB: return V_flt(x - y);
                    case OP_MUL: return V_flt(x * y);
                    case OP_LT: return V_bool(x < y);
                    case OP_GT: return V_bool(x > y);
                    case OP_LE: return V_bool(x <= y);
                    case OP_GE: return V_bool(x >= y);
                }
            }
            return binop(vm, op, a, b, n->line);
        }
        case N_CALL: { Value r = ev_call(vm, n, env); if (r.t == T_STREAM && stream_arrchain(r)) return stream_materialize(vm, r); return r; }
        case N_IS: return ev_is_expr(vm, n, env);
        case N_ASSIGN: return ev_assign(vm, n, env);
    }
    return ev_slow(vm, n, env);
}
__attribute__((noinline)) static Value ev_slow(VM *vm, Node *n, Env *env) {
    switch (n->type) {
        case N_INT: case N_STR: case N_BOOL: return n->v;
        case N_NONE: return V_none();
        case N_CTL: vm->ctl = n->flag; return V_none();
        case N_RETURN: return do_return(vm, n, env);
        case N_IDENT: {
            Var *b = env_find_c(env, n);
            if (!b && is_method_name(n->name->s)) {   /* split без точки: если переменной нет — метод элемента, split(x) = x.split() */
                Fn *mf = (Fn *)amalloc(vm, sizeof(Fn));
                mf->name = n->name; mf->mname = n->name; mf->bline = n->line; mf->nb = -1;
                mf->target = (Value *)amalloc(vm, sizeof(Value)); *mf->target = V_none();
                Value mv = V_none(); mv.t = T_FN; mv.u.fn = mf; return mv;
            }
            if (!b) { const char *sg = suggest_env(env, n->name->s, 0); if (sg) rt(vm, n->line, "undefined variable \"%s\". Did you mean: \"%s\"?", n->name->s, sg); rt(vm, n->line, "undefined variable \"%s\"", n->name->s); }
            return b->v;
        }
        case N_ARR: {
            Arr *a = arr_new(vm, n->n);
            for (int i = 0; i < n->n; i++) {
                Node *x = n->list[i];
                if (x->type == N_SPREAD) {
                    Value v = ev(vm, x->a, env), it; iter_of(vm, v, &it, x->line);
                    for (int j = 0; j < it.u.a->n; j++) arr_push(vm, a, it.u.a->v[j]);
                } else arr_push(vm, a, ev(vm, x, env));
            }
            return V_arr(a);
        }
        case N_OBJ: {
            Obj *o = obj_new(vm);
            for (int i = 0; i < n->n; i++) {
                Node *p = n->list[i];
                if (p->flag) {
                    Value v = ev(vm, p->a, env);
                    if (v.t != T_OBJ) rt(vm, n->line, "spread into object requires an object, got %s", type_of(v));
                    for (int j = 0; j < v.u.o->n; j++) obj_set(vm, o, v.u.o->k[j], v.u.o->v[j]);
                } else obj_set(vm, o, p->name, ev(vm, p->a, env));
            }
            return V_obj(o);
        }
        case N_IFMT: {
            SB sb; sb_init(vm, &sb);
            for (int i = 0; i < n->n; i++) {
                Value v = ev(vm, n->list[i], env);
                Str *s = to_str(vm, v, 1); sb_add(&sb, s->s, s->len);
            }
            return V_str(sb_str(&sb));
        }
        case N_CHAIN: {
            Value prev = ev(vm, n->list[0], env);
            for (int i = 0; i < n->n - 1; i++) {
                Value cur = ev(vm, n->list[i + 1], env);
                if (!cmp_values(vm, n->ops[i], prev, cur, n->line)) return V_bool(0);
                prev = cur;
            }
            return V_bool(1);
        }
        case N_UN: {
            Value a = ev(vm, n->a, env);
            if (n->op == OP_NEG) {
                if (a.t == T_INT) { if (a.u.i == INT64_MIN) return big_norm(big_neg(vm, big_from_i64(vm, a.u.i))); return V_int(-a.u.i); }
                if (a.t == T_BIG) return big_norm(big_neg(vm, a.u.big));
                if (a.t == T_FLT) return V_flt(-a.u.f);
                if (a.t == T_DEC) return dec_neg(vm, a);
                rt(vm, n->line, "unary - requires a number, got %s", type_of(a));
            }
            if (a.t != T_BOOL) rt(vm, n->line, "not requires bool, got %s", type_of(a));
            return V_bool(!a.u.b);
        }
        case N_BIN: {
            int op = n->op;
            if (n->fold) {
                Value x = evq(vm, n->c, env); int64_t r;
                if (x.t == T_INT && !__builtin_add_overflow(x.u.i, n->v.u.i, &r)) return V_int(r);
                for (int i = 0; i < n->n; i++) x = binop(vm, n->ops[i], x, n->list[i]->v, n->line);
                return x;
            }
            if (op == OP_AND || op == OP_OR) {
                Value a = ev(vm, n->a, env);
                const char *nm = op == OP_AND ? "and" : "or";
                if (a.t != T_BOOL) rt(vm, n->line, "%s requires bool on the left, got %s", nm, type_of(a));
                if (op == OP_AND ? !a.u.b : a.u.b) return a;
                Value b = ev(vm, n->b, env);
                if (b.t != T_BOOL) rt(vm, n->line, "%s requires bool on the right, got %s", nm, type_of(b));
                return b;
            }
            if (op == OP_IS) {
                Value v = ev(vm, n->a, env);
                if (n->b->type != N_IDENT) rt(vm, n->line, "expected a type name after is");
                Str *nm = n->b->name; Var *b = env_find(env, nm, NULL);
                if (b && b->v.t == T_TYPE) return V_bool(has_type(v, b->v.u.td));
                if (is_typename(nm->s)) return V_bool(match_typename(nm->s, v));
                rt(vm, n->line, "unknown type \"%s\"", nm->s);
            }
            if (op == OP_IN) { Value a = ev(vm, n->a, env); Value b = ev(vm, n->b, env); return V_bool(ev_in(vm, a, b, n->line)); }
            if (op == OP_NC) { Value a = ev(vm, n->a, env); return (a.t == T_NONE || a.t == T_ERR) ? ev(vm, n->b, env) : a; }
            Value a = evq(vm, n->a, env); Value b = evq(vm, n->b, env);
            if (a.t == T_INT && b.t == T_INT) {
                int64_t x = a.u.i, y = b.u.i, r;
                switch (op) {
                    case OP_ADD: if (!__builtin_add_overflow(x, y, &r)) return V_int(r); break;
                    case OP_SUB: if (!__builtin_sub_overflow(x, y, &r)) return V_int(r); break;
                    case OP_MUL: if (!__builtin_mul_overflow(x, y, &r)) return V_int(r); break;
                    case OP_MOD: if (y > 0) { r = x % y; if (r < 0) r += y; return V_int(r); } break;
                    case OP_EQ: return V_bool(x == y);
                    case OP_NE: return V_bool(x != y);
                    case OP_LT: return V_bool(x < y);
                    case OP_GT: return V_bool(x > y);
                    case OP_LE: return V_bool(x <= y);
                    case OP_GE: return V_bool(x >= y);
                }
            }
            return binop(vm, op, a, b, n->line);
        }
        case N_LAMBDA: return mk_fn(vm, n->cl, n->ncl, env, NULL);
        case N_CALL: {
            Value r = ev_call(vm, n, env);
            if (r.t == T_STREAM && stream_arrchain(r)) return stream_materialize(vm, r);
            return r;
        }
        case N_MEMBER: {
            Value o = ev(vm, n->a, env);
            if (n->flag && o.t == T_NONE) return V_none();
            return get_member(vm, o, n->name, n, env);
        }
        case N_INDEX: { Value o = evq(vm, n->a, env); Value i = evq(vm, n->b, env); return ev_index(vm, o, i, n->line); }
        case N_BLOCK: { Env *e = env_new(vm, env, 0); Value r = exec_block(vm, n->list, n->n, e); env_release(vm, e); if (vm->ctl == 3) { vm->ctl = 0; r = vm->retv; } return r; }
        case N_WHEN: {
            Value c = ev(vm, n->b, env);
            if (c.t != T_BOOL) rt(vm, n->line, "when condition must be bool, got %s", type_of(c));
            if (!c.u.b) return V_none();
            Value f = ev(vm, n->a, env);
            if (!(f.t == T_FN || f.t == T_NAT)) rt(vm, n->line, "when requires a callback function on the left, got %s", type_of(f));
            return call_fn(vm, f, NULL, 0, 0, n->line, NULL);
        }
        case N_BIND: {
            Value f = ev(vm, n->a, env);
            if (!(f.t == T_FN || f.t == T_NAT)) rt(vm, n->line, "f:(...) requires a function, got %s", type_of(f));
            Fn *b = (Fn *)amalloc(vm, sizeof(Fn));
            b->nb = n->n; b->bline = n->line; b->bargs = (Value *)amalloc(vm, sizeof(Value) * (size_t)(n->n + 1));
            for (int i = 0; i < n->n; i++) b->bargs[i] = ev(vm, n->list[i], env);
            b->target = (Value *)amalloc(vm, sizeof(Value)); *b->target = f;
            Value v = V_none(); v.t = T_FN; v.u.fn = b; return v;
        }
        case N_AWAIT: { Value v = ev(vm, n->a, env); return await_value(vm, v, n->line); }
        case N_SLICE: return ev_slice(vm, n, env);
        case N_MATCH: return ev_match(vm, n, env);
        case N_IS: return ev_is_expr(vm, n, env);
        case N_FOREACH: return ev_foreach(vm, n, env);
        case N_AS: return ev_as(vm, n, env);
        case N_ASSIGN: return ev_assign(vm, n, env);
        default: rt(vm, n->line, "unknown node %d", n->type);
    }
    return V_none();
}
