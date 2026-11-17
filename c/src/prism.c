/* ===== a_core ===== */
/*
 * Prism 0.1 — ядро: лексер, раскладка отступов, парсер, интерпретатор дерева.
 * Единый файл (amalgamation): собирается из src/parts/ скриптом tools/amalgamate.sh.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "prism.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#include <unistd.h>
#define PRISM_THREADS 1
#endif

#define PRISM_MAX_DEPTH 1200

/* ---------------------------------------------------------------- типы */
typedef struct VM VM;
typedef struct Str Str;
typedef struct Arr Arr;
typedef struct Obj Obj;
typedef struct Fn Fn;
typedef struct Env Env;
typedef struct Node Node;
typedef struct Big Big;
typedef struct Stream Stream;
typedef struct Clause Clause;
typedef struct Task Task;

struct Str { int len; unsigned ep; char *s; };   /* s — байты (обычно сразу за заголовком; у строк, растущих через +=, общий буфер). За s[len] может быть не 0 — для C-функций см. cz() */   /* ep — «эпоха» имени: растёт при каждом новом определении переменной с этим именем (кэш поиска) */

typedef struct TypeDef TypeDef;
typedef enum { T_NONE, T_BOOL, T_INT, T_FLT, T_STR, T_ARR, T_OBJ, T_FN, T_NAT, T_ERR, T_TYPE, T_STREAM, T_BIG, T_TASK, T_DEC } VT;
typedef struct Dec Dec;

typedef struct Value {
    VT t;
    union { int b; int64_t i; double f; Str *s; Arr *a; Obj *o; Fn *fn; int nat; TypeDef *td; Stream *sm; Big *big; Task *task; Dec *dec; } u;
} Value;

struct Arr { int n, cap; Value *v; };
struct Obj { int n, cap; Str **k; Value *v; TypeDef *type; int *h; int hcap; };
struct Fn { Clause *cl; int ncl; int multi; Env *env; Str *name; int nb, bline; Value *bargs; Value *target; Str *mname; };
struct TypeDef { Str *name; int n; Str **fields; Node **defs; Env *env; };

typedef struct Var { Str *name; Value v; int mut, param, frozen; } Var;
struct Env { Env *parent; Var *vars; int n, cap; int par, captured, owner, iter; unsigned gen; Var inl[6]; };

typedef struct Block { struct Block *next; size_t cap, used; } Block;

typedef struct Intern { Str *s; struct Intern *next; } Intern;

typedef struct Mod { Str *name; char *src; Obj *exp; int state; } Mod;
typedef struct Mark { struct Block *blk; size_t used; int id, esc; } Mark;
struct VM {
    Mod *mods; int nmods, capmods; char *moddir;
    Block *blocks;
    size_t total, limit;
    Env *globals, *user;
    jmp_buf jb, *jbp;
    unsigned egen;
    int maxdepth, userdepth, inl, prog_hw, shutdown;
    Task *curtask, *mainTask, **tasks, **alltasks;
    int ntasks, captasks, nall, capall;
    double t0;
    char err[480];
    int errline;
    int depth;
    PrismWriteFn out; void *outud;
    PrismReadFn in; void *inud;
    Intern **tab;
    int wid, own, nthreads;
    int iter, iter_seq, nmk, norewind, poison; Mark mk[8];
    struct { char *end, *lim; } sbuf[4]; int sbi;   /* буферы дозаписи строк: конец занятой части и предел */
    Value retval;
    uint64_t rng;
    char *froot; PrismHttpFn http; void *httpud;   /* file/http: корень файлов и обработчик HTTP от хоста (NULL — выключено) */
    int loopcall; Value retv;   /* ctl==3: return из ветки is/else — выходит из ближайшей функции */
    int ctl;   /* 1 — next, 2 — stop: ждёт обработки в ближайшем цикле :: или в select/where/… */
    int prec;   /* precision(n): 0 — float это double, n > 0 — десятичные дробные числа с n знаками */
    Str *s_empty;
    int curline;
    Env *freeenv;
    size_t maxrange;
};

/* ---------------------------------------------------------------- ошибки */
static void fail(VM *vm, int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(vm->err, sizeof vm->err, fmt, ap);
    va_end(ap);
    vm->errline = line;
    longjmp(*vm->jbp, 1);
}

/* ---------------------------------------------------------------- память: арена */
static void *amalloc_nz(VM *vm, size_t sz);   /* без обнуления: для буферов, которые тут же целиком заполняются */
static void *amalloc(VM *vm, size_t sz) {
    void *p = amalloc_nz(vm, sz);
    memset(p, 0, (sz + 7u) & ~(size_t)7u);
    return p;
}
static void *amalloc_nz(VM *vm, size_t sz) {
    sz = (sz + 7u) & ~(size_t)7u;
    Block *b = vm->blocks;
    if (!b || b->used + sz > b->cap) {
        size_t cap = sz + 64 * 1024;
        if (vm->total + cap > vm->limit) fail(vm, 0, "out of memory (limit %zu MB)", vm->limit >> 20);
        b = (Block *)malloc(sizeof(Block) + cap);
        if (!b) fail(vm, 0, "out of memory");
        b->cap = cap; b->used = 0; b->next = vm->blocks; vm->blocks = b;
        vm->total += cap;
    }
    void *p = (char *)(b + 1) + b->used;
    b->used += sz;
    return p;
}

static void arena_free(VM *vm) {
    Block *b = vm->blocks;
    while (b) { Block *n = b->next; free(b); b = n; }
    vm->blocks = NULL; vm->total = 0;
}

/* ---------------------------------------------------------------- строки */
static Str *mkstr(VM *vm, const char *s, int len) {
    Str *r = (Str *)amalloc_nz(vm, sizeof(Str) + (size_t)len + 1);
    r->len = len; r->ep = 0; r->s = (char *)(r + 1);
    if (len) memcpy(r->s, s, (size_t)len);
    r->s[len] = 0;
    return r;
}
/* поиск подстроки в [h, h+hl) без опоры на нуль-терминатор */
static const char *mem_find(const char *h, int hl, const char *nd, int nl) {
    if (nl == 0) return h;
    for (int i = 0; i + nl <= hl; i++) { const char *q = memchr(h + i, nd[0], (size_t)(hl - i - nl + 1)); if (!q) return NULL; i = (int)(q - h); if (!memcmp(q, nd, (size_t)nl)) return q; }
    return NULL;
}
/* C-строка с нулём на конце: у строки, выросшей через +=, за s[len] могут лежать чужие байты */
static const char *cz(VM *vm, const Str *s) { return s->s[s->len] == 0 ? s->s : mkstr(vm, s->s, s->len)->s; }
static Str *cstr(VM *vm, const char *s) { return mkstr(vm, s, (int)strlen(s)); }

static unsigned hash_bytes(const char *s, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}
#ifdef PRISM_THREADS
static pthread_mutex_t INTERN_MX = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_par_active;
static __thread int tls_own;
#else
static int tls_own;
#define g_par_active 0
#endif
static Str *intern_locked(VM *vm, const char *s, int len);
static void esc_all(VM *vm);
static Str *intern(VM *vm, const char *s, int len) {
#ifdef PRISM_THREADS
    if (g_par_active) { pthread_mutex_lock(&INTERN_MX); Str *r = intern_locked(vm, s, len); pthread_mutex_unlock(&INTERN_MX); return r; }
#endif
    return intern_locked(vm, s, len);
}
static Str *intern_locked(VM *vm, const char *s, int len) {
    unsigned h = hash_bytes(s, len) & 255u;
    for (Intern *e = vm->tab[h]; e; e = e->next)
        if (e->s->len == len && memcmp(e->s->s, s, (size_t)len) == 0) return e->s;
    if (vm->nmk) esc_all(vm);
    Intern *e = (Intern *)amalloc(vm, sizeof(Intern));
    e->s = mkstr(vm, s, len); e->next = vm->tab[h]; vm->tab[h] = e;
    return e->s;
}
static int str_eq(const Str *a, const Str *b) { return a == b || (a->len == b->len && memcmp(a->s, b->s, (size_t)a->len) == 0); }

/* UTF-8 */
static int u8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
static int u8_count(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i += u8_len((unsigned char)s[i])) n++;
    return n;
}
/* смещение в байтах начала символа с номером k (k может быть = число символов) */
static int u8_off(const char *s, int len, int k) {
    int i = 0;
    while (k > 0 && i < len) { i += u8_len((unsigned char)s[i]); k--; }
    return i > len ? len : i;
}
static uint32_t u8_dec(const char *s, int *adv) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *adv = 1; return c; }
    if (c < 0xE0) { *adv = 2; return ((c & 0x1Fu) << 6) | ((unsigned char)s[1] & 0x3Fu); }
    if (c < 0xF0) { *adv = 3; return ((c & 0x0Fu) << 12) | (((unsigned char)s[1] & 0x3Fu) << 6) | ((unsigned char)s[2] & 0x3Fu); }
    *adv = 4;
    return ((c & 0x07u) << 18) | (((unsigned char)s[1] & 0x3Fu) << 12) | (((unsigned char)s[2] & 0x3Fu) << 6) | ((unsigned char)s[3] & 0x3Fu);
}
static int u8_enc(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4;
}
static uint32_t case_map(uint32_t c, int up) {
    if (up) {
        if (c >= 'a' && c <= 'z') return c - 32;
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
    } else {
        if (c >= 'A' && c <= 'Z') return c + 32;
        if (c >= 0x410 && c <= 0x42F) return c + 32;
        if (c >= 0x400 && c <= 0x40F) return c + 80;
    }
    return c;
}

/* ---------------------------------------------------------------- построитель строк */
typedef struct SB { VM *vm; char *b; int n, cap; } SB;
static void sb_init(VM *vm, SB *sb) { sb->vm = vm; sb->b = NULL; sb->n = 0; sb->cap = 0; }
static void sb_add(SB *sb, const char *s, int len) {
    if (sb->n + len + 1 > sb->cap) {
        int nc = sb->cap ? sb->cap * 2 : 64;
        while (nc < sb->n + len + 1) nc *= 2;
        char *nb = (char *)amalloc(sb->vm, (size_t)nc);
        if (sb->n) memcpy(nb, sb->b, (size_t)sb->n);
        sb->b = nb; sb->cap = nc;
    }
    if (len) memcpy(sb->b + sb->n, s, (size_t)len);
    sb->n += len;
    sb->b[sb->n] = 0;
}
static void sb_cs(SB *sb, const char *s) { sb_add(sb, s, (int)strlen(s)); }
static void sb_c(SB *sb, char c) { sb_add(sb, &c, 1); }
static Str *sb_str(SB *sb) { return mkstr(sb->vm, sb->b ? sb->b : "", sb->n); }

/* ---------------------------------------------------------------- значения */
static inline Value V_none(void) { Value v = { T_NONE, { 0 } }; return v; }
static inline Value V_bool(int b) { Value v = { T_BOOL, { 0 } }; v.u.b = b ? 1 : 0; return v; }
static inline Value V_int(int64_t i) { Value v = { T_INT, { 0 } }; v.u.i = i; return v; }
static inline Value V_flt(double f) { Value v = { T_FLT, { 0 } }; v.u.f = f; return v; }
static inline Value V_str(Str *s) { Value v = { T_STR, { 0 } }; v.u.s = s; return v; }
static inline Value V_arr(Arr *a) { Value v = { T_ARR, { 0 } }; v.u.a = a; return v; }
static inline Value V_obj(Obj *o) { Value v = { T_OBJ, { 0 } }; v.u.o = o; return v; }
static Value V_err(VM *vm, const char *msg) { Value v = V_none(); v.t = T_ERR; v.u.s = cstr(vm, msg); return v; }


/* ---- освобождение памяти итерации цикла ::: арена откатывается, если ничто из созданного в итерации не попало в старые данные */
static int blk_has(const Block *b, const void *p) { return (const char *)p >= (const char *)(b + 1) && (const char *)p < (const char *)(b + 1) + b->cap; }
static int in_region(const VM *vm, int d, const void *p) {
    const Block *mb = vm->mk[d].blk;
    for (const Block *b = vm->blocks; b; b = b->next) {
        if (b == mb) return blk_has(b, p) && (const char *)p >= (const char *)(b + 1) + vm->mk[d].used;
        if (blk_has(b, p)) return 1;
    }
    return 0;
}
static void esc_container(VM *vm, const void *p) {
    for (int d = vm->nmk - 1; d >= 0; d--) { if (in_region(vm, d, p)) return; vm->mk[d].esc = 1; }
}
static void esc_var(VM *vm, int env_iter) { for (int d = 0; d < vm->nmk; d++) if (vm->mk[d].id > env_iter) vm->mk[d].esc = 1; }
static void esc_all(VM *vm) { for (int d = 0; d < vm->nmk; d++) vm->mk[d].esc = 1; }
static inline int is_ptr_val(Value v) { return v.t != T_NONE && v.t != T_BOOL && v.t != T_INT && v.t != T_FLT && v.t != T_NAT; }

static Arr *arr_new(VM *vm, int cap) {
    Arr *a = (Arr *)amalloc(vm, sizeof(Arr));
    a->cap = cap < 4 ? 4 : cap;
    a->v = (Value *)amalloc(vm, sizeof(Value) * (size_t)a->cap);
    return a;
}
static void arr_push(VM *vm, Arr *a, Value x) {
    if (vm->nmk && (a->n == a->cap || is_ptr_val(x))) esc_container(vm, a);
    if (a->n == a->cap) {
        int nc = a->cap * 2;
        Value *nv = (Value *)amalloc_nz(vm, sizeof(Value) * (size_t)nc);
        memcpy(nv, a->v, sizeof(Value) * (size_t)a->n);
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = x;
}
static Obj *obj_new(VM *vm) {
    Obj *o = (Obj *)amalloc(vm, sizeof(Obj));
    o->cap = 4;
    o->k = (Str **)amalloc(vm, sizeof(Str *) * 4);
    o->v = (Value *)amalloc(vm, sizeof(Value) * 4);
    return o;
}
static unsigned str_hash(const Str *k) {
    unsigned h = 2166136261u;
    for (int i = 0; i < k->len; i++) { h ^= (unsigned char)k->s[i]; h *= 16777619u; }
    return h;
}
static int obj_find(const Obj *o, const Str *k) {
    if (o->h) {
        unsigned m = (unsigned)o->hcap - 1, i = str_hash(k) & m;
        while (o->h[i] >= 0) { if (str_eq(o->k[o->h[i]], k)) return o->h[i]; i = (i + 1) & m; }
        return -1;
    }
    for (int i = 0; i < o->n; i++) if (str_eq(o->k[i], k)) return i;
    return -1;
}
static void obj_rehash(VM *vm, Obj *o) {
    int hc = 32; while (hc < o->n * 2 + 2) hc *= 2;
    o->h = (int *)amalloc(vm, sizeof(int) * (size_t)hc); o->hcap = hc;
    for (int i = 0; i < hc; i++) o->h[i] = -1;
    unsigned m = (unsigned)hc - 1;
    for (int i = 0; i < o->n; i++) { unsigned j = str_hash(o->k[i]) & m; while (o->h[j] >= 0) j = (j + 1) & m; o->h[j] = i; }
}
/* ключ-«индекс» (0, 1, 2…): как в песочнице, такие ключи идут первыми по возрастанию */
static int key_index(const Str *k, unsigned long long *out) {
    if (!k->len || k->len > 10) return 0;
    if (k->s[0] == '0' && k->len > 1) return 0;
    unsigned long long v = 0;
    for (int i = 0; i < k->len; i++) { if (k->s[i] < '0' || k->s[i] > '9') return 0; v = v * 10 + (unsigned)(k->s[i] - '0'); }
    if (v >= 4294967295ULL) return 0;
    *out = v; return 1;
}
static void obj_set(VM *vm, Obj *o, Str *k, Value x) {
    int i = obj_find(o, k);
    if (vm->nmk && (i < 0 || is_ptr_val(x))) esc_container(vm, o);
    if (i >= 0) { o->v[i] = x; return; }
    if (o->n == o->cap) {
        int nc = o->cap * 2;
        Str **nk = (Str **)amalloc(vm, sizeof(Str *) * (size_t)nc);
        Value *nv = (Value *)amalloc(vm, sizeof(Value) * (size_t)nc);
        memcpy(nk, o->k, sizeof(Str *) * (size_t)o->n);
        memcpy(nv, o->v, sizeof(Value) * (size_t)o->n);
        o->k = nk; o->v = nv; o->cap = nc;
    }
    int pos = o->n; unsigned long long kv;
    if (key_index(k, &kv)) {
        unsigned long long ov; pos = 0;
        if (o->n && key_index(o->k[o->n - 1], &ov) && ov < kv) pos = o->n;
        else while (pos < o->n && key_index(o->k[pos], &ov) && ov < kv) pos++;
    }
    for (int i = o->n; i > pos; i--) { o->k[i] = o->k[i - 1]; o->v[i] = o->v[i - 1]; }
    o->k[pos] = k; o->v[pos] = x; o->n++;
    if (pos != o->n - 1 || (o->h && o->n * 2 + 2 > o->hcap) || (!o->h && o->n >= 16)) obj_rehash(vm, o);
    else if (o->h) {
        unsigned m = (unsigned)o->hcap - 1, j = str_hash(k) & m;
        while (o->h[j] >= 0) j = (j + 1) & m;
        o->h[j] = pos;
    }
    (void)x;
}

static const char *type_name(Value v) {
    switch (v.t) {
        case T_NONE: return "none";
        case T_BOOL: return "bool";
        case T_INT: case T_BIG: return "int";
        case T_FLT: case T_DEC: return "float";
        case T_STR: return "string";
        case T_ARR: return "array";
        case T_OBJ: return "object";
        case T_FN: case T_NAT: return "function";
        case T_TYPE: return "type";
        case T_STREAM: return "stream";
        case T_TASK: return "task";
        case T_ERR: return "error";
    }
    return "?";
}

static void fmt_float(double f, char *buf, size_t cap) {
    if (isnan(f)) { snprintf(buf, cap, "nan"); return; }
    if (isinf(f)) { snprintf(buf, cap, f < 0 ? "-inf" : "inf"); return; }
    if (f == floor(f) && fabs(f) < 1e21) { if (f == 0) f = 0.0; snprintf(buf, cap, "%.1f", f); return; }
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, cap, "%.*g", p, f);
        if (strtod(buf, NULL) == f) break;
    }
}

static void repr_to(SB *sb, Value v, int top);
static Arr *stream_to_arr(VM *vm, Value v);
static void big_repr(SB *sb, Value v);
static void dec_repr(SB *sb, Value v);
static int dec_cmp_values(Value a, Value b);
static double big_to_double(Value v);
static const char *nat_name(int id);
static int big_eq(Value a, Value b);
static void repr_to(SB *sb, Value v, int top) {
    char buf[64];
    switch (v.t) {
        case T_NONE: sb_cs(sb, "none"); break;
        case T_BOOL: sb_cs(sb, v.u.b ? "true" : "false"); break;
        case T_INT: snprintf(buf, sizeof buf, "%lld", (long long)v.u.i); sb_cs(sb, buf); break;
        case T_FLT: fmt_float(v.u.f, buf, sizeof buf); sb_cs(sb, buf); break;
        case T_STR:
            if (top) { sb_add(sb, v.u.s->s, v.u.s->len); break; }
            sb_c(sb, '"');
            for (int i = 0; i < v.u.s->len; i++) {
                char c = v.u.s->s[i];
                if (c == '"') sb_cs(sb, "\\\"");
                else if (c == '\\') sb_cs(sb, "\\\\");
                else if (c == '\n') sb_cs(sb, "\\n");
                else if (c == '\t') sb_cs(sb, "\\t");
                else sb_c(sb, c);
            }
            sb_c(sb, '"');
            break;
        case T_ARR:
            sb_c(sb, '[');
            for (int i = 0; i < v.u.a->n; i++) { if (i) sb_cs(sb, ", "); repr_to(sb, v.u.a->v[i], 0); }
            sb_c(sb, ']');
            break;
        case T_OBJ:
            if (v.u.o->type) sb_add(sb, v.u.o->type->name->s, v.u.o->type->name->len);
            sb_c(sb, '[');
            if (v.u.o->n == 0) sb_c(sb, ':');
            for (int i = 0; i < v.u.o->n; i++) {
                if (i) sb_cs(sb, ", ");
                sb_add(sb, v.u.o->k[i]->s, v.u.o->k[i]->len);
                sb_cs(sb, ": ");
                repr_to(sb, v.u.o->v[i], 0);
            }
            sb_c(sb, ']');
            break;
        case T_FN:
            sb_cs(sb, "<fn ");
            sb_cs(sb, v.u.fn->name ? v.u.fn->name->s : "lambda");
            sb_c(sb, '>');
            break;
        case T_NAT: sb_cs(sb, "<fn "); sb_cs(sb, nat_name(v.u.nat)); sb_c(sb, '>'); break;
        case T_BIG: big_repr(sb, v); break;
        case T_DEC: dec_repr(sb, v); break;
        case T_TASK: sb_cs(sb, "<task>"); break;
        case T_STREAM: repr_to(sb, V_arr(stream_to_arr(sb->vm, v)), top); break;
        case T_TYPE: sb_cs(sb, "<type "); sb_add(sb, v.u.td->name->s, v.u.td->name->len); sb_c(sb, '>'); break;
        case T_ERR: sb_cs(sb, "error("); { Value s = V_str(v.u.s); repr_to(sb, s, 0); } sb_c(sb, ')'); break;
    }
}
static Str *to_str(VM *vm, Value v, int top) {
    if (v.t == T_STR && top) return v.u.s;
    SB sb; sb_init(vm, &sb);
    repr_to(&sb, v, top);
    return sb_str(&sb);
}

static int is_num(Value v) { return v.t == T_INT || v.t == T_FLT || v.t == T_BIG || v.t == T_DEC; }
static double dec_to_double(Value v);
static double as_f(Value v) { return v.t == T_INT ? (double)v.u.i : v.t == T_BIG ? big_to_double(v) : v.t == T_DEC ? dec_to_double(v) : v.u.f; }

static int val_eq(Value a, Value b) {
    if (is_num(a) && is_num(b)) {
        if (a.t == T_INT && b.t == T_INT) return a.u.i == b.u.i;
        if (a.t == T_BIG && b.t == T_BIG) return big_eq(a, b);
        if ((a.t == T_BIG && b.t == T_INT) || (a.t == T_INT && b.t == T_BIG)) return 0;
        if (a.t == T_DEC || b.t == T_DEC) return dec_cmp_values(a, b) == 0;
        return as_f(a) == as_f(b);
    }
    if (a.t != b.t) return 0;
    switch (a.t) {
        case T_NONE: return 1;
        case T_BOOL: return a.u.b == b.u.b;
        case T_STR: case T_ERR: return str_eq(a.u.s, b.u.s);
        case T_ARR:
            if (a.u.a == b.u.a) return 1;
            if (a.u.a->n != b.u.a->n) return 0;
            for (int i = 0; i < a.u.a->n; i++) if (!val_eq(a.u.a->v[i], b.u.a->v[i])) return 0;
            return 1;
        case T_OBJ: {
            if (a.u.o == b.u.o) return 1;
            if (a.u.o->type != b.u.o->type) return 0;
            if (a.u.o->n != b.u.o->n) return 0;
            for (int i = 0; i < a.u.o->n; i++) {
                int j = obj_find(b.u.o, a.u.o->k[i]);
                if (j < 0 || !val_eq(a.u.o->v[i], b.u.o->v[j])) return 0;
            }
            return 1;
        }
        case T_FN: return a.u.fn == b.u.fn;
        case T_NAT: return a.u.nat == b.u.nat;
        case T_TYPE: return a.u.td == b.u.td;
        case T_STREAM: return a.u.sm == b.u.sm;
        case T_TASK: return a.u.task == b.u.task;
        default: return 0;
    }
}

