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
