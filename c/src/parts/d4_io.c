
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