static Value deep_copy(VM *vm, Value v) {
    if (v.t == T_ARR) {
        Arr *a = arr_new(vm, v.u.a->n);
        for (int i = 0; i < v.u.a->n; i++) arr_push(vm, a, deep_copy(vm, v.u.a->v[i]));
        return V_arr(a);
    }
    if (v.t == T_OBJ) {
        Obj *o = obj_new(vm); o->type = v.u.o->type;
        for (int i = 0; i < v.u.o->n; i++) obj_set(vm, o, v.u.o->k[i], deep_copy(vm, v.u.o->v[i]));
        return V_obj(o);
    }
    return v;
}
static Value shallow_copy(VM *vm, Value v) {
    if (v.t == T_ARR) {
        Arr *a = arr_new(vm, v.u.a->n);
        for (int i = 0; i < v.u.a->n; i++) arr_push(vm, a, v.u.a->v[i]);
        return V_arr(a);
    }
    if (v.t == T_OBJ) {
        Obj *o = obj_new(vm); o->type = v.u.o->type;
        for (int i = 0; i < v.u.o->n; i++) obj_set(vm, o, v.u.o->k[i], v.u.o->v[i]);
        return V_obj(o);
    }
    return v;
}
/* ===== a2_big ===== */

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
/* ===== a3_dec ===== */
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
/* ===== b_lex ===== */

/* ---------------------------------------------------------------- ошибки с префиксом */
static void synerr(VM *vm, int line, const char *fmt, ...) {
    char tmp[400]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (line > 0) fail(vm, line, "SyntaxError (line %d): %s", line, tmp);
    fail(vm, line, "SyntaxError: %s", tmp);
}
static void rt(VM *vm, int line, const char *fmt, ...) {
    char tmp[400]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (line <= 0) line = vm->curline;
    if (line > 0) fail(vm, line, "RuntimeError (line %d): %s", line, tmp);
    fail(vm, line, "RuntimeError: %s", tmp);
}

/* ---------------------------------------------------------------- лексер */
typedef enum { TK_EOF, TK_NL, TK_INT, TK_FLT, TK_STR, TK_ISTR, TK_ID, TK_KW, TK_OP } TK;

typedef struct Tok {
    TK t;
    int line, col, sp, gen, del, adj;
    char o[4];
    Str *str;
    int64_t i;
    double f;
} Tok;

typedef struct TokVec { Tok *v; int n, cap; } TokVec;

static void tv_reserve(VM *vm, TokVec *tv, int need) {
    if (need <= tv->cap) return;
    int nc = tv->cap ? tv->cap : 256;
    while (nc < need) nc *= 2;
    Tok *nv = (Tok *)amalloc(vm, sizeof(Tok) * (size_t)nc);
    if (tv->n) memcpy(nv, tv->v, sizeof(Tok) * (size_t)tv->n);
    tv->v = nv; tv->cap = nc;
}
static void tv_push(VM *vm, TokVec *tv, Tok t) { tv_reserve(vm, tv, tv->n + 1); tv->v[tv->n++] = t; }
/* заменить del токенов начиная с s на ins[0..nins) */
static void tv_splice(VM *vm, TokVec *tv, int s, int del, const Tok *ins, int nins) {
    int nn = tv->n - del + nins;
    Tok *nv = (Tok *)amalloc(vm, sizeof(Tok) * (size_t)(nn + 16));
    memcpy(nv, tv->v, sizeof(Tok) * (size_t)s);
    if (nins) memcpy(nv + s, ins, sizeof(Tok) * (size_t)nins);
    memcpy(nv + s + nins, tv->v + s + del, sizeof(Tok) * (size_t)(tv->n - s - del));
    tv->v = nv; tv->n = nn; tv->cap = nn + 16;
}

static const char *KEYWORDS[] = { "fn", "return", "match", "is", "else", "as", "and", "or", "not", "true", "false", "none", "when", "in", "await", "next", "stop", NULL };

static int op_is(const Tok *t, const char *o) { return t->t == TK_OP && strcmp(t->o, o) == 0; }
static int kw_is(const Tok *t, const char *k) { return t->t == TK_KW && strcmp(t->str->s, k) == 0; }
static int id_is(const Tok *t, const char *k) { return t->t == TK_ID && strcmp(t->str->s, k) == 0; }
static Tok mk_tok(TK t, const char *o, int line) {
    Tok r; memset(&r, 0, sizeof r); r.t = t; r.line = line; r.sp = 1;
    if (o) strncpy(r.o, o, 3);
    return r;
}
static Tok mk_id(VM *vm, const char *name, int line) {
    Tok r = mk_tok(TK_ID, NULL, line); r.str = intern(vm, name, (int)strlen(name)); return r;
}

