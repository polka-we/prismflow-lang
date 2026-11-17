
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
