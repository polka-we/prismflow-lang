
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