static int is_idstart(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static int is_idchar(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

static void lex(VM *vm, const char *src, int n, TokVec *tv, int line0) {
    int i = 0, line = line0, ls = 0;
    while (i < n) {
        char c = src[i];
        if (c == '\n') {
            if (tv->n && tv->v[tv->n - 1].t != TK_NL) tv_push(vm, tv, mk_tok(TK_NL, NULL, line));
            line++; i++; ls = i; continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') i++; continue; }
        Tok t; memset(&t, 0, sizeof t);
        t.line = line; t.col = i - ls; t.sp = i > 0 && isspace((unsigned char)src[i - 1]);
        if (isdigit((unsigned char)c)) {
            int j = i;
            while (j < n && isdigit((unsigned char)src[j])) j++;
            int isf = 0;
            if (j + 1 < n && src[j] == '.' && isdigit((unsigned char)src[j + 1])) { isf = 1; j++; while (j < n && isdigit((unsigned char)src[j])) j++; }
            if (!isf && j < n && src[j] == '_') { /* дырка уровня: 1_, 2_1 */
                int e = j + 1; while (e < n && isdigit((unsigned char)src[e])) e++;
                if (!(e < n && is_idchar((unsigned char)src[e]))) { t.t = TK_ID; t.str = intern(vm, src + i, e - i); i = e; tv_push(vm, tv, t); continue; }
            }
            char buf[80]; int len = j - i;
            if (len > 70) synerr(vm, line, "number literal too long");
            memcpy(buf, src + i, (size_t)len); buf[len] = 0;
            if (j < n && src[j] == 'd' && !(j + 1 < n && is_idchar((unsigned char)src[j + 1]))) {   /* 0.1d, 5d: точное десятичное */
                t.t = TK_FLT; t.f = strtod(buf, NULL); t.gen = 1; t.str = mkstr(vm, buf, len);
                i = j + 1; tv_push(vm, tv, t); continue;
            }
            if (isf) { t.t = TK_FLT; t.f = strtod(buf, NULL); t.str = mkstr(vm, buf, len); }
            else {
                t.t = TK_INT;
                if (len > 19 || (len == 19 && strcmp(buf, "9223372036854775807") > 0)) { t.gen = 1; t.str = mkstr(vm, buf, len); } /* большое целое */
                else t.i = (int64_t)strtoull(buf, NULL, 10);
            }
            i = j; tv_push(vm, tv, t); continue;
        }
        if (is_idstart((unsigned char)c)) {
            int j = i;
            while (j < n && is_idchar((unsigned char)src[j])) j++;
            int len = j - i;
            t.t = TK_ID; t.str = intern(vm, src + i, len);
            for (int k = 0; KEYWORDS[k]; k++) if ((int)strlen(KEYWORDS[k]) == len && memcmp(KEYWORDS[k], src + i, (size_t)len) == 0) { t.t = TK_KW; break; }
            i = j; tv_push(vm, tv, t); continue;
        }
        if (c == '"' && i + 2 < n && src[i + 1] == '"' && src[i + 2] == '"') {
            int j = i + 3;
            while (j + 2 < n && !(src[j] == '"' && src[j + 1] == '"' && src[j + 2] == '"')) j++;
            if (j + 2 >= n) synerr(vm, line, "unterminated multiline string");
            int bs = i + 3; if (bs < j && src[bs] == '\n') bs++;
            t.t = TK_STR; t.str = mkstr(vm, src + bs, j - bs);
            for (int q = i; q < j + 3; q++) if (src[q] == '\n') { line++; ls = q + 1; }
            i = j + 3; tv_push(vm, tv, t); continue;
        }
        if (c == '`' || c == '"' || c == '\'') {
            SB sb; sb_init(vm, &sb);
            int j = i + 1;
            for (;;) {
                if (j >= n || src[j] == '\n') synerr(vm, line, c == '`' ? "unterminated string" : "unterminated string", c);
                char d = src[j];
                if (d == c) { j++; break; }
                if (d == '\\') {
                    char e = j + 1 < n ? src[j + 1] : 0;
                    if (e == 'n') sb_c(&sb, '\n');
                    else if (e == 't') sb_c(&sb, '\t');
                    else if (e == '\\') sb_c(&sb, '\\');
                    else if (e == c || (c != '`' && (e == '"' || e == '\''))) sb_c(&sb, e);
                    else { sb_c(&sb, '\\'); if (e) sb_c(&sb, e); }
                    j += 2; continue;
                }
                sb_c(&sb, d); j++;
            }
            t.t = c == '`' ? TK_ISTR : TK_STR; t.str = sb_str(&sb);
            i = j; tv_push(vm, tv, t); continue;
        }
        t.t = TK_OP;
        if (i + 2 < n && src[i] == '.' && src[i + 1] == '.' && src[i + 2] == '.') { memcpy(t.o, "...", 3); i += 3; tv_push(vm, tv, t); continue; }
        static const char *OPS2[] = { "..", ":=", "::", "->", "==", "!=", "<=", ">=", "**", "??", "+=", "-=", "*=", "/=", "?.", "<<", ">>", NULL };
        int done = 0;
        for (int k = 0; OPS2[k]; k++) if (i + 1 < n && src[i] == OPS2[k][0] && src[i + 1] == OPS2[k][1]) { memcpy(t.o, OPS2[k], 2); i += 2; done = 1; break; }
        if (!done) {
            if (c && strchr("+-*/%<>=()[]{},.:@&|^", c)) { t.o[0] = c; i++; }
            else synerr(vm, line, "unexpected character \"%c\"", c);
        }
        if (t.o[0] == ':' && t.o[1] == 0 && i < n && src[i] == '(' && i >= 2 && !isspace((unsigned char)src[i - 2])) t.adj = 1; /* f:(…) без пробела */
        tv_push(vm, tv, t);
    }
    if (tv->n && tv->v[tv->n - 1].t != TK_NL) tv_push(vm, tv, mk_tok(TK_NL, NULL, line));
    tv_push(vm, tv, mk_tok(TK_EOF, NULL, line));
}

static int is_assign_op(const Tok *t) {
    if (t->t != TK_OP) return 0;
    return !strcmp(t->o, "=") || !strcmp(t->o, ":=") || !strcmp(t->o, "+=") || !strcmp(t->o, "-=") || !strcmp(t->o, "*=") || !strcmp(t->o, "/=");
}
static int is_open(const Tok *t) { return t->t == TK_OP && t->o[1] == 0 && (t->o[0] == '(' || t->o[0] == '[' || t->o[0] == '{'); }
static int is_close(const Tok *t) { return t->t == TK_OP && t->o[1] == 0 && (t->o[0] == ')' || t->o[0] == ']' || t->o[0] == '}'); }
static int ends_expr(const Tok *t) {
    return t->t == TK_ID || t->t == TK_INT || t->t == TK_FLT || t->t == TK_STR || t->t == TK_ISTR ||
           (t->t == TK_KW && (kw_is(t, "true") || kw_is(t, "false") || kw_is(t, "none"))) || is_close(t);
}

/* фигурные скобки в Prism не нужны: обычная синтаксическая ошибка, а если похоже на объект — подсказка */
static void stray_brace(VM *vm, Tok *o, int n, int i) {
    Tok *t = &o[i];
    if (t->o[0] == '{') {
        int d = 0, k = i + 1, has_key = 0;
        for (; k < n; k++) {
            Tok *x = &o[k]; if (x->t != TK_OP) continue;
            char c = x->o[0];
            if (x->o[1] == 0 && (c == '(' || c == '[' || c == '{')) d++;
            else if (x->o[1] == 0 && (c == ')' || c == ']' || c == '}')) { if (d == 0) break; d--; }
            else if (d == 0 && c == ':' && x->o[1] == 0) has_key = 1;
        }
        int closed = k < n && o[k].t == TK_OP && o[k].o[0] == '}';
        int empty = closed;
        for (int m = i + 1; empty && m < k; m++) if (o[m].t != TK_NL) empty = 0;
        if (has_key) synerr(vm, t->line, "unexpected character \"{\"");
        if (empty) synerr(vm, t->line, "unexpected character \"{\"");
    }
    synerr(vm, t->line, "unexpected character \"%c\"", t->o[0]);
}

/* 1. скобки: [k: v] -> объект, [:] -> пустой объект, [a:, b:] -> шаблон; type P [x, y] и as [a, b] -> список полей */
static void bracket_pass(VM *vm, TokVec *tv) {
    int n = tv->n;
    Tok *o = tv->v;
    int *match = (int *)amalloc(vm, sizeof(int) * (size_t)(n + 1));
    int *st = (int *)amalloc(vm, sizeof(int) * (size_t)(n + 1)); int sp = 0;
    for (int i = 0; i < n; i++) match[i] = -1;
    for (int i = 0; i < n; i++) {
        if (o[i].t == TK_OP && (o[i].o[0] == '{' || o[i].o[0] == '}') && o[i].o[1] == 0) stray_brace(vm, o, n, i);
        if (is_open(&o[i])) st[sp++] = i;
        else if (is_close(&o[i])) {
            if (!sp) synerr(vm, o[i].line, "unmatched \"%s\"", o[i].o);
            int j = st[--sp]; match[j] = i; match[i] = j;
        }
    }
    if (sp) synerr(vm, o[st[sp - 1]].line, "unclosed \"%s\"", o[st[sp - 1]].o);
    for (int i = 0; i < n; i++) {
        Tok *t = &o[i];
        if (!(t->t == TK_OP && t->o[0] == '[' && t->o[1] == 0)) continue;
        Tok *p = i > 0 ? &o[i - 1] : NULL, *p2 = i > 1 ? &o[i - 2] : NULL;
        int j = match[i];
        if (p && p->t != TK_NL && ends_expr(p) && !t->sp && !(p->t == TK_ID && p2 && id_is(p2, "type"))) continue;
        int fields = (p && kw_is(p, "as")) || (p && p->t == TK_ID && p2 && id_is(p2, "type") && (i < 3 || o[i - 3].t == TK_NL));
        int obj = fields, empty = 0;
        if (!obj) {
            if (j == i + 2 && op_is(&o[i + 1], ":")) { obj = 1; empty = 1; }
            else {
                int d = 0;
                for (int k = i + 1; k < j; k++) {
                    Tok *x = &o[k]; if (x->t != TK_OP) continue;
                    if (is_open(x)) d++; else if (is_close(x)) d--;
                    else if (d == 0 && !strcmp(x->o, ":") && !x->adj) { obj = 1; break; }
                }
            }
        }
        if (!obj) continue;
        o[i].o[0] = '{'; o[i].gen = 1; o[j].o[0] = '}'; o[j].gen = 1;
        if (empty) o[i + 1].del = 1;
        else if (!fields) {
            int d = 0;
            for (int k = i + 1; k < j; k++) {
                Tok *x = &o[k]; if (x->t != TK_OP) continue;
                if (is_open(x)) d++; else if (is_close(x)) d--;
                else if (d == 0 && !strcmp(x->o, ":") && !x->adj) {
                    Tok *nx = &o[k + 1];
                    if (k + 1 == j || op_is(nx, ",") || nx->t == TK_NL) x->del = 1;
                }
            }
        }
    }
    int w = 0;
    for (int i = 0; i < n; i++) if (!o[i].del) o[w++] = o[i];
    tv->n = w;
}

/* 2. слова where / select: A where B select C -> (A).filter(B).map(C); ведущая .имя в B и C = _.имя */
static int is_low(const Tok *t) { return t->t == TK_ID && (id_is(t, "where") || id_is(t, "select")); }
static int prev_blocks_dot(const Tok *pv) { /* перед .имя / ["имя"] нет выражения */
    if (!pv) return 1;
    if (pv->t == TK_OP && !is_close(pv)) return 1;
    if (pv->t == TK_KW && (kw_is(pv, "and") || kw_is(pv, "or") || kw_is(pv, "not") || kw_is(pv, "is") || kw_is(pv, "in"))) return 1;
    return 0;
}
static void where_pass(VM *vm, TokVec *tv) {
    for (int guard = 0; guard < 5000; guard++) {
        Tok *a = tv->v; int n = tv->n;
        int i = -1;
        for (int k = 0; k < n; k++) if (is_low(&a[k])) { i = k; break; }
        if (i < 0) break;
        Tok kw = a[i];
        int s = i, d = 0;
        for (int k = i - 1; k >= 0; k--) {
            Tok *x = &a[k];
            if (is_close(x)) { d++; s = k; continue; }
            if (is_open(x)) { if (d == 0) { s = k + 1; break; } d--; s = k; continue; }
            if (d > 0) { s = k; continue; }
            if (x->t == TK_NL) {
                Tok *nx = &a[k + 1];
                if (is_low(nx) || op_is(nx, ".") || op_is(nx, "::")) { s = k + 1; continue; }
                s = k + 1; break;
            }
            if (x->t == TK_OP && (is_assign_op(x) || !strcmp(x->o, "->") || !strcmp(x->o, ","))) { s = k + 1; break; }
            if (x->t == TK_KW && (kw_is(x, "return") || kw_is(x, "else") || kw_is(x, "is"))) { s = k + 1; break; }
            s = k;
        }
        while (s < n && a[s].t == TK_NL) s++;
        if (s >= i) synerr(vm, kw.line, "expected expression before %s", kw.str->s, kw.str->s);
        int e = n; d = 0;
        for (int k = i + 1; k < n; k++) {
            Tok *x = &a[k];
            if (x->t == TK_EOF) { e = k; break; }
            if (is_open(x)) { d++; continue; }
            if (is_close(x)) { if (d == 0) { e = k; break; } d--; continue; }
            if (d == 0 && (x->t == TK_NL || is_low(x) || op_is(x, "::") || op_is(x, ","))) { e = k; break; }
        }
        int rn = e - (i + 1);
        if (rn <= 0) synerr(vm, kw.line, "expected expression after %s", kw.str->s);
        Tok *r = a + i + 1;
        Tok *ins = (Tok *)amalloc(vm, sizeof(Tok) * (size_t)((i - s) + rn * 2 + 8));
        int ni = 0;
        int line = kw.line, col0 = a[s].col;
        Tok open = mk_tok(TK_OP, "(", line); open.col = col0; open.gen = 1;
        ins[ni++] = open;
        for (int k = s; k < i; k++) ins[ni++] = a[k];
        ins[ni++] = mk_tok(TK_OP, ")", line);
        ins[ni++] = mk_tok(TK_OP, ".", line);
        ins[ni++] = mk_id(vm, id_is(&kw, "where") ? "filter" : "map", line);
        ins[ni++] = mk_tok(TK_OP, "(", line);
        for (int m = 0; m < rn; m++) {
            Tok *x = &r[m];
            Tok *pv = m > 0 ? &r[m - 1] : NULL;
            if (op_is(x, "[") && m + 2 < rn && r[m + 1].t == TK_STR && op_is(&r[m + 2], "]")) {
                if (prev_blocks_dot(pv)) ins[ni++] = mk_id(vm, "_", x->line);
            } else if (op_is(x, ".") && m + 1 < rn && r[m + 1].t == TK_ID) {
                if (prev_blocks_dot(pv)) ins[ni++] = mk_id(vm, "_", x->line);
            }
            ins[ni++] = *x;
        }
        ins[ni++] = mk_tok(TK_OP, ")", line);
        tv_splice(vm, tv, s, e - s, ins, ni);
    }
}

/* 3. отступы -> блоки */
typedef struct Ln { int s, e; } Ln;
static int line_is_header(const Tok *L, int n) {
    if (n < 2 || L[0].t != TK_ID || !op_is(&L[n - 1], "=")) return 0;
    int d = 0;
    for (int k = 1; k < n - 1; k++) {
        const Tok *x = &L[k];
        if (x->t != TK_OP) continue;
        if (is_open(x)) d++;
        else if (is_close(x)) d--;
        else if (d == 0 && (is_assign_op(x) || !strcmp(x->o, ".") || !strcmp(x->o, "::") || !strcmp(x->o, "->"))) return 0;
    }
    return 1;
}
static int line_foreach_tail(const Tok *L, int n) {
    int k = n - 1;
    while (k >= 0 && (L[k].t == TK_ID || op_is(&L[k], ","))) k--;
    return k >= 0 && op_is(&L[k], "::");
}
static void layout_pass(VM *vm, TokVec *in, TokVec *out) {
    Ln *lines = (Ln *)amalloc(vm, sizeof(Ln) * (size_t)(in->n + 1));
    int nl = 0, depth = 0, start = 0;
    for (int i = 0; i < in->n; i++) {
        Tok *t = &in->v[i];
        if (t->t == TK_EOF) break;
        if (t->t == TK_NL && depth == 0) { if (i > start) { lines[nl].s = start; lines[nl].e = i; nl++; } start = i + 1; continue; }
        if (is_open(t)) depth++; else if (is_close(t)) depth--;
    }
    if (in->n > 0) {
        int e = in->n; while (e > start && (in->v[e - 1].t == TK_EOF || in->v[e - 1].t == TK_NL)) e--;
        if (e > start) { lines[nl].s = start; lines[nl].e = e; nl++; }
    }
    int *stack = (int *)amalloc(vm, sizeof(int) * (size_t)(nl + 2)); int sp = 0;
    int lastline = 1;
    for (int li = 0; li < nl; li++) {
        int n0 = lines[li].e - lines[li].s;
        Tok *L = (Tok *)amalloc(vm, sizeof(Tok) * (size_t)(n0 + 4));
        memcpy(L, in->v + lines[li].s, sizeof(Tok) * (size_t)n0);
        int n = n0;
        int indent = L[0].col, ln = L[0].line;
        while (sp > 0 && indent < stack[sp - 1]) {
            sp--; Tok b = mk_tok(TK_OP, "}", ln); b.gen = 1; tv_push(vm, out, b); tv_push(vm, out, mk_tok(TK_NL, NULL, ln));
        }
        /* xs::[par] a[,b] -> тело (в одну строку) */
        for (int k = 0; k < n; k++) {
            if (!op_is(&L[k], "::")) continue;
            int e = k + 1;
            while (e < n && (L[e].t == TK_ID || op_is(&L[e], ","))) e++;
            if (e > k + 1 && e < n && op_is(&L[e], "->") && (e - k > 2 || id_is(&L[k + 1], "par") || (L[e - 1].t == TK_ID && e - 1 > k + 1))) {
                Tok b = mk_tok(TK_OP, "{", ln); b.gen = 1; L[e] = b;
                Tok c = mk_tok(TK_OP, "}", ln); c.gen = 1; L[n++] = c;
                break;
            }
        }
        const Tok *last = &L[n - 1];
        int open = 0, drop = 0;
        if (li + 1 < nl) {
            const Tok *N = &in->v[lines[li + 1].s];
            if (N->col > indent) {
                if (op_is(last, "->") || kw_is(last, "match") || line_foreach_tail(L, n)) open = 1;
                else if (line_is_header(L, n)) { open = 1; drop = 1; }
            }
        }
        for (int k = 0; k < n - drop; k++) tv_push(vm, out, L[k]);
        if (open) {
            Tok b = mk_tok(TK_OP, "{", L[n - 1].line); b.gen = 1; tv_push(vm, out, b);
            stack[sp++] = in->v[lines[li + 1].s].col;
        }
        tv_push(vm, out, mk_tok(TK_NL, NULL, L[n - 1].line));
        lastline = L[n - 1].line;
    }
    while (sp > 0) {
        sp--; Tok b = mk_tok(TK_OP, "}", lastline); b.gen = 1; tv_push(vm, out, b); tv_push(vm, out, mk_tok(TK_NL, NULL, lastline));
    }
    tv_push(vm, out, mk_tok(TK_EOF, NULL, lastline));
}

/* 4. многострочная лямбда внутри скобок: f(x ->⏎    тело⏎) — тело идёт до сдвига влево или до закрывающей скобки */
static void lambda_block_pass(VM *vm, TokVec *tv) {
    int depth = 0;
    for (int i = 0; i < tv->n; i++) {
        Tok *t = &tv->v[i];
        if (is_open(t)) depth++; else if (is_close(t)) depth--;
        if (!(depth > 0 && op_is(t, "->") && i + 1 < tv->n && tv->v[i + 1].t == TK_NL)) continue;
        int b = i + 1; while (b < tv->n && tv->v[b].t == TK_NL) b++;
        if (b >= tv->n || tv->v[b].t == TK_EOF) continue;
        int bodyCol = tv->v[b].col, d = 0, e = -1;
        for (int k = b; k < tv->n; k++) {
            Tok *x = &tv->v[k];
            if (x->t == TK_EOF) { e = k; break; }
            if (is_open(x)) { d++; continue; }
            if (is_close(x)) { if (d == 0) { e = k; break; } d--; continue; }
            if (d == 0 && x->t == TK_NL) {
                int m = k + 1; while (m < tv->n && tv->v[m].t == TK_NL) m++;
                if (m >= tv->n || tv->v[m].t == TK_EOF || tv->v[m].col < bodyCol || is_close(&tv->v[m])) { e = k; break; }
            }
        }
        if (e < 0) continue;
        int line = t->line;
        int be = e; while (be > b && tv->v[be - 1].t == TK_NL) be--;
        TokVec body = { 0 }, inner = { 0 };
        for (int k = b; k < be; k++) tv_push(vm, &body, tv->v[k]);
        lambda_block_pass(vm, &body);
        layout_pass(vm, &body, &inner);
        int in_n = inner.n; if (in_n > 0 && inner.v[in_n - 1].t == TK_EOF) in_n--;
        Tok *ins = (Tok *)amalloc(vm, sizeof(Tok) * (size_t)(in_n + 2));
        int ni = 0;
        Tok ob = mk_tok(TK_OP, "{", line); ob.gen = 1; ins[ni++] = ob;
        for (int k = 0; k < in_n; k++) ins[ni++] = inner.v[k];
        Tok cb = mk_tok(TK_OP, "}", line); cb.gen = 1; ins[ni++] = cb;
        tv_splice(vm, tv, i + 1, e - (i + 1), ins, ni);
    }
}

/* исходный текст -> поток токенов ядра */
static void lex_src(VM *vm, const char *src, int n, TokVec *out, int with_layout) {
    TokVec raw = { 0 };
    lex(vm, src, n, &raw, 1);
    bracket_pass(vm, &raw);
    where_pass(vm, &raw);
    if (with_layout) { lambda_block_pass(vm, &raw); layout_pass(vm, &raw, out); }
    else *out = raw;
}
/* ===== c_parse ===== */

/* ---------------------------------------------------------------- AST */
typedef struct Pat Pat;

enum { P_ID, P_LIT, P_COND, P_OBJ, P_ARR, P_TYPED };
struct Pat {
    int k, line, n;
    Str *name, *type, *rest;
    Node *expr;
    Pat *sub;
    Pat **items;
    Str **keys;
};

struct Clause {
    Pat **params; int np;
    int async;                /* в теле есть time.wait / await: вызов запускает задачу */
    Node **body; int nbody;   /* блок */
    Node *expr;               /* выражение (если не блок) */
    int isBlock;
    int simple;               /* 0 — не проверено, 1 — все параметры простые имена, 2 — нет */
    struct VCode *code; int vmstate, vmw;   /* байткод тела: vmstate 0 — не пробовали, 1 — скомпилировано, 2 — не подходит */
};

enum {
    N_INT, N_FLT, N_STR, N_ISTR, N_BOOL, N_NONE, N_IDENT, N_ARR, N_OBJ, N_SPREAD, N_LAMBDA, N_CALL, N_MEMBER,
    N_INDEX, N_SLICE, N_BIN, N_UN, N_CHAIN, N_IS, N_MATCH, N_ARM, N_FOREACH, N_AS, N_ASSIGN, N_RETURN, N_CTL,
    N_FNDECL, N_TYPEDECL, N_DESTR, N_PROP, N_IFMT, N_BLOCK, N_BIND, N_WHEN, N_AWAIT
};

struct Node {
    int type, line, op, flag, n, fold, chops, cidx, pure;
    Value dv;   /* N_FLT: готовое десятичное значение (вне арены) */
    Var *cv; Env *cte; unsigned cgen, cep; int ccr, chit, cfail;   /* кэш поиска имени (см. env_find_cxo) */
    Value (*fn)(VM *, struct Node *, Env *);   /* специализированный вычислитель узла (выбирается при первом выполнении) */
    Str *name;
    Value v;
    Node *a, *b, *c;
    Node **list;
    Str **names;
    int *ops;
    Pat *pat;
    Clause *cl; int ncl;
};

typedef struct NV { Node **v; int n, cap; } NV;
static void nv_push(VM *vm, NV *nv, Node *x) {
    if (nv->n == nv->cap) {
        int nc = nv->cap ? nv->cap * 2 : 8;
        Node **a = (Node **)amalloc(vm, sizeof(Node *) * (size_t)nc);
        if (nv->n) memcpy(a, nv->v, sizeof(Node *) * (size_t)nv->n);
        nv->v = a; nv->cap = nc;
    }
    nv->v[nv->n++] = x;
}

typedef struct Slot { int kind; Str *nm[24]; int n; int sid; Node *nodes[32]; int nn; } Slot;
typedef struct Parser {
    VM *vm;
    Tok *t; int n, pos;
    int noArrow; int sidN; int arm;
    Slot *slots[64]; int nslots;
    char dbuf[160];
} Parser;

static Node *mknode(Parser *p, int type, int line) {
    Node *n = (Node *)amalloc(p->vm, sizeof(Node));
    n->type = type; n->line = line; return n;
}
static Str *pi(Parser *p, const char *s) { return intern(p->vm, s, (int)strlen(s)); }

static Tok *peek(Parser *p, int k) { int i = p->pos + k; if (i >= p->n) i = p->n - 1; return &p->t[i]; }
static Tok *next(Parser *p) { Tok *t = &p->t[p->pos < p->n - 1 ? p->pos : p->n - 1]; if (p->pos < p->n - 1) p->pos++; return t; }
static int isOp(Parser *p, const char *v, int k) { return op_is(peek(p, k), v); }
static int isKw(Parser *p, const char *v, int k) { return kw_is(peek(p, k), v); }
static void skipNl(Parser *p) { while (peek(p, 0)->t == TK_NL) p->pos++; }
static int nlCount(Parser *p) { int k = 0; while (peek(p, k)->t == TK_NL) k++; return k; }
static const char *desc(Parser *p, Tok *t) {
    switch (t->t) {
        case TK_EOF: return "end of file";
        case TK_NL: return "end of line";
        case TK_STR: snprintf(p->dbuf, sizeof p->dbuf, "\"\"%.100s\"\"", t->str->s); return p->dbuf;
        case TK_ISTR: snprintf(p->dbuf, sizeof p->dbuf, "\"%.100s\"", t->str->s); return p->dbuf;
        case TK_ID: case TK_KW: snprintf(p->dbuf, sizeof p->dbuf, "\"%.100s\"", t->str->s); return p->dbuf;
        case TK_INT: snprintf(p->dbuf, sizeof p->dbuf, "\"%lld\"", (long long)t->i); return p->dbuf;
        case TK_FLT: snprintf(p->dbuf, sizeof p->dbuf, "\"%g\"", t->f); return p->dbuf;
        default: snprintf(p->dbuf, sizeof p->dbuf, "\"%s\"", t->o); return p->dbuf;
    }
}
static void pfail(Parser *p, Tok *t, const char *fmt, ...) {
    char tmp[300]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    synerr(p->vm, (t ? t : peek(p, 0))->line, "%s", tmp);
}
static void expectOp(Parser *p, const char *v) {
    if (!isOp(p, v, 0)) {
        /* { и } создаёт сам отступ: в сообщении про них не говорим */
        if (!strcmp(v, "{")) pfail(p, NULL, "expected indented block, got %s", desc(p, peek(p, 0)));
        pfail(p, NULL, "expected \"%s\", got %s", v, desc(p, peek(p, 0)));
    }
    next(p);
}
static Str *expectId(Parser *p) {
    Tok *t = peek(p, 0);
    if (t->t != TK_ID) pfail(p, NULL, "expected identifier, got %s", desc(p, t));
    return next(p)->str;
}
static int findClose(Parser *p, int from) {
    int d = 0;
    for (int k = from; k < p->n; k++) {
        Tok *t = &p->t[k];
        if (t->t == TK_OP) {
            if (is_open(t)) d++;
            else if (is_close(t)) { d--; if (d == 0) return k; }
        }
    }
    return -1;
}
static int arrowAfterGroup(Parser *p) {
    if (p->noArrow) return 0;
    int c = findClose(p, p->pos);
    if (c < 0) return 0;
    int k = c + 1; while (k < p->n && p->t[k].t == TK_NL) k++;
    return k < p->n && op_is(&p->t[k], "->");
}

static Node *parseExpr(Parser *p);
static Node *parseBinary(Parser *p, int min);
static Node *parseBinaryFrom(Parser *p, Node *left, int min);
static Node *parseUnary(Parser *p);
static Node *parsePostfix(Parser *p, Node *e);
static Node *parsePrimary(Parser *p);
static Pat *parsePat(Parser *p, int mm);
static Pat *parsePatValue(Parser *p, int mm);
static Node *parseIsExpr(Parser *p, int allowNoElse);
static Node *parseMatch(Parser *p, Node *subject);
static Node *parseLambdaHere(Parser *p);
static Node **parseStatements(Parser *p, int untilBrace, int *outn);
static Node **parseBlock(Parser *p, int *outn);
static Node *parseStatement(Parser *p);
static Node *parseIStr(Parser *p, Str *raw, int line);

static int binPrec(const char *o) {
    static const struct { const char *o; int p; } T[] = {
        {"or",1},{"and",2},{"??",3},{"==",4},{"!=",4},{"<",4},{">",4},{"<=",4},{">=",4},{"in",4},{"is",4},
        {"|",5},{"^",6},{"&",7},{"<<",8},{">>",8},{"+",9},{"-",9},{"*",10},{"/",10},{"%",10},{"**",11},{NULL,0}};
    for (int i = 0; T[i].o; i++) if (!strcmp(T[i].o, o)) return T[i].p;
    return -1;
}
static int opCode(const char *o) {
    static const struct { const char *o; int c; } T[] = {
        {"+",OP_ADD},{"-",OP_SUB},{"*",OP_MUL},{"/",OP_DIV},{"%",OP_MOD},{"**",OP_POW},{"==",OP_EQ},{"!=",OP_NE},
        {"<",OP_LT},{">",OP_GT},{"<=",OP_LE},{">=",OP_GE},{"and",OP_AND},{"or",OP_OR},{"in",OP_IN},{"is",OP_IS},{"??",OP_NC},{"&",OP_BAND},{"|",OP_BOR},{"^",OP_BXOR},{"<<",OP_SHL},{">>",OP_SHR},{NULL,0}};
    for (int i = 0; T[i].o; i++) if (!strcmp(T[i].o, o)) return T[i].c;
    return -1;
}
static int isCmp(int op) { return op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE; }
static int isImplicitOp(const Tok *t) {
    if (t->t != TK_OP) return 0;
    static const char *I[] = { "%", "+", "*", "/", "**", "==", "!=", "<", ">", "<=", ">=", NULL };
    for (int i = 0; I[i]; i++) if (!strcmp(t->o, I[i])) return 1;
    return 0;
}

/* ---------------------------------------------------------------- «дырки» _ */
static int isHoleName(Str *s) { /* _ или _N */
    if (s->s[0] != '_') return 0;
    if (s->len == 1) return 1;
    for (int i = 1; i < s->len; i++) if (!isdigit((unsigned char)s->s[i])) return 0;
    return 1;
}
static Pat *mkIdPat(Parser *p, Str *name, int line) { Pat *x = (Pat *)amalloc(p->vm, sizeof(Pat)); x->k = P_ID; x->name = name; x->line = line; return x; }

static Node *mkLambdaFromSlot(Parser *p, Slot *col, Node *body, Node **stmts, int nst, int isBlock, int line) {
    int maxn = 0;
    for (int i = 0; i < col->n; i++) {
        Str *s = col->nm[i];
        if (s->s[0] == '_' && s->len > 1) { int k = atoi(s->s + 1); if (k > maxn) maxn = k; }
    }
    char suf[16] = ""; if (col->sid) snprintf(suf, sizeof suf, "$%d", col->sid);
    for (int i = 0; i < col->nn; i++) if (!strchr(col->nodes[i]->name->s, '$')) { char b[40]; snprintf(b, sizeof b, "%s%s", col->nodes[i]->name->s, suf); col->nodes[i]->name = pi(p, b); }
    int np = maxn;
    for (int i = 0; i < col->n; i++) if (!(col->nm[i]->s[0] == '_' && col->nm[i]->len > 1)) np++;
    Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
    c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * (size_t)(np + 1)); c->np = np;
    for (int i = 1; i <= maxn; i++) { char b[16]; snprintf(b, sizeof b, "_%d%s", i, suf); c->params[i - 1] = mkIdPat(p, pi(p, b), line); }
    int w = maxn;
    for (int i = 0; i < col->n; i++) if (!(col->nm[i]->s[0] == '_' && col->nm[i]->len > 1)) c->params[w++] = mkIdPat(p, col->nm[i], line);
    c->isBlock = isBlock;
    if (isBlock) { c->body = stmts; c->nbody = nst; } else c->expr = body;
    Node *n = mknode(p, N_LAMBDA, line); n->cl = c; n->ncl = 1;
    return n;
}
static void slotAdd(Slot *s, Str *nm) {
    for (int i = 0; i < s->n; i++) if (s->nm[i] == nm) return;
    if (s->n < 24) s->nm[s->n++] = nm;
}
static void pushSlot(Parser *p, Slot *s, int kind) {
    if (p->nslots >= 64) pfail(p, NULL, "nesting too deep");
    memset(s, 0, sizeof *s); s->kind = kind; p->slots[p->nslots++] = s;
}
static void popSlot(Parser *p) { p->nslots--; }

/* ---------------------------------------------------------------- операторы */
static Node **parseStatements(Parser *p, int untilBrace, int *outn) {
    NV out = { 0 };
    for (;;) {
        skipNl(p);
        Tok *t = peek(p, 0);
        if (t->t == TK_EOF) { if (untilBrace) pfail(p, NULL, "unclosed \"{\""); break; }
        if (isOp(p, "}", 0)) { if (untilBrace) break; pfail(p, NULL, "unmatched \"}\""); }
        Node *st = parseStatement(p);
        if (isOp(p, ",", 0) && (st->type == N_IDENT || st->type == N_MEMBER || st->type == N_INDEX)) {
            /* a, b := b, a — все правые части вычисляются до присваивания (через скрытые временные имена) */
            Node *ts[16]; Node *vs[16]; int nt = 1, nvs = 0; ts[0] = st; int mline = st->line;
            while (isOp(p, ",", 0)) {
                next(p); skipNl(p);
                if (nt >= 16) pfail(p, NULL, "multiple assignment: at most 16 targets");
                Node *t2 = parseUnary(p);
                if (t2->type != N_IDENT && t2->type != N_MEMBER && t2->type != N_INDEX) pfail(p, NULL, "multiple assignment: target must be a name, element or field");
                ts[nt++] = t2;
            }
            Tok *aop = peek(p, 0);
            if (!(aop->t == TK_OP && (!strcmp(aop->o, "=") || !strcmp(aop->o, ":=")))) pfail(p, NULL, "multiple assignment: expected = or := after the target list, got %s", desc(p, aop));
            int mop = !strcmp(aop->o, "=") ? 0 : 1;
            next(p); skipNl(p);
            for (;;) {
                if (nvs >= 16) pfail(p, NULL, "multiple assignment: at most 16 values");
                vs[nvs++] = parseExpr(p);
                if (isOp(p, ",", 0)) { next(p); skipNl(p); continue; }
                break;
            }
            if (nvs != nt) pfail(p, NULL, "multiple assignment: %d targets but %d values", nt, nvs);
            static int mcount = 0; int mid = mcount++;
            for (int k = 0; k < nt; k++) {
                char nm[40]; snprintf(nm, sizeof nm, "$m%d_%d", mid, k);
                Node *id = mknode(p, N_IDENT, mline); id->name = pi(p, nm);
                Node *as = mknode(p, N_ASSIGN, mline); as->a = id; as->b = vs[k]; as->op = 1;
                nv_push(p->vm, &out, as);
            }
            for (int k = 0; k < nt; k++) {
                char nm[40]; snprintf(nm, sizeof nm, "$m%d_%d", mid, k);
                Node *id = mknode(p, N_IDENT, mline); id->name = pi(p, nm);
                Node *as = mknode(p, N_ASSIGN, mline); as->a = ts[k]; as->b = id; as->op = mop;
                nv_push(p->vm, &out, as);
            }
            Tok *e2 = peek(p, 0);
            if (e2->t == TK_NL || e2->t == TK_EOF || isOp(p, "}", 0)) continue;
            pfail(p, NULL, "unexpected %s after expression", desc(p, e2));
        }
        Node *pv = out.n ? out.v[out.n - 1] : NULL;
        if (st->type == N_FNDECL && st->flag && pv && pv->type == N_FNDECL && pv->flag && pv->name == st->name) {
            Clause *nc = (Clause *)amalloc(p->vm, sizeof(Clause) * (size_t)(pv->ncl + st->ncl));
            memcpy(nc, pv->cl, sizeof(Clause) * (size_t)pv->ncl);
            memcpy(nc + pv->ncl, st->cl, sizeof(Clause) * (size_t)st->ncl);
            pv->cl = nc; pv->ncl += st->ncl;
        } else if (pv && pv->type == N_FNDECL && pv->flag && st->type == N_ASSIGN && st->op == 0 && st->a->type == N_IDENT && st->a->name == pv->name) {
            /* «имя = _ * …» после частных случаев — ещё один случай той же функции */
            Clause *nc = (Clause *)amalloc(p->vm, sizeof(Clause) * (size_t)(pv->ncl + 1));
            memcpy(nc, pv->cl, sizeof(Clause) * (size_t)pv->ncl);
            if (st->b->type == N_LAMBDA) nc[pv->ncl] = st->b->cl[0];
            else { memset(&nc[pv->ncl], 0, sizeof(Clause)); nc[pv->ncl].expr = st->b; }
            pv->cl = nc; pv->ncl += 1;
        } else nv_push(p->vm, &out, st);
        Tok *e = peek(p, 0); Tok *prev = &p->t[p->pos > 0 ? p->pos - 1 : 0];
        if (e->t == TK_NL || e->t == TK_EOF || isOp(p, "}", 0) || (prev->t == TK_OP && !strcmp(prev->o, "}"))) continue;
        pfail(p, NULL, "unexpected %s after expression", desc(p, e));
    }
    *outn = out.n;
    return out.v;
}
static Node **parseBlock(Parser *p, int *outn) {
    expectOp(p, "{");
    Node **s = parseStatements(p, 1, outn);
    expectOp(p, "}");
    return s;
}

static int isHeadTok(Tok *t) { return t->t == TK_ID || t->t == TK_INT || t->t == TK_FLT || t->t == TK_STR || (t->t == TK_KW && (kw_is(t, "true") || kw_is(t, "false") || kw_is(t, "none"))); }
static int headMode(Parser *p) { /* 0 - нет, 1 - paren, 2 - bare */
    Tok *t = peek(p, 0); if (t->t != TK_ID) return 0;
    Tok *n1 = peek(p, 1);
    if (op_is(n1, "(")) {
        int c = findClose(p, p->pos + 1);
        if (c > 0 && c + 1 < p->n) { Tok *a = &p->t[c + 1]; return a->t == TK_OP && (!strcmp(a->o, "=") || !strcmp(a->o, "{")) ? 1 : 0; }
        return 0;
    }
    if (n1->t == TK_OP && n1->o[0] == '{' && n1->o[1] == 0 && n1->gen) return 2;
    if (isImplicitOp(n1)) {
        /* имя < 0 = …  — случай по условию на аргумент */
        int d = 0;
        for (int k = 1; ; k++) {
            Tok *x = peek(p, k);
            if (x->t == TK_NL || x->t == TK_EOF) return 0;
            if (x->t != TK_OP) continue;
            int single = x->o[1] == 0;
            if (single && (x->o[0] == '(' || x->o[0] == '[' || (x->o[0] == '{' && !(d == 0 && x->gen)))) d++;
            else if (single && (x->o[0] == ')' || x->o[0] == ']' || x->o[0] == '}')) d--;
            else if (d == 0 && ((single && x->o[0] == '=') || (single && x->o[0] == '{' && x->gen))) return 2;
        }
    }
    #define ISGRP(tk) ((tk)->t == TK_OP && (tk)->o[0] == '[' && (tk)->o[1] == 0 && (tk)->sp)
    if (isHeadTok(n1) || ISGRP(n1)) {
        int k = 1;
        for (;;) {
            Tok *x = peek(p, k);
            if (isHeadTok(x)) k++;
            else if (ISGRP(x)) { int c = findClose(p, p->pos + k); if (c <= 0) return 0; k = c - p->pos + 1; }
            else break;
        }
        Tok *a = peek(p, k);
        return a->t == TK_OP && (!strcmp(a->o, "=") || !strcmp(a->o, "{")) ? 2 : 0;
    }
    return 0;
}
static Value tok_int(Parser *p, Tok *t) { return t->gen ? big_parse(p->vm, t->str->s, t->str->len, 0) : V_int(t->i); }
static Pat *newPat(Parser *p, int k, int line) { Pat *x = (Pat *)amalloc(p->vm, sizeof(Pat)); x->k = k; x->line = line; return x; }
static Pat *litPat(Parser *p, Node *e) { Pat *x = newPat(p, P_LIT, e->line); x->expr = e; return x; }

static Pat *headParam(Parser *p, int inParens) {
    Tok *t = peek(p, 0);
    if (t->t == TK_ID && inParens) {
        Tok *nx = peek(p, 1);
        if (nx->t == TK_ID) { next(p); next(p); Pat *x = newPat(p, P_TYPED, t->line); x->type = t->str; x->sub = mkIdPat(p, nx->str, t->line); return x; }
        if (nx->t == TK_OP && (!strcmp(nx->o, "{") || !strcmp(nx->o, "["))) { next(p); Pat *x = newPat(p, P_TYPED, t->line); x->type = t->str; x->sub = parsePat(p, 1); return x; }
    }
    if (t->t == TK_ID) { next(p); return mkIdPat(p, t->str, t->line); }
    if (t->t == TK_INT) { next(p); Node *e = mknode(p, N_INT, t->line); e->v = tok_int(p, t); return litPat(p, e); }
    if (t->t == TK_FLT) { next(p); Node *e = mknode(p, N_FLT, t->line); e->v = V_flt(t->f); e->name = t->str; e->flag = t->gen; return litPat(p, e); }
    if (t->t == TK_STR) { next(p); Node *e = mknode(p, N_STR, t->line); e->v = V_str(t->str); return litPat(p, e); }
    if (kw_is(t, "true") || kw_is(t, "false")) { next(p); Node *e = mknode(p, N_BOOL, t->line); e->v = V_bool(kw_is(t, "true")); return litPat(p, e); }
    if (kw_is(t, "none")) { next(p); return litPat(p, mknode(p, N_NONE, t->line)); }
    if (op_is(t, "-") && (peek(p, 1)->t == TK_INT || peek(p, 1)->t == TK_FLT)) {
        next(p); Tok *x = next(p);
        Node *e = mknode(p, x->t == TK_INT ? N_INT : N_FLT, t->line);
        e->v = x->t == TK_INT ? (x->gen ? big_norm(big_neg(p->vm, big_of(p->vm, tok_int(p, x)))) : V_int(-x->i)) : V_flt(-x->f);
        if (x->t == TK_FLT && x->str) { char nb[96]; snprintf(nb, sizeof nb, "-%s", x->str->s); e->name = mkstr(p->vm, nb, (int)strlen(nb)); e->flag = x->gen; }
        return litPat(p, e);
    }
    if (op_is(t, "{") || op_is(t, "[")) return parsePat(p, 1);
    if (isImplicitOp(t)) {
        Node *left = mknode(p, N_IDENT, t->line); left->name = pi(p, "$subject");
        int sv = p->noArrow; p->noArrow = 1;
        Pat *x = newPat(p, P_COND, t->line); x->expr = parseBinaryFrom(p, left, 0);
        p->noArrow = sv; return x;
    }
    pfail(p, t, "function header: expected parameter name, literal or pattern, got %s", desc(p, t));
    return NULL;
}
static Node *parseHead(Parser *p, int mode) {
    Tok *t = next(p);
    Pat *params[64]; int np = 0;
    if (mode == 1) {
        next(p); skipNl(p);
        while (!isOp(p, ")", 0)) {
            if (np >= 64) pfail(p, NULL, "too many parameters");
            params[np++] = headParam(p, 1); skipNl(p);
            if (isOp(p, ",", 0)) { next(p); skipNl(p); }
            else if (!isOp(p, ")", 0)) pfail(p, NULL, "expected \",\" or \")\", got %s", desc(p, peek(p, 0)));
        }
        expectOp(p, ")");
    } else while (!isOp(p, "=", 0) && !isOp(p, "{", 0)) { if (np >= 64) pfail(p, NULL, "too many parameters"); params[np++] = headParam(p, 0); }
    Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
    c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * (size_t)(np + 1)); memcpy(c->params, params, sizeof(Pat *) * (size_t)np); c->np = np;
    if (isOp(p, "{", 0)) {
        int zero = mode == 2 && np == 0; /* «имя =» с телом на следующих строках */
        Slot col; memset(&col, 0, sizeof col);
        if (zero) pushSlot(p, &col, 'd');
        c->body = parseBlock(p, &c->nbody); c->isBlock = 1;
        if (zero) popSlot(p);
        if (zero && col.n) {
            Node *lam = mkLambdaFromSlot(p, &col, NULL, c->body, c->nbody, 1, t->line);
            Node *n = mknode(p, N_FNDECL, t->line); n->name = t->str; n->flag = 1; n->cl = lam->cl; n->ncl = 1; return n;
        }
        if (zero) {
            Node *blk = mknode(p, N_BLOCK, t->line); blk->list = c->body; blk->n = c->nbody;
            Node *id = mknode(p, N_IDENT, t->line); id->name = t->str;
            Node *as = mknode(p, N_ASSIGN, t->line); as->a = id; as->b = blk; as->op = 0; return as;
        }
    }
    else {
        next(p); skipNl(p);
        Slot col; pushSlot(p, &col, 'd');
        int sv = p->noArrow; p->noArrow = 0;
        Node *body = parseExpr(p);
        p->noArrow = sv; popSlot(p);
        if (col.n) body = mkLambdaFromSlot(p, &col, body, NULL, 0, 0, body->line);
        c->expr = body; c->isBlock = 0;
    }
    Node *n = mknode(p, N_FNDECL, t->line); n->name = t->str; n->flag = 1; n->cl = c; n->ncl = 1;
    return n;
}

static Pat **parseParamList(Parser *p, const char *close, int *outn) {
    Pat *tmp[64]; int n = 0; skipNl(p);
    while (!isOp(p, close, 0)) {
        if (n >= 64) pfail(p, NULL, "too many parameters");
        tmp[n++] = parsePat(p, 0); skipNl(p);
        if (isOp(p, ",", 0)) { next(p); skipNl(p); }
        else if (!isOp(p, close, 0)) pfail(p, NULL, "expected \",\" or \"%s\", got %s", close, desc(p, peek(p, 0)));
    }
    expectOp(p, close);
    Pat **r = (Pat **)amalloc(p->vm, sizeof(Pat *) * (size_t)(n + 1)); memcpy(r, tmp, sizeof(Pat *) * (size_t)n);
    *outn = n; return r;
}
static void parseFnBody(Parser *p, Clause *c) {
    skipNl(p);
    if (isOp(p, "->", 0)) {
        next(p); skipNl(p);
        if (isOp(p, "{", 0)) { c->body = parseBlock(p, &c->nbody); c->isBlock = 1; return; }
        int sv = p->noArrow; p->noArrow = 0; c->expr = parseExpr(p); p->noArrow = sv; c->isBlock = 0; return;
    }
    c->body = parseBlock(p, &c->nbody); c->isBlock = 1;
}

static Node *parseTypeDecl(Parser *p) {
    Tok *t = next(p); Str *name = next(p)->str; next(p); skipNl(p);
    Str *fields[64]; Node *defs[64]; int n = 0;
    while (!isOp(p, "}", 0)) {
        Tok *f = peek(p, 0);
        if (f->t != TK_ID) pfail(p, NULL, "type: expected field name, got %s", desc(p, f));
        next(p); Node *def = NULL;
        if (isOp(p, "=", 0)) { next(p); skipNl(p); int sv = p->noArrow; p->noArrow = 0; def = parseExpr(p); p->noArrow = sv; }
        if (n >= 64) pfail(p, NULL, "too many fields");
        fields[n] = f->str; defs[n] = def; n++;
        int sep = 0;
        while (isOp(p, ",", 0) || peek(p, 0)->t == TK_NL) { next(p); sep = 1; }
        if (!sep && !isOp(p, "}", 0)) pfail(p, NULL, "expected \",\" or \"}\", got %s", desc(p, peek(p, 0)));
    }
    expectOp(p, "}");
    Node *nd = mknode(p, N_TYPEDECL, t->line); nd->name = name; nd->n = n;
    nd->names = (Str **)amalloc(p->vm, sizeof(Str *) * (size_t)(n + 1)); memcpy(nd->names, fields, sizeof(Str *) * (size_t)n);
    nd->list = (Node **)amalloc(p->vm, sizeof(Node *) * (size_t)(n + 1)); memcpy(nd->list, defs, sizeof(Node *) * (size_t)n);
    return nd;
}

static Node *parseStatement(Parser *p) {
    Tok *t = peek(p, 0);
    if (t->t == TK_ID && id_is(t, "type") && peek(p, 1)->t == TK_ID && isOp(p, "{", 2)) return parseTypeDecl(p);
    if (kw_is(t, "is")) return parseIsExpr(p, 1);
    { int hm = headMode(p); if (hm) return parseHead(p, hm); }
    if (kw_is(t, "fn") && peek(p, 1)->t == TK_ID) {
        next(p); Str *name = next(p)->str; expectOp(p, "(");
        Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
        c->params = parseParamList(p, ")", &c->np);
        parseFnBody(p, c);
        Node *n = mknode(p, N_FNDECL, t->line); n->name = name; n->cl = c; n->ncl = 1;
        return n;
    }
    if (kw_is(t, "return")) {
        next(p); Tok *e = peek(p, 0);
        Node *n = mknode(p, N_RETURN, t->line); n->flag = p->arm > 0;
        if (e->t == TK_NL || e->t == TK_EOF || isOp(p, "}", 0)) return n;
        n->a = parseExpr(p); return n;
    }
    if (t->t == TK_OP && (!strcmp(t->o, "{") || !strcmp(t->o, "["))) {
        int c = findClose(p, p->pos);
        if (c >= 0 && c + 1 < p->n && p->t[c + 1].t == TK_OP && (!strcmp(p->t[c + 1].o, "=") || !strcmp(p->t[c + 1].o, ":="))) {
            Pat *pat = parsePat(p, 0);
            Tok *op = next(p);
            Node *n = mknode(p, N_DESTR, t->line); n->pat = pat; n->flag = !strcmp(op->o, ":=");
            n->a = parseExpr(p);
            return n;
        }
    }
    return parseExpr(p);
}

/* ---------------------------------------------------------------- выражения */
static Node *parseExpr(Parser *p) {
    int line = peek(p, 0)->line;
    Node *left = parseUnary(p);
    int ak = nlCount(p); Tok *at = peek(p, ak);
    if (at->t == TK_OP && is_assign_op(at) && (left->type == N_IDENT || left->type == N_MEMBER || left->type == N_INDEX)) {
        p->pos += ak; Tok *op = next(p); skipNl(p);
        Node *value;
        if (left->type == N_IDENT && (!strcmp(op->o, "=") || !strcmp(op->o, ":="))) {
            Slot col; pushSlot(p, &col, 'd');
            value = parseExpr(p);
            popSlot(p);
            Slot *up = p->nslots ? p->slots[p->nslots - 1] : NULL;
            if (col.n && up && value->type == N_IDENT && value->name->s[0] == '_' && value->name->len > 1 && isdigit((unsigned char)value->name->s[1])) {
                for (int i = 0; i < col.n; i++) slotAdd(up, col.nm[i]); /* k = _ : называет параметр охватывающей функции */
            } else if (col.n) value = mkLambdaFromSlot(p, &col, value, NULL, 0, 0, value->line);
        } else value = parseExpr(p);
        Node *n = mknode(p, N_ASSIGN, line);
        n->a = left; n->b = value;
        n->op = !strcmp(op->o, "=") ? 0 : !strcmp(op->o, ":=") ? 1 : !strcmp(op->o, "+=") ? 2 : !strcmp(op->o, "-=") ? 3 : !strcmp(op->o, "*=") ? 4 : 5;
        return n;
    }
    left = parseBinaryFrom(p, left, 0);
    for (;;) {
        int k = nlCount(p); Tok *t = peek(p, k);
        if (kw_is(t, "match")) { p->pos += k; left = parseMatch(p, left); } else break;
    }
    if (isKw(p, "when", 0)) {
        Tok *w = next(p); skipNl(p);
        int sv = p->noArrow; p->noArrow = 0;
        Node *cond = parseBinary(p, 0);
        p->noArrow = sv;
        Node *n = mknode(p, N_WHEN, w->line); n->a = left; n->b = cond; return n;
    }
    return left;
}
static Node *parseBinary(Parser *p, int min) { return parseBinaryFrom(p, parseUnary(p), min); }
static Node *parseBinaryFrom(Parser *p, Node *left, int min) {
    for (;;) {
        int k = nlCount(p); Tok *t = peek(p, k);
        const char *os = NULL;
        if (t->t == TK_OP && binPrec(t->o) >= 0) os = t->o;
        else if (t->t == TK_KW && (kw_is(t, "and") || kw_is(t, "or") || kw_is(t, "in") || kw_is(t, "is"))) os = t->str->s;
        if (!os) break;
        int op = opCode(os);
        if (op < 0) break;
        if (k > 0 && (op == OP_SUB || op == OP_IS)) break;
        int pr = binPrec(os);
        if (pr < min) break;
        p->pos += k; next(p); skipNl(p);
        Node *right = op == OP_POW ? parseBinary(p, pr) : parseBinary(p, pr + 1);
        if (isCmp(op) && left->type == N_BIN && isCmp(left->op) && !left->flag) {
            Node *c = mknode(p, N_CHAIN, t->line);
            c->n = 3; c->list = (Node **)amalloc(p->vm, sizeof(Node *) * 3);
            c->list[0] = left->a; c->list[1] = left->b; c->list[2] = right;
            c->ops = (int *)amalloc(p->vm, sizeof(int) * 2); c->ops[0] = left->op; c->ops[1] = op;
            left = c;
        } else if (isCmp(op) && left->type == N_CHAIN && !left->flag) {
            Node **nl = (Node **)amalloc(p->vm, sizeof(Node *) * (size_t)(left->n + 1));
            int *no = (int *)amalloc(p->vm, sizeof(int) * (size_t)left->n);
            memcpy(nl, left->list, sizeof(Node *) * (size_t)left->n); memcpy(no, left->ops, sizeof(int) * (size_t)(left->n - 1));
            nl[left->n] = right; no[left->n - 1] = op;
            left->list = nl; left->ops = no; left->n++;
        } else {
            Node *b = mknode(p, N_BIN, t->line); b->op = op; b->a = left; b->b = right;
            if ((op == OP_ADD || op == OP_SUB) && right->type == N_INT && right->v.t == T_INT) {
                /* цепочка «x ± 1 ± 2 …» с целыми литералами: считаем одной операцией */
                Node *base = NULL; int m = 0; Node **ls = NULL; int *os2 = NULL;
                if (left->type == N_BIN && left->fold) {
                    base = left->c; m = left->n;
                    ls = (Node **)amalloc(p->vm, sizeof(Node *) * (size_t)(m + 1)); os2 = (int *)amalloc(p->vm, sizeof(int) * (size_t)(m + 1));
                    memcpy(ls, left->list, sizeof(Node *) * (size_t)m); memcpy(os2, left->ops, sizeof(int) * (size_t)m);
                } else if (left->type == N_BIN && !left->fold && (left->op == OP_ADD || left->op == OP_SUB) && left->b->type == N_INT && left->b->v.t == T_INT) {
                    base = left->a;
                    ls = (Node **)amalloc(p->vm, sizeof(Node *) * 2); os2 = (int *)amalloc(p->vm, sizeof(int) * 2);
                    ls[0] = left->b; os2[0] = left->op; m = 1;
                }
                if (base) {
                    ls[m] = right; os2[m] = op; m++;
                    int64_t tot = 0; int ok = 1;
                    for (int i = 0; i < m && ok; i++) {
                        int64_t v = ls[i]->v.u.i;
                        if (os2[i] == OP_ADD ? __builtin_add_overflow(tot, v, &tot) : __builtin_sub_overflow(tot, v, &tot)) ok = 0;
                    }
                    if (ok) { b->fold = 1; b->c = base; b->list = ls; b->ops = os2; b->n = m; b->v = V_int(tot); }
                }
            }
            left = b;
        }
    }
    return left;
}
static Node *parseUnary(Parser *p) {
    Tok *t = peek(p, 0);
    if (kw_is(t, "is")) return parseIsExpr(p, 0);
    if (kw_is(t, "await")) { next(p); Node *n = mknode(p, N_AWAIT, t->line); n->a = parseUnary(p); return n; }
    if (op_is(t, "-")) { next(p); skipNl(p); Node *n = mknode(p, N_UN, t->line); n->op = OP_NEG; n->a = parseBinary(p, 11); return n; }
    if (kw_is(t, "not")) { next(p); skipNl(p); Node *n = mknode(p, N_UN, t->line); n->op = OP_NOT; n->a = parseBinary(p, 4); return n; }
    return parsePostfix(p, parsePrimary(p));
}

static int namesAhead(Parser *p, int k) {
    int i = k;
    for (;;) {
        if (peek(p, i)->t != TK_ID) return -1;
        i++;
        if (isOp(p, ",", i)) { i++; continue; }
        break;
    }
    return isOp(p, "{", i) ? i : -1;
}
static Node *parseBraceLambda(Parser *p);

static Node *parsePostfix(Parser *p, Node *e) {
    for (;;) {
        Tok *t = peek(p, 0);
        if (t->t == TK_OP) {
            if (!strcmp(t->o, "(")) {
                next(p);
                int sv = p->noArrow; p->noArrow = 0;
                NV args = { 0 }; skipNl(p);
                int isMethod = e->type == N_MEMBER;
                while (!isOp(p, ")", 0)) {
                    Node *x;
                    if (isMethod) {
                        Slot col; pushSlot(p, &col, 'm');
                        x = parseExpr(p); popSlot(p);
                        if (col.n) x = mkLambdaFromSlot(p, &col, x, NULL, 0, 0, x->line);
                    } else {
                        int inM = 0; for (int i = 0; i < p->nslots; i++) if ((p->slots[i]->kind == 'm' || p->slots[i]->kind == 'd')) inM = 1;
                        if (inM) x = parseExpr(p);
                        else {
                            Slot col; pushSlot(p, &col, 'p');
                            x = parseExpr(p); popSlot(p);
                            if (col.n) {
                                if (x->type == N_IDENT && x->name->s[0] == '_' && x->name->len > 1 && isdigit((unsigned char)x->name->s[1])) {
                                    Slot *up = p->nslots ? p->slots[p->nslots - 1] : NULL;
                                    if (!up) pfail(p, t, x->name == pi(p, "_1") ? "_ is only valid inside a call argument, a :: body or the right side of x = ..." : "%s has no enclosing function", x->name->s);
                                    for (int i = 0; i < col.n; i++) slotAdd(up, col.nm[i]);
                                } else x = mkLambdaFromSlot(p, &col, x, NULL, 0, 0, x->line);
                            }
                        }
                    }
                    nv_push(p->vm, &args, x);
                    skipNl(p);
                    if (isOp(p, ",", 0)) { next(p); skipNl(p); }
                    else if (!isOp(p, ")", 0)) pfail(p, NULL, "expected \",\" or \")\", got %s", desc(p, peek(p, 0)));
                }
                p->noArrow = sv;
                expectOp(p, ")");
                Node *c = mknode(p, N_CALL, t->line); c->a = e; c->list = args.v; c->n = args.n; e = c; continue;
            }
            if (!strcmp(t->o, ":") && t->adj) {
                next(p); next(p);
                int sv = p->noArrow; p->noArrow = 0;
                NV args = { 0 }; skipNl(p);
                while (!isOp(p, ")", 0)) {
                    nv_push(p->vm, &args, parseExpr(p)); skipNl(p);
                    if (isOp(p, ",", 0)) { next(p); skipNl(p); }
                    else if (!isOp(p, ")", 0)) pfail(p, NULL, "expected \",\" or \")\", got %s", desc(p, peek(p, 0)));
                }
                p->noArrow = sv;
                expectOp(p, ")");
                Node *b = mknode(p, N_BIND, t->line); b->a = e; b->list = args.v; b->n = args.n; e = b; continue;
            }
            if (!strcmp(t->o, "[")) {
                next(p);
                int sv = p->noArrow; p->noArrow = 0;
                skipNl(p);
                Node *a = NULL, *b = NULL; int slice = 0;
                if (!isOp(p, "..", 0)) a = parseExpr(p);
                if (isOp(p, "..", 0)) { next(p); slice = 1; if (!isOp(p, "]", 0) && peek(p, 0)->t != TK_NL) b = parseExpr(p); }
                skipNl(p);
                p->noArrow = sv;
                expectOp(p, "]");
                Node *n = mknode(p, slice ? N_SLICE : N_INDEX, t->line); n->a = e; n->b = a; n->c = b; e = n; continue;
            }
            if (!strcmp(t->o, ".") || !strcmp(t->o, "?.")) {
                next(p); skipNl(p); Str *nm = expectId(p);
                Node *n = mknode(p, N_MEMBER, t->line); n->a = e; n->name = nm; n->flag = t->o[0] == '?'; e = n; continue;
            }
            if (!strcmp(t->o, "::")) {
                next(p); skipNl(p);
                int par = 0, pairs = 0; Node *fn;
                if (peek(p, 0)->t == TK_ID && id_is(peek(p, 0), "par")) {
                    int k = 1; while (peek(p, k)->t == TK_NL) k++;
                    if (isOp(p, "{", k)) { p->pos += k; par = 1; }
                    else if (namesAhead(p, 1) >= 0) { next(p); par = 1; }
                }
                if (namesAhead(p, 0) >= 0) {
                    Str *names[2]; int nn = 0;
                    while (!isOp(p, "{", 0)) { Str *s = next(p)->str; if (nn < 2) names[nn] = s; nn++; if (isOp(p, ",", 0)) next(p); }
                    if (nn > 2) pfail(p, t, "at most two names allowed after ::");
                    next(p); skipNl(p);
                    int sv = p->noArrow; p->noArrow = 0;
                    int bn; Node **body = parseStatements(p, 1, &bn); expectOp(p, "}");
                    p->noArrow = sv;
                    Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
                    c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * 3); c->np = nn;
                    for (int i = 0; i < nn; i++) c->params[i] = mkIdPat(p, names[i], t->line);
                    c->body = body; c->nbody = bn; c->isBlock = 1;
                    fn = mknode(p, N_LAMBDA, t->line); fn->cl = c; fn->ncl = 1;
                    pairs = nn == 2;
                } else if (isOp(p, "{", 0)) {
                    next(p); skipNl(p);
                    int sv = p->noArrow; p->noArrow = 0;
                    fn = parseBraceLambda(p);
                    p->noArrow = sv;
                } else {
                    int save = p->pos; Slot ptmp; pushSlot(p, &ptmp, 'm'); Node *probe = parsePostfix(p, parsePrimary(p)); popSlot(p); Tok *at = peek(p, 0);   /* пробный разбор: дырки `_` в цели (p[_] := …) не должны быть ошибкой */
                    if (at->t == TK_OP && is_assign_op(at) && (probe->type == N_IDENT || probe->type == N_MEMBER || probe->type == N_INDEX)) {
                        /* xs:: total += _ — тело-присваивание в одну строку */
                        p->pos = save;
                        Slot col; pushSlot(p, &col, 'm');
                        Node *ex = parseExpr(p); popSlot(p);
                        Node **body = (Node **)amalloc(p->vm, sizeof(Node *)); body[0] = ex;
                        fn = mkLambdaFromSlot(p, &col, NULL, body, 1, 1, t->line);
                    } else fn = probe;
                }
                Node *f = mknode(p, N_FOREACH, t->line); f->a = e; f->b = fn; f->flag = par; f->op = pairs; e = f; continue;
            }
        } else if (t->t == TK_NL) {
            int k = nlCount(p); Tok *c = peek(p, k);
            if (isOp(p, ".", k) || isOp(p, "::", k) || (kw_is(c, "as") && isOp(p, "{", k + 1))) { p->pos += k; continue; }
        } else if (kw_is(t, "as") && isOp(p, "{", 1)) {
            next(p); next(p); skipNl(p);
            Str *fields[64]; int nf = 0;
            while (!isOp(p, "}", 0)) {
                Str *s = expectId(p); skipNl(p);
                if (nf < 64) fields[nf++] = s;
                if (isOp(p, ",", 0)) { next(p); skipNl(p); }
            }
            expectOp(p, "}");
            Node *n = mknode(p, N_AS, t->line); n->a = e; n->n = nf;
            n->names = (Str **)amalloc(p->vm, sizeof(Str *) * (size_t)(nf + 1)); memcpy(n->names, fields, sizeof(Str *) * (size_t)nf);
            e = n; continue;
        }
        break;
    }
    return e;
}

