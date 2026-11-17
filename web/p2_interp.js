/* ===== Prism: значения и ядро интерпретатора ===== */
const REPR2 = { on: false }; /* в синтаксисе v2 объекты печатаются как [k: v] */
const TYPENAMES = new Set(['number', 'int', 'float', 'string', 'bool', 'none', 'array', 'object', 'function', 'error', 'stream']);
const isPlain = v => v !== null && typeof v === 'object' && !(v instanceof Dec) && !Array.isArray(v) && !(v instanceof Stream) && !(v instanceof PError) && !(v instanceof Closure) && !(v instanceof Native) && !(v instanceof TypeDef) && !(v instanceof Task);
function typeOf(v) {
  if (v === null || v === undefined) return 'none';
  if (typeof v === 'boolean') return 'bool';
  if (typeof v === 'bigint') return 'int';
  if (typeof v === 'number' || v instanceof Dec) return 'float';
  if (typeof v === 'string') return 'string';
  if (Array.isArray(v)) return 'array';
  if (v instanceof Stream) return 'stream';
  if (v instanceof Closure || v instanceof Native) return 'function';
  if (v instanceof PError) return 'error';
  if (v instanceof TypeDef) return 'type';
  if (v instanceof Task) return 'task';
  if (v[TYPE]) return v[TYPE].name;
  return 'object';
}
function repr(v) {
  if (v === null || v === undefined) return 'none';
  if (typeof v === 'string') return '"' + v.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n') + '"';
  if (typeof v === 'bigint') return String(v);
  if (typeof v === 'number') return fmtFloat(v);
  if (v instanceof Dec) return decStr(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return '[' + v.map(repr).join(', ') + ']';
  if (v instanceof Stream) return repr(Array.from(v));
  if (v instanceof Closure) return '<fn ' + (v.name || 'lambda') + '>';
  if (v instanceof Native) return '<fn ' + v.name + '>';
  if (v instanceof PError) return 'error(' + repr(v.message) + ')';
  if (v instanceof TypeDef) return '<type ' + v.name + '>';
  if (v instanceof Task) return '<task' + (v.done ? ' done' : '') + '>';
  const body = Object.keys(v).map(k => k + ': ' + repr(v[k])).join(', '), nm = v[TYPE] ? v[TYPE].name : '';
  return REPR2.on ? nm + '[' + (body || ':') + ']' : nm + '{' + body + '}';
}
const hasType = (v, def) => isPlain(v) && v[TYPE] === def;
function tagLike(from, to) { if (from && from[TYPE]) Object.defineProperty(to, TYPE, { value: from[TYPE] }); return to; }
function matchTypeName(name, v) { return name === 'number' ? isNum(v) : name === 'object' ? isPlain(v) : typeOf(v) === name; }
function fmtFloat(v) {
  if (Number.isNaN(v)) return 'nan';
  if (v === Infinity) return 'inf'; if (v === -Infinity) return '-inf';
  return Number.isInteger(v) && Math.abs(v) < 1e21 ? v.toFixed(1) : String(v);
}
const isNum = v => typeof v === 'bigint' || typeof v === 'number' || v instanceof Dec;
const isInt = v => typeof v === 'bigint';
const toF = v => Number(v);
/* индекс/счётчик: только int; возвращает обычное число JS */
const idx = v => Number(v);
function deepCopy(v) {
  if (Array.isArray(v)) return v.map(deepCopy);
  if (isPlain(v)) { const o = {}; for (const k of Object.keys(v)) o[k] = deepCopy(v[k]); return tagLike(v, o); }
  return v;
}
const str = v => typeof v === 'string' ? v : repr(v);
function eq(a, b) {
  if (a === b) return true;
  if (isNum(a) && isNum(b)) { if (a instanceof Dec || b instanceof Dec) { const x = toDec(a), y = toDec(b); return x instanceof Dec && y instanceof Dec ? decCmp(x, y) === 0 : Number(a) === Number(b); } return a == b; } // eslint-disable-line eqeqeq

  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => eq(x, b[i]));
  if (isPlain(a) && isPlain(b)) { if (a[TYPE] !== b[TYPE]) return false; const ka = Object.keys(a), kb = Object.keys(b); return ka.length === kb.length && ka.every(k => Object.prototype.hasOwnProperty.call(b, k) && eq(a[k], b[k])); }
  return false;
}

/* строки считаются по символам (кодовым точкам), а не по внутренним единицам JS */
const SURR = /[\ud800-\udfff]/;
const sLen = s => (SURR.test(s) ? Array.from(s).length : s.length);
const sSlice = (s, a, b) => (SURR.test(s) ? Array.from(s).slice(a, b).join('') : s.slice(a, b));
const sAt = (s, i) => (SURR.test(s) ? Array.from(s)[i] : s[i]);

/* таблица имён окружения: маленькие области — линейный поиск по массивам (быстрее Map), большие — индекс в Map */
class VarMap {
  constructor() { this.k = [null, null, null, null]; this.v = [null, null, null, null]; this.n = 0; this.m = null; }
  get size() { return this.n; }
  get(name) {
    if (this.m !== null) { const i = this.m.get(name); return i === undefined ? undefined : this.v[i]; }
    const k = this.k;
    for (let i = this.n - 1; i >= 0; i--) if (k[i] === name) return this.v[i];
    return undefined;
  }
  has(name) { return this.get(name) !== undefined; }
  set(name, b) {
    const k = this.k, n = this.n;
    if (this.m !== null) {
      const i = this.m.get(name);
      if (i !== undefined) { this.v[i] = b; return this; }
      this.m.set(name, n); k[n] = name; this.v[n] = b; this.n = n + 1; return this;
    }
    for (let i = n - 1; i >= 0; i--) if (k[i] === name) { this.v[i] = b; return this; }
    k[n] = name; this.v[n] = b; this.n = n + 1;
    if (n >= 8) { this.m = new Map(); for (let i = 0; i <= n; i++) this.m.set(k[i], i); }
    return this;
  }
  clear(keep) { this.n = keep; this.m = null; }
  values() { return this.v.slice(0, this.n)[Symbol.iterator](); }
  keys() { return this.k.slice(0, this.n)[Symbol.iterator](); }
  *entries() { for (let i = 0; i < this.n; i++) yield [this.k[i], this.v[i]]; }
  [Symbol.iterator]() { return this.entries(); }
}
class Env {
  constructor(parent, par) { this.vars = new VarMap(); this.parent = parent || null; this.par = !!par; this.cap = false; }
  lookup(name) { for (let e = this; e; e = e.parent) { const b = e.vars.get(name); if (b !== undefined) return b; } return null; }
}

