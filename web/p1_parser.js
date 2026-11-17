'use strict';
/* ===== Prism: общие классы, лексер, парсер ===== */
class PrismError extends Error {
  constructor(msg, line, kind) { super(msg); this.line = line == null ? null : line; this.kind = kind || 'runtime'; this.file = null; }
}
class LoopCtl extends PrismError { constructor(ctl, line) { super('next/stop must be inside a :: loop or a stream stage (select, where, ...)', line); this.ctl = ctl; } }
/* «Did you mean»: как в Python — ближайшее по расстоянию Левенштейна имя, если оно достаточно близко */
function levDist(a, b) {
  if (a.length > 60 || b.length > 60) return 99;
  const row = []; for (let j = 0; j <= b.length; j++) row[j] = j;
  for (let i = 1; i <= a.length; i++) {
    let prev = row[0]; row[0] = i;
    for (let j = 1; j <= b.length; j++) { const tmp = row[j]; row[j] = Math.min(prev + (a[i - 1] === b[j - 1] ? 0 : 1), row[j] + 1, row[j - 1] + 1); prev = tmp; }
  }
  return row[b.length];
}
function suggestName(name, cands) {
  let best = null, bd = 99;
  if (name.length < 3) return null;   /* для коротких имён подсказки только мешают */
  for (const c of cands) {
    if (!c || c[0] === '_' || c[0] === '$' || c === name) continue;
    let d = levDist(name, c); if (name.toLowerCase() === c.toLowerCase()) d = 0;
    if (d > Math.floor((name.length + c.length + 3) / 6)) continue;
    if (best === null || d < bd) { best = c; bd = d; }
  }
  return best;
}
class RetSig { constructor(v) { this.v = v; } }   /* return из ветки is/else: выход из ближайшей функции */
class NeedInput extends Error { constructor() { super('input required'); } }
class PError { constructor(message) { this.message = message; } }
class Stream { constructor(mk, stateful) { this.mk = mk; this.stateful = !!stateful; } [Symbol.iterator]() { return this.mk(); } }
const TYPE = Symbol('type'); /* скрытая метка типа у объекта */
class TypeDef { constructor(name, fields, env) { this.name = name; this.fields = fields; this.env = env; } }
class ArrChain extends Stream {} /* цепочка над массивом: выполняется поэлементно, в конце превращается в массив */
class Closure { constructor(params, body, env, isBlock, name) { this.params = params; this.body = body; this.env = env; this.isBlock = isBlock; this.name = name || null; this.clauses = null; } }
class Native { constructor(name, fn) { this.name = name; this.fn = fn; } }
class Task { constructor(gen) { this.gen = gen; this.done = false; this.value = null; this.error = null; this.wake = undefined; this.waitFor = null; this.running = false; this.awaited = false; } }

const KW = new Set(['fn', 'return', 'match', 'is', 'else', 'as', 'and', 'or', 'not', 'true', 'false', 'none', 'when', 'in', 'await', 'next', 'stop']);
const OPS2 = ['..', ':=', '::', '->', '==', '!=', '<=', '>=', '**', '??', '+=', '-=', '*=', '/=', '?.', '<<', '>>'];
const OPS1 = '+-*/%<>=(){}[],.:@&|^';

function syntaxErr(msg, line) { return new PrismError(msg, line, 'syntax'); }

function lex(src) {
  const toks = []; let i = 0, line = 1, ls = 0; const n = src.length;
  const push = (t, v) => toks.push({ t, v, line, col: i - ls, sp: i > 0 && /\s/.test(src[i - 1]) });
  const idRe = /[\p{L}_][\p{L}\p{N}_]*/uy;
  while (i < n) {
    const c = src[i];
    if (c === '\n') { if (toks.length && toks[toks.length - 1].t !== 'nl') push('nl'); line++; i++; ls = i; continue; }
    if (c === ' ' || c === '\t' || c === '\r') { i++; continue; }
    if (c === '/' && src[i + 1] === '/') { while (i < n && src[i] !== '\n') i++; continue; }
    if (c >= '0' && c <= '9') {
      let j = i; while (j < n && src[j] >= '0' && src[j] <= '9') j++;
      if (src[j] === '.' && src[j + 1] >= '0' && src[j + 1] <= '9') { j++; while (j < n && src[j] >= '0' && src[j] <= '9') j++; }
      { const hm = /_(\d*)(?![\p{L}\p{N}_])/uy; hm.lastIndex = j; const mm = hm.exec(src); /* дырка уровня: 1_, 2_1 */
        if (mm && !/[.]/.test(src.slice(i, j))) { push('id', src.slice(i, j) + mm[0]); i = j + mm[0].length; continue; } }
      const lit = src.slice(i, j);
      if (src[j] === 'd' && !/[\p{L}\p{N}_]/u.test(src[j + 1] || '')) { push('num', decParse(lit)); toks[toks.length - 1].txt = lit; toks[toks.length - 1].dec = true; i = j + 1; continue; }   /* 0.1d, 5d: точное десятичное */
      push('num', /[.]/.test(lit) ? parseFloat(lit) : BigInt(lit));
      if (/[.]/.test(lit)) toks[toks.length - 1].txt = lit;   /* текст дробного литерала нужен, когда включена precision */
      i = j; continue;
    }
    idRe.lastIndex = i;
    const m = idRe.exec(src);
    if (m) { push(KW.has(m[0]) ? 'kw' : 'id', m[0]); i += m[0].length; continue; }
    if (src.startsWith('"""', i)) {
      const end = src.indexOf('"""', i + 3);
      if (end < 0) throw syntaxErr('unterminated multiline string', line);
      let body = src.slice(i + 3, end);
      if (body[0] === '\n') body = body.slice(1);
      push('str', body);
      for (let q = i; q < end + 3; q++) if (src[q] === '\n') { line++; ls = q + 1; }
      i = end + 3; continue;
    }
    if (c === '`') {
      let j = i + 1, s = '';
      for (;;) {
        if (j >= n || src[j] === '\n') throw syntaxErr('unterminated string', line);
        const d = src[j];
        if (d === '`') { j++; break; }
        if (d === '\\') {
          const e = src[j + 1];
          if (e === 'n') s += '\n'; else if (e === 't') s += '\t'; else if (e === '\\') s += '\\'; else if (e === '`') s += '`';
          else if (e === '{' || e === '}') s += '\\' + e;
          else s += '\\' + (e === undefined ? '' : e);
          j += 2; continue;
        }
        s += d; j++;
      }
      push('istr', s); i = j; continue;
    }
    if (c === '"' || c === "'") {
      let j = i + 1, s = '';
      for (;;) {
        if (j >= n || src[j] === '\n') throw syntaxErr('unterminated string', line);
        const d = src[j];
        if (d === c) { j++; break; }
        if (d === '\\') {
          const e = src[j + 1];
          if (e === 'n') s += '\n'; else if (e === 't') s += '\t'; else if (e === '\\') s += '\\';
          else if (e === '"' || e === "'") s += e;
          else if (e === '{' || e === '}') s += '\\' + e;
          else s += '\\' + (e === undefined ? '' : e);
          j += 2; continue;
        }
        s += d; j++;
      }
      push('str', s); i = j; continue;
    }
    if (src.startsWith('...', i)) { push('op', '...'); i += 3; continue; }
    const two = src.substr(i, 2);
    if (OPS2.includes(two)) { push('op', two); i += 2; continue; }
    if (OPS1.includes(c)) {
      push('op', c);
      if (c === ':' && src[i + 1] === '(' && i > 0 && !/\s/.test(src[i - 1])) toks[toks.length - 1].adj = true; /* f:(...) без пробела */
      i++; continue;
    }
    throw syntaxErr('unexpected character "' + c + '"', line);
  }
  if (toks.length && toks[toks.length - 1].t !== 'nl') push('nl');
  push('eof');
  return toks;
}