static Node *parseBraceLambda(Parser *p) {
    int line = peek(p, 0)->line;
    /* hasLambdaHead */
    int head = 0; Tok *t = peek(p, 0);
    if (t->t == TK_ID) { int k = 1; while (peek(p, k)->t == TK_NL) k++; head = isOp(p, "->", k); }
    else if (t->t == TK_OP && (!strcmp(t->o, "(") || !strcmp(t->o, "{") || !strcmp(t->o, "["))) head = arrowAfterGroup(p);
    Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
    if (!head) { /* ::{ ... _ ... }: параметры берутся из «дырок» */
        Slot col; pushSlot(p, &col, 'm');
        int bn; Node **body = parseStatements(p, 1, &bn);
        popSlot(p); expectOp(p, "}");
        return mkLambdaFromSlot(p, &col, NULL, body, bn, 1, line);
    }
    /* заголовок лямбды */
    if (t->t == TK_ID) { next(p); c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * 2); c->params[0] = mkIdPat(p, t->str, t->line); c->np = 1; }
    else if (!strcmp(t->o, "(")) { next(p); c->params = parseParamList(p, ")", &c->np); }
    else { c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * 2); c->params[0] = parsePat(p, 0); c->np = 1; }
    skipNl(p); expectOp(p, "->"); skipNl(p);
    if (isOp(p, "{", 0)) {
        int cl = findClose(p, p->pos), k = cl + 1;
        while (k < p->n && p->t[k].t == TK_NL) k++;
        if (cl >= 0 && k < p->n && op_is(&p->t[k], "}")) {
            c->body = parseBlock(p, &c->nbody); skipNl(p); expectOp(p, "}");
            c->isBlock = 1;
            Node *n = mknode(p, N_LAMBDA, line); n->cl = c; n->ncl = 1; return n;
        }
    }
    c->body = parseStatements(p, 1, &c->nbody); expectOp(p, "}"); c->isBlock = 1;
    Node *n = mknode(p, N_LAMBDA, line); n->cl = c; n->ncl = 1; return n;
}