/* все узлы AST — одного класса с общей формой: обращения n.type/n.op/... остаются мономорфными */
class ANode {
  constructor() {
    this.type = undefined; this.line = undefined; this.name = undefined; this.v = undefined; this.callee = undefined; this.args = undefined;
    this.expr = undefined; this.obj = undefined; this.op = undefined; this.left = undefined; this.right = undefined; this.items = undefined;
    this.value = undefined; this.target = undefined; this.body = undefined; this.params = undefined; this.isBlock = undefined; this.async = undefined;
    this.head = undefined; this.clauses = undefined; this.props = undefined; this.index = undefined; this.src = undefined; this.fn = undefined;
    this.par = undefined; this.pairs = undefined; this.arg = undefined; this.hw = undefined; this.paren = undefined; this.arms = undefined;
    this.pat = undefined; this.fields = undefined; this.k = undefined; this.subject = undefined; this.start = undefined; this.end = undefined;
    this.cond = undefined; this.opt = undefined; this.terms = undefined; this.ops = undefined; this.fold = undefined;
  }
}
function normAst(x, memo) {
  if (x === null || typeof x !== 'object') return x;
  const m = memo.get(x); if (m !== undefined) return m;
  if (Array.isArray(x)) {
    memo.set(x, x);
    for (let i = 0; i < x.length; i++) x[i] = normAst(x[i], memo);
    return x;
  }
  if (typeof x.type === 'string' && Object.getPrototypeOf(x) === Object.prototype) {
    const n = new ANode(); memo.set(x, n);
    for (const k of Object.keys(x)) { if (!(k in n)) { n[k] = x[k]; continue; } n[k] = normAst(x[k], memo); }
    return n;
  }
  if (Object.getPrototypeOf(x) === Object.prototype) { memo.set(x, x); for (const k of Object.keys(x)) x[k] = normAst(x[k], memo); }
  return x;
}

/* окружение «захвачено» (замыканием, привязанным методом): его нельзя переиспользовать между итерациями цикла */
function markCap(env) { for (let e = env; e && !e.cap; e = e.parent) e.cap = true; }

class Interp {
  constructor(opts) {
    this.opts = opts; this.files = opts.files || {}; this.stdin = (opts.stdin || []).slice(); this.stdinPos = 0;
    this.lines = 0; this.maxLines = opts.maxLines || 20000; this.cache = new Map();
    this.tasks = []; this.main = null; this.warp = 0; this.t0 = performance.now();
    this.globals = new Env(null); this.setupGlobals();
    if (typeof PRELUDE_SRC === 'string') { this.exec(this.parseSrc(PRELUDE_SRC), this.globals); this.lines = 0; }   /* библиотека на Prismflow */
  }
  /* split без точки: если переменной нет — метод элемента (split(x) = x.split()); иначе ошибка с подсказкой */
  undefVarOrMethod(name, env, n) {
    if (METHOD_NAMES.has(name)) return new Native(name, a => {
      if (!a.length) throw this.err('method .' + name + ' needs a value to call it on', n);
      const m = this.getMember(a[0], name, n, env);
      return (m instanceof Native || m instanceof Closure) ? this.callFn(m, a.slice(1), false, n) : m;
    });
    throw this.undefVar(name, env, n);
  }
  undefVar(name, env, n) {
    const c = []; for (let e = env; e; e = e.parent) for (const k of e.vars.keys()) c.push(k);
    const sg = suggestName(name, c);
    return this.err('undefined variable "' + name + '"' + (sg ? '. Did you mean: "' + sg + '"?' : ''), n);
  }
  noMethod(what, name, list, env, n) {
    let sg = suggestName(name, list);
    return this.err(what + ' has no method .' + name + (sg ? '. Did you mean: .' + sg + '?' : ''), n);
  }
  err(msg, n) { return new PrismError(msg, n && n.line); }

  /* ---- запуск ---- */
  parseSrc(src) { const p = new Parser(this.lexSrc(src, true)).parseProgram(); p.hw = markHw(p); return normAst(p, new WeakMap()); }
  runMain(src) {
    const prog = this.parseSrc(src);
    const env = new Env(this.globals);
    if (prog.hw) this.main = this.spawn(this.gExec(prog, env)); else { try { this.exec(prog, env); if (this.last instanceof Stream) for (const _ of this.last); } catch (e) { if (!(e instanceof RetSig)) throw e; } }
    this.drain();
  }
  loadModule(name, n) {
    name = String(name).replace(/\.prism$/, '');
    if (!Object.prototype.hasOwnProperty.call(this.files, name)) throw this.err('module "' + name + '" not found (available: ' + Object.keys(this.files).join(', ') + ')', n);
    if (this.cache.has(name)) {
      const c = this.cache.get(name);
      if (c === 'loading') throw this.err('circular import of module "' + name + '"', n);
      return c;
    }
    this.cache.set(name, 'loading');
    const env = new Env(this.globals);
    try { this.exec(this.parseSrc(this.files[name]), env); }
    catch (e) { if (e instanceof PrismError && !e.file) e.file = name; throw e; }
    const out = {};
    for (const [k, b] of env.vars) if (k[0] !== '_') out[k] = b.v;
    this.cache.set(name, out);
    return out;
  }

