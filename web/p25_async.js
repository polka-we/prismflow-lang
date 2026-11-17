/* ===== Prism: задачи, time.wait и await ===== */
/* Помечаем узлы, внутри которых (не заходя в вложенные функции) есть time.wait(...) или await: узел.hw = true.
   Функция, в теле которой есть такой узел, становится «асинхронной» (узел.async): вызов запускает задачу и сразу возвращает её. */
const isWaitCall = n => n.type === 'Call' && n.callee.type === 'Member' && n.callee.name === 'wait' && n.callee.obj.type === 'Ident' && n.callee.obj.name === 'time';
function markHw(n) {
  if (Array.isArray(n)) { let h = false; for (const x of n) if (markHw(x)) h = true; return h; }
  if (!n || typeof n !== 'object') return false;
  let h = false;
  for (const k of Object.keys(n)) { if (k === 'line' || k === 'hw' || k === 'async') continue; if (markHw(n[k])) h = true; }
  if (n.params && 'body' in n) { n.async = h; return false; } // функция: граница
  if (n.type === 'FnDecl') return false;
  if (n.type === 'Await' || isWaitCall(n)) h = true;
  if (n.type === 'Foreach' && n.fn && n.fn.type === 'Lambda' && n.fn.async && !n.par) h = true; // тело ::{…} с ожиданием — часть цикла
  if (h) n.hw = true;
  return h;
}