static Node *parseLambdaHere(Parser *p) {
    Tok *t = peek(p, 0);
    Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
    if (t->t == TK_ID) { next(p); c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * 2); c->params[0] = mkIdPat(p, t->str, t->line); c->np = 1; }
    else if (op_is(t, "(")) { next(p); c->params = parseParamList(p, ")", &c->np); }
    else if (op_is(t, "{") || op_is(t, "[")) { c->params = (Pat **)amalloc(p->vm, sizeof(Pat *) * 2); c->params[0] = parsePat(p, 0); c->np = 1; }
    else pfail(p, t, "expected function (x -> ...), got %s", desc(p, t));
    skipNl(p); expectOp(p, "->"); skipNl(p);
    if (isOp(p, "{", 0)) { c->body = parseBlock(p, &c->nbody); c->isBlock = 1; }
    else { int sv = p->noArrow; p->noArrow = 0; c->expr = parseExpr(p); p->noArrow = sv; }
    Node *n = mknode(p, N_LAMBDA, t->line); n->cl = c; n->ncl = 1; return n;
}

static Node *parsePrimary(Parser *p) {
    Tok *t = peek(p, 0);
    if (t->t == TK_INT) { next(p); Node *n = mknode(p, N_INT, t->line); n->v = tok_int(p, t); return n; }
    if (t->t == TK_FLT) { next(p); Node *n = mknode(p, N_FLT, t->line); n->v = V_flt(t->f); n->name = t->str; n->flag = t->gen; return n; }
    if (t->t == TK_STR) { next(p); Node *n = mknode(p, N_STR, t->line); n->v = V_str(t->str); return n; }
    if (t->t == TK_ISTR) { next(p); return parseIStr(p, t->str, t->line); }
    if (t->t == TK_KW) {
        if (kw_is(t, "true") || kw_is(t, "false")) { next(p); Node *n = mknode(p, N_BOOL, t->line); n->v = V_bool(kw_is(t, "true")); return n; }
        if (kw_is(t, "none")) { next(p); return mknode(p, N_NONE, t->line); }
        if (kw_is(t, "return") && p->arm > 0) {
            next(p); Tok *e = peek(p, 0);
            Node *n = mknode(p, N_RETURN, t->line); n->flag = 1;
            if (e->t == TK_NL || e->t == TK_EOF || isOp(p, "}", 0) || isKw(p, "is", 0) || isKw(p, "else", 0)) return n;
            n->a = parseExpr(p); return n;
        }
        if (kw_is(t, "next") || kw_is(t, "stop")) { next(p); Node *n = mknode(p, N_CTL, t->line); n->flag = kw_is(t, "stop") ? 2 : 1; return n; }
        if (kw_is(t, "fn")) {
            next(p); expectOp(p, "(");
            Clause *c = (Clause *)amalloc(p->vm, sizeof(Clause));
            c->params = parseParamList(p, ")", &c->np);
            parseFnBody(p, c);
            Node *n = mknode(p, N_LAMBDA, t->line); n->cl = c; n->ncl = 1; return n;
        }
        pfail(p, t, "unexpected token \"%s\"", t->str->s);
    }
    if (t->t == TK_ID) {
        int k = 1; while (peek(p, k)->t == TK_NL) k++;
        if (!p->noArrow && isOp(p, "->", k)) return parseLambdaHere(p);
        if (isdigit((unsigned char)t->str->s[0])) { /* дырка уровня: N_ или N_k */
            next(p);
            int L = atoi(t->str->s); const char *u = strchr(t->str->s, '_'); int k = u[1] ? atoi(u + 1) : 1;
            Slot *cnt[64]; int nc = 0;
            for (int i = 0; i < p->nslots; i++) if (p->slots[i]->kind == 'm') cnt[nc++] = p->slots[i];
            if (L < 1 || L > nc || k < 1) pfail(p, t, "%s: no enclosing function at level %d", t->str->s, L);
            Slot *tgt = cnt[L - 1], *top = p->slots[p->nslots - 1];
            char b[40];
            if (tgt != top) { if (!tgt->sid) tgt->sid = ++p->sidN; snprintf(b, sizeof b, "_%d$%d", k, tgt->sid); }
            else snprintf(b, sizeof b, "_%d", k);
            Str *nm = pi(p, b);
            slotAdd(tgt, nm);
            Node *n = mknode(p, N_IDENT, t->line); n->name = nm; return n;
        }
        if (isHoleName(t->str)) {
            next(p);
            Slot *slot = p->nslots ? p->slots[p->nslots - 1] : NULL;
            if (!slot) pfail(p, t, "%s is only valid inside a call argument, a :: body or the right side of x = ...", t->str->s);
            Str *nm = t->str->len == 1 ? pi(p, "_1") : t->str;
            slotAdd(slot, nm);
            Node *n = mknode(p, N_IDENT, t->line); n->name = nm;
            if (slot->nn < 32) slot->nodes[slot->nn++] = n;
            return n;
        }
        next(p);
        Node *n = mknode(p, N_IDENT, t->line); n->name = t->str; return n;
    }
    if (t->t == TK_OP) {
        if (!strcmp(t->o, "@")) { /* старая запись параметра: @имя */
            next(p);
            Tok *nt = peek(p, 0);
            if (nt->t != TK_ID) pfail(p, NULL, "expected parameter name after @");
            next(p);
            Slot *slot = p->nslots ? p->slots[p->nslots - 1] : NULL;
            if (!slot) pfail(p, nt, "@%s is only valid inside a method argument or a :: body", nt->str->s, nt->str->s);
            slotAdd(slot, nt->str);
            Node *n = mknode(p, N_IDENT, nt->line); n->name = nt->str; return n;
        }
        if (!strcmp(t->o, "(")) {
            if (arrowAfterGroup(p)) return parseLambdaHere(p);
            next(p);
            int sv = p->noArrow; p->noArrow = 0;
            skipNl(p); Node *e = parseExpr(p); skipNl(p);
            p->noArrow = sv;
            expectOp(p, ")"); e->flag = 1; /* paren */
            return e;
        }
        if (!strcmp(t->o, "[")) {
            if (arrowAfterGroup(p)) return parseLambdaHere(p);
            next(p);
            int sv = p->noArrow; p->noArrow = 0;
            NV items = { 0 }; skipNl(p);
            while (!isOp(p, "]", 0)) {
                if (isOp(p, "...", 0)) { Tok *st = next(p); Node *sp = mknode(p, N_SPREAD, st->line); sp->a = parseExpr(p); nv_push(p->vm, &items, sp); }
                else nv_push(p->vm, &items, parseExpr(p));
                int sep = 0;
                while (isOp(p, ",", 0) || peek(p, 0)->t == TK_NL) { next(p); sep = 1; }
                if (!sep && !isOp(p, "]", 0)) pfail(p, NULL, "expected \",\" or \"]\", got %s", desc(p, peek(p, 0)));
            }
            p->noArrow = sv;
            expectOp(p, "]");
            Node *n = mknode(p, N_ARR, t->line); n->list = items.v; n->n = items.n; return n;
        }
        if (!strcmp(t->o, "{")) {
            if (arrowAfterGroup(p)) return parseLambdaHere(p);
            next(p);
            int sv = p->noArrow; p->noArrow = 0;
            NV props = { 0 }; skipNl(p);
            while (!isOp(p, "}", 0)) {
                Tok *k = peek(p, 0);
                Node *pr = mknode(p, N_PROP, k->line);
                if (isOp(p, "...", 0)) {
                    next(p); pr->flag = 1; pr->a = parseExpr(p);
                } else {
                    if (k->t != TK_ID && k->t != TK_STR) pfail(p, NULL, "expected field name, got %s", desc(p, k));
                    pr->name = k->str; next(p);
                    if (isOp(p, ":", 0)) { next(p); skipNl(p); pr->a = parseExpr(p); }
                    else if (k->t == TK_ID) { Node *id = mknode(p, N_IDENT, k->line); id->name = k->str; pr->a = id; }
                    else pfail(p, NULL, "expected \":\" after string key");
                }
                nv_push(p->vm, &props, pr);
                int sep = 0;
                while (isOp(p, ",", 0) || peek(p, 0)->t == TK_NL) { next(p); sep = 1; }
                if (!sep && !isOp(p, "}", 0)) pfail(p, NULL, "expected \",\" or \"}\", got %s", desc(p, peek(p, 0)));
            }
            p->noArrow = sv;
            expectOp(p, "}");
            Node *n = mknode(p, N_OBJ, t->line); n->list = props.v; n->n = props.n; return n;
        }
    }
    pfail(p, t, "unexpected %s", desc(p, t));
    return NULL;
}

/* ---------------------------------------------------------------- шаблоны */
static Pat *parsePat(Parser *p, int mm) {
    Tok *t = peek(p, 0);
    if (op_is(t, "{")) {
        next(p); skipNl(p);
        Str *keys[64]; Pat *vals[64]; int n = 0;
        while (!isOp(p, "}", 0)) {
            Tok *k = peek(p, 0);
            if (k->t != TK_ID && k->t != TK_STR) pfail(p, NULL, "expected field name, got %s", desc(p, k));
            if (k->t == TK_STR && (isOp(p, ".", 1) || isOp(p, "(", 1))) pfail(p, k, "computed keys in patterns are not supported");
            next(p);
            Pat *pat;
            if (isOp(p, ":", 0)) { next(p); skipNl(p); pat = parsePatValue(p, mm); }
            else pat = mkIdPat(p, k->str, k->line);
            if (n >= 64) pfail(p, NULL, "too many pattern fields");
            keys[n] = k->str; vals[n] = pat; n++;
            skipNl(p); if (isOp(p, ",", 0)) { next(p); skipNl(p); }
        }
        expectOp(p, "}");
        Pat *x = newPat(p, P_OBJ, t->line); x->n = n;
        x->keys = (Str **)amalloc(p->vm, sizeof(Str *) * (size_t)(n + 1)); memcpy(x->keys, keys, sizeof(Str *) * (size_t)n);
        x->items = (Pat **)amalloc(p->vm, sizeof(Pat *) * (size_t)(n + 1)); memcpy(x->items, vals, sizeof(Pat *) * (size_t)n);
        return x;
    }
    if (op_is(t, "[")) {
        next(p); skipNl(p);
        Pat *items[64]; int n = 0; Str *rest = NULL;
        while (!isOp(p, "]", 0)) {
            if (isOp(p, "...", 0)) { next(p); rest = expectId(p); }
            else { if (n >= 64) pfail(p, NULL, "too many pattern elements"); items[n++] = parsePatValue(p, mm); }
            skipNl(p); if (isOp(p, ",", 0)) { next(p); skipNl(p); }
        }
        expectOp(p, "]");
        Pat *x = newPat(p, P_ARR, t->line); x->n = n; x->rest = rest;
        x->items = (Pat **)amalloc(p->vm, sizeof(Pat *) * (size_t)(n + 1)); memcpy(x->items, items, sizeof(Pat *) * (size_t)n);
        return x;
    }
    if (t->t == TK_ID) { next(p); return mkIdPat(p, t->str, t->line); }
    pfail(p, t, "expected name or pattern, got %s", desc(p, t));
    return NULL;
}
static Pat *parsePatValue(Parser *p, int mm) {
    Tok *t = peek(p, 0);
    if (t->t == TK_OP && (!strcmp(t->o, "{") || !strcmp(t->o, "["))) return parsePat(p, mm);
    if (!mm) return parsePat(p, 0);
    if (isImplicitOp(t)) {
        Node *left = mknode(p, N_IDENT, t->line); left->name = pi(p, "$subject");
        Pat *x = newPat(p, P_COND, t->line); x->expr = parseBinaryFrom(p, left, 0); return x;
    }
    if (t->t == TK_ID && !isOp(p, "(", 1) && !isOp(p, ".", 1)) {
        Tok *n = peek(p, 1);
        if (n->t == TK_NL || (n->t == TK_OP && (!strcmp(n->o, ",") || !strcmp(n->o, "}") || !strcmp(n->o, "]") || !strcmp(n->o, "->")))) { next(p); return mkIdPat(p, t->str, t->line); }
    }
    int sv = p->noArrow; p->noArrow = 1;
    Pat *x = litPat(p, parseBinary(p, 0));
    p->noArrow = sv;
    return x;
}

/* ---------------------------------------------------------------- is / match */
static Node *parseIsExpr(Parser *p, int allowNoElse) {
    Tok *it = peek(p, 0);
    NV arms = { 0 }; int hasElse = 0;
    for (;;) {
        Tok *t = next(p); /* is | else */
        Node *cond = NULL;
        if (kw_is(t, "is")) {
            int sv = p->noArrow; p->noArrow = 1;
            cond = parseBinary(p, 0);
            p->noArrow = sv;
        } else hasElse = 1;
        skipNl(p);
        if (!isOp(p, "->", 0)) pfail(p, NULL, "expected \"->\" after is condition, got %s", desc(p, peek(p, 0)));
        next(p); skipNl(p);
        Node *arm = mknode(p, N_ARM, t->line); arm->a = cond;
        p->arm++;
        if (isOp(p, "{", 0)) { arm->list = parseBlock(p, &arm->n); arm->flag = 1; }
        else { int sv = p->noArrow; p->noArrow = 0; arm->b = parseExpr(p); p->noArrow = sv; }
        p->arm--;
        nv_push(p->vm, &arms, arm);
        if (hasElse) break;
        int k = nlCount(p); Tok *c = peek(p, k);
        if (kw_is(c, "is") || kw_is(c, "else")) { p->pos += k; continue; }
        break;
    }
    if (!hasElse && !allowNoElse) pfail(p, it, "is expression requires an else branch");
    Node *n = mknode(p, N_IS, it->line); n->list = arms.v; n->n = arms.n; return n;
}

static Node *parseMatch(Parser *p, Node *subject) {
    Tok *mt = next(p);
    skipNl(p);
    if (!isOp(p, "{", 0)) pfail(p, NULL, "match: expected indented branches (is ... / else ...), got %s", desc(p, peek(p, 0)));
    next(p); skipNl(p);
    NV arms = { 0 }; int hasElse = 0;
    while (!isOp(p, "}", 0)) {
        Tok *t = peek(p, 0);
        Node *arm = mknode(p, N_ARM, t->line);
        if (kw_is(t, "else")) { next(p); arm->op = 2; hasElse = 1; }
        else if (kw_is(t, "is")) {
            next(p); skipNl(p); Tok *c = peek(p, 0);
            int sv = p->noArrow; p->noArrow = 1;
            if (c->t == TK_OP && (!strcmp(c->o, "{") || !strcmp(c->o, "["))) { arm->op = 1; arm->pat = parsePat(p, 1); }
            else if (isImplicitOp(c)) {
                Node *left = mknode(p, N_IDENT, c->line); left->name = pi(p, "$subject");
                arm->op = 0; arm->a = parseBinaryFrom(p, left, 0);
            } else { arm->op = 0; arm->a = parseBinary(p, 0); }
            p->noArrow = sv;
        } else pfail(p, NULL, "match: expected \"is ...\" or \"else\", got %s", desc(p, t));
        skipNl(p); expectOp(p, "->"); skipNl(p);
        p->arm++;
        if (isOp(p, "{", 0)) { arm->list = parseBlock(p, &arm->n); arm->flag = 1; }
        else if (isKw(p, "return", 0)) { Node **one = (Node **)amalloc(p->vm, sizeof(Node *)); one[0] = parseStatement(p); arm->list = one; arm->n = 1; arm->flag = 1; }
        else arm->b = parseExpr(p);
        p->arm--;
        nv_push(p->vm, &arms, arm);
        Tok *e = peek(p, 0);
        if (!(e->t == TK_NL || isOp(p, "}", 0) || isKw(p, "is", 0) || isKw(p, "else", 0))) pfail(p, NULL, "match: expected end of line after branch, got %s", desc(p, e));
        skipNl(p);
    }
    expectOp(p, "}");
    if (!hasElse) pfail(p, mt, "match requires an else branch");
    Node *n = mknode(p, N_MATCH, mt->line); n->a = subject; n->list = arms.v; n->n = arms.n; return n;
}

/* ---------------------------------------------------------------- интерполяция `... {expr} ...` */
static Node *parseIStr(Parser *p, Str *raw, int line) {
    VM *vm = p->vm;
    NV parts = { 0 };
    SB sb; sb_init(vm, &sb);
    const char *s = raw->s; int n = raw->len, i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && (s[i + 1] == '{' || s[i + 1] == '}')) { sb_c(&sb, s[i + 1]); i += 2; continue; }
        if (c == '{') {
            int d = 1, j = i + 1;
            while (j < n && d > 0) { if (s[j] == '{') d++; else if (s[j] == '}') d--; j++; }
            if (d != 0) rt(vm, line, "unclosed \"{\" in interpolation");
            if (sb.n) { Node *l = mknode(p, N_STR, line); l->v = V_str(sb_str(&sb)); nv_push(vm, &parts, l); sb_init(vm, &sb); }
            TokVec tv = { 0 };
            lex_src(vm, s + i + 1, j - i - 2, &tv, 0);
            Parser q; memset(&q, 0, sizeof q); q.vm = vm; q.t = tv.v; q.n = tv.n;
            q.nslots = p->nslots; q.sidN = p->sidN; memcpy(q.slots, p->slots, sizeof(Slot *) * (size_t)p->nslots);   /* дырки `_` внутри {…} относятся к охватывающей функции */
            skipNl(&q);
            Node *e = parseExpr(&q); skipNl(&q);
            if (peek(&q, 0)->t != TK_EOF) pfail(&q, NULL, "unexpected token: %s", desc(&q, peek(&q, 0)));
            for (int k = 0; k < tv.n; k++) if (tv.v[k].line) tv.v[k].line = line;
            p->sidN = q.sidN;
            nv_push(vm, &parts, e);
            i = j; continue;
        }
        sb_c(&sb, c); i++;
    }
    if (sb.n) { Node *l = mknode(p, N_STR, line); l->v = V_str(sb_str(&sb)); nv_push(vm, &parts, l); }
    Node *nd = mknode(p, N_IFMT, line); nd->list = parts.v; nd->n = parts.n; return nd;
}

/* ---------------------------------------------------------------- асинхронные функции */
/* Функция «асинхронная» (clause.async), если в её теле (не заходя во вложенные функции) есть time.wait(…), await
   или тело ::-цикла с ожиданием. Вызов такой функции запускает задачу. */
static int mark_hw(Node *n);
static int mark_hw_list(Node **l, int n) { int h = 0; for (int i = 0; i < n; i++) if (mark_hw(l[i])) h = 1; return h; }
static int is_wait_call(Node *n) {
    return n->type == N_CALL && n->a->type == N_MEMBER && !strcmp(n->a->name->s, "wait") && n->a->a->type == N_IDENT && !strcmp(n->a->a->name->s, "time");
}
static int mark_hw(Node *n) {
    if (!n) return 0;
    if (n->type == N_LAMBDA || n->type == N_FNDECL) {
        for (int i = 0; i < n->ncl; i++) {
            Clause *c = &n->cl[i];
            c->async = c->isBlock ? mark_hw_list(c->body, c->nbody) : mark_hw(c->expr);
        }
        return 0;
    }
    int h = 0;
    if (mark_hw(n->a)) h = 1;
    if (mark_hw(n->b)) h = 1;
    if (mark_hw(n->c)) h = 1;
    if (n->list && n->type != N_TYPEDECL && mark_hw_list(n->list, n->n)) h = 1;
    if (n->type == N_AWAIT || is_wait_call(n)) h = 1;
    if (n->type == N_FOREACH && n->b && n->b->type == N_LAMBDA && n->b->cl[0].async && !n->flag) h = 1;
    return h;
}

static Node **parse_program(VM *vm, const char *src, int len, int *outn) {
    TokVec raw = { 0 }, tv = { 0 };
    lex(vm, src, len, &raw, 1);
    bracket_pass(vm, &raw);
    where_pass(vm, &raw);
    lambda_block_pass(vm, &raw);
    layout_pass(vm, &raw, &tv);
    Parser p; memset(&p, 0, sizeof p); p.vm = vm; p.t = tv.v; p.n = tv.n;
    Node **prog = parseStatements(&p, 0, outn);
    vm->prog_hw = mark_hw_list(prog, *outn);
    return prog;
}
/* ===== d_interp ===== */

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
/* ===== d3_vm ===== */
/* ---------------------------------------------------------------- регистровая ВМ (ярус 1)
   Тело простой функции (без присваиваний, лямбд, блоков в ветках и т.п.) компилируется в байткод и выполняется без
   окружения: параметры лежат в регистрах. Что не подходит — остаётся в дереве (vmstate = 2) и работает как раньше. */
typedef struct { uint8_t op, a, b, c; int32_t imm, nd; } VIns;
typedef struct VCode { VIns *ins; int n; Value *k; Node **nodes; int np, nregs; } VCode;

enum { VO_K, VO_MOV, VO_LOADO, VO_NONE, VO_RET, VO_JMP, VO_JF, VO_CALL, VO_DRAIN, VO_GEN, VO_LINE, VO_CHKC, VO_ASGO, VO_FLT,
       VO_ADD, VO_SUB, VO_MUL, VO_MOD, VO_EQ, VO_NE, VO_LT, VO_GT, VO_LE, VO_GE,
       VO_ADDI, VO_SUBI, VO_MULI, VO_MODI, VO_EQI, VO_NEI, VO_LTI, VO_GTI, VO_LEI, VO_GEI };
#define VM_MAXREG 32

typedef struct { Clause *c; Env *cenv; Str *nm[VM_MAXREG]; unsigned char nr[VM_MAXREG], nmut[VM_MAXREG]; int nnm, pin, writes; VIns *ins; int n, cap; Value *k; int nk, kcap; Node **nodes; int nn, ncap; int np, next, maxreg, ok; } VComp;

