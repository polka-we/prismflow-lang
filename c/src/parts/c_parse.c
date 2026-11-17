
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