const ASSIGN_OPS = new Set(['=', ':=', '+=', '-=', '*=', '/=']);
const IMPLICIT_OPS = new Set(['%', '+', '*', '/', '**', '==', '!=', '<', '>', '<=', '>=']);
const CMP = new Set(['<', '>', '<=', '>=']);
const BIN_PREC = { 'is': 4, 'in': 4, 'or': 1, 'and': 2, '??': 3, '==': 4, '!=': 4, '<': 4, '>': 4, '<=': 4, '>=': 4, '|': 5, '^': 6, '&': 7, '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10, '**': 11 };

class Parser {
  constructor(toks) { this.toks = toks; this.pos = 0; this.noArrow = false; this.slots = []; this.sidN = 0; }
  peek(k) { return this.toks[Math.min(this.pos + (k || 0), this.toks.length - 1)]; }
  next() { return this.toks[this.pos++]; }
  isOp(v, k) { const t = this.peek(k); return t.t === 'op' && t.v === v; }
  isKw(v, k) { const t = this.peek(k); return t.t === 'kw' && t.v === v; }
  skipNl() { while (this.peek().t === 'nl') this.pos++; }
  nlCount() { let k = 0; while (this.peek(k).t === 'nl') k++; return k; }
  desc(t) { return t.t === 'eof' ? 'end of file' : t.t === 'nl' ? 'end of line' : '"' + (t.t === 'str' ? '"' + t.v + '"' : t.v) + '"'; }
  fail(msg, t) { throw syntaxErr(msg, (t || this.peek()).line); }
  expectOp(v) {
    if (!this.isOp(v)) {
      // { и } создаёт сам отступ: пользователь их не пишет, поэтому в сообщении про них не говорим
      if (v === '{') this.fail('expected indented block, got ' + this.desc(this.peek()));
      this.fail('expected "' + v + '", got ' + this.desc(this.peek()));
    }
    return this.next();
  }
  expectId() { const t = this.peek(); if (t.t !== 'id') this.fail('expected identifier, got ' + this.desc(t)); return this.next().v; }
  withArrows(f) { const s = this.noArrow; this.noArrow = false; try { return f(); } finally { this.noArrow = s; } }
  findClose(p) {
    let d = 0;
    for (let k = p; k < this.toks.length; k++) {
      const t = this.toks[k];
      if (t.t === 'op') {
        if (t.v === '(' || t.v === '[' || t.v === '{') d++;
        else if (t.v === ')' || t.v === ']' || t.v === '}') { d--; if (d === 0) return k; }
      }
    }
    return -1;
  }
  arrowAfterGroup() {
    if (this.noArrow) return false;
    const c = this.findClose(this.pos);
    if (c < 0) return false;
    let k = c + 1; while (this.toks[k] && this.toks[k].t === 'nl') k++;
    const t = this.toks[k];
    return !!t && t.t === 'op' && t.v === '->';
  }

  parseProgram() { const s = this.parseStatements(false); return s; }
  parseExprOnly() {
    this.skipNl(); const e = this.parseExpr(); this.skipNl();
    if (this.peek().t !== 'eof') this.fail('unexpected token: ' + this.desc(this.peek()));
    return e;
  }
  parseStatements(untilBrace) {
    const out = [];
    for (;;) {
      this.skipNl();
      const t = this.peek();
      if (t.t === 'eof') { if (untilBrace) this.fail('unclosed "{"'); break; }
      if (this.isOp('}')) { if (untilBrace) break; this.fail('unmatched "}"'); }
      const st = this.parseStatement();
      if (this.isOp(',') && st.type === 'ExprStmt' && (st.expr.type === 'Ident' || st.expr.type === 'Member' || st.expr.type === 'Index')) {
        /* a, b := b, a — все правые части вычисляются до присваивания (через скрытые временные имена) */
        const ts = [st.expr], vs = [], ml = st.line, okT = x => x.type === 'Ident' || x.type === 'Member' || x.type === 'Index';
        while (this.isOp(',')) {
          this.next(); this.skipNl();
          if (ts.length >= 16) this.fail('multiple assignment: at most 16 targets');
          const t2 = this.parseUnary();
          if (!okT(t2)) this.fail('multiple assignment: target must be a name, element or field');
          ts.push(t2);
        }
        const aop = this.peek();
        if (!(aop.t === 'op' && (aop.v === '=' || aop.v === ':='))) this.fail('multiple assignment: expected = or := after the target list, got ' + this.desc(aop));
        const mop = aop.v; this.next(); this.skipNl();
        for (;;) {
          if (vs.length >= 16) this.fail('multiple assignment: at most 16 values');
          vs.push(this.parseExpr());
          if (this.isOp(',')) { this.next(); this.skipNl(); continue; }
          break;
        }
        if (vs.length !== ts.length) this.fail('multiple assignment: ' + ts.length + ' targets but ' + vs.length + ' values');
        const mid = (Parser.mcount = (Parser.mcount || 0) + 1);
        ts.forEach((_, k) => out.push({ type: 'ExprStmt', expr: { type: 'Assign', target: { type: 'Ident', name: '$m' + mid + '_' + k, line: ml }, op: ':=', value: vs[k], line: ml }, line: ml }));
        ts.forEach((tg, k) => out.push({ type: 'ExprStmt', expr: { type: 'Assign', target: tg, op: mop, value: { type: 'Ident', name: '$m' + mid + '_' + k, line: ml }, line: ml }, line: ml }));
        const e2 = this.peek();
        if (e2.t === 'nl' || e2.t === 'eof' || this.isOp('}')) continue;
        this.fail('unexpected ' + this.desc(e2) + ' after expression');
      }
      const pv = out[out.length - 1];
      if (st.type === 'FnDecl' && st.head && pv && pv.type === 'FnDecl' && pv.head && pv.name === st.name) pv.clauses.push(...st.clauses);
      else if (pv && pv.type === 'FnDecl' && pv.head && st.type === 'ExprStmt' && st.expr.type === 'Assign' && st.expr.op === '=' && st.expr.target.type === 'Ident' && st.expr.target.name === pv.name) {
        /* «имя = _ * …» после частных случаев — ещё один случай той же функции */
        const lam = st.expr.value; pv.clauses.push(lam.type === 'Lambda' ? { params: lam.params, body: lam.body, isBlock: lam.isBlock } : { params: [], body: lam, isBlock: false });
      } else out.push(st);
      const e = this.peek(), prev = this.toks[this.pos - 1];
      if (e.t === 'nl' || e.t === 'eof' || this.isOp('}') || (prev && prev.t === 'op' && prev.v === '}')) continue;
      this.fail('unexpected ' + this.desc(e) + ' after expression');
    }
    return out;
  }
  /* тело fn: { ... } или -> выражение (значение возвращается сразу) */
  parseFnBody() {
    this.skipNl();
    if (this.isOp('->')) {
      this.next(); this.skipNl();
      if (this.isOp('{')) return { body: this.parseBlock(), isBlock: true };
      return { body: this.withArrows(() => this.parseExpr()), isBlock: false };
    }
    return { body: this.parseBlock(), isBlock: true };
  }
  parseBlock() { this.expectOp('{'); const s = this.parseStatements(true); this.expectOp('}'); return s; }

  /* type Point {x, y}  /  type User {name, age = 18} */
  parseTypeDecl() {
    const t = this.next(), name = this.next().v; this.next(); this.skipNl();
    const fields = [];
    while (!this.isOp('}')) {
      const f = this.peek(); if (f.t !== 'id') this.fail('type: expected field name, got ' + this.desc(f));
      this.next(); let def = null;
      if (this.isOp('=')) { this.next(); this.skipNl(); def = this.withArrows(() => this.parseExpr()); }
      fields.push({ name: f.v, def });
      let sep = false;
      while (this.isOp(',') || this.peek().t === 'nl') { this.next(); sep = true; }
      if (!sep && !this.isOp('}')) this.fail('expected "," or "}", got ' + this.desc(this.peek()));
    }
    this.expectOp('}');
    return { type: 'TypeDecl', name, fields, line: t.line };
  }
  /* заголовок функции: имя a b = выражение | имя(a, b) = выражение | имя a { блок } | имя(a, b) { блок } */
  isHeadTok(t) { return t.t === 'id' || t.t === 'num' || t.t === 'str' || (t.t === 'kw' && (t.v === 'true' || t.v === 'false' || t.v === 'none')); }
  headMode() {
    const t = this.peek(); if (t.t !== 'id') return null;
    const n1 = this.peek(1);
    if (n1.t === 'op' && n1.v === '(') {
      const c = this.findClose(this.pos + 1), a = this.toks[c + 1];
      return c > 0 && a && a.t === 'op' && (a.v === '=' || a.v === '{') ? 'paren' : null;
    }
    if (n1.t === 'op' && n1.v === '{' && n1.gen) return 'bare';
    if (n1.t === 'op' && IMPLICIT_OPS.has(n1.v)) {
      /* имя < 0 = …  — случай по условию на аргумент */
      let d = 0;
      for (let k = 1; ; k++) {
        const x = this.peek(k);
        if (x.t === 'nl' || x.t === 'eof') return null;
        if (x.t !== 'op') continue;
        if (x.v === '(' || x.v === '[' || (x.v === '{' && !(d === 0 && x.gen))) d++;
        else if (x.v === ')' || x.v === ']' || x.v === '}') d--;
        else if (d === 0 && (x.v === '=' || (x.v === '{' && x.gen))) return 'bare';
      }
    }
    const isGrp = tk => tk.t === 'op' && tk.v === '[' && tk.sp;
    if (this.isHeadTok(n1) || isGrp(n1)) {
      let k = 1;
      for (;;) {
        const x = this.peek(k);
        if (this.isHeadTok(x)) k++;
        else if (isGrp(x)) { const c = this.findClose(this.pos + k); if (c <= 0) return null; k = c - this.pos + 1; }
        else break;
      }
      const a = this.peek(k);
      return a.t === 'op' && (a.v === '=' || a.v === '{') ? 'bare' : null;
    }
    return null;
  }
  headParam(inParens) {
    const t = this.peek();
    if (t.t === 'id' && inParens) { /* Point p  /  Rect {w, h} : параметр с типом */
      const nx = this.peek(1);
      if (nx.t === 'id') { this.next(); this.next(); return { k: 'typed', type: t.v, pat: { k: 'id', name: nx.v } }; }
      if (nx.t === 'op' && (nx.v === '{' || nx.v === '[')) { this.next(); return { k: 'typed', type: t.v, pat: this.parsePat(true) }; }
    }
    if (t.t === 'id') { this.next(); return { k: 'id', name: t.v }; }
    if (t.t === 'num') { this.next(); return { k: 'lit', expr: { type: 'Num', v: t.v, txt: t.txt, dec: t.dec, line: t.line } }; }
    if (t.t === 'str') { this.next(); return { k: 'lit', expr: { type: 'Str', v: t.v, line: t.line } }; }
    if (t.t === 'kw' && (t.v === 'true' || t.v === 'false')) { this.next(); return { k: 'lit', expr: { type: 'Bool', v: t.v === 'true', line: t.line } }; }
    if (t.t === 'kw' && t.v === 'none') { this.next(); return { k: 'lit', expr: { type: 'None', line: t.line } }; }
    if (t.t === 'op' && t.v === '-' && this.peek(1).t === 'num') { this.next(); const x = this.next(); return { k: 'lit', expr: { type: 'Num', v: x.v instanceof Dec ? decNeg(x.v) : -x.v, txt: x.txt === undefined ? undefined : '-' + x.txt, dec: x.dec, line: t.line } }; }
    if (t.t === 'op' && (t.v === '{' || t.v === '[')) return this.parsePat(true);
    if (t.t === 'op' && IMPLICIT_OPS.has(t.v)) { const s = this.noArrow; this.noArrow = true; try { return { k: 'cond', expr: this.parseBinaryFrom({ type: 'Ident', name: '$subject', line: t.line }, 0) }; } finally { this.noArrow = s; } }
    this.fail('function header: expected parameter name, literal or pattern, got ' + this.desc(t));
  }
  parseHead(mode) {
    const t = this.next(), params = [];
    if (mode === 'paren') {
      this.next(); this.skipNl();
      while (!this.isOp(')')) {
        params.push(this.headParam(true)); this.skipNl();
        if (this.isOp(',')) { this.next(); this.skipNl(); } else if (!this.isOp(')')) this.fail('expected "," or ")", got ' + this.desc(this.peek()));
      }
      this.expectOp(')');
    } else while (!this.isOp('=') && !this.isOp('{')) params.push(this.headParam(false));
    let body, isBlock;
    if (this.isOp('{')) {
      const zero = mode === 'bare' && !params.length; /* «имя =» с телом на следующих строках */
      const col = []; col.kind = 'd'; if (zero) this.slots.push(col);
      try { body = this.parseBlock(); } finally { if (zero) this.slots.pop(); }
      if (zero && col.length) { const lam = this.mkLambda(col, body, true, t.line); return { type: 'FnDecl', name: t.v, head: true, clauses: [{ params: lam.params, body, isBlock: true }], line: t.line }; }
      if (zero) return { type: 'ExprStmt', expr: { type: 'Assign', target: { type: 'Ident', name: t.v, line: t.line }, op: '=', value: { type: 'Block', body, line: t.line }, line: t.line }, line: t.line };
      isBlock = true;
    } else {
      this.next(); this.skipNl();
      const col = []; col.kind = "d"; this.slots.push(col);
      try { body = this.withArrows(() => this.parseExpr()); } finally { this.slots.pop(); }
      if (col.length) body = this.mkLambda(col, body, false, body.line);
      isBlock = false;
    }
    return { type: 'FnDecl', name: t.v, head: true, clauses: [{ params, body, isBlock }], line: t.line };
  }
  /* параметры лямбды из «дырок»: _ (=_1), _1, _2..., затем старые @имена */
  mkLambda(col, body, isBlock, line) {
    const HR = /^_(\d+)(\$\d+)?$/;
    const us = col.filter(n => HR.test(n)).map(n => +HR.exec(n)[1]), names = col.filter(n => !HR.test(n));
    const params = [];
    const suf = col.sid ? '$' + col.sid : '';
    if (col.sid && col.nodes) for (const nd of col.nodes) if (!nd.name.includes('$')) nd.name += suf;
    if (us.length) for (let i = 1; i <= Math.max(...us); i++) params.push({ k: 'id', name: '_' + i + suf });
    for (const n of names) params.push({ k: 'id', name: n });
    return { type: 'Lambda', params, body, isBlock, line };
  }

  parseStatement() {
    const t = this.peek();
    if (t.t === 'id' && t.v === 'type' && this.peek(1).t === 'id' && this.isOp('{', 2)) return this.parseTypeDecl();
    { const hm = this.headMode(); if (hm) return this.parseHead(hm); }
    if (t.t === 'kw' && t.v === 'fn' && this.peek(1).t === 'id') {
      this.next(); const name = this.next().v; this.expectOp('(');
      const params = this.parseParamList(')');
      const fb = this.parseFnBody();
      return { type: 'FnDecl', name, params, body: fb.body, isBlock: fb.isBlock, line: t.line };
    }
    if (t.t === 'kw' && t.v === 'return') {
      this.next(); const e = this.peek();
      const prop = (this.arm || 0) > 0;
      if (e.t === 'nl' || e.t === 'eof' || this.isOp('}')) return { type: 'Return', value: null, prop, line: t.line };
      return { type: 'Return', value: this.parseExpr(), prop, line: t.line };
    }
    if (t.t === 'op' && (t.v === '{' || t.v === '[')) {
      const c = this.findClose(this.pos), a = this.toks[c + 1];
      if (c >= 0 && a && a.t === 'op' && (a.v === '=' || a.v === ':=')) {
        const pat = this.parsePat(false);
        const op = this.next().v;
        const value = this.parseExpr();
        return { type: 'Destr', pat, op, value, line: t.line };
      }
    }
    if (t.t === 'kw' && t.v === 'is') return { type: 'ExprStmt', expr: this.parseIsExpr(true), line: t.line };
    const e = this.parseExpr();
    return { type: 'ExprStmt', expr: e, line: t.line };
  }

  /* --- выражения --- */
  parseExpr() {
    const line = this.peek().line;
    let left = this.parseUnary();
    const ak = this.nlCount(), at = this.peek(ak);
    if (at.t === 'op' && ASSIGN_OPS.has(at.v) && (left.type === 'Ident' || left.type === 'Member' || left.type === 'Index')) {
      this.pos += ak; const op = this.next().v; this.skipNl();
      let value;
      if (left.type === 'Ident' && (op === '=' || op === ':=')) { /* x = _ + 1: дырки в правой части делают функцию */
        const col = []; col.kind = "d"; this.slots.push(col);
        try { value = this.parseExpr(); } finally { this.slots.pop(); }
        const up = this.slots[this.slots.length - 1];
        if (col.length && up && value.type === 'Ident' && /^_\d+$/.test(value.name)) { for (const nm of col) if (!up.includes(nm)) up.push(nm); } // k = _ : называет параметр охватывающей функции
        else if (col.length) value = this.mkLambda(col, value, false, value.line);
      } else value = this.parseExpr();
      return { type: 'Assign', target: left, op, value, line };
    }
    left = this.parseBinaryFrom(left, 0);
    for (;;) {
      const k = this.nlCount(), t = this.peek(k);
      if (t.t === 'kw' && t.v === 'match') { this.pos += k; left = this.parseMatch(left); } else break;
    }
    if (this.isKw('when')) {
      const w = this.next(); this.skipNl();
      const cond = this.withArrows(() => this.parseBinary(0));
      left = { type: 'When', fn: left, cond, line: w.line };
    }
    return left;
  }
  parseBinary(min) { return this.parseBinaryFrom(this.parseUnary(), min); }
  parseBinaryFrom(left, min) {
    for (;;) {
      const k = this.nlCount(), t = this.peek(k);
      let op = null;
      if (t.t === 'op' && BIN_PREC[t.v] !== undefined) op = t.v;
      else if (t.t === 'kw' && (t.v === 'and' || t.v === 'or' || t.v === 'in' || t.v === 'is')) op = t.v;
      if (op === null) break;
      if (k > 0 && (op === '-' || op === 'is')) break;
      const p = BIN_PREC[op];
      if (p < min) break;
      this.pos += k; this.next(); this.skipNl();
      const right = op === '**' ? this.parseBinary(p) : this.parseBinary(p + 1);
      if (CMP.has(op) && left.type === 'Bin' && CMP.has(left.op) && !left.paren) left = { type: 'Chain', terms: [left.left, left.right, right], ops: [left.op, op], line: t.line };
      else if (CMP.has(op) && left.type === 'Chain' && !left.paren) { left.terms.push(right); left.ops.push(op); }
      else {
        const node = { type: 'Bin', op, left, right, line: t.line };
        const lit = x => x.type === 'Num' && typeof x.v === 'bigint';
        if ((op === '+' || op === '-') && lit(right)) {
          // цепочка «x ± 1 ± 2 …» с целыми литералами считается одной операцией
          let base = null, lits = null, ops = null;
          if (left.type === 'Bin' && left.fold) { base = left.fold.base; lits = left.fold.lits.concat([right.v]); ops = left.fold.ops.concat([op]); }
          else if (left.type === 'Bin' && (left.op === '+' || left.op === '-') && lit(left.right)) { base = left.left; lits = [left.right.v, right.v]; ops = [left.op, op]; }
          if (base) { let total = 0n; for (let i = 0; i < lits.length; i++) total += ops[i] === '+' ? lits[i] : -lits[i]; node.fold = { base, lits, ops, total }; }
        }
        left = node;
      }
    }
    return left;
  }
  parseUnary() {
    const t = this.peek();
    if (t.t === 'kw' && t.v === 'is') return this.parseIsExpr(false);
    if (t.t === 'op' && t.v === '-') { this.next(); this.skipNl(); return { type: 'Un', op: '-', arg: this.parseBinary(11), line: t.line }; }
    if (t.t === 'kw' && t.v === 'await') { this.next(); return { type: 'Await', arg: this.parseUnary(), line: t.line }; }
    if (t.t === 'kw' && t.v === 'not') { this.next(); this.skipNl(); return { type: 'Un', op: 'not', arg: this.parseBinary(4), line: t.line }; }
    if (t.t === 'op' && t.v === '@') {
      this.next(); const n = this.peek();
      if (n.t !== 'id') this.fail('expected parameter name after @');
      this.next();
      const slot = this.slots[this.slots.length - 1];
      if (!slot) this.fail('@' + n.v + ' is only valid inside a method argument or a :: body', n);
      if (!slot.includes(n.v)) slot.push(n.v);
      return this.parsePostfix({ type: 'Ident', name: n.v, line: n.line });
    }
    return this.parsePostfix(this.parsePrimary());
  }
  parsePostfix(e, noRange, noForeach) {
    for (;;) {
      const t = this.peek();
      if (t.t === 'op') {
        if (t.v === '(') {
          this.next();
          const args = this.withArrows(() => {
            const a = []; this.skipNl();
            const isMethod = e.type === 'Member';
            while (!this.isOp(')')) {
              if (isMethod) {
                const col = []; col.kind = 'm'; this.slots.push(col); let x;
                try { x = this.parseExpr(); } finally { this.slots.pop(); }
                if (col.length) x = this.mkLambda(col, x, false, x.line);
                a.push(x);
              } else if (this.slots.some(c => c.kind === 'm' || c.kind === 'd')) a.push(this.parseExpr()); // внутри аргумента метода или определения x = … дырки уходят наружу
              else {
                /* обычный вызов вне аргумента метода: twice(_ * 2, 5) — аргумент сам функция; голый _ (add(1, _)) — вся запись */
                const col = []; col.kind = 'p'; this.slots.push(col); let x;
                try { x = this.parseExpr(); } finally { this.slots.pop(); }
                if (col.length) {
                  if (x.type === 'Ident' && /^_\d+$/.test(x.name)) {
                    const up = this.slots[this.slots.length - 1];
                    if (!up) this.fail(x.name === '_1' ? '_ is only valid inside a call argument, a :: body or the right side of x = ...' : x.name + ' has no enclosing function', t);
                    for (const nm of col) if (!up.includes(nm)) up.push(nm);
                  } else x = this.mkLambda(col, x, false, x.line);
                }
                a.push(x);
              }
              this.skipNl();
              if (this.isOp(',')) { this.next(); this.skipNl(); } else if (!this.isOp(')')) this.fail('expected "," or ")", got ' + this.desc(this.peek()));
            }
            return a;
          });
          this.expectOp(')');
          e = { type: 'Call', callee: e, args, line: t.line }; continue;
        }
        if (t.v === ':' && t.adj) {
          this.next(); this.next();
          const args = this.withArrows(() => {
            const a = []; this.skipNl();
            while (!this.isOp(')')) {
              a.push(this.parseExpr()); this.skipNl();
              if (this.isOp(',')) { this.next(); this.skipNl(); } else if (!this.isOp(')')) this.fail('expected "," or ")", got ' + this.desc(this.peek()));
            }
            return a;
          });
          this.expectOp(')');
          e = { type: 'Bind', callee: e, args, line: t.line }; continue;
        }
        if (t.v === '[') {
          this.next();
          const r = this.withArrows(() => {
            this.skipNl();
            let a = null, slice = false, b = null;
            if (!this.isOp('..')) a = this.parseExpr();
            if (this.isOp('..')) { this.next(); slice = true; if (!this.isOp(']') && this.peek().t !== 'nl') b = this.parseExpr(); }
            this.skipNl();
            return { a, slice, b };
          });
          this.expectOp(']');
          e = r.slice ? { type: 'Slice', obj: e, start: r.a, end: r.b, line: t.line } : { type: 'Index', obj: e, index: r.a, line: t.line };
          continue;
        }
        if (t.v === '.') {
          this.next(); this.skipNl(); const name = this.expectId();
          e = { type: 'Member', obj: e, name, line: t.line }; continue;
        }
        if (t.v === '?.') {
          this.next(); this.skipNl(); const name = this.expectId();
          e = { type: 'Member', obj: e, name, opt: true, line: t.line }; continue;
        }
        if (t.v === '::' && !noForeach) {
          this.next(); this.skipNl();
          let par = false, names = null, pairs = false;
          if (this.peek().t === 'id' && this.peek().v === 'par') {
            let k = 1; while (this.peek(k).t === 'nl') k++;
            if (this.isOp('{', k)) { this.pos += k; par = true; }
            else if (this.namesAhead(1) >= 0) { this.next(); par = true; }
          }
          let fn;
          if (this.namesAhead(0) >= 0) {
            names = [];
            while (!this.isOp('{')) { names.push(this.next().v); if (this.isOp(',')) this.next(); }
            if (names.length > 2) this.fail('at most two names allowed after ::', t);
            this.next(); this.skipNl();
            const body = this.withArrows(() => { const b = this.parseStatements(true); this.expectOp('}'); return b; });
            fn = { type: 'Lambda', params: names.map(n => ({ k: 'id', name: n })), body, isBlock: true, line: t.line };
            pairs = names.length === 2;
          } else if (this.isOp('{')) {
            this.next(); this.skipNl();
            fn = this.withArrows(() => this.parseBraceLambda());
          } else {
            const save = this.pos; const ptmp = []; ptmp.kind = 'm'; this.slots.push(ptmp); let probe; try { probe = this.parsePostfix(this.parsePrimary()); } finally { this.slots.pop(); } const at = this.peek();
            if (at.t === 'op' && ASSIGN_OPS.has(at.v) && (probe.type === 'Ident' || probe.type === 'Member' || probe.type === 'Index')) {
              /* xs:: total += _ — тело-присваивание в одну строку */
              this.pos = save;
              const col = []; col.kind = "m"; this.slots.push(col); let ex;
              try { ex = this.parseExpr(); } finally { this.slots.pop(); }
              fn = this.mkLambda(col, [{ type: 'ExprStmt', expr: ex, line: t.line }], true, t.line);
            } else fn = probe;
          }
          e = { type: 'Foreach', src: e, fn, par, pairs, line: t.line }; continue;
        }
      } else if (t.t === 'nl') {
        const k = this.nlCount(), c = this.peek(k);
        if (this.isOp('.', k) || this.isOp('::', k) || (c.t === 'kw' && c.v === 'as' && this.isOp('{', k + 1))) { this.pos += k; continue; }
      } else if (t.t === 'kw' && t.v === 'as' && this.isOp('{', 1)) {
        this.next(); this.next(); this.skipNl();
        const fields = [];
        while (!this.isOp('}')) {
          fields.push(this.expectId()); this.skipNl();
          if (this.isOp(',')) { this.next(); this.skipNl(); }
        }
        this.expectOp('}');
        e = { type: 'As', src: e, fields, line: t.line }; continue;
      }
      break;
    }
    return e;
  }
  namesAhead(k) {
    let i = k;
    for (;;) {
      if (this.peek(i).t !== 'id') return -1;
      i++;
      if (this.isOp(',', i)) { i++; continue; }
      break;
    }
    return this.isOp('{', i) ? i : -1;
  }
  parsePrimary() {
    const t = this.peek();
    if (t.t === 'num') { this.next(); return { type: 'Num', v: t.v, txt: t.txt, dec: t.dec, line: t.line }; }
    if (t.t === 'str') { this.next(); return { type: 'Str', v: t.v, line: t.line }; }
    if (t.t === 'istr') {
      this.next();
      /* дырка `_` внутри {…} относится к охватывающей функции: такие строки разбираем сразу, остальные — при выполнении */
      if (this.slots.length && /\{(?:[^}]*[^A-Za-z0-9_}])?\d*_\d*(?:[^A-Za-z0-9_}][^}]*)?\}/.test(t.v.replace(/\\[{}]/g, ''))) {
        const parts = []; const s = t.v; let i = 0, lit = '';
        while (i < s.length) {
          const c = s[i];
          if (c === '\\' && (s[i + 1] === '{' || s[i + 1] === '}')) { lit += s[i + 1]; i += 2; continue; }
          if (c === '{') {
            let d = 1, j = i + 1; while (j < s.length && d > 0) { if (s[j] === '{') d++; else if (s[j] === '}') d--; j++; }
            if (d !== 0) this.fail('unclosed "{" in interpolation', t);
            if (lit) { parts.push(lit); lit = ''; }
            const sub = new Parser(toV1(lex(s.slice(i + 1, j - 1)), { layout: false }));
            sub.slots = this.slots; sub.sidN = this.sidN;
            parts.push(sub.parseExprOnly()); this.sidN = sub.sidN; i = j; continue;
          }
          lit += c; i++;
        }
        if (lit) parts.push(lit);
        return { type: 'IFmt', parts, line: t.line };
      }
      return { type: 'IStr', v: t.v, line: t.line };
    }
    if (t.t === 'kw') {
      if (t.v === 'true' || t.v === 'false') { this.next(); return { type: 'Bool', v: t.v === 'true', line: t.line }; }
      if (t.v === 'none') { this.next(); return { type: 'None', line: t.line }; }
      if (t.v === 'return' && (this.arm || 0) > 0) {
        this.next(); const e = this.peek();
        if (e.t === 'nl' || e.t === 'eof' || this.isOp('}') || (e.t === 'kw' && (e.v === 'is' || e.v === 'else'))) return { type: 'Return', value: null, prop: true, line: t.line };
        return { type: 'Return', value: this.parseExpr(), prop: true, line: t.line };
      }
      if (t.v === 'next' || t.v === 'stop') { this.next(); return { type: 'Ctl', v: t.v === 'stop' ? 2 : 1, line: t.line }; }
      if (t.v === 'fn') {
        this.next(); this.expectOp('(');
        const params = this.parseParamList(')');
        const fb = this.parseFnBody();
        return { type: 'Lambda', params, body: fb.body, isBlock: fb.isBlock, line: t.line };
      }
      this.fail('unexpected token "' + t.v + '"');
    }
    if (t.t === 'id') {
      let k = 1; while (this.peek(k).t === 'nl') k++;
      if (!this.noArrow && this.isOp('->', k)) return this.parseLambdaHere();
      const lv = /^(\d+)_(\d*)$/.exec(t.v);
      if (lv) { /* дырка уровня: 1_ — самая внешняя неявная функция, 2_ — следующая, N_k — её k-й параметр */
        this.next();
        const L = +lv[1], k = lv[2] === '' ? 1 : +lv[2];
        const cnt = this.slots.filter(c => c.kind === 'm');
        if (L < 1 || L > cnt.length || k < 1) this.fail(t.v + ': no enclosing function at level ' + L, t);
        const tgt = cnt[L - 1], top = this.slots[this.slots.length - 1];
        let nm = '_' + k;
        if (tgt !== top) { if (!tgt.sid) tgt.sid = ++this.sidN; nm += '$' + tgt.sid; }
        if (!tgt.includes(nm)) tgt.push(nm);
        if (nm.includes('$') && tgt.nodes) { /* уже есть */ }
        return { type: 'Ident', name: nm, line: t.line };
      }
      if (t.v === '_' || /^_\d+$/.test(t.v)) { /* дырка: параметр безымянной функции */
        this.next();
        const slot = this.slots[this.slots.length - 1];
        if (!slot) this.fail(t.v + ' is only valid inside a call argument, a :: body or the right side of x = ...', t);
        const nm = t.v === '_' ? '_1' : t.v;
        if (!slot.includes(nm)) slot.push(nm);
        const nd = { type: 'Ident', name: nm, line: t.line };
        (slot.nodes || (slot.nodes = [])).push(nd);
        return nd;
      }
      this.next(); return { type: 'Ident', name: t.v, line: t.line };
    }
    if (t.t === 'op') {
      if (t.v === '(') {
        if (this.arrowAfterGroup()) return this.parseLambdaHere();
        this.next();
        const e = this.withArrows(() => { this.skipNl(); const x = this.parseExpr(); this.skipNl(); return x; });
        this.expectOp(')'); e.paren = true; return e;
      }
      if (t.v === '[') {
        if (this.arrowAfterGroup()) return this.parseLambdaHere();
        this.next();
        const items = this.withArrows(() => {
          const a = []; this.skipNl();
          while (!this.isOp(']')) {
            if (this.isOp('...')) { const st = this.next(); a.push({ type: 'Spread', arg: this.parseExpr(), line: st.line }); } else a.push(this.parseExpr());
            let sep = false;
            while (this.isOp(',') || this.peek().t === 'nl') { this.next(); sep = true; }
            if (!sep && !this.isOp(']')) this.fail('expected "," or "]", got ' + this.desc(this.peek()));
          }
          return a;
        });
        this.expectOp(']');
        return { type: 'Arr', items, line: t.line };
      }
      if (t.v === '{') {
        if (this.arrowAfterGroup()) return this.parseLambdaHere();
        this.next();
        const props = this.withArrows(() => {
          const a = []; this.skipNl();
          while (!this.isOp('}')) {
            const k = this.peek(); let key;
            if (this.isOp('...')) {
              this.next(); a.push({ spread: true, value: this.parseExpr() });
              let sep2 = false;
              while (this.isOp(',') || this.peek().t === 'nl') { this.next(); sep2 = true; }
              if (!sep2 && !this.isOp('}')) this.fail('expected "," or "}", got ' + this.desc(this.peek()));
              continue;
            }
            if (k.t === 'id' || k.t === 'str') { key = k.v; this.next(); } else this.fail('expected field name, got ' + this.desc(k));
            let value;
            if (this.isOp(':')) { this.next(); this.skipNl(); value = this.parseExpr(); }
            else if (k.t === 'id') value = { type: 'Ident', name: key, line: k.line };
            else this.fail('expected ":" after string key');
            a.push({ key, value });
            let sep = false;
            while (this.isOp(',') || this.peek().t === 'nl') { this.next(); sep = true; }
            if (!sep && !this.isOp('}')) this.fail('expected "," or "}", got ' + this.desc(this.peek()));
          }
          return a;
        });
        this.expectOp('}');
        return { type: 'Obj', props, line: t.line };
      }
    }
    this.fail('unexpected ' + this.desc(t));
  }

  /* --- лямбды и шаблоны --- */
  parseLambdaHead() {
    const t = this.peek(); let params;
    if (t.t === 'id') { this.next(); params = [{ k: 'id', name: t.v }]; }
    else if (t.t === 'op' && t.v === '(') { this.next(); params = this.parseParamList(')'); }
    else if (t.t === 'op' && (t.v === '{' || t.v === '[')) { params = [this.parsePat(false)]; }
    else this.fail('expected function (x -> ...), got ' + this.desc(t));
    this.skipNl(); this.expectOp('->'); this.skipNl();
    return params;
  }
  /* тело после "::{": скобки и есть блок функции; внутри любое число операторов */
  hasLambdaHead() {
    const t = this.peek();
    if (t.t === 'id') { let k = 1; while (this.peek(k).t === 'nl') k++; return this.isOp('->', k); }
    if (t.t === 'op' && (t.v === '(' || t.v === '{' || t.v === '[')) return this.arrowAfterGroup();
    return false;
  }
  parseBraceLambda() {
    const line = this.peek().line;
    if (!this.hasLambdaHead()) { /* ::{ ... @i ... }: параметры берутся из @-имён */
      const col = []; col.kind = "m"; this.slots.push(col); let body;
      try { body = this.parseStatements(true); } finally { this.slots.pop(); }
      this.expectOp('}');
      return this.mkLambda(col, body, true, line);
    }
    const params = this.parseLambdaHead();
    if (this.isOp('{')) { /* старая запись ::{i -> {...}}: внутренний блок занимает всё тело */
      const c = this.findClose(this.pos); let k = c + 1;
      while (this.toks[k] && this.toks[k].t === 'nl') k++;
      const a = this.toks[k];
      if (c >= 0 && a && a.t === 'op' && a.v === '}') {
        const body = this.parseBlock(); this.skipNl(); this.expectOp('}');
        return { type: 'Lambda', params, body, isBlock: true, line };
      }
    }
    const body = this.parseStatements(true); this.expectOp('}');
    return { type: 'Lambda', params, body, isBlock: true, line };
  }
  parseLambdaHere() {
    const t = this.peek();
    const params = this.parseLambdaHead();
    let body, isBlock = false;
    if (this.isOp('{')) { body = this.parseBlock(); isBlock = true; }
    else body = this.withArrows(() => this.parseExpr());
    return { type: 'Lambda', params, body, isBlock, line: t.line };
  }
  parseParamList(close) {
    const ps = []; this.skipNl();
    while (!this.isOp(close)) {
      ps.push(this.parsePat(false)); this.skipNl();
      if (this.isOp(',')) { this.next(); this.skipNl(); } else if (!this.isOp(close)) this.fail('expected "," or "' + close + '", got ' + this.desc(this.peek()));
    }
    this.expectOp(close);
    return ps;
  }
  /* mm=true: шаблон для match (литералы, условия); false: распаковка */
  parsePat(mm) {
    const t = this.peek();
    if (t.t === 'op' && t.v === '{') {
      this.next(); this.skipNl(); const props = [];
      while (!this.isOp('}')) {
        const k = this.peek(); if (k.t !== 'id' && k.t !== 'str') this.fail('expected field name, got ' + this.desc(k));
        if (k.t === 'str' && (this.isOp('.', 1) || this.isOp('(', 1))) {
          /* вычисляемый ключ: {"l{k}".interpret(), name} = obj; имя переменной = значение ключа */
          const keyExpr = this.withArrows(() => this.parsePostfix(this.parsePrimary()));
          let pat = { k: 'dyn' };
          if (this.isOp(':')) { this.next(); this.skipNl(); pat = this.parsePatValue(mm); }
          props.push({ keyExpr, pat });
          this.skipNl(); if (this.isOp(',')) { this.next(); this.skipNl(); }
          continue;
        }
        this.next(); let pat;
        if (this.isOp(':')) { this.next(); this.skipNl(); pat = this.parsePatValue(mm); }
        else pat = { k: 'id', name: k.v };
        props.push({ key: k.v, pat });
        this.skipNl(); if (this.isOp(',')) { this.next(); this.skipNl(); }
      }
      this.expectOp('}'); return { k: 'obj', props };
    }
    if (t.t === 'op' && t.v === '[') {
      this.next(); this.skipNl(); const items = []; let rest = null;
      while (!this.isOp(']')) {
        if (this.isOp('...')) { this.next(); rest = this.expectId(); }
        else items.push(this.parsePatValue(mm));
        this.skipNl(); if (this.isOp(',')) { this.next(); this.skipNl(); }
      }
      this.expectOp(']'); return { k: 'arr', items, rest };
    }
    if (t.t === 'id') { this.next(); return { k: 'id', name: t.v }; }
    this.fail('expected name or pattern, got ' + this.desc(t));
  }
  parsePatValue(mm) {
    const t = this.peek();
    if (t.t === 'op' && (t.v === '{' || t.v === '[')) return this.parsePat(mm);
    if (!mm) return this.parsePat(false);
    if (t.t === 'op' && IMPLICIT_OPS.has(t.v)) {
      const left = { type: 'Ident', name: '$subject', line: t.line };
      return { k: 'cond', expr: this.parseBinaryFrom(left, 0) };
    }
    if (t.t === 'id' && !this.isOp('(', 1) && !this.isOp('.', 1)) {
      const n = this.peek(1);
      if (n.t === 'nl' || (n.t === 'op' && (n.v === ',' || n.v === '}' || n.v === ']' || n.v === '->'))) { this.next(); return { k: 'id', name: t.v }; }
    }
    const s = this.noArrow; this.noArrow = true;
    try { return { k: 'lit', expr: this.parseBinary(0) }; } finally { this.noArrow = s; }
  }

  /* is-выражение: is условие -> значение / is ... / else -> значение */
  parseIsExpr(allowNoElse) {
    const it = this.peek(), arms = []; let hasElse = false;
    for (;;) {
      const t = this.next(); // is | else
      let cond = null;
      if (t.v === 'is') {
        const s = this.noArrow; this.noArrow = true;
        try { cond = this.parseBinary(0); } finally { this.noArrow = s; }
      } else hasElse = true;
      this.skipNl();
      if (!this.isOp('->')) this.fail('expected "->" after is condition, got ' + this.desc(this.peek()));
      this.next(); this.skipNl();
      const arm = { cond, line: t.line };
      this.arm = (this.arm || 0) + 1;
      try {
        if (this.isOp('{')) { arm.body = this.parseBlock(); arm.block = true; }
        else arm.body = this.withArrows(() => this.parseExpr());
      } finally { this.arm--; }
      arms.push(arm);
      if (hasElse) break;
      const k = this.nlCount(), c = this.peek(k);
      if (c.t === 'kw' && (c.v === 'is' || c.v === 'else')) { this.pos += k; continue; }
      break;
    }
    if (!hasElse && !allowNoElse) this.fail('is expression requires an else branch', it);
    return { type: 'IsExpr', arms, line: it.line };
  }

  parseMatch(subject) {
    const mt = this.next(); // match
    this.skipNl();
    if (!this.isOp('{')) this.fail('match: expected indented branches (is ... / else ...), got ' + this.desc(this.peek()));
    this.next(); this.skipNl();
    const arms = []; let hasElse = false;
    while (!this.isOp('}')) {
      const t = this.peek(); let arm;
      if (t.t === 'kw' && t.v === 'else') { this.next(); arm = { kind: 'else' }; hasElse = true; }
      else if (t.t === 'kw' && t.v === 'is') {
        this.next(); this.skipNl(); const c = this.peek();
        const s = this.noArrow; this.noArrow = true;
        try {
          if (c.t === 'op' && (c.v === '{' || c.v === '[')) arm = { kind: 'pat', pat: this.parsePat(true) };
          else if (c.t === 'op' && IMPLICIT_OPS.has(c.v)) arm = { kind: 'cond', expr: this.parseBinaryFrom({ type: 'Ident', name: '$subject', line: c.line }, 0) };
          else arm = { kind: 'cond', expr: this.parseBinary(0) };
        } finally { this.noArrow = s; }
      } else this.fail('match: expected "is ..." or "else", got ' + this.desc(t));
      arm.line = t.line;
      this.skipNl(); this.expectOp('->'); this.skipNl();
      this.arm = (this.arm || 0) + 1;
      try {
        if (this.isOp('{')) { arm.body = this.parseBlock(); arm.block = true; }
        else if (this.isKw('return')) { arm.body = [this.parseStatement()]; arm.block = true; } /* is … -> return x в одну строку */
        else arm.body = this.parseExpr();
      } finally { this.arm--; }
      arms.push(arm);
      const e = this.peek();
      if (!(e.t === 'nl' || this.isOp('}') || this.isKw('is') || this.isKw('else'))) this.fail('match: expected end of line after branch, got ' + this.desc(e));
      this.skipNl();
    }
    this.expectOp('}');
    if (!hasElse) this.fail('match requires an else branch', mt);
    return { type: 'Match', subject, arms, line: mt.line };
  }
}