static int vc_emit(VComp *C, int op, int a, int b, int c, int imm, int nd) {
    if (C->n == C->cap) { C->cap = C->cap ? C->cap * 2 : 32; C->ins = (VIns *)realloc(C->ins, sizeof(VIns) * (size_t)C->cap); }
    VIns *x = &C->ins[C->n]; x->op = (uint8_t)op; x->a = (uint8_t)a; x->b = (uint8_t)b; x->c = (uint8_t)c; x->imm = imm; x->nd = nd;
    return C->n++;
}
static int vc_k(VComp *C, Value v) {
    if (C->nk == C->kcap) { C->kcap = C->kcap ? C->kcap * 2 : 8; C->k = (Value *)realloc(C->k, sizeof(Value) * (size_t)C->kcap); }
    C->k[C->nk] = v; return C->nk++;
}
static int vc_node(VComp *C, Node *n) {
    if (C->nn == C->ncap) { C->ncap = C->ncap ? C->ncap * 2 : 8; C->nodes = (Node **)realloc(C->nodes, sizeof(Node *) * (size_t)C->ncap); }
    C->nodes[C->nn] = n; return C->nn++;
}
static int vc_tmp(VComp *C) {
    int r = C->next++;
    if (C->next > C->maxreg) C->maxreg = C->next;
    if (C->next > VM_MAXREG) C->ok = 0;
    return C->ok ? r : 0;
}
static int vc_nameidx(VComp *C, Str *name) {
    for (int i = C->nnm - 1; i >= 0; i--) if (C->nm[i] == name) return i;
    return -1;
}
static int vc_param(VComp *C, Node *n) {   /* регистр переменной (параметр или локальная константа) или -1 */
    if (n->type != N_IDENT) return -1;
    int i = vc_nameidx(C, n->name);
    return i < 0 ? -1 : C->nr[i];
}
static void vc_name(VComp *C, Str *name, int reg, int mut) {
    if (C->nnm >= VM_MAXREG) { C->ok = 0; return; }
    C->nm[C->nnm] = name; C->nr[C->nnm] = (uint8_t)reg; C->nmut[C->nnm] = (uint8_t)mut; C->nnm++;
}
static void vc_expr(VComp *C, Node *n, int dst);
static int vc_op(VComp *C, Node *n) {   /* регистр, в котором окажется значение выражения */
    int p = vc_param(C, n);
    if (p >= 0) return p;
    int t = vc_tmp(C);
    vc_expr(C, n, t);
    return t;
}
static void vc_expr(VComp *C, Node *n, int dst) {
    if (!C->ok) return;
    switch (n->type) {
        case N_FLT: vc_emit(C, VO_FLT, dst, 0, 0, 0, vc_node(C, n)); return;
        case N_INT: case N_STR: case N_BOOL:
            vc_emit(C, VO_K, dst, 0, 0, vc_k(C, n->v), 0); return;
        case N_NONE: vc_emit(C, VO_NONE, dst, 0, 0, 0, 0); return;
        case N_IDENT: {
            int p = vc_param(C, n);
            if (p >= 0) { if (p != dst) vc_emit(C, VO_MOV, dst, p, 0, 0, 0); }
            else vc_emit(C, VO_LOADO, dst, 0, 0, 0, vc_node(C, n));
            return;
        }
        case N_BIN: {
            int op = n->op;
            if (n->fold || op > OP_GE) { C->ok = 0; return; }
            int save = C->next;
            int a = vc_op(C, n->a);
            int simple = op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_MOD || op >= OP_EQ;
            if (simple && n->b->type == N_INT && n->b->v.t == T_INT && n->b->v.u.i >= -1000000000 && n->b->v.u.i <= 1000000000) {
                int base = op == OP_ADD ? VO_ADDI : op == OP_SUB ? VO_SUBI : op == OP_MUL ? VO_MULI : op == OP_MOD ? VO_MODI
                         : VO_EQI + (op - OP_EQ);
                vc_emit(C, base, dst, a, 0, (int32_t)n->b->v.u.i, vc_node(C, n));
            } else {
                int b = vc_op(C, n->b);
                int code = !simple ? VO_GEN : op == OP_ADD ? VO_ADD : op == OP_SUB ? VO_SUB : op == OP_MUL ? VO_MUL : op == OP_MOD ? VO_MOD
                         : VO_EQ + (op - OP_EQ);
                vc_emit(C, code, dst, a, b, 0, vc_node(C, n));
            }
            C->next = save;
            return;
        }
        case N_IS: {
            int jend[64], nj = 0;
            for (int i = 0; i < n->n && C->ok; i++) {
                Node *arm = n->list[i];
                if (arm->flag || !arm->b) { C->ok = 0; return; }
                int jf = -1;
                if (arm->a) {
                    int save = C->next;
                    int r = vc_op(C, arm->a);
                    jf = vc_emit(C, VO_JF, r, 0, 0, 0, vc_node(C, arm));
                    C->next = save;
                }
                vc_expr(C, arm->b, dst);
                if (nj >= 64) { C->ok = 0; return; }
                jend[nj++] = vc_emit(C, VO_JMP, 0, 0, 0, 0, 0);
                if (jf >= 0) C->ins[jf].imm = C->n;
            }
            vc_emit(C, VO_NONE, dst, 0, 0, 0, 0);   /* ни одна ветка не подошла */
            for (int i = 0; i < nj; i++) C->ins[jend[i]].imm = C->n;
            return;
        }
        case N_CALL: {
            Node *f = n->a;
            if (f->type != N_IDENT || vc_param(C, f) >= 0 || n->n > 6) { C->ok = 0; return; }
            for (int i = 0; i < n->n; i++) {
                Node *x = n->list[i];
                if (x->type == N_SPREAD || root_ident(x)) { C->ok = 0; return; }   /* аргументы-пути требуют знать изменяемость: оставляем дереву */
            }
            int save = C->next, base = C->next;
            for (int i = 0; i < n->n; i++) vc_tmp(C);
            for (int i = 0; i < n->n; i++) vc_expr(C, n->list[i], base + i);
            vc_emit(C, VO_CALL, dst, base, n->n, n->line, vc_node(C, n));
            C->next = save;
            return;
        }
        default: C->ok = 0; return;
    }
}
static void vc_assign(VComp *C, Node *n, int want) {   /* want — регистр для значения присваивания или -1 */
    Node *t = n->a;
    if (t->type != N_IDENT) { C->ok = 0; return; }
    int op = n->op, ni = vc_nameidx(C, t->name), p = ni < 0 ? -1 : C->nr[ni];
    if (op == 0) {                                   /* x = e: новая локальная константа */
        if (p >= 0) { C->ok = 0; return; }
        int L = vc_tmp(C); C->pin = C->next;
        vc_expr(C, n->b, L);
        vc_emit(C, VO_CHKC, 0, 0, 0, n->line, vc_node(C, t));
        vc_name(C, t->name, L, 0);
        if (want >= 0 && want != L) vc_emit(C, VO_MOV, want, L, 0, 0, 0);
    } else if (op == 1 && p >= 0) {                  /* x := e для параметра */
        if (!C->nmut[ni]) { C->ok = 0; return; }
        vc_expr(C, n->b, p);
        if (want >= 0 && want != p) vc_emit(C, VO_MOV, want, p, 0, 0, 0);
    } else if (p >= 0) { C->ok = 0; return; }        /* составное присваивание параметру: проверки содержимого оставляем дереву */
    else {                                           /* внешняя переменная */
        if (op == 1 && !env_find_o(C->cenv, t->name, NULL, 0)) { C->ok = 0; return; }   /* x := e может создавать локальную: неясно заранее */
        int save = C->next;
        int v = vc_op(C, n->b);
        int d = want >= 0 ? want : vc_tmp(C);
        vc_emit(C, VO_ASGO, d, v, 0, 0, vc_node(C, n));
        C->writes = 1;
        C->next = save;
    }
}
static void vm_try_compile(VM *vm, Clause *c, Env *cenv) {
    (void)vm;
    if (c->vmstate) return;
    VComp C; memset(&C, 0, sizeof C);
    if (getenv("PRISM_NOVM")) { __atomic_store_n(&c->vmstate, 2, __ATOMIC_RELEASE); return; }
    C.c = c; C.cenv = cenv; C.np = c->np; C.next = c->np; C.maxreg = c->np; C.ok = c->simple == 1 && !c->async && c->np <= 6;
    if (C.ok) {
        for (int i = 0; i < c->np; i++) vc_name(&C, c->params[i]->name, i, 1);
        int r = vc_tmp(&C); C.pin = C.next;   /* регистр результата */
        if (!c->isBlock) { vc_expr(&C, c->expr, r); vc_emit(&C, VO_RET, r, 0, 0, 0, 0); }
        else {
            int done = 0;
            vc_emit(&C, VO_NONE, r, 0, 0, 0, 0);
            for (int i = 0; i < c->nbody && C.ok && !done; i++) {
                Node *s = c->body[i];
                int last = i == c->nbody - 1;
                C.next = C.pin;
                vc_emit(&C, VO_LINE, 0, 0, 0, s->line, 0);
                if (s->type == N_RETURN) {
                    if (s->a) { int x = vc_op(&C, s->a); vc_emit(&C, VO_RET, x, 0, 0, 0, 0); }
                    else { int t = vc_tmp(&C); vc_emit(&C, VO_NONE, t, 0, 0, 0, 0); vc_emit(&C, VO_RET, t, 0, 0, 0, 0); }
                    done = 1;
                } else if (s->type == N_ASSIGN) {
                    vc_assign(&C, s, last ? r : -1);
                } else {
                    int d = last ? r : vc_tmp(&C);
                    vc_expr(&C, s, d);
                    vc_emit(&C, VO_DRAIN, d, 0, 0, 0, 0);
                }
            }
            if (!done && C.ok) vc_emit(&C, VO_RET, r, 0, 0, 0, 0);
        }
    }
    if (getenv("PRISM_VMDEBUG")) fprintf(stderr, "[vm] clause line %d: %s (%d ins)\n", c->isBlock && c->nbody ? c->body[0]->line : c->expr ? c->expr->line : 0, C.ok && C.maxreg <= VM_MAXREG ? "compiled" : "tree", C.n);
    if (!C.ok || C.maxreg > VM_MAXREG) {
        free(C.ins); free(C.k); free(C.nodes);
        __atomic_store_n(&c->vmstate, 2, __ATOMIC_RELEASE);
        return;
    }
    VCode *vc = (VCode *)malloc(sizeof(VCode));
    vc->ins = C.ins; vc->n = C.n; vc->k = C.k; vc->nodes = C.nodes; vc->np = c->np; vc->nregs = C.maxreg;
    c->code = vc; c->vmw = C.writes;
    __atomic_store_n(&c->vmstate, 1, __ATOMIC_RELEASE);
}

static Value vm_exec(VM *vm, VCode *code, Env *cenv, Value *args, int nargs) {
    Value R[VM_MAXREG];
    int np = code->np;
    for (int i = 0; i < np; i++) R[i] = i < nargs ? args[i] : V_none();
    VIns *ip = code->ins;
    static const void *tab[] = { &&L_K, &&L_MOV, &&L_LOADO, &&L_NONE, &&L_RET, &&L_JMP, &&L_JF, &&L_CALL, &&L_DRAIN, &&L_GEN, &&L_LINE, &&L_CHKC, &&L_ASGO, &&L_FLT,
        &&L_ADD, &&L_SUB, &&L_MUL, &&L_MOD, &&L_EQ, &&L_NE, &&L_LT, &&L_GT, &&L_LE, &&L_GE,
        &&L_ADDI, &&L_SUBI, &&L_MULI, &&L_MODI, &&L_EQI, &&L_NEI, &&L_LTI, &&L_GTI, &&L_LEI, &&L_GEI };
#define NEXT do { ip++; goto *tab[ip->op]; } while (0)
    goto *tab[ip->op];
L_K: R[ip->a] = code->k[ip->imm]; NEXT;
L_MOV: R[ip->a] = R[ip->b]; NEXT;
L_NONE: R[ip->a] = V_none(); NEXT;
L_LOADO: {
        Node *nd = code->nodes[ip->nd];
        Var *b = env_find_c(cenv, nd);
        R[ip->a] = b ? b->v : ev_slow(vm, nd, cenv);
        NEXT;
    }
L_LINE: vm->curline = ip->imm; NEXT;
L_FLT: R[ip->a] = ev_flt(vm, code->nodes[ip->nd], cenv); NEXT;
L_CHKC: {
        Node *nd = code->nodes[ip->nd];
        Var *o = env_find_c(cenv, nd);
        if (o && o->mut) rt(vm, ip->imm, "\"%s\" is a mutable variable of an outer scope", nd->name->s);
        NEXT;
    }
L_ASGO: {
        Node *nd = code->nodes[ip->nd];
        if (nd->op == 1 && !env_find_c(cenv, nd->a)) rt(vm, nd->line, "undefined variable \"%s\"", nd->a->name->s);
        R[ip->a] = ev_assign_v(vm, nd, cenv, R[ip->b]);
        NEXT;
    }
L_RET: return R[ip->a];
L_JMP: ip = code->ins + ip->imm; goto *tab[ip->op];
L_JF: {
        Value v = R[ip->a];
        if (v.t != T_BOOL) rt(vm, code->nodes[ip->nd]->line, "is condition must be bool, got %s", type_of(v));
        if (!v.u.b) { ip = code->ins + ip->imm; goto *tab[ip->op]; }
        NEXT;
    }
L_DRAIN: {
        Value v = R[ip->a];
        if (v.t == T_STREAM) { Iter *it = iter_new(vm, v, vm->curline); Value x; while (iter_next(vm, it, &x)) {} R[ip->a] = V_none(); }
        NEXT;
    }
L_GEN: { Node *nd = code->nodes[ip->nd]; R[ip->a] = binop(vm, nd->op, R[ip->b], R[ip->c], nd->line); NEXT; }
#define RR(LBL, FAST) LBL: { Value a = R[ip->b], b = R[ip->c]; \
        if (a.t == T_INT && b.t == T_INT) { int64_t x = a.u.i, y = b.u.i, r; (void)r; FAST } \
        R[ip->a] = ev_bin_rest(vm, code->nodes[ip->nd], a, b); NEXT; }
#define RI(LBL, FAST) LBL: { Value a = R[ip->b]; \
        if (a.t == T_INT) { int64_t x = a.u.i, y = ip->imm, r; (void)r; FAST } \
        R[ip->a] = ev_bin_rest(vm, code->nodes[ip->nd], a, V_int(ip->imm)); NEXT; }
#define F_ADD if (!__builtin_add_overflow(x, y, &r)) { R[ip->a] = V_int(r); NEXT; }
#define F_SUB if (!__builtin_sub_overflow(x, y, &r)) { R[ip->a] = V_int(r); NEXT; }
#define F_MUL if (!__builtin_mul_overflow(x, y, &r)) { R[ip->a] = V_int(r); NEXT; }
#define F_MOD if (y > 0) { r = x % y; if (r < 0) r += y; R[ip->a] = V_int(r); NEXT; }
#define F_CMP(OPR) { R[ip->a] = V_bool(x OPR y); NEXT; }
    RR(L_ADD, F_ADD) RR(L_SUB, F_SUB) RR(L_MUL, F_MUL) RR(L_MOD, F_MOD)
    RR(L_EQ, F_CMP(==)) RR(L_NE, F_CMP(!=)) RR(L_LT, F_CMP(<)) RR(L_GT, F_CMP(>)) RR(L_LE, F_CMP(<=)) RR(L_GE, F_CMP(>=))
    RI(L_ADDI, F_ADD) RI(L_SUBI, F_SUB) RI(L_MULI, F_MUL) RI(L_MODI, F_MOD)
    RI(L_EQI, F_CMP(==)) RI(L_NEI, F_CMP(!=)) RI(L_LTI, F_CMP(<)) RI(L_GTI, F_CMP(>)) RI(L_LEI, F_CMP(<=)) RI(L_GEI, F_CMP(>=))
L_CALL: {
        Node *callee = code->nodes[ip->nd]->a;
        Var *b = env_find_c(cenv, callee);
        Value f = b ? b->v : ev_slow(vm, callee, cenv);
        int na = ip->c, line = ip->imm;
        Value r;
        vm->curline = line;
        if (f.t == T_FN) {
            Fn *fn = f.u.fn; Clause *cl = &fn->cl[0];
            if (!fn->multi && !fn->target && !cl->async) {
                if (!cl->simple) clause_classify(cl);
                if (cl->simple == 1) {
                    if (!cl->vmstate && vm->wid == 0) vm_try_compile(vm, cl, fn->env);
                    if (cl->vmstate == 1 && VM_OK(cl, 0)) {
                        vm->inl = 0;
                        if (++vm->depth > vm->maxdepth) { vm->depth = 0; fail(vm, 0, "RecursionError: maximum recursion depth exceeded"); }
                        r = vm_exec(vm, cl->code, fn->env, &R[ip->b], na);
                        vm->depth--;
                        goto have_result;
                    }
                }
            }
        } else if (f.t != T_NAT && f.t != T_TYPE) err_not_callable(vm, callee, line, f);
        r = call_fn(vm, f, &R[ip->b], na, 0, line, NULL);
    have_result:
        if (r.t == T_STREAM && stream_arrchain(r)) r = stream_materialize(vm, r);
        R[ip->a] = r;
        NEXT;
    }
#undef NEXT
}
/* ===== d2_sched ===== */

/* ---------------------------------------------------------------- задачи: time.wait, await */
/*
 * Каждая асинхронная функция выполняется как сопрограмма на собственном потоке ОС, но одновременно работает
 * ровно один поток (управление передаётся через мьютекс и условные переменные), поэтому ВМ не нуждается в блокировках.
 * Порядок планирования повторяет песочницу: готовые задачи по возрастанию времени пробуждения.
 */
#ifdef PRISM_THREADS
typedef struct Co { pthread_cond_t cv; int go; } Co;
static pthread_mutex_t SCHED_MX = PTHREAD_MUTEX_INITIALIZER;
#endif

struct Task {
#ifdef PRISM_THREADS
    Co co; Co *resumer; pthread_t th;
#endif
    VM *vm;
    int started, done, running, awaited, kill, hasWake, hasErr, errline, line;
    char errmsg[480];
    double wake;
    Task *waitFor;
    Value value;
    Clause *c; Env *env;
    jmp_buf jb;
};

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static void sleep_ms(double d) {
    if (d <= 0) return;
    struct timespec ts; ts.tv_sec = (time_t)(d / 1000.0); ts.tv_nsec = (long)((d - (double)ts.tv_sec * 1000.0) * 1e6);
    while (nanosleep(&ts, &ts) != 0) {}
}

static int reg_nat(VM *vm, const char *name, NatFn fn);
static Value V_task(Task *t) { Value v = V_none(); v.t = T_TASK; v.u.task = t; return v; }

static void list_push(VM *vm, Task ***arr, int *n, int *cap, Task *t) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        Task **na = (Task **)amalloc(vm, sizeof(Task *) * (size_t)nc);
        if (*n) memcpy(na, *arr, sizeof(Task *) * (size_t)*n);
        *arr = na; *cap = nc;
    }
    (*arr)[(*n)++] = t;
}

#ifdef PRISM_THREADS
static void co_init(Co *c) { pthread_cond_init(&c->cv, NULL); c->go = 0; }

static void *task_main(void *arg) {
    Task *t = (Task *)arg; VM *vm = t->vm;
    pthread_mutex_lock(&SCHED_MX); pthread_mutex_unlock(&SCHED_MX);
    vm->jbp = &t->jb; vm->depth = 0; vm->maxdepth = 200;
    int rc = setjmp(t->jb);
    if (rc == 0) {
        t->value = run_clause(vm, t->c, t->env);
    } else if (rc == 1) {
        t->hasErr = 1; t->errline = vm->errline;
        snprintf(t->errmsg, sizeof t->errmsg, "%s", vm->err);
    }
    pthread_mutex_lock(&SCHED_MX);
    t->done = 1;
    t->resumer->go = 1; pthread_cond_signal(&t->resumer->cv);
    pthread_mutex_unlock(&SCHED_MX);
    return NULL;
}

/* запустить/продолжить задачу и ждать, пока она не приостановится или не закончится */
static void task_resume(VM *vm, Task *t) {
    Co me; co_init(&me);
    Task *prev = vm->curtask; jmp_buf *sj = vm->jbp; int sd = vm->depth, sm = vm->maxdepth, sl = vm->curline;
    t->hasWake = 0; t->waitFor = NULL; t->running = 1; t->resumer = &me;
    vm->curtask = t;
    pthread_mutex_lock(&SCHED_MX);
    if (!t->started) {
        t->started = 1; co_init(&t->co);
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, (size_t)8 << 20);
        if (pthread_create(&t->th, &at, task_main, t) != 0) { pthread_mutex_unlock(&SCHED_MX); t->running = 0; vm->curtask = prev; fail(vm, vm->curline, "RuntimeError: cannot create task thread"); }
        pthread_detach(t->th); pthread_attr_destroy(&at);
    } else { t->co.go = 1; pthread_cond_signal(&t->co.cv); }
    while (!me.go) pthread_cond_wait(&me.cv, &SCHED_MX);
    pthread_mutex_unlock(&SCHED_MX);
    pthread_cond_destroy(&me.cv);
    vm->curtask = prev; vm->jbp = sj; vm->depth = sd; vm->maxdepth = sm; vm->curline = sl;
    t->running = 0;
    if (vm->shutdown && vm->curtask) longjmp(vm->curtask->jb, 2);
}
/* вызывается в потоке задачи: отдать управление тому, кто её запустил */
static void task_yield(VM *vm, Task *t) {
    int sd = vm->depth, sm = vm->maxdepth, sl = vm->curline;
    pthread_mutex_lock(&SCHED_MX);
    t->co.go = 0;
    t->resumer->go = 1; pthread_cond_signal(&t->resumer->cv);
    while (!t->co.go) pthread_cond_wait(&t->co.cv, &SCHED_MX);
    pthread_mutex_unlock(&SCHED_MX);
    vm->jbp = &t->jb; vm->depth = sd; vm->maxdepth = sm; vm->curline = sl;
    if (t->kill) longjmp(t->jb, 2);
}
#else
static void task_resume(VM *vm, Task *t) {
    Task *prev = vm->curtask; vm->curtask = t; t->running = 1;
    t->value = run_clause(vm, t->c, t->env);
    t->done = 1; t->running = 0; vm->curtask = prev;
}
static void task_yield(VM *vm, Task *t) { (void)vm; (void)t; }
#endif

static Task *task_new(VM *vm, Clause *c, Env *env, int line) {
    Task *t = (Task *)amalloc(vm, sizeof(Task));
    memset(t, 0, sizeof *t);
    t->vm = vm; t->c = c; t->env = env; t->line = line; t->value = V_none();
    return t;
}
static Value spawn_task(VM *vm, Clause *c, Env *env, int line) {
    esc_all(vm);
    if (vm->wid) rt(vm, line, "await and time.wait cannot be used inside ::par");
    Task *t = task_new(vm, c, env, line);
    list_push(vm, &vm->tasks, &vm->ntasks, &vm->captasks, t);
    list_push(vm, &vm->alltasks, &vm->nall, &vm->capall, t);
    if (vm->nall == 1 && vm->prog_hw && !vm->mainTask && vm->curtask == NULL && !vm->inl) vm->mainTask = t;
    env_capture(env);
    task_resume(vm, t);
    return V_task(t);
}

static int task_ready(Task *t) {
    if (t->done || t->running) return 0;
    if (t->waitFor) return t->waitFor->done;
    return !t->hasWake || now_ms() >= t->wake;
}
enum { PR_FALSE, PR_TASK_DONE, PR_MAIN_ERR };
static int pred_ok(VM *vm, int mode, Task *pt) {
    if (mode == PR_TASK_DONE) return pt->done;
    if (mode == PR_MAIN_ERR) return vm->mainTask && vm->mainTask->done && vm->mainTask->hasErr;
    return 0;
}
/* крутим задачи, пока условие не выполнится или не кончатся задачи; deadline < 0 — без срока */
static void sched_loop(VM *vm, int mode, Task *pt, double deadline) {
    for (;;) {
        if (pred_ok(vm, mode, pt)) return;
        if (deadline >= 0 && now_ms() >= deadline) return;
        int nlive = 0;
        for (int i = 0; i < vm->ntasks; i++) if (!vm->tasks[i]->done) nlive++;
        if (!nlive) { vm->ntasks = 0; if (deadline >= 0) sleep_ms(deadline - now_ms()); return; }
        Task **rd = (Task **)amalloc(vm, sizeof(Task *) * (size_t)(nlive + 1)); int nr = 0;
        for (int i = 0; i < vm->ntasks; i++) if (task_ready(vm->tasks[i])) rd[nr++] = vm->tasks[i];
        if (nr) {
            for (int i = 1; i < nr; i++) { /* устойчивая сортировка по времени пробуждения */
                Task *x = rd[i]; double wx = x->hasWake ? x->wake : 0; int j = i - 1;
                while (j >= 0 && (rd[j]->hasWake ? rd[j]->wake : 0) > wx) { rd[j + 1] = rd[j]; j--; }
                rd[j + 1] = x;
            }
            for (int i = 0; i < nr; i++) if (task_ready(rd[i])) { task_resume(vm, rd[i]); if (pred_ok(vm, mode, pt)) return; }
            continue;
        }
        double w = 1e300;
        for (int i = 0; i < vm->ntasks; i++) { Task *t = vm->tasks[i]; if (!t->done && t->hasWake && t->wake < w) w = t->wake; }
        if (deadline >= 0 && deadline < w) w = deadline;
        if (w >= 1e300) rt(vm, 0, "deadlock: all tasks are waiting for each other");
        sleep_ms(w - now_ms());
    }
}

static void task_rethrow(VM *vm, Task *t) { fail(vm, t->errline, "%s", t->errmsg); }