Object.assign(Interp.prototype, {
  now() { return performance.now() + this.warp; },
  spawn(gen) { const t = new Task(gen); this.tasks.push(t); (this.allTasks || (this.allTasks = [])).push(t); this.resume(t); return t; },
  resume(t) {
    t.wake = undefined; t.waitFor = null; t.running = true;
    let r;
    try { r = t.gen.next(); }
    catch (e) { t.running = false; if (e instanceof NeedInput) throw e; t.done = true; t.error = e; return; }
    t.running = false;
    if (r.done) { t.done = true; t.value = r.value === undefined ? null : r.value; return; }
    const y = r.value;
    if (y.sleep !== undefined) t.wake = this.now() + y.sleep; else t.waitFor = y.wait;
  },
  ready(t) { return !t.done && !t.running && (t.waitFor ? t.waitFor.done : (t.wake === undefined || this.now() >= t.wake)); },
  /* крутим задачи, пока pred() не станет истинным или пока не кончатся задачи; deadline - момент, до которого можно ждать */
  loop(pred, deadline) {
    for (;;) {
      if (pred()) return;
      if (deadline !== undefined && this.now() >= deadline) return;
      const live = this.tasks.filter(t => !t.done);
      if (!live.length) { this.tasks = []; if (deadline !== undefined) this.sleepUntil(deadline); return; }
      const rd = live.filter(t => this.ready(t)).sort((a, b) => (a.wake || 0) - (b.wake || 0));
      if (rd.length) { for (const t of rd) { if (this.ready(t)) { this.resume(t); if (pred()) return; } } continue; }
      let w = Infinity;
      for (const t of live) if (t.wake !== undefined && t.wake < w) w = t.wake;
      if (deadline !== undefined && deadline < w) w = deadline;
      if (w === Infinity) throw new PrismError('deadlock: all tasks are waiting for each other');
      this.sleepUntil(w);
    }
  },
  sleepUntil(w) {
    const d = w - this.now();
    if (d <= 0) return;
    const o = this.opts;
    if (o.skip > 0 && this.lines <= o.skip) { this.warp += d; return; } // повторный проход после ввода: уже прошедшее ожидание не повторяем
    if (o.flush) o.flush();
    if (o.onSleep) o.onSleep(d);
    const end = performance.now() + d;
    try { if (typeof SharedArrayBuffer !== 'undefined' && typeof Atomics !== 'undefined') { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, d); return; } } catch (e) { /* нет SharedArrayBuffer */ }
    while (performance.now() < end) { /* ждём */ }
  },
  drain() {
    const m = this.main;
    this.loop(() => m && m.done && m.error);
    if (m && m.error) throw m.error;
    for (const t of this.allTasks || []) if (t.error && !t.awaited) throw t.error;
  },
  runFor(ms) { this.loop(() => false, this.now() + ms); },
  syncAwait(v, n) {
    if (Array.isArray(v) && v.some(x => x instanceof Task)) return v.map(x => this.syncAwait(x, n));
    if (!(v instanceof Task)) return v;
    v.awaited = true;
    this.loop(() => v.done);
    if (!v.done) throw this.err('await: task can never complete', n);
    if (v.error) throw v.error;
    return v.value;
  },

  /* ---- вычисление в режиме генератора: только для узлов с hw ---- */
  *gRunBlock(stmts, env) { const r = yield* this.gExec(stmts, env); return r ? r.ret : this.last; },
  *gExec(stmts, env) {
    this.hoist(stmts, env); this.last = null;
    for (const s of stmts) {
      if (!s.hw) { const r = this.execStmt(s, env); if (r) return r; continue; }
      switch (s.type) {
        case 'Return': return { ret: s.value ? yield* this.gEv(s.value, env) : null };
        case 'ExprStmt': { const v = yield* this.gEv(s.expr, env); this.last = null; if (s.expr.type === 'Assign') this.last = v; else if (v instanceof Stream) for (const _ of v) { /* до конца */ } else this.last = v; break; }
        case 'Destr': { this.last = null; const v = yield* this.gEv(s.value, env); this.destructure(s.pat, v, env, s.op === ':=', s); break; }
      }
    }
    return undefined;
  },
  *gSub(v, env, line) {
    if (Array.isArray(v)) { if (!v.some(x => x && x.hw)) return v; const out = []; for (const x of v) out.push(yield* this.gSub(x, env, line)); return out; }
    if (v && typeof v === 'object' && v.hw) {
      if (v.type === 'Spread') return { type: 'Spread', arg: yield* this.gSub(v.arg, env, line), line };
      if (v.type) return { type: 'Val', v: yield* this.gEv(v, env), line };
      const c = Object.assign({}, v); c.hw = false;
      for (const k of Object.keys(v)) if (k !== 'line') c[k] = yield* this.gSub(v[k], env, line);
      return c;
    }
    return v;
  },
  *gEv(n, env) {
    if (!n.hw) return this.ev(n, env);
    switch (n.type) {
      case 'Call':
        if (isWaitCall(n)) {
          if (n.args.length !== 1) throw this.err('time.wait(ms) takes one number', n);
          const ms = yield* this.gEv(n.args[0], env);
          if (!isNum(ms) || Number(ms) < 0) throw this.err('time.wait(ms): expected number >= 0, got ' + repr(ms), n);
          yield { sleep: Number(ms) };
          return null;
        }
        break;
      case 'Await': {
        const v = yield* this.gEv(n.arg, env);
        return yield* this.gAwait(v, n);
      }
      case 'Bin':
        if (n.fold) {
          let x = yield* this.gEv(n.fold.base, env);
          if (typeof x === 'bigint') return x + n.fold.total;
          for (let i = 0; i < n.fold.ops.length; i++) x = this.binop(n.fold.ops[i], x, n.fold.lits[i], n);
          return x;
        }
        if (n.op === 'and' || n.op === 'or') {
          const a = yield* this.gEv(n.left, env);
          if (typeof a !== 'boolean') throw this.err(n.op + ' requires bool on the left, got ' + typeOf(a), n);
          if (n.op === 'and' ? !a : a) return a;
          const b = yield* this.gEv(n.right, env);
          if (typeof b !== 'boolean') throw this.err(n.op + ' requires bool on the right, got ' + typeOf(b), n);
          return b;
        }
        if (n.op === '??') { const a = yield* this.gEv(n.left, env); return (a === null || a instanceof PError) ? yield* this.gEv(n.right, env) : a; }
        break;
      case 'IsExpr':
        for (const arm of n.arms) {
          if (arm.cond) {
            const r = yield* this.gEv(arm.cond, env);
            if (typeof r !== 'boolean') throw this.err('is condition must be bool, got ' + typeOf(r), arm);
            if (!r) continue;
          }
          return arm.block ? yield* this.gRunBlock(arm.body, new Env(env)) : yield* this.gEv(arm.body, env);
        }
        return null;
      case 'Match': {
        const subj = yield* this.gEv(n.subject, env);
        for (const arm of n.arms) {
          const aenv = this.matchArm(arm, subj, env);
          if (aenv) return arm.block ? yield* this.gRunBlock(arm.body, aenv) : yield* this.gEv(arm.body, aenv);
        }
        return null;
      }
      case 'When': {
        const c = yield* this.gEv(n.cond, env);
        if (typeof c !== 'boolean') throw this.err('when condition must be bool, got ' + typeOf(c), n);
        if (!c) return null;
        const f = yield* this.gEv(n.fn, env);
        if (!(f instanceof Closure || f instanceof Native)) throw this.err('when requires a callback function on the left, got ' + typeOf(f), n);
        return this.callFn(f, [], false, n);
      }
      case 'Foreach': return yield* this.gForeach(n, env);
    }
    const c = Object.assign({}, n); c.hw = false;
    for (const k of Object.keys(n)) if (k !== 'line' && k !== 'type') c[k] = yield* this.gSub(n[k], env, n.line);
    return this.ev(c, env);
  },
  *gAwait(v, n) {
    if (Array.isArray(v) && v.some(x => x instanceof Task)) { const out = []; for (const x of v) out.push(yield* this.gAwait(x, n)); return out; }
    if (!(v instanceof Task)) return v;
    v.awaited = true;
    while (!v.done) yield { wait: v };
    if (v.error) throw v.error;
    return v.value;
  },
  /* src::{…} с ожиданием внутри: тело - часть цикла, итерации идут по очереди (par и именованные функции запускаются как задачи) */
  *gForeach(n, env) {
    const raw = n.src.hw ? yield* this.gEv(n.src, env) : this.evRaw(n.src, env);
    const f = n.fn.hw ? yield* this.gEv(n.fn, env) : this.ev(n.fn, env);
    const inline = f instanceof Closure && f.async && !f.clauses && !n.par;
    const call = function* (self, args) {
      try {
        if (!inline) { self.callFn(f, args, n.par, n); return false; }
        const e = self.bindParams(f, args, false, n);
        if (f.isBlock) yield* self.gRunBlock(f.body, e); else yield* self.gEv(f.body, e);
        return false;
      } catch (e) { if (e instanceof LoopCtl) return e.ctl === 2; throw e; }
    };
    if (n.pairs) {
      if (isPlain(raw)) { for (const k of Object.keys(raw)) if (yield* call(this, [k, raw[k]])) break; return null; }
      let i = 0n; for (const x of this.iterOf(raw, n)) if (yield* call(this, [i++, x])) break;
      return null;
    }
    const src = this.iterOf(raw, n);
    if (n.par && src instanceof Stream && src.stateful) throw this.err('::par cannot be used on a stateful stream', n);
    for (const x of src) if (yield* call(this, [x])) break;
    return null;
  },

  /* ---- модуль time ---- */
  makeTime() {
    const self = this, nat = (name, fn) => new Native('time.' + name, fn);
    const pad = (x, k) => String(x).padStart(k || 2, '0');
    const stamp = (d, utc) => {
      const g = utc ? { y: d.getUTCFullYear(), mo: d.getUTCMonth() + 1, d: d.getUTCDate(), h: d.getUTCHours(), mi: d.getUTCMinutes(), s: d.getUTCSeconds(), ms: d.getUTCMilliseconds() }
        : { y: d.getFullYear(), mo: d.getMonth() + 1, d: d.getDate(), h: d.getHours(), mi: d.getMinutes(), s: d.getSeconds(), ms: d.getMilliseconds() };
      return { year: BigInt(g.y), month: BigInt(g.mo), day: BigInt(g.d), hour: BigInt(g.h), minute: BigInt(g.mi), second: BigInt(g.s), ms: BigInt(g.ms), unix: BigInt(d.getTime()), text: g.y + '-' + pad(g.mo) + '-' + pad(g.d) + ' ' + pad(g.h) + ':' + pad(g.mi) + ':' + pad(g.s) };
    };
    return {
      wait: nat('wait', a => {
        if (a.length !== 1 || !isNum(a[0]) || Number(a[0]) < 0) throw new PrismError('time.wait(ms) takes one number >= 0');
        self.runFor(Number(a[0])); return null;
      }),
      program: nat('program', () => self.now() - self.t0),
      time: nat('time', () => stamp(new Date(Date.now() + self.warp), false)),
      world: nat('world', () => stamp(new Date(Date.now() + self.warp), true)),
      test: nat('test', a => {
        const f = a[0], k = a.length > 1 ? a[1] : 1n;
        if (!(f instanceof Closure || f instanceof Native)) throw new PrismError('time.test(fn, n): first argument must be a function, got ' + typeOf(f));
        if (typeof k !== 'bigint' || k < 1n) throw new PrismError('time.test: n must be int >= 1');
        const times = Number(k); const t0 = self.now();
        for (let i = 0; i < times; i++) { const r = self.callFn(f, [], false, null); if (r instanceof Task) self.syncAwait(r, null); else if (r instanceof Stream) for (const _ of r) { /* до конца */ } }
        return (self.now() - t0) / times;
      })
    };
  }
});

/* выбор синтаксиса: v2 (по умолчанию) или старый v1 (первая строка "// syntax: v1") */
Interp.prototype.lexSrc = function (src, layout) {
  if (this.v1 === undefined) this.v1 = (this.opts.syntax || SYNTAX_DEFAULT) === 'v1' || /^\s*\/\/\s*syntax:\s*v1/.test(src);
  REPR2.on = !this.v1;
  const t = lex(src);
  return this.v1 ? t : toV1(t, { layout });
};
