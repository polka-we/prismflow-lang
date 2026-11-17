
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