static void wait_ms(VM *vm, double ms) {
    Task *t = vm->curtask;
    if (t) { t->wake = now_ms() + ms; t->hasWake = 1; t->waitFor = NULL; task_yield(vm, t); }
    else sched_loop(vm, PR_FALSE, NULL, now_ms() + ms);
}
static Value await_value(VM *vm, Value v, int line) {
    if (vm->wid) rt(vm, line, "await and time.wait cannot be used inside ::par");
    if (v.t == T_ARR) {
        int any = 0; for (int i = 0; i < v.u.a->n; i++) if (v.u.a->v[i].t == T_TASK) any = 1;
        if (!any) return v;
        Arr *r = arr_new(vm, v.u.a->n);
        for (int i = 0; i < v.u.a->n; i++) arr_push(vm, r, await_value(vm, v.u.a->v[i], line));
        return V_arr(r);
    }
    if (v.t != T_TASK) return v;
    Task *t = v.u.task; t->awaited = 1;
    if (vm->curtask) { while (!t->done) { vm->curtask->waitFor = t; vm->curtask->hasWake = 0; task_yield(vm, vm->curtask); } }
    else { sched_loop(vm, PR_TASK_DONE, t, -1); if (!t->done) rt(vm, line, "await: task can never complete"); }
    if (t->hasErr) task_rethrow(vm, t);
    return t->value;
}

/* после выполнения программы: довести задачи до конца (как drain в песочнице) */
static void sched_drain(VM *vm) {
    sched_loop(vm, PR_MAIN_ERR, NULL, -1);
    if (vm->mainTask && vm->mainTask->hasErr) task_rethrow(vm, vm->mainTask);
    for (int i = 0; i < vm->nall; i++) { Task *t = vm->alltasks[i]; if (t->hasErr && !t->awaited) task_rethrow(vm, t); }
}
/* завершить зависшие потоки (после успеха или ошибки) */
static void sched_shutdown(VM *vm) {
    vm->shutdown = 1;
    vm->curtask = NULL; vm->jbp = &vm->jb;
#ifdef PRISM_THREADS
    for (int i = 0; i < vm->nall; i++) {
        Task *t = vm->alltasks[i];
        if (t->started && !t->done && !t->running) { t->kill = 1; task_resume(vm, t); }
    }
#endif
    vm->shutdown = 0; vm->ntasks = 0; vm->nall = 0; vm->mainTask = NULL;
}

/* ---------------------------------------------------------------- модуль time */
static Value stamp_obj(VM *vm, int utc) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    time_t sec = ts.tv_sec; struct tm g;
    if (utc) gmtime_r(&sec, &g); else localtime_r(&sec, &g);
    Obj *o = obj_new(vm); char buf[40];
    obj_set(vm, o, S(vm, "year"), V_int(g.tm_year + 1900)); obj_set(vm, o, S(vm, "month"), V_int(g.tm_mon + 1));
    obj_set(vm, o, S(vm, "day"), V_int(g.tm_mday)); obj_set(vm, o, S(vm, "hour"), V_int(g.tm_hour));
    obj_set(vm, o, S(vm, "minute"), V_int(g.tm_min)); obj_set(vm, o, S(vm, "second"), V_int(g.tm_sec));
    obj_set(vm, o, S(vm, "ms"), V_int(ts.tv_nsec / 1000000));
    obj_set(vm, o, S(vm, "unix"), V_int((int64_t)sec * 1000 + ts.tv_nsec / 1000000));
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    obj_set(vm, o, S(vm, "text"), V_str(cstr(vm, buf)));
    return V_obj(o);
}
static Value nat_time_wait(VM *vm, Value *a, int n) {
    if (vm->wid) rt(vm, vm->curline, "await and time.wait cannot be used inside ::par");
    if (n != 1) rt(vm, vm->curline, "time.wait(ms) takes one number");
    if (!is_num(a[0]) || as_f(a[0]) < 0) { SB sb; sb_init(vm, &sb); repr_to(&sb, a[0], 0); rt(vm, vm->curline, "time.wait(ms): expected number >= 0, got %s", sb.b ? sb.b : ""); }
    wait_ms(vm, as_f(a[0])); return V_none();
}
static Value nat_time_program(VM *vm, Value *a, int n) { (void)a; (void)n; return V_flt(now_ms() - vm->t0); }
static Value nat_time_time(VM *vm, Value *a, int n) { (void)a; (void)n; return stamp_obj(vm, 0); }
static Value nat_time_world(VM *vm, Value *a, int n) { (void)a; (void)n; return stamp_obj(vm, 1); }
static Value nat_time_test(VM *vm, Value *a, int n) {
    if (n < 1 || !(a[0].t == T_FN || a[0].t == T_NAT)) rt(vm, 0, "time.test(fn, n): first argument must be a function, got %s", type_of(n ? a[0] : V_none()));
    int64_t k = 1;
    if (n > 1) { if (a[1].t != T_INT || a[1].u.i < 1) rt(vm, 0, "time.test: n must be int >= 1"); k = a[1].u.i; }
    double t0 = now_ms();
    for (int64_t i = 0; i < k; i++) {
        Value r = call_fn(vm, a[0], NULL, 0, 0, 0, NULL);
        if (r.t == T_TASK) await_value(vm, r, 0);
        else if (r.t == T_STREAM) { Iter *it = iter_new(vm, r, 0); Value x; while (iter_next(vm, it, &x)) {} }
    }
    return V_flt((now_ms() - t0) / (double)k);
}
static void setup_time(VM *vm) {
    Obj *o = obj_new(vm);
    obj_set(vm, o, S(vm, "wait"), V_nat(reg_nat(vm, "time.wait", nat_time_wait)));
    obj_set(vm, o, S(vm, "program"), V_nat(reg_nat(vm, "time.program", nat_time_program)));
    obj_set(vm, o, S(vm, "time"), V_nat(reg_nat(vm, "time.time", nat_time_time)));
    obj_set(vm, o, S(vm, "world"), V_nat(reg_nat(vm, "time.world", nat_time_world)));
    obj_set(vm, o, S(vm, "test"), V_nat(reg_nat(vm, "time.test", nat_time_test)));
    env_def(vm, vm->globals, S(vm, "time"), V_obj(o), 0);
}

/* ---------------------------------------------------------------- параллельный ::par */
static double now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
#ifdef PRISM_THREADS
static int par_default_threads(void) {
    const char *e = getenv("PRISM_THREADS_PAR");
    if (e && atoi(e) > 0) return atoi(e) > 64 ? 64 : atoi(e);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return n > 8 ? 8 : (int)n;
}
typedef struct ParSeg { int w; size_t off, len; } ParSeg;
typedef struct ParCtx {
    Value f; Arr *items; int64_t base; int op, line, n, chunk;
    int next, minerr;
    pthread_mutex_t mx;
    char err[sizeof(((VM *)0)->err)]; int errline;
    ParSeg *segs;
} ParCtx;
typedef struct ParW {
    ParCtx *c; VM vm; int wid, started;
    char *buf; size_t len, cap;
    pthread_t th;
} ParW;

static void par_out(const char *t, size_t n, void *u) {
    ParW *w = (ParW *)u;
    if (w->len + n > w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 4096;
        while (nc < w->len + n) nc *= 2;
        char *nb = (char *)realloc(w->buf, nc);
        if (!nb) return;
        w->buf = nb; w->cap = nc;
    }
    memcpy(w->buf + w->len, t, n); w->len += n;
}
static void par_work(ParW *w) {
    ParCtx *c = w->c; VM *vm = &w->vm;
    int old = tls_own; tls_own = w->wid;
    for (;;) {
        int s = __atomic_fetch_add(&c->next, c->chunk, __ATOMIC_RELAXED);
        if (s >= c->n) break;
        int e = s + c->chunk < c->n ? s + c->chunk : c->n;
        for (volatile int i = s; i < e; i++) {
            if (i > __atomic_load_n(&c->minerr, __ATOMIC_RELAXED)) break;
            ParSeg *sg = &c->segs[i]; sg->w = w->wid - 1; sg->off = w->len;
            vm->depth = 0; vm->jbp = &vm->jb; vm->inl = 0; vm->curline = 0;
            if (setjmp(vm->jb)) {
                pthread_mutex_lock(&c->mx);
                if (i < c->minerr) { c->minerr = i; memcpy(c->err, vm->err, sizeof c->err); c->errline = vm->errline; }
                pthread_mutex_unlock(&c->mx);
                sg->len = w->len - sg->off;
                continue;
            }
            Value x = c->items->v[i];
            if (c->op) { Value a[2]; a[0] = V_int(c->base + i); a[1] = x; call_fn(vm, c->f, a, 2, 1, c->line, NULL); }
            else { Value a[1]; a[0] = x; call_fn(vm, c->f, a, 1, 1, c->line, NULL); }
            if (vm->ctl) { int cc = vm->ctl; vm->ctl = 0; if (cc == 2) rt(vm, c->line, "stop cannot be used in ::par"); }
            sg->len = w->len - sg->off;
        }
    }
    tls_own = old;
}
static void *par_thread(void *arg) { par_work((ParW *)arg); return NULL; }

static int par_run(VM *vm, Value f, Arr *items, int64_t base, int op, int line) {
    int n = items->n;
    int T = vm->nthreads < n ? vm->nthreads : n;
    if (T < 2) return 0;
    ParCtx c; memset(&c, 0, sizeof c);
    c.f = f; c.items = items; c.base = base; c.op = op; c.line = line; c.n = n; c.minerr = 0x7fffffff;
    c.chunk = n / (T * 8); if (c.chunk < 1) c.chunk = 1;
    pthread_mutex_init(&c.mx, NULL);
    c.segs = (ParSeg *)calloc((size_t)n, sizeof(ParSeg));
    ParW *ws = (ParW *)calloc((size_t)T, sizeof(ParW));
    if (!c.segs || !ws) { free(c.segs); free(ws); return 0; }
    size_t room = vm->limit > vm->total ? vm->limit - vm->total : 0;
    for (int k = 0; k < T; k++) {
        ParW *w = &ws[k]; w->c = &c; w->wid = k + 1;
        w->vm = *vm; VM *v = &w->vm;
        memset(v->sbuf, 0, sizeof v->sbuf); v->sbi = 0;
        v->blocks = NULL; v->total = 0; v->limit = room / (size_t)T; v->freeenv = NULL;
        v->wid = v->own = k + 1; v->nmk = 0; v->iter = 0; v->curtask = NULL; v->mainTask = NULL; v->tasks = NULL; v->alltasks = NULL; v->ntasks = v->nall = v->captasks = v->capall = 0;
        v->out = par_out; v->outud = w; v->depth = 0;
        v->rng ^= (uint64_t)(k + 1) * 0x9E3779B97F4A7C15ULL;
        v->maxdepth = k == 0 ? (vm->maxdepth - vm->depth > 100 ? vm->maxdepth - vm->depth : 100) : (vm->maxdepth < 6000 ? vm->maxdepth : 6000);
    }
    g_par_active = 1;
    for (int k = 1; k < T; k++) {
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, (size_t)64 << 20);
        if (pthread_create(&ws[k].th, &at, par_thread, &ws[k]) == 0) ws[k].started = 1;
        pthread_attr_destroy(&at);
    }
    par_work(&ws[0]);              /* текущий поток тоже работает (рабочий №1) */
    for (int k = 1; k < T; k++) if (ws[k].started) pthread_join(ws[k].th, NULL);
    g_par_active = 0;
    /* память рабочих переходит в главную арену: значения в ней остаются живыми */
    for (int k = 0; k < T; k++) {
        Block *wb = ws[k].vm.blocks;
        if (!wb) continue;
        Block *t = wb; while (t->next) t = t->next;
        if (vm->blocks) { t->next = vm->blocks->next; vm->blocks->next = wb; } else vm->blocks = wb;
        vm->total += ws[k].vm.total;
    }
    int last = c.minerr == 0x7fffffff ? n - 1 : c.minerr;
    for (int i = 0; i <= last; i++) {
        ParSeg *sg = &c.segs[i];
        if (sg->len && sg->w >= 0) { if (vm->out) vm->out(ws[sg->w].buf + sg->off, sg->len, vm->outud); }
    }
    int failed = c.minerr != 0x7fffffff; int eline = c.errline; char emsg[sizeof c.err]; memcpy(emsg, c.err, sizeof emsg);
    for (int k = 0; k < T; k++) free(ws[k].buf);
    free(ws); free(c.segs); pthread_mutex_destroy(&c.mx);
    if (failed) { memcpy(vm->err, emsg, sizeof vm->err); vm->errline = eline; longjmp(*vm->jbp, 1); }
    return 1;
}
#else
static int par_default_threads(void) { return 1; }
static int par_run(VM *vm, Value f, Arr *items, int64_t base, int op, int line) { (void)vm; (void)f; (void)items; (void)base; (void)op; (void)line; return 0; }
#endif
/* ===== d4_io ===== */

/* ---------------------------------------------------------------- JSON, file, http */
/* Файлы и сеть по умолчанию выключены: хост включает их через prism_set_file_root / prism_set_http_handler.
   Ошибки возвращаются как значения error(...), как и в остальном языке. */

typedef struct JP { VM *vm; const char *s; int n, i; char err[120]; } JP;
static void jp_ws(JP *p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\n' || p->s[p->i] == '\t' || p->s[p->i] == '\r')) p->i++; }
static Value jp_fail(JP *p, const char *m) { if (!p->err[0]) snprintf(p->err, sizeof p->err, "invalid JSON at position %d: %s", p->i, m); return V_none(); }
static int jp_hex4(JP *p, unsigned *out) {
    if (p->i + 4 > p->n) return 0; unsigned v = 0;
    for (int k = 0; k < 4; k++) { char c = p->s[p->i + k]; v <<= 4; if (c >= '0' && c <= '9') v |= (unsigned)(c - '0'); else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10); else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10); else return 0; }
    p->i += 4; *out = v; return 1;
}
static Str *jp_string(JP *p) {   /* p->i стоит на открывающей кавычке */
    SB sb; sb_init(p->vm, &sb); p->i++;
    for (;;) {
        if (p->i >= p->n) { jp_fail(p, "unterminated string"); return NULL; }
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') break;
        if (c < 0x20) { p->i--; jp_fail(p, "control character in string"); return NULL; }
        if (c != '\\') { sb_c(&sb, (char)c); continue; }
        if (p->i >= p->n) { jp_fail(p, "bad escape"); return NULL; }
        char e = p->s[p->i++];
        switch (e) {
            case '"': sb_c(&sb, '"'); break; case '\\': sb_c(&sb, '\\'); break; case '/': sb_c(&sb, '/'); break;
            case 'b': sb_c(&sb, '\b'); break; case 'f': sb_c(&sb, '\f'); break; case 'n': sb_c(&sb, '\n'); break;
            case 'r': sb_c(&sb, '\r'); break; case 't': sb_c(&sb, '\t'); break;
            case 'u': {
                unsigned u; if (!jp_hex4(p, &u)) { jp_fail(p, "bad \\u escape"); return NULL; }
                if (u >= 0xD800 && u < 0xDC00 && p->i + 1 < p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    int sv = p->i; p->i += 2; unsigned lo;
                    if (jp_hex4(p, &lo) && lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); else p->i = sv;
                }
                char b[4]; int k = u8_enc(u, b); sb_add(&sb, b, k); break;
            }
            default: p->i--; jp_fail(p, "bad escape"); return NULL;
        }
    }
    return sb_str(&sb);
}
static Value jp_value(JP *p, int depth) {
    jp_ws(p);
    if (p->err[0]) return V_none();
    if (depth > 200) return jp_fail(p, "nesting too deep");
    if (p->i >= p->n) return jp_fail(p, "unexpected end");
    char c = p->s[p->i];
    if (c == '{') {
        Obj *o = obj_new(p->vm); p->i++; jp_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; return V_obj(o); }
        for (;;) {
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') return jp_fail(p, "expected string key");
            Str *k = jp_string(p); if (!k) return V_none();
            jp_ws(p); if (p->i >= p->n || p->s[p->i] != ':') return jp_fail(p, "expected ':'");
            p->i++; Value v = jp_value(p, depth + 1); if (p->err[0]) return V_none();
            obj_set(p->vm, o, intern(p->vm, k->s, k->len), v);
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; return V_obj(o); }
            return jp_fail(p, "expected ',' or '}'");
        }
    }
    if (c == '[') {
        Arr *a = arr_new(p->vm, 4); p->i++; jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; return V_arr(a); }
        for (;;) {
            Value v = jp_value(p, depth + 1); if (p->err[0]) return V_none();
            arr_push(p->vm, a, v); jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; return V_arr(a); }
            return jp_fail(p, "expected ',' or ']'");
        }
    }
    if (c == '"') { Str *s = jp_string(p); return s ? V_str(s) : V_none(); }
    if (!strncmp(p->s + p->i, "true", 4) && p->i + 4 <= p->n) { p->i += 4; return V_bool(1); }
    if (!strncmp(p->s + p->i, "false", 5) && p->i + 5 <= p->n) { p->i += 5; return V_bool(0); }
    if (!strncmp(p->s + p->i, "null", 4) && p->i + 4 <= p->n) { p->i += 4; return V_none(); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        int st = p->i, neg = 0, isf = 0; if (c == '-') { neg = 1; p->i++; }
        int ds = p->i; while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == ds) return jp_fail(p, "bad number");
        if (p->i < p->n && p->s[p->i] == '.') { isf = 1; p->i++; int fs = p->i; while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++; if (p->i == fs) return jp_fail(p, "bad number"); }
        if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) { isf = 1; p->i++; if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++; int es = p->i; while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++; if (p->i == es) return jp_fail(p, "bad number"); }
        char buf[400]; int L = p->i - st; if (L >= (int)sizeof buf) return jp_fail(p, "number too long");
        memcpy(buf, p->s + st, (size_t)L); buf[L] = 0;
        if (!isf) { int dl = p->i - ds; if (dl <= 18) return V_int(strtoll(buf, NULL, 10)); return big_parse(p->vm, p->s + ds, dl, neg); }
        if (p->vm->prec > 0) { Value d = dec_parse_str(p->vm, buf, L); if (d.t == T_DEC) return d; }
        return V_flt(strtod(buf, NULL));
    }
    return jp_fail(p, "unexpected character");
}
static Value json_parse_str(VM *vm, Str *s) {
    JP p; p.vm = vm; p.s = s->s; p.n = s->len; p.i = 0; p.err[0] = 0;
    Value v = jp_value(&p, 0);
    if (!p.err[0]) { jp_ws(&p); if (p.i < p.n) jp_fail(&p, "extra data after value"); }
    if (p.err[0]) return V_err(vm, p.err);
    return v;
}
static void js_str(SB *sb, const char *s, int len) {
    sb_c(sb, '"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': sb_cs(sb, "\\\""); break; case '\\': sb_cs(sb, "\\\\"); break; case '\n': sb_cs(sb, "\\n"); break;
            case '\r': sb_cs(sb, "\\r"); break; case '\t': sb_cs(sb, "\\t"); break; case '\b': sb_cs(sb, "\\b"); break; case '\f': sb_cs(sb, "\\f"); break;
            default: if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); sb_cs(sb, b); } else sb_c(sb, (char)c);
        }
    }
    sb_c(sb, '"');
}
static int js_val(VM *vm, SB *sb, Value v, int depth, char *bad, size_t badcap) {   /* 0 — ошибка (bad заполнен) */
    if (depth > 200) { snprintf(bad, badcap, "json_str: nesting too deep (cyclic data?)"); return 0; }
    switch (v.t) {
        case T_NONE: sb_cs(sb, "null"); return 1;
        case T_BOOL: sb_cs(sb, v.u.b ? "true" : "false"); return 1;
        case T_INT: case T_BIG: case T_DEC: { Value s = V_str(NULL); SB t; sb_init(vm, &t); repr_to(&t, v, 0); (void)s; sb_add(sb, t.b ? t.b : "", t.n); return 1; }
        case T_FLT: if (v.u.f != v.u.f || v.u.f > 1.7976931348623157e308 || v.u.f < -1.7976931348623157e308) { snprintf(bad, badcap, "json_str: cannot encode inf/nan"); return 0; }
            { char b[64]; fmt_float(v.u.f, b, sizeof b); sb_cs(sb, b); } return 1;
        case T_STR: js_str(sb, v.u.s->s, v.u.s->len); return 1;
        case T_ARR: sb_c(sb, '['); for (int i = 0; i < v.u.a->n; i++) { if (i) sb_c(sb, ','); if (!js_val(vm, sb, v.u.a->v[i], depth + 1, bad, badcap)) return 0; } sb_c(sb, ']'); return 1;
        case T_OBJ: sb_c(sb, '{'); for (int i = 0; i < v.u.o->n; i++) { if (i) sb_c(sb, ','); js_str(sb, v.u.o->k[i]->s, v.u.o->k[i]->len); sb_c(sb, ':'); if (!js_val(vm, sb, v.u.o->v[i], depth + 1, bad, badcap)) return 0; } sb_c(sb, '}'); return 1;
        default: snprintf(bad, badcap, "json_str: cannot encode %s", type_of(v)); return 0;
    }
}
static Value nat_json_str(VM *vm, Value *a, int n) {
    if (n != 1) rt(vm, 0, "json_str(x) takes one argument");
    SB sb; sb_init(vm, &sb); char bad[120];
    if (!js_val(vm, &sb, a[0], 0, bad, sizeof bad)) return V_err(vm, bad);
    return V_str(sb_str(&sb));
}

/* --- пути: только внутри корня, без абсолютных и «..» --- */
static int path_ok(const char *p) {
    if (!p[0] || p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':')) return 0;
    for (const char *q = p; *q;) {
        const char *e = q; while (*e && *e != '/' && *e != '\\') e++;
        if (e - q == 2 && q[0] == '.' && q[1] == '.') return 0;
        if (e - q == 0 && *e == 0) break;
        q = *e ? e + 1 : e;
    }
    return 1;
}
static Value io_path(VM *vm, Value *a, int n, int need, const char *fn, char *out, size_t cap, Value *errv) {
    if (n < need || a[0].t != T_STR) rt(vm, 0, "%s: path must be a string", fn);
    if (!vm->froot) { *errv = V_err(vm, "file access is disabled"); return V_bool(0); }
    const char *ps = cz(vm, a[0].u.s);
    if (!path_ok(ps) || memchr(a[0].u.s->s, 0, (size_t)a[0].u.s->len)) { char m[300]; snprintf(m, sizeof m, "%s: path \"%.200s\" is outside the allowed folder", fn, ps); *errv = V_err(vm, m); return V_bool(0); }
    snprintf(out, cap, "%s/%s", vm->froot, ps);
    return V_bool(1);
}
static Value io_errno(VM *vm, const char *fn, Value path) {
    char m[400]; snprintf(m, sizeof m, "%s: %s (\"%.150s\")", fn, strerror(errno), cz(vm, path.u.s)); return V_err(vm, m);
}
static Value nat_file_read(VM *vm, Value *a, int n) {
    char fp[1024]; Value ev; if (!io_path(vm, a, n, 1, "file.read", fp, sizeof fp, &ev).u.b) return ev;
    FILE *f = fopen(fp, "rb"); if (!f) return io_errno(vm, "file.read", a[0]);
    SB sb; sb_init(vm, &sb); char buf[8192]; size_t k; size_t total = 0;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) { total += k; if (vm->limit && total > vm->limit / 2) { fclose(f); return V_err(vm, "file.read: file is too large"); } sb_add(&sb, buf, (int)k); }
    fclose(f); return V_str(sb_str(&sb));
}
static Value nat_file_lines(VM *vm, Value *a, int n) {
    Value t = nat_file_read(vm, a, n); if (t.t != T_STR) return t;
    Arr *r = arr_new(vm, 8); int st = 0, L = t.u.s->len; const char *s = t.u.s->s;
    for (int i = 0; i <= L; i++) if (i == L || s[i] == '\n') {
        if (i == L && st == L) break;
        int e = i; if (e > st && s[e - 1] == '\r') e--;
        arr_push(vm, r, V_str(mkstr(vm, s + st, e - st))); st = i + 1;
    }
    return V_arr(r);
}
static Value io_write(VM *vm, Value *a, int n, const char *mode, const char *fn) {
    char fp[1024]; Value ev; if (!io_path(vm, a, n, 1, fn, fp, sizeof fp, &ev).u.b) return ev;
    if (n != 2 || a[1].t != T_STR) rt(vm, 0, "%s(path, text): text must be a string", fn);
    FILE *f = fopen(fp, mode); if (!f) return io_errno(vm, fn, a[0]);
    size_t w = fwrite(a[1].u.s->s, 1, (size_t)a[1].u.s->len, f);
    int bad = (w != (size_t)a[1].u.s->len) | (fclose(f) != 0);
    if (bad) return io_errno(vm, fn, a[0]);
    return V_none();
}
static Value nat_file_write(VM *vm, Value *a, int n) { return io_write(vm, a, n, "wb", "file.write"); }
static Value nat_file_append(VM *vm, Value *a, int n) { return io_write(vm, a, n, "ab", "file.append"); }
static Value nat_file_write_lines(VM *vm, Value *a, int n) {
    if (n != 2 || (a[1].t != T_ARR && a[1].t != T_STREAM)) rt(vm, 0, "file.write_lines(path, lines): lines must be an array or stream");
    SB sb; sb_init(vm, &sb); Iter *it = iter_new(vm, a[1], 0); Value x;
    while (iter_next(vm, it, &x)) { if (x.t == T_STR) sb_add(&sb, x.u.s->s, x.u.s->len); else repr_to(&sb, x, 1); sb_c(&sb, '\n'); }
    Value args[2]; args[0] = a[0]; args[1] = V_str(sb_str(&sb));
    return io_write(vm, args, 2, "wb", "file.write_lines");
}
static Value nat_file_exists(VM *vm, Value *a, int n) {
    char fp[1024]; Value ev; if (!io_path(vm, a, n, 1, "file.exists", fp, sizeof fp, &ev).u.b) return ev;
    FILE *f = fopen(fp, "rb"); if (f) { fclose(f); return V_bool(1); }
    struct stat st; return V_bool(stat(fp, &st) == 0);
}
static Value nat_file_delete(VM *vm, Value *a, int n) {
    char fp[1024]; Value ev; if (!io_path(vm, a, n, 1, "file.delete", fp, sizeof fp, &ev).u.b) return ev;
    if (remove(fp) != 0) return io_errno(vm, "file.delete", a[0]);
    return V_none();
}
static int cmp_cstr(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }
static Value nat_file_list(VM *vm, Value *a, int n) {
    Value path = n ? a[0] : V_str(S(vm, "."));
    Value args[1]; args[0] = path; char fp[1024]; Value ev;
    if (!io_path(vm, args, 1, 1, "file.list", fp, sizeof fp, &ev).u.b) return ev;
    DIR *d = opendir(fp); if (!d) return io_errno(vm, "file.list", path);
    char **names = NULL; int cnt = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 16; names = (char **)realloc(names, sizeof(char *) * (size_t)cap); }
        names[cnt++] = strdup(e->d_name);
    }
    closedir(d); if (cnt) qsort(names, (size_t)cnt, sizeof(char *), cmp_cstr);
    Arr *r = arr_new(vm, cnt + 1);
    for (int i = 0; i < cnt; i++) { arr_push(vm, r, V_str(cstr(vm, names[i]))); free(names[i]); }
    free(names); return V_arr(r);
}