  /* ---- вызовы ---- */
  /* откуда пришёл аргумент: изменяемое имя (или свежее значение) -> true; константа -> false */
  isMutPath(node, env) {
    const r = this.rootIdent(node);
    if (!r) return true;
    const f = this.findBinding(env, r.name);
    return !f || (f.b.mut && !f.b.frozenRef);
  }
  construct(def, args, n) {
    if (args.length > def.fields.length) throw this.err(def.name + ' takes at most ' + def.fields.length + ' arguments (' + def.fields.map(x => x.name).join(', ') + '), got ' + args.length, n);
    const o = {};
    def.fields.forEach((fl, i) => {
      if (i < args.length) o[fl.name] = args[i];
      else if (fl.def) o[fl.name] = this.ev(fl.def, def.env);
      else throw this.err('missing argument "' + fl.name + '" in ' + def.name + '(' + def.fields.map(x => x.name).join(', ') + ')', n);
    });
    Object.defineProperty(o, TYPE, { value: def });
    return o;
  }
  callFn(f, args, par, n, muts) {
    if (f instanceof TypeDef) return this.construct(f, args, n);
    if (f instanceof Native) return f.fn(args);
    if (!(f instanceof Closure)) throw this.err('expected a function, got ' + typeOf(f) + (f === null ? '' : ' ' + repr(f)), n);
    const lc = this.loopcall; this.loopcall = false;
    try {
      if (f.clauses) return this.callClauses(f, args, par, n, muts);
      const env = this.bindParams(f, args, par, n, muts);
      if (f.async) return this.spawn(f.isBlock ? this.gRunBlock(f.body, env) : this.gEv(f.body, env));
      if (!f.isBlock) return this.ev(f.body, env);
      const r = this.exec(f.body, env);
      return r ? r.ret : this.last;
    } catch (e) { if (e instanceof RetSig && !lc) return e.v; throw e; }
  }
  bindParams(f, args, par, n, muts) {
    const env = new Env(f.env, par);
    const ps = f.params;
    for (let i = 0; i < ps.length; i++) {
      const p = ps[i], a = i < args.length ? args[i] : null;
      if (p.k === 'id') env.vars.set(p.name, { v: a, mut: true, param: true, frozenRef: !!muts && muts[i] === false && (Array.isArray(a) || isPlain(a)) });
      else this.destructure(p, a, env, true, n);
    }
    return env;
  }
  /* функция из нескольких случаев: первый подошедший по шаблонам */
  callClauses(f, args, par, n, muts) {
    let env = null;
    for (const c of f.clauses) {
      if (env === null) env = new Env(f.env, par);
      else if (env.vars.n) env.vars.clear(0);
      let ok = true;
      for (let i = 0; i < c.params.length; i++) if (!this.matchPat(c.params[i], i < args.length ? args[i] : null, env, n)) { ok = false; break; }
      if (!ok) continue;
      for (const b of env.vars.values()) { b.mut = true; b.param = true; }
      if (muts) c.params.forEach((p, i) => { const q = p.k === 'typed' ? p.pat : p; if (q.k === 'id' && muts[i] === false) { const b = env.vars.get(q.name); if (b && (Array.isArray(b.v) || isPlain(b.v))) b.frozenRef = true; } });
      if (c.async) return this.spawn(c.isBlock ? this.gRunBlock(c.body, env) : this.gEv(c.body, env));
      return c.isBlock ? this.runBlock(c.body, env) : this.ev(c.body, env);
    }
    throw this.err('no matching clause in ' + (f.name || 'function') + ' for arguments (' + args.map(repr).join(', ') + ')', n);
  }
  hasLit(p) {
    if (!p) return false;
    if (p.k === 'lit' || p.k === 'cond' || p.k === 'typed') return true;
    if (p.k === 'obj') return p.props.some(q => this.hasLit(q.pat));
    if (p.k === 'arr') return p.items.some(q => this.hasLit(q));
    return false;
  }
  mkFn(s, env) {
    markCap(env);
    const mk = (c, blk) => { const f = new Closure(c.params, c.body, env, blk, s.name); f.async = !!c.async; return f; };
    if (!s.clauses) return mk(s, s.isBlock !== false);
    if (s.clauses.length === 1 && !s.clauses[0].params.some(p => this.hasLit(p))) { const c = s.clauses[0]; return mk(c, c.isBlock); }
    const f = new Closure([], null, env, true, s.name); f.clauses = s.clauses; return f;
  }
  runBlock(stmts, env) { const r = this.exec(stmts, env); return r ? r.ret : this.last; } /* без return значение блока = значение его последнего выражения */

