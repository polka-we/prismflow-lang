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