/* --- http: через колбэк хоста (TLS и сеть — забота хоста) --- */
static Value http_do(VM *vm, const char *fn, Str *method, Str *url, const char *hdrs, Str *body, int timeout) {
    if (!vm->http) return V_err(vm, "http is disabled");
    PrismHttpResponse rs; memset(&rs, 0, sizeof rs);
    int rc = vm->http(cz(vm, method), cz(vm, url), hdrs ? hdrs : "", body ? body->s : "", body ? (size_t)body->len : 0, timeout, &rs, vm->httpud);
    if (rc != 0) { char m[400]; snprintf(m, sizeof m, "%s: %.300s", fn, rs.error ? rs.error : "request failed"); free(rs.error); free(rs.body); free(rs.headers); return V_err(vm, m); }
    if (rs.status >= 400) {
        char m[400]; snprintf(m, sizeof m, "%s: HTTP %d", fn, rs.status);
        free(rs.error); free(rs.body); free(rs.headers); return V_err(vm, m);
    }
    Value body_v = V_str(mkstr(vm, rs.body ? rs.body : "", (int)rs.blen));
    Obj *h = obj_new(vm);
    if (rs.headers) {   /* "name: value\n" — имена в нижнем регистре */
        const char *p = rs.headers;
        while (*p) {
            const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
            const char *c = memchr(p, ':', (size_t)(e - p));
            if (c) {
                char nm[200]; int nl = (int)(c - p); if (nl > 199) nl = 199;
                for (int i = 0; i < nl; i++) nm[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : (p[i] == '-' ? '_' : p[i]));
                nm[nl] = 0; const char *v = c + 1; while (v < e && *v == ' ') v++;
                const char *ve = e; if (ve > v && ve[-1] == '\r') ve--;
                obj_set(vm, h, intern(vm, nm, nl), V_str(mkstr(vm, v, (int)(ve - v))));
            }
            p = *e ? e + 1 : e;
        }
    }
    Obj *o = obj_new(vm);
    obj_set(vm, o, S(vm, "status"), V_int(rs.status));
    obj_set(vm, o, S(vm, "ok"), V_bool(1));
    obj_set(vm, o, S(vm, "body"), body_v);
    obj_set(vm, o, S(vm, "headers"), V_obj(h));
    free(rs.error); free(rs.body); free(rs.headers);
    return V_obj(o);
}
static const char *http_hdr_str(VM *vm, Value h, const char *fn) {   /* объект [name: value] -> "name: value\n..." */
    if (h.t == T_NONE) return NULL;
    if (h.t != T_OBJ) rt(vm, 0, "%s: headers must be an object like [name: \"value\"]", fn);
    SB sb; sb_init(vm, &sb);
    for (int i = 0; i < h.u.o->n; i++) {
        Value v = h.u.o->v[i]; if (v.t != T_STR) rt(vm, 0, "%s: header values must be strings", fn);
        for (int q = 0; q < h.u.o->k[i]->len; q++) { char ch = h.u.o->k[i]->s[q]; sb_c(&sb, ch == '_' ? '-' : ch); } sb_cs(&sb, ": "); sb_add(&sb, v.u.s->s, v.u.s->len); sb_c(&sb, '\n');
    }
    return sb_str(&sb)->s;
}
static Value nat_http_get(VM *vm, Value *a, int n) {
    if (n < 1 || n > 2 || a[0].t != T_STR) rt(vm, 0, "http.get(url[, headers]): url must be a string");
    return http_do(vm, "http.get", S(vm, "GET"), a[0].u.s, n > 1 ? http_hdr_str(vm, a[1], "http.get") : NULL, NULL, 30000);
}
static Value http_body(VM *vm, Value *a, int n, const char *fn, const char *method) {
    if (n < 2 || n > 3 || a[0].t != T_STR || a[1].t != T_STR) rt(vm, 0, "%s(url, body[, headers]): url and body must be strings (use json_str(x) for objects)", fn);
    return http_do(vm, fn, S(vm, method), a[0].u.s, n > 2 ? http_hdr_str(vm, a[2], fn) : NULL, a[1].u.s, 30000);
}
static Value nat_http_post(VM *vm, Value *a, int n) { return http_body(vm, a, n, "http.post", "POST"); }
static Value nat_http_put(VM *vm, Value *a, int n) { return http_body(vm, a, n, "http.put", "PUT"); }
static Value nat_http_delete(VM *vm, Value *a, int n) {
    if (n < 1 || n > 2 || a[0].t != T_STR) rt(vm, 0, "http.delete(url[, headers]): url must be a string");
    return http_do(vm, "http.delete", S(vm, "DELETE"), a[0].u.s, n > 1 ? http_hdr_str(vm, a[1], "http.delete") : NULL, NULL, 30000);
}
static Value nat_http_request(VM *vm, Value *a, int n) {   /* http.request([method:, url:, headers:, body:, timeout:]) */
    if (n != 1 || a[0].t != T_OBJ) rt(vm, 0, "http.request(options): options must be an object [method:, url:, headers:, body:, timeout:]");
    Obj *o = a[0].u.o; Str *method = S(vm, "GET"), *url = NULL, *body = NULL; Value hv = V_none(); int timeout = 30000;
    for (int i = 0; i < o->n; i++) {
        const char *k = o->k[i]->s; Value v = o->v[i];
        if (!strcmp(k, "method") && v.t == T_STR) method = v.u.s;
        else if (!strcmp(k, "url") && v.t == T_STR) url = v.u.s;
        else if (!strcmp(k, "body") && v.t == T_STR) body = v.u.s;
        else if (!strcmp(k, "headers")) hv = v;
        else if (!strcmp(k, "timeout") && v.t == T_INT && v.u.i > 0) timeout = (int)(v.u.i > 600000 ? 600000 : v.u.i);
        else rt(vm, 0, "http.request: bad option '%s' (method, url, headers, body, timeout; strings, timeout int ms)", k);
    }
    if (!url) rt(vm, 0, "http.request: url is required");
    return http_do(vm, "http.request", method, url, http_hdr_str(vm, hv, "http.request"), body, timeout);
}
static void setup_io(VM *vm) {
    Env *g = vm->globals;
    Obj *fo = obj_new(vm);
    obj_set(vm, fo, S(vm, "read"), V_nat(reg_nat(vm, "file.read", nat_file_read)));
    obj_set(vm, fo, S(vm, "lines"), V_nat(reg_nat(vm, "file.lines", nat_file_lines)));
    obj_set(vm, fo, S(vm, "write"), V_nat(reg_nat(vm, "file.write", nat_file_write)));
    obj_set(vm, fo, S(vm, "append"), V_nat(reg_nat(vm, "file.append", nat_file_append)));
    obj_set(vm, fo, S(vm, "write_lines"), V_nat(reg_nat(vm, "file.write_lines", nat_file_write_lines)));
    obj_set(vm, fo, S(vm, "exists"), V_nat(reg_nat(vm, "file.exists", nat_file_exists)));
    obj_set(vm, fo, S(vm, "delete"), V_nat(reg_nat(vm, "file.delete", nat_file_delete)));
    obj_set(vm, fo, S(vm, "list"), V_nat(reg_nat(vm, "file.list", nat_file_list)));
    env_def(vm, g, S(vm, "file"), V_obj(fo), 0);
    Obj *ho = obj_new(vm);
    obj_set(vm, ho, S(vm, "get"), V_nat(reg_nat(vm, "http.get", nat_http_get)));
    obj_set(vm, ho, S(vm, "post"), V_nat(reg_nat(vm, "http.post", nat_http_post)));
    obj_set(vm, ho, S(vm, "put"), V_nat(reg_nat(vm, "http.put", nat_http_put)));
    obj_set(vm, ho, S(vm, "delete"), V_nat(reg_nat(vm, "http.delete", nat_http_delete)));
    obj_set(vm, ho, S(vm, "request"), V_nat(reg_nat(vm, "http.request", nat_http_request)));
    env_def(vm, g, S(vm, "http"), V_obj(ho), 0);
    env_def(vm, g, S(vm, "json_str"), V_nat(reg_nat(vm, "json_str", nat_json_str)), 0);
}
/* ===== d5_prelude ===== */
/* сгенерировано tools/gen_prelude.sh из lib/prelude.pr — не править вручную */
static const char PRELUDE_SRC[] =
"// Стандартная библиотека Prismflow, написанная на самом Prismflow:\n"
"// один и тот же текст исполняют C-ядро и JS-песочница.\n"
"// После правки запустите tools/gen_prelude.sh (C) и пересоберите песочницу (prism/build.js).\n"
"\n"
"// ---- числа ----\n"
"lcm a b = is a == 0 or b == 0 -> 0 else -> div(abs(a * b), gcd(a, b))\n"
"factorial n = range(2, n + 1).reduce(1, (a, b) -> a * b)\n"
"prod xs = xs.reduce(1, (a, b) -> a * b)\n"
"comb n k =\n"
"    is k < 0 or k > n -> 0\n"
"    else -> div(factorial(n), factorial(k) * factorial(n - k))\n"
"bit_count n =\n"
"    bits := 0\n"
"    m := n\n"
"    range(64):: _i\n"
"        is m <= 0 -> stop\n"
"        bits := bits + m % 2\n"
"        m := div(m, 2)\n"
"    bits\n"
"isqrt n =\n"
"    is n < 0 -> error(\"isqrt: negative number\")\n"
"    is n < 2 -> n\n"
"    else ->\n"
"        nr x = div(x + div(n, x), 2)\n"
"        nr.from(n).until(x -> x * x <= n).last()\n"
"\n"
"// ---- потоки ----\n"
"until s p =\n"
"    done := false\n"
"    chk x =\n"
"        r = not done\n"
"        done := p(x)\n"
"        r\n"
"    s.take_while(chk)\n"
"iterate f x = f.from(x)\n"
"unfold f seed =\n"
"    st := seed\n"
"    step _k =\n"
"        r = f(st)\n"
"        is r == none -> none\n"
"        else ->\n"
"            st := r[1]\n"
"            r[0]\n"
"    step.from(0).skip(1).take_while(x -> x != none)\n"
"\n"
"// ---- поиск в отсортированном массиве ----\n"
"bisect_left xs x =\n"
"    lo := 0\n"
"    hi := len(xs)\n"
"    range(64):: _i\n"
"        is lo >= hi -> stop\n"
"        mid = div(lo + hi, 2)\n"
"        is xs[mid] < x -> lo := mid + 1\n"
"        else -> hi := mid\n"
"    lo\n"
"bisect_right xs x =\n"
"    lo := 0\n"
"    hi := len(xs)\n"
"    range(64):: _i\n"
"        is lo >= hi -> stop\n"
"        mid = div(lo + hi, 2)\n"
"        is x < xs[mid] -> hi := mid\n"
"        else -> lo := mid + 1\n"
"    lo\n"
"partition_point xs p =\n"
"    lo := 0\n"
"    hi := len(xs)\n"
"    range(64):: _i\n"
"        is lo >= hi -> stop\n"
"        mid = div(lo + hi, 2)\n"
"        is p(xs[mid]) -> lo := mid + 1\n"
"        else -> hi := mid\n"
"    lo\n"
"\n"
"// ---- куча (минимальная; элементы сравниваются как числа, строки или массивы) ----\n"
"heap_push h x =\n"
"    h += x\n"
"    i := len(h) - 1\n"
"    range(64):: _k\n"
"        is i <= 0 -> stop\n"
"        par = div(i - 1, 2)\n"
"        is h[i] < h[par] ->\n"
"            h[i], h[par] := h[par], h[i]\n"
"            i := par\n"
"        else -> stop\n"
"    none\n"
"heap_peek h = is len(h) == 0 -> error(\"heap_peek: heap is empty\") else -> h[0]\n"
"heap_pop h =\n"
"    is len(h) == 0 -> error(\"heap_pop: heap is empty\")\n"
"    else ->\n"
"        top = h[0]\n"
"        tail = h.pop()\n"
"        is len(h) > 0 ->\n"
"            h[0] := tail\n"
"            i := 0\n"
"            range(64):: _k\n"
"                l = 2 * i + 1\n"
"                is l >= len(h) -> stop\n"
"                r = l + 1\n"
"                c = is r < len(h) and h[r] < h[l] -> r else -> l\n"
"                is h[c] < h[i] ->\n"
"                    h[c], h[i] := h[i], h[c]\n"
"                    i := c\n"
"                else -> stop\n"
"        top\n"
"\n"
"// ---- массивы ----\n"
"transpose m = is len(m) == 0 -> [] else -> range(len(m[0])).map(j -> m.map(r -> r[j]).to_list()).to_list()\n"
"rotate xs k =\n"
"    n = len(xs)\n"
"    is n == 0 -> xs\n"
"    else ->\n"
"        s = k % n\n"
"        [...xs[s..n], ...xs[0..s]]\n"
"\n"
"// ---- комбинаторика (результат — массив массивов) ----\n"
"product xs ys = xs.map(x -> ys.map(y -> [x, y]).to_list()).flat().to_list()\n"
"combinations xs k =\n"
"    is k == 0 -> [[]]\n"
"    is len(xs) < k -> []\n"
"    else ->\n"
"        head = xs[0]\n"
"        tl = xs[1..len(xs)]\n"
"        with_head = combinations(tl, k - 1).map(c -> [head, ...c]).to_list()\n"
"        [...with_head, ...combinations(tl, k)]\n"
"_perm_at xs i = permutations([...xs[0..i], ...xs[i + 1..len(xs)]]).map(p -> [xs[i], ...p]).to_list()\n"
"permutations xs =\n"
"    is len(xs) <= 1 -> [xs]\n"
"    else -> range(len(xs)).map(i -> _perm_at(xs, i)).flat().to_list()\n"
;
/* ===== e_builtins ===== */

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
/* ===== f_api ===== */

/* ---------------------------------------------------------------- публичный API */
struct PrismVM { VM vm; };

PrismVM *prism_new(void) {
    PrismVM *volatile p = (PrismVM *)calloc(1, sizeof(PrismVM));
    if (!p) return NULL;
    VM *vm = &p->vm;
    vm->limit = (size_t)256 << 20;
    vm->maxrange = 50000000u;
    { /* зерно генератора — от времени и адреса, иначе каждый запуск выдаёт одни и те же «случайные» числа; PRISM_SEED=n задаёт его вручную */
        static unsigned long counter = 0;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t s = (uint64_t)ts.tv_nsec * 0x9E3779B97F4A7C15ULL ^ (uint64_t)ts.tv_sec * 0xBF58476D1CE4E5B9ULL ^ (uint64_t)(uintptr_t)p ^ (++counter * 0x94D049BB133111EBULL);
        const char *es = getenv("PRISM_SEED");
        if (es && *es) s = (uint64_t)strtoull(es, NULL, 10) * 0x9E3779B97F4A7C15ULL + 1;
        if (!s) s = 0x9E3779B97F4A7C15ULL;
        vm->rng = s;
    }
    vm->jbp = &vm->jb; vm->maxdepth = PRISM_MAX_DEPTH;
    vm->tab = (Intern **)calloc(256, sizeof(Intern *));
    vm->nthreads = par_default_threads();
    vm->norewind = getenv("PRISM_NO_REWIND") != NULL; vm->poison = getenv("PRISM_POISON") != NULL;
    if (!vm->tab) { free(p); return NULL; }
    if (setjmp(vm->jb)) { arena_free(vm); free(vm->tab); free(p); return NULL; }
    vm->s_empty = mkstr(vm, "", 0);
    vm->globals = env_new(vm, NULL, 0);
    setup_globals(vm);
    { int pn = 0; Node **pg = parse_program(vm, PRELUDE_SRC, (int)sizeof(PRELUDE_SRC) - 1, &pn); exec_block(vm, pg, pn, vm->globals); vm->ctl = 0; }   /* библиотека на Prismflow */
    vm->user = env_new(vm, vm->globals, 0);
    return p;
}
void prism_free(PrismVM *p) { if (!p) return; free(p->vm.froot); arena_free(&p->vm); free(p->vm.tab); free(p); }
void prism_set_file_root(PrismVM *p, const char *dir) {
    if (!p) return;
    free(p->vm.froot); p->vm.froot = NULL;
    if (dir && *dir) { size_t l = strlen(dir); while (l > 1 && (dir[l - 1] == '/')) l--; p->vm.froot = (char *)malloc(l + 1); if (p->vm.froot) { memcpy(p->vm.froot, dir, l); p->vm.froot[l] = 0; } }
}
void prism_set_http_handler(PrismVM *p, PrismHttpFn fn, void *user) { if (p) { p->vm.http = fn; p->vm.httpud = user; } }
void prism_set_threads(PrismVM *p, int n) { if (p) p->vm.nthreads = n < 1 ? 1 : (n > 64 ? 64 : n); }
void prism_set_output(PrismVM *p, PrismWriteFn fn, void *user) { p->vm.out = fn; p->vm.outud = user; }
void prism_set_input(PrismVM *p, PrismReadFn fn, void *user) { p->vm.in = fn; p->vm.inud = user; }
void prism_set_memory_limit(PrismVM *p, size_t bytes) { p->vm.limit = bytes; }
void prism_set_max_depth(PrismVM *pv, int depth) { VM *v = &pv->vm; v->userdepth = depth > 0 ? depth : 0; v->maxdepth = depth > 0 ? depth : PRISM_MAX_DEPTH; }
void prism_add_module(PrismVM *p, const char *name, const char *source) { if (p && name && source) mod_add(&p->vm, name, source, strlen(source)); }
void prism_set_module_dir(PrismVM *p, const char *dir) { if (!p) return; VM *v = &p->vm; v->moddir = NULL; if (dir && *dir) { size_t l = strlen(dir); v->moddir = (char *)amalloc(v, l + 1); memcpy(v->moddir, dir, l + 1); } }
const char *prism_version(void) { return PRISM_VERSION; }

PrismResult prism_run(PrismVM *p, const char *source) {
    PrismResult res; memset(&res, 0, sizeof res);
    VM *vm = &p->vm;
    vm->depth = 0; vm->err[0] = 0; vm->errline = 0; vm->curline = 0; vm->own = 0; tls_own = 0; vm->nmk = 0; vm->iter = 0;
    vm->jbp = &vm->jb; vm->curtask = NULL; vm->maxdepth = vm->userdepth ? vm->userdepth : PRISM_MAX_DEPTH; vm->inl = 0;
    vm->ntasks = 0; vm->nall = 0; vm->mainTask = NULL; vm->t0 = now_ms();
    if (setjmp(vm->jb)) {
        char keep[sizeof vm->err]; int kl = vm->errline; memcpy(keep, vm->err, sizeof keep);
        sched_shutdown(vm);
        memcpy(vm->err, keep, sizeof keep); vm->errline = kl;
        res.ok = 0; res.line = vm->errline;
        snprintf(res.message, sizeof res.message, "%s", vm->err[0] ? vm->err : "InternalError");
        return res;
    }
    const char *src = source ? source : "";
    int len = (int)strlen(src);
    /* первая строка «// syntax: v1» (старый синтаксис со скобками) этой версией не поддерживается */
    if (!strncmp(src, "// syntax: v1", 13)) fail(vm, 1, "RuntimeError: old syntax (// syntax: v1) is not supported");
    int n = 0;
    Node **prog = parse_program(vm, src, len, &n);
    if (vm->prog_hw) { /* на верхнем уровне есть ожидание: программа сама становится задачей */
        Clause *c = (Clause *)amalloc(vm, sizeof(Clause));
        memset(c, 0, sizeof *c); c->isBlock = 1; c->body = prog; c->nbody = n; c->async = 1;
        Value mt = spawn_task(vm, c, vm->user, 0); vm->mainTask = mt.u.task;
    } else {
        Value top = exec_block(vm, prog, n, vm->user);
        if (vm->ctl == 3) vm->ctl = 0;
        else if (!vm->ctl && top.t == T_STREAM) { Iter *tit = iter_new(vm, top, 0); Value tx; while (iter_next(vm, tit, &tx)) {} }
        if (vm->ctl) { vm->ctl = 0; rt(vm, vm->curline, "next/stop must be inside a :: loop or a stream stage (select, where, ...)"); }
    }
    sched_drain(vm);
    sched_shutdown(vm);
    res.ok = 1;
    return res;
}

typedef struct BufOut { char *b; size_t cap, n; } BufOut;
static void buf_write(const char *t, size_t len, void *u) {
    BufOut *o = (BufOut *)u;
    if (!o->cap) return;
    size_t room = o->cap - 1 - o->n;
    if (len > room) len = room;
    memcpy(o->b + o->n, t, len); o->n += len; o->b[o->n] = 0;
}
int prism_run_to_buffer(const char *source, char *out, size_t outcap, char *err, size_t errcap) {
    if (out && outcap) out[0] = 0;
    if (err && errcap) err[0] = 0;
    PrismVM *p = prism_new();
    if (!p) { if (err && errcap) snprintf(err, errcap, "cannot create VM"); return 1; }
    BufOut bo = { out, out ? outcap : 0, 0 };
    prism_set_output(p, buf_write, &bo);
    PrismResult r = prism_run(p, source);
    prism_free(p);
    if (!r.ok) { if (err && errcap) snprintf(err, errcap, "%s", r.message); return 1; }
    return 0;
}