  /* ---- операторы ---- */
  hoist(stmts, env) {
    for (const s of stmts) {
      if (s.type === 'FnDecl') {
        const ex = env.vars.get(s.name);
        if (ex) {
          if (ex.v instanceof Closure && ex.v.clauses && s.clauses) { ex.v.clauses = ex.v.clauses.concat(s.clauses); continue; } // ещё один случай уже объявленной функции
          throw this.err('name "' + s.name + '" is already defined in this scope', s);
        }
        env.vars.set(s.name, { v: this.mkFn(s, env), mut: false });
      } else if (s.type === 'TypeDecl') {
        if (env.vars.has(s.name)) throw this.err('name "' + s.name + '" is already defined in this scope', s);
        env.vars.set(s.name, { v: new TypeDef(s.name, s.fields, env), mut: false });
      }
    }
  }
  exec(stmts, env) {
    if (stmts.hd === undefined) stmts.hd = stmts.some(s => s.type === 'FnDecl' || s.type === 'TypeDecl');
    if (stmts.hd) this.hoist(stmts, env);
    this.last = null;
    const last = stmts.length - 1;
    for (let i = 0; i <= last; i++) { const r = this.execStmt(stmts[i], env, i === last); if (r) return r; }
    return undefined;
  }
  execStmt(s, env, isLast) {
    this.last = null;
    switch (s.type) {
      case 'Return': { const rv = s.value ? this.ev(s.value, env) : null; if (s.prop) throw new RetSig(rv); return { ret: rv }; }
      case 'ExprStmt': {
        const v = this.ev(s.expr, env);
        if (s.expr.type === 'Assign') this.last = v;
        else if (v instanceof Stream && !isLast) for (const _ of v) { /* поток на уровне оператора выполняется до конца; последний оператор блока отдаёт поток как значение */ }
        else this.last = v;
        break;
      }
      case 'Destr': this.evDestr(s, env); break;
    }
    return undefined;
  }
  evDestr(s, env) {
    const v = this.ev(s.value, env);
    this.destructure(s.pat, v, env, s.op === ':=', s);
    return null;
  }
  patKey(q, env, n) {
    if (!q.keyExpr) return q.key;
    const k = this.ev(q.keyExpr, env);
    if (typeof k !== 'string') throw this.err('pattern key must be a string, got ' + typeOf(k), n);
    return k;
  }
  /* распаковка в переменные (мягкая: недостающее = none) */
  destructure(p, v, env, mut, n) {
    if (p.k === 'id') { if (p.name !== '_') env.vars.set(p.name, { v, mut }); return; }
    if (p.k === 'obj') {
      if (!isPlain(v)) throw this.err('cannot destructure ' + typeOf(v) + ' as object: expected object', n);
      for (const q of p.props) {
        const key = this.patKey(q, env, n);
        const val = Object.prototype.hasOwnProperty.call(v, key) ? v[key] : null;
        this.destructure(q.pat.k === 'dyn' ? { k: 'id', name: key } : q.pat, val, env, mut, n);
      }
      return;
    }
    if (p.k === 'arr') {
      if (!Array.isArray(v)) throw this.err('cannot destructure ' + typeOf(v) + ' as array: expected array', n);
      p.items.forEach((q, i) => this.destructure(q, i < v.length ? v[i] : null, env, mut, n));
      if (p.rest) env.vars.set(p.rest, { v: v.slice(p.items.length), mut });
      return;
    }
    throw this.err('unsupported pattern', n);
  }
  /* строгое сопоставление для match */
  matchPat(p, v, env, n) {
    switch (p.k) {
      case 'id': if (p.name !== '_') env.vars.set(p.name, { v, mut: false }); return true;
      case 'typed': {
        const b = env.lookup(p.type);
        let ok;
        if (b && b.v instanceof TypeDef) ok = hasType(v, b.v);
        else if (TYPENAMES.has(p.type)) ok = matchTypeName(p.type, v);
        else throw this.err('unknown type "' + p.type + '" in function header', n);
        return ok && this.matchPat(p.pat, v, env, n);
      }
      case 'lit': return eq(this.ev(p.expr, env), v);
      case 'cond': { const e2 = new Env(env); e2.vars.set('$subject', { v, mut: false }); const r = this.ev(p.expr, e2); if (typeof r !== 'boolean') throw this.err('pattern condition must be bool, got ' + typeOf(r), n); return r; }
      case 'obj':
        if (!isPlain(v)) return false;
        for (const q of p.props) { const key = this.patKey(q, env, n); if (!Object.prototype.hasOwnProperty.call(v, key)) return false; if (!this.matchPat(q.pat.k === 'dyn' ? { k: 'id', name: key } : q.pat, v[key], env, n)) return false; }
        return true;
      case 'arr':
        if (!Array.isArray(v)) return false;
        if (p.rest ? v.length < p.items.length : v.length !== p.items.length) return false;
        for (let i = 0; i < p.items.length; i++) if (!this.matchPat(p.items[i], v[i], env, n)) return false;
        if (p.rest) env.vars.set(p.rest, { v: v.slice(p.items.length), mut: false });
        return true;
    }
    return false;
  }

  findBinding(env, name) {
    let crossed = false;
    for (let e = env; e; e = e.parent) { const b = e.vars.get(name); if (b) return { b, crossed, env: e }; if (e.par) crossed = true; }
    return null;
  }
  checkWritable(f, name, n) {
    if (!f.b.mut) throw this.err('"' + name + '" is a constant (declared with =)', n);
    if (f.crossed) throw this.err('cannot assign to outer variable "' + name + '" inside ::par', n);
  }
  /* менять содержимое массива/объекта: имя изменяемое и значение пришло не как константа */
  checkContent(f, name, n) {
    this.checkWritable(f, name, n);
    if (f.b.frozenRef) throw this.err('"' + name + '" is a constant argument: its contents cannot be modified', n);
  }
  rootIdent(t) { while (t && (t.type === 'Member' || t.type === 'Index')) t = t.obj; return t && t.type === 'Ident' ? t : null; }
  requireMutable(node, env, n) {
    const r = this.rootIdent(node);
    if (!r) throw this.err('cannot modify a value that is not stored in a mutable variable', n);
    const f = this.findBinding(env, r.name);
    if (!f) throw this.err('undefined variable "' + r.name + '"', n);
    this.checkContent(f, r.name, n);
  }

  evAssign(n, env) {
    const t = n.target, op = n.op;
    const val = this.ev(n.value, env);
    if (t.type === 'Ident') {
      const name = t.name;
      this.checkAlias(n, op, val, env);
      if (op === '=') {
        const own = env.vars.get(name);
        if (own) throw this.err(own.mut ? '"' + name + '" is already declared as mutable' : '"' + name + '" is already declared as a constant', n);
        const outer = this.findBinding(env, name);
        if (outer && outer.b.mut) throw this.err('"' + name + '" is a mutable variable of an outer scope', n);
        env.vars.set(name, { v: val, mut: false }); return val;
      }
      const f = this.findBinding(env, name);
      if (op === ':=') {
        if (!f || f.env === this.globals) { env.vars.set(name, { v: val, mut: true }); return val; }   /* встроенные имена можно перекрыть своей переменной */
        this.checkWritable(f, name, n); f.b.v = val; return val;
      }
      if (!f) throw this.err('undefined variable "' + name + '"', n);
      if (Array.isArray(f.b.v)) this.checkContent(f, name, n); else this.checkWritable(f, name, n);
      f.b.v = this.compound(op, f.b.v, val, n);
      return f.b.v;
    }
    if (op === '=') throw this.err('cannot assign to a field or element with =', n);
    const root = this.rootIdent(t);
    if (!root) throw this.err('cannot assign to an expression result', n);
    const f = this.findBinding(env, root.name);
    if (!f) throw this.err('undefined variable "' + root.name + '"', n);
    this.checkContent(f, root.name, n);
    const cont = this.ev(t.obj, env);
    if (t.type === 'Member') {
      if (!isPlain(cont)) throw this.err('cannot assign a field of ' + typeOf(cont) + '', n);
      const cur = Object.prototype.hasOwnProperty.call(cont, t.name) ? cont[t.name] : null;
      cont[t.name] = op === ':=' ? val : this.compound(op, cur, val, n);
      return cont[t.name];
    }
    const idx = this.ev(t.index, env);
    if (Array.isArray(cont)) {
      if (typeof idx !== 'bigint') throw this.err('array index must be int, got ' + typeOf(idx), n);
      const i = Number(idx) < 0 ? cont.length + Number(idx) : Number(idx);
      if (i < 0 || i >= cont.length) throw this.err('index ' + idx + ' out of range for array of length ' + cont.length, n);
      cont[i] = op === ':=' ? val : this.compound(op, cont[i], val, n);
      return cont[i];
    }
    if (isPlain(cont)) {
      const k = String(idx);
      const cur = Object.prototype.hasOwnProperty.call(cont, k) ? cont[k] : null;
      cont[k] = op === ':=' ? val : this.compound(op, cur, val, n);
      return cont[k];
    }
    throw this.err('cannot assign an element of ' + typeOf(cont), n);
  }
  /* b = a, где a — изменяемое имя массива/объекта: ошибка; b := a, где a — константа: тоже ошибка */
  checkAlias(n, op, val, env) {
    if (n.value.type !== 'Ident' || !(Array.isArray(val) || isPlain(val))) return;
    const src = this.findBinding(env, n.value.name);
    if (!src || src.b.param) return;
    if (op === '=' && src.b.mut) throw this.err('cannot alias mutable "' + n.value.name + '" as constant "' + n.target.name + '"', n);
    if (op === ':=' && !src.b.mut) throw this.err('cannot alias constant "' + n.value.name + '" as mutable "' + n.target.name + '"', n);
  }
  compound(op, cur, val, n) {
    if (op === '+=') { if (Array.isArray(cur)) { cur.push(val); return cur; } return this.binop('+', cur, val, n); }
    if (op === '-=') {
      if (Array.isArray(cur)) {
        if (typeof val !== 'bigint' || val < 0n) throw this.err('array -= k: k must be int >= 0', n);
        if (Number(val) > cur.length) throw this.err('cannot remove ' + val + ' elements: array length is ' + cur.length, n);
        cur.length = cur.length - Number(val); return cur;
      }
      return this.binop('-', cur, val, n);
    }
    return this.binop(op[0], cur, val, n);
  }

  /* ---- выражения ---- */
  ev(n, env) {
    const t = n.type;
    if (t === 'Ident') {
      for (let e = env; e; e = e.parent) { const b = e.vars.get(n.name); if (b !== undefined) return b.v; }
      return this.undefVarOrMethod(n.name, env, n);
    }
    if (t === 'Str' || t === 'Bool' || (t === 'Num' && n.txt === undefined)) return n.v;
    switch (t) {
      case 'Num': if (n.txt !== undefined && (DEC.prec > 0 || n.dec) && !(n.v instanceof Dec)) return n.dv || (n.dv = decParse(n.txt)); return n.v;
      case 'Str': case 'Bool': return n.v;
      case 'None': return null;
      case 'Ident': { const b = env.lookup(n.name); if (!b) return this.undefVarOrMethod(n.name, env, n); return b.v; }
      case 'Arr': {
        const out = [];
        for (const x of n.items) {
          if (x.type === 'Spread') { const v = this.ev(x.arg, env); for (const y of this.iterOf(v, x)) out.push(y); } else out.push(this.ev(x, env));
        }
        return out;
      }
      case 'Obj': {
        const o = {};
        for (const p of n.props) {
          if (p.spread) { const v = this.ev(p.value, env); if (!isPlain(v)) throw this.err('spread into object requires an object, got ' + typeOf(v), n); Object.assign(o, v); } else o[p.key] = this.ev(p.value, env);
        }
        return o;
      }
      case 'IStr': return this.interpret(n.v, env, n);
      case 'IFmt': { let o = ''; for (const x of n.parts) o += typeof x === 'string' ? x : str(this.ev(x, env)); return o; }
      case 'Chain': {
        let prev = this.ev(n.terms[0], env);
        for (let i = 0; i < n.ops.length; i++) {
          const cur = this.ev(n.terms[i + 1], env);
          if (!this.cmp(n.ops[i], prev, cur, n)) return false;
          prev = cur;
        }
        return true;
      }
      case 'Un': {
        const a = this.ev(n.arg, env);
        if (n.op === '-') { if (!isNum(a)) throw this.err('unary - requires a number, got ' + typeOf(a), n); return a instanceof Dec ? decNeg(a) : -a; }
        if (typeof a !== 'boolean') throw this.err('not requires bool, got ' + typeOf(a), n);
        return !a;
      }
      case 'Bin': {
        if (n.fold) {
          let x = this.ev(n.fold.base, env);
          if (typeof x === 'bigint') return x + n.fold.total;
          for (let i = 0; i < n.fold.ops.length; i++) x = this.binop(n.fold.ops[i], x, n.fold.lits[i], n);
          return x;
        }
        if (n.op === 'and' || n.op === 'or') {
          const a = this.ev(n.left, env);
          if (typeof a !== 'boolean') throw this.err(n.op + ' requires bool on the left, got ' + typeOf(a), n);
          if (n.op === 'and' ? !a : a) return a;
          const b = this.ev(n.right, env);
          if (typeof b !== 'boolean') throw this.err(n.op + ' requires bool on the right, got ' + typeOf(b), n);
          return b;
        }
        if (n.op === 'is') return this.evIsType(n, env);
        if (n.op === 'in') return this.evIn(this.ev(n.left, env), this.ev(n.right, env), n);
        if (n.op === '??') { const a = this.ev(n.left, env); return (a === null || a instanceof PError) ? this.ev(n.right, env) : a; }
        return this.binop(n.op, this.ev(n.left, env), this.ev(n.right, env), n);
      }
      case 'Lambda': { markCap(env); const c = new Closure(n.params, n.body, env, n.isBlock, null); c.async = !!n.async; return c; }
      case 'Val': return n.v;
      case 'Block': try { return this.runBlock(n.body, new Env(env)); } catch (e) { if (e instanceof RetSig) return e.v; throw e; }
      case 'Await': return this.syncAwait(this.ev(n.arg, env), n);
      case 'Call': { const r = this.evCall(n, env); return r instanceof ArrChain ? Array.from(r) : r; }
      case 'Member': { const o = this.ev(n.obj, env); if (n.opt && o === null) return null; if (n.name === 'len') { if (Array.isArray(o)) return BigInt(o.length); if (typeof o === 'string') return BigInt(sLen(o)); } const r = this.getMember(o, n.name, n, env); if (r instanceof Native) { markCap(env); } return r; }
      case 'Index': return this.evIndex(this.ev(n.obj, env), this.ev(n.index, env), n);
      case 'Slice': return this.evSlice(n, env);
      case 'Match': return this.evMatch(n, env);
      case 'IsExpr': return this.evIs(n, env);
      case 'When': return this.evWhen(n, env);
      case 'Bind': return this.evBind(n, env);
      case 'Ctl': throw new LoopCtl(n.v, n.line);
      case 'Return': throw new RetSig(n.value ? this.ev(n.value, env) : null);
      case 'Foreach': return this.evForeach(n, env);
      case 'As': return this.evAs(n, env);
      case 'Assign': return this.evAssign(n, env);
    }
    throw this.err('unknown node ' + n.type, n);
  }
  /* значение без сворачивания цепочки над массивом (для поэлементного выполнения) */
  evIsType(n, env) {
    const v = this.ev(n.left, env);
    if (n.right.type !== 'Ident') throw this.err('expected a type name after is', n);
    const nm = n.right.name, b = env.lookup(nm);
    if (b && b.v instanceof TypeDef) return hasType(v, b.v);
    if (TYPENAMES.has(nm)) return matchTypeName(nm, v);
    throw this.err('unknown type "' + nm + '"', n);
  }
  evIn(a, b, n) {
    if (Array.isArray(b)) return b.some(x => eq(x, a));
    if (typeof b === 'string') { if (typeof a !== 'string') throw this.err('"x in string" requires a string on the left, got ' + typeOf(a), n); return b.includes(a); }
    if (isPlain(b)) return Object.prototype.hasOwnProperty.call(b, str(a));
    if (b instanceof Stream) {
      if (b.range && isNum(a)) {
        if (typeof a === 'number' && !Number.isInteger(a)) return false;
        const x = BigInt(a), { s, e, st } = b.range;
        if (st > 0n) return x >= s && (e === null || x < e) && (x - s) % st === 0n;
        return x <= s && (e === null || x > e) && (s - x) % (-st) === 0n;
      }
      for (const y of b) if (eq(y, a)) return true;
      return false;
    }
    throw this.err('"in" is not supported for ' + typeOf(b), n);
  }
  evRaw(n, env) { return n.type === 'Call' ? this.evCall(n, env) : this.ev(n, env); }
  evIs(n, env) {
    for (const arm of n.arms) {
      if (arm.cond) {
        const r = this.ev(arm.cond, env);
        if (typeof r !== 'boolean') throw this.err('is condition must be bool, got ' + typeOf(r), arm);
        if (!r) continue;
      }
      return arm.block ? this.runBlock(arm.body, new Env(env)) : this.ev(arm.body, env);
    }
    return null;
  }
  evWhen(n, env) {
    const c = this.ev(n.cond, env);
    if (typeof c !== 'boolean') throw this.err('when condition must be bool, got ' + typeOf(c), n);
    if (!c) return null;
    const f = this.ev(n.fn, env);
    if (!(f instanceof Closure || f instanceof Native)) throw this.err('when requires a callback function on the left, got ' + typeOf(f), n);
    return this.callFn(f, [], false, n);
  }
  evBind(n, env) {
    const f = this.ev(n.callee, env);
    if (!(f instanceof Closure || f instanceof Native)) throw this.err('f:(...) requires a function, got ' + typeOf(f), n);
    const args = n.args.map(a => this.ev(a, env));
    return new Native((f.name || 'call') + ':(…)', extra => {
      if (extra.length) throw this.err('bound call f:(...) takes no arguments, got ' + extra.length, n);
      return this.callFn(f, args, false, n);
    });
  }
  evCall(n, env) {
    let f;
    if (n.callee.type === 'Member') {
      const recv = this.evRaw(n.callee.obj, env);
      if (n.callee.opt && recv === null) return null;
      f = this.getMember(recv, n.callee.name, n.callee, env);
    } else f = this.ev(n.callee, env);
    const na = n.args, args = new Array(na.length);
    let hasObj = false;
    for (let i = 0; i < na.length; i++) { const a = this.ev(na[i], env); args[i] = a; if (typeof a === 'object' && a !== null && (Array.isArray(a) || isPlain(a))) hasObj = true; }
    const muts = hasObj && f instanceof Closure ? na.map(a => this.isMutPath(a, env)) : undefined; /* нужны только для массивов и объектов */
    if (!(f instanceof Closure || f instanceof Native || f instanceof TypeDef)) throw this.err('cannot call ' + this.calleeText(n.callee) + ': it is ' + typeOf(f) + (f === null ? '' : ' ' + repr(f)) + ', not a function' + (f === null ? ' (variable or member is not defined, or holds none)' : ''), n);
    try { return this.callFn(f, args, false, n, muts); }
    catch (e) { if (e instanceof PrismError && e.line == null) e.line = n.line; throw e; }
  }
  calleeText(c) {
    if (c.type === 'Ident') return '"' + c.name + '"';
    if (c.type === 'Member') { const o = c.obj; const t = o.type === 'Ident' ? o.name : o.type === 'Member' ? this.calleeText(o).slice(1, -1) : '…'; return '"' + t + '.' + c.name + '"'; }
    return 'the expression';
  }
  evIndex(o, i, n) {
    if (Array.isArray(o) || typeof o === 'string') {
      if (typeof i !== 'bigint') throw this.err('index must be int, got ' + typeOf(i) + (typeof i === 'number' ? '' : ''), n);
      const L = typeof o === 'string' ? sLen(o) : o.length;
      const k = Number(i) < 0 ? L + Number(i) : Number(i);
      if (k < 0 || k >= L) return null;
      return typeof o === 'string' ? sAt(o, k) : o[k];
    }
    if (isPlain(o)) { const k = String(i); return Object.prototype.hasOwnProperty.call(o, k) ? o[k] : null; }
    throw this.err('cannot index ' + typeOf(o), n);
  }
  /* срез a[от..до]: конец не включается, отрицательные счёт с конца, выход за границы обрезается */
  evSlice(n, env) {
    const o = this.ev(n.obj, env);
    const isS = typeof o === 'string';
    if (!isS && !Array.isArray(o)) throw this.err('slice requires an array or string, got ' + typeOf(o), n);
    const bound = (node, dflt) => {
      if (!node) return dflt;
      const v = this.ev(node, env);
      if (typeof v !== 'bigint') throw this.err('slice bounds must be int, got ' + typeOf(v), n);
      return Number(v);
    };
    const L = isS ? sLen(o) : o.length;
    let a = bound(n.start, 0), b = bound(n.end, L);
    a = a < 0 ? Math.max(0, L + a) : Math.min(a, L);
    b = b < 0 ? Math.max(0, L + b) : Math.min(b, L);
    if (a >= b) return isS ? '' : [];
    return isS ? sSlice(o, a, b) : o.slice(a, b);
  }
  matchArm(arm, subj, env) {
    const aenv = new Env(env);
    aenv.vars.set('$subject', { v: subj, mut: false });
    let ok;
    if (arm.kind === 'else') ok = true;
    else if (arm.kind === 'pat') ok = this.matchPat(arm.pat, subj, aenv, arm);
    else {
      const ex = arm.expr;
      const tb = ex.type === 'Ident' ? env.lookup(ex.name) : null;
      if (tb && tb.v instanceof TypeDef) ok = hasType(subj, tb.v);
      else if (ex.type === 'Ident' && TYPENAMES.has(ex.name)) ok = matchTypeName(ex.name, subj);
      else {
        const r = this.ev(ex, aenv);
        ok = typeof r === 'boolean' ? r : eq(r, subj);
      }
    }
    return ok ? aenv : null;
  }
  evMatch(n, env) {
    const subj = this.ev(n.subject, env);
    for (const arm of n.arms) {
      const aenv = this.matchArm(arm, subj, env);
      if (aenv) return arm.block ? this.runBlock(arm.body, aenv) : this.ev(arm.body, aenv);
    }
    return null;
  }
  iterOf(v, n) {
    if (Array.isArray(v) || v instanceof Stream) return v;
    if (typeof v === 'string') return Array.from(v);
    throw this.err('cannot iterate over ' + typeOf(v), n);
  }
  evForeach(n, env) {
    const raw = this.evRaw(n.src, env);
    const f = this.ev(n.fn, env);
    /* next/stop: LoopCtl из тела; возвращает true, если нужно остановиться */
    const call = (args) => {
      try { this.loopcall = !n.par; const rv = this.callFn(f, args, n.par, n); if (rv instanceof Stream) for (const _ of rv); return false; }
      catch (e) { this.loopcall = false; if (e instanceof LoopCtl) { if (n.par) throw this.err('next/stop cannot be used in ::par', n); return e.ctl === 2; } throw e; }
    };
    if (n.pairs) {
      if (isPlain(raw)) { for (const k of Object.keys(raw)) if (call([k, raw[k]])) break; return null; }
      let i = 0n; for (const x of this.iterOf(raw, n)) if (call([i++, x])) break;
      return null;
    }
    const src = this.iterOf(raw, n);
    if (n.par && src instanceof Stream && src.stateful) throw this.err('::par cannot be used on a stateful stream', n);
    if (f instanceof Closure && !f.clauses && !f.async && f.params.length === 1 && f.params[0].k === 'id') {
      /* простое тело: одно окружение на все итерации, пока его не захватило замыкание */
      const pn = f.params[0].name, body = f.body, blk = f.isBlock, par = n.par;
      let env = null;
      const drain = r => { const lv = r ? r.ret : this.last; if (lv instanceof Stream) for (const _ of lv); };
      const step = x => {
        if (env === null || env.cap) { env = new Env(f.env, par); env.vars.set(pn, { v: x, mut: true, param: true, frozenRef: false }); }
        else { const vm = env.vars; vm.clear(1); const b = vm.v[0]; b.v = x; b.mut = true; b.param = true; b.frozenRef = false; }
        try { if (blk) drain(this.exec(body, env)); else { const rv = this.ev(body, env); if (rv instanceof Stream) for (const _ of rv); } }
        catch (e) { if (e instanceof LoopCtl) { if (par) throw this.err('next/stop cannot be used in ::par', n); return e.ctl === 2; } throw e; }
        return false;
      };
      if (src instanceof Stream && src.range !== undefined && !(src instanceof ArrChain)) { /* range без генератора */
        const { s, e, st } = src.range;
        if (st > 0n) { for (let i = s; e === null || i < e; i += st) if (step(i)) break; } else { for (let i = s; e === null || i > e; i += st) if (step(i)) break; }
      } else for (const x of src) if (step(x)) break;
      return null;
    }
    for (const x of src) if (call([x])) break;
    return null;
  }
  evAs(n, env) {
    let src = this.ev(n.src, env);
    if (src instanceof Stream) src = Array.from(src);
    if (!Array.isArray(src)) throw this.err('"as [...]" requires an array of arrays, got ' + typeOf(src), n);
    return src.map((row, i) => {
      if (!Array.isArray(row)) throw this.err('row ' + (i + 1) + ' must be an array, got ' + typeOf(row), n);
      if (row.length !== n.fields.length) throw this.err('row ' + (i + 1) + ' has ' + row.length + ' values, schema has ' + n.fields.length + ' (' + n.fields.join(', ') + ')', n);
      const o = {}; n.fields.forEach((k, j) => { o[k] = row[j]; }); return o;
    });
  }

  /* ---- арифметика: int = BigInt, float = number ---- */
  num(v, op, n) {
    if (typeof v === 'boolean') return BigInt(v ? 1 : 0);
    if (!isNum(v)) throw this.err('operator ' + op + ' requires a number, got ' + typeOf(v) + (v instanceof PError ? ' (' + v.message + ')' : ''), n);
    return v;
  }
  arith(op, a, b, n) {
    a = this.num(a, op, n); b = this.num(b, op, n);
    if (a instanceof Dec || b instanceof Dec) return decArith(op, a, b);
    if (typeof a === 'bigint' && typeof b === 'bigint') {
      switch (op) {
        case '+': return a + b; case '-': return a - b; case '*': return a * b;
        case '%': return b === 0n ? new PError('division by zero') : ((a % b) + b) % b;
        case '**': return b >= 0n ? a ** b : DEC.prec > 0 ? decArith('**', a, b) : Math.pow(Number(a), Number(b));
      }
    }
    const x = Number(a), y = Number(b);
    switch (op) {
      case '+': return x + y; case '-': return x - y; case '*': return x * y;
      case '%': return y === 0 ? new PError('division by zero') : ((x % y) + y) % y;
      case '**': return Math.pow(x, y);
    }
  }
  add(a, b, n) {
    if (typeof a === 'string' || typeof b === 'string') return str(a) + str(b);
    if ((isNum(a) || typeof a === 'boolean') && (isNum(b) || typeof b === 'boolean')) return this.arith('+', a, b, n);
    if (Array.isArray(a)) return a.concat([b]); // arr + x: новый массив с x в конце
    throw this.err('cannot add ' + typeOf(a) + ' and ' + typeOf(b), n);
  }
  mul(a, b, n) {
    if (isNum(a) && isNum(b)) return this.arith('*', a, b, n);
    let s = null, k = null;
    if ((typeof a === 'string' || Array.isArray(a)) && typeof b === 'bigint') { s = a; k = Number(b); }
    else if ((typeof b === 'string' || Array.isArray(b)) && typeof a === 'bigint') { s = b; k = Number(a); }
    if (s === null) throw this.err('cannot multiply ' + typeOf(a) + ' and ' + typeOf(b), n);
    if (k < 0) throw this.err('repeat count must be int >= 0', n);
    if (typeof s === 'string') return s.repeat(k);
    const out = []; for (let i = 0; i < k; i++) for (const x of s) out.push(deepCopy(x)); return out;
  }
  /* порядок: числа, строки, массивы (по элементам, более короткий префикс меньше); иначе null */
  order(a, b) {
    if (isNum(a) && isNum(b)) {
      if (a instanceof Dec || b instanceof Dec) { const x = toDec(a), y = toDec(b); if (x instanceof Dec && y instanceof Dec) return decCmp(x, y); a = Number(a); b = Number(b); }
      return a < b ? -1 : a > b ? 1 : 0;
    }
    if (typeof a === 'string' && typeof b === 'string') return a < b ? -1 : a > b ? 1 : 0;
    if (Array.isArray(a) && Array.isArray(b)) {
      const m = Math.min(a.length, b.length);
      for (let i = 0; i < m; i++) { const r = this.order(a[i], b[i]); if (r === null || r !== 0) return r; }
      return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    }
    return null;
  }
  cmp(op, a, b, n) {
    const r = this.order(a, b);
    if (r === null) throw this.err('cannot compare ' + typeOf(a) + ' and ' + typeOf(b) + ' with ' + op, n);
    switch (op) { case '<': return r < 0; case '>': return r > 0; case '<=': return r <= 0; default: return r >= 0; }
  }
  binop(op, a, b, n) {
    if (typeof a === 'bigint' && typeof b === 'bigint') {
      switch (op) {
        case '+': return a + b; case '-': return a - b; case '*': return a * b;
        case '<': return a < b; case '>': return a > b; case '<=': return a <= b; case '>=': return a >= b;
        case '==': return a === b; case '!=': return a !== b;
        case '%': if (b !== 0n) { const r = a % b; return r !== 0n && (r < 0n) !== (b < 0n) ? r + b : r; } break;
      }
    } else if (typeof a === 'number' && typeof b === 'number') {
      switch (op) {
        case '+': return a + b; case '-': return a - b; case '*': return a * b;
        case '<': return a < b; case '>': return a > b; case '<=': return a <= b; case '>=': return a >= b;
      }
    }
    switch (op) {
      case '+': return this.add(a, b, n);
      case '*': return this.mul(a, b, n);
      case '-':
        if (typeof a === 'string') { // s - k: новая строка без k последних символов
          if (typeof b !== 'bigint' || b < 0n) throw this.err('string - k: k must be int >= 0' + (typeof b === 'string' ? '' : ''), n);
          const L = sLen(a), k = Number(b);
          if (k > L) throw this.err('cannot remove ' + b + ' characters: string length is ' + L, n);
          return sSlice(a, 0, L - k);
        }
        if (Array.isArray(a)) { // arr - k: новый массив без k последних элементов
          if (typeof b !== 'bigint' || b < 0n) throw this.err('array - k: k must be int >= 0', n);
          if (Number(b) > a.length) throw this.err('cannot remove ' + b + ' elements: array length is ' + a.length, n);
          return a.slice(0, a.length - Number(b));
        }
        return this.arith('-', a, b, n);
      case '/': {
        a = this.num(a, op, n); b = this.num(b, op, n);
        if (a instanceof Dec || b instanceof Dec || DEC.prec > 0) return decArith('/', a, b);
        const x = Number(a), y = Number(b); return y === 0 ? new PError('division by zero') : x / y;
      }
      case '%': return this.arith('%', a, b, n);
      case '**': return this.arith('**', a, b, n);
      case '==': return eq(a, b);
      case '!=': return !eq(a, b);
      case '&': case '|': case '^': case '<<': case '>>': {
        if (typeof a !== 'bigint' || typeof b !== 'bigint') throw this.err('operator ' + op + ' requires int, got ' + typeOf(typeof a !== 'bigint' ? a : b), n);
        if (op === '&') return a & b;
        if (op === '|') return a | b;
        if (op === '^') return a ^ b;
        if (b < 0n) throw this.err('negative shift count', n);
        if (b > 65536n) throw this.err('shift count too large', n);
        return op === '<<' ? a << b : a >> b;
      }
      default: return this.cmp(op, a, b, n);
    }
  }
}
