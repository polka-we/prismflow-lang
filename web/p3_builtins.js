/* ===== Prism: встроенные функции и методы ===== */
const nat = (name, fn) => new Native(name, fn);
const parseIntStr = s => { const t = s.trim(); return /^[+-]?\d+$/.test(t) ? BigInt(t) : new PError('"' + s + '" is not a valid int'); };
const parseFloatStr = s => { const t = s.trim(); if (!/^[+-]?(\d+\.?\d*|\.\d+)$/.test(t)) return new PError('"' + s + '" is not a valid float'); return DEC.prec > 0 ? decParse(t) : parseFloat(t); };
const ARR_METHODS = ['all', 'any', 'avg', 'chunk', 'count', 'cycle', 'enumerate', 'expand', 'expand_bfs', 'expand_bfs_once', 'expand_once', 'filter', 'find', 'first', 'flat', 'flat_map', 'group_by', 'map', 'max_by', 'min_by', 'par', 'partition', 'reduce', 'repeat', 'scan', 'skip', 'skip_while', 'sort', 'sum', 'take', 'take_while', 'to_list', 'unique', 'window', 'zip', 'contains', 'copy', 'dcopy', 'index_of', 'insert', 'join', 'last', 'len', 'max', 'min', 'pop', 'remove', 'reverse'];
const STR_METHODS = ['chars', 'contains', 'ends_with', 'first', 'index_of', 'insert', 'interpret', 'json', 'last', 'len', 'lines', 'lower', 'pad_left', 'pad_right', 'remove', 'repeat', 'replace', 'reverse', 'skip', 'split', 'starts_with', 'take', 'to_float', 'to_int', 'to_number', 'trim', 'upper'];
const NUM_METHODS = ['to_string', 'to_int', 'to_float'];
const METHOD_NAMES = new Set([...ARR_METHODS, ...STR_METHODS, ...NUM_METHODS]);
const ARR_ONLY = new Set(['copy', 'dcopy', 'len', 'last', 'reverse', 'contains', 'index_of', 'join', 'min', 'max', 'pop', 'remove', 'insert']);
const LAZY = new Set(['map', 'filter', 'take', 'skip', 'take_while', 'skip_while', 'zip', 'flat', 'enumerate']);

Object.assign(Interp.prototype, {
  setupGlobals() {
    const g = this.globals, self = this;
    const def = (name, fn) => g.vars.set(name, { v: nat(name, fn), mut: false });
    def('print', a => {
      const k = ++self.lines;
      if (k > self.maxLines) throw new PrismError('output limit exceeded (' + self.maxLines + ' lines)');
      if (k <= (self.opts.skip || 0)) return null; // повторный запуск после ввода: уже показанное пропускаем
      self.opts.print(a.map(str).join(' ')); return null;
    });
    self.mkRange = (s, e, st) => {
      const stream = new Stream(function* () { if (st > 0n) for (let i = s; e === null || i < e; i += st) yield i; else for (let i = s; e === null || i > e; i += st) yield i; });
      stream.range = { s, e, st };
      return stream;
    };
    def('range', a => {
      const nums = a.map(x => { if (typeof x !== 'bigint') throw new PrismError('range: expected int, got ' + typeOf(x) + (typeof x === 'number' ? '' : '')); return x; });
      let s = 0n, e, st = 1n;
      if (nums.length === 1) e = nums[0]; else if (nums.length === 2) { s = nums[0]; e = nums[1]; } else if (nums.length === 3) { s = nums[0]; e = nums[1]; st = nums[2]; }
      else throw new PrismError('range takes 1 to 3 arguments');
      if (st === 0n) throw new PrismError('range: step cannot be 0');
      return self.mkRange(s, e, st);
    });
    def('len', a => { const v = a[0]; if (typeof v === 'string') return BigInt(sLen(v)); if (Array.isArray(v)) return BigInt(v.length); if (v instanceof Stream) return BigInt(Array.from(v).length); if (isPlain(v)) return BigInt(Object.keys(v).length); throw new PrismError('len: not supported for ' + typeOf(v)); });
    def('str', a => str(a[0]));
    def('type', a => typeOf(a[0]));
    def('error', a => new PError(a.length ? str(a[0]) : 'error'));
    def('from', a => self.loadModule(a[0], null));
    const m1 = (name, fi, ff, fd) => def(name, a => {
      const v = a[0];
      if (v instanceof Dec) { if (fd) return fd(v); return ff(Number(v)); }
      if (fd && DEC.prec > 0 && (typeof v === 'bigint' || typeof v === 'number') && name === 'sqrt') return fd(toDec(v));
      if (typeof v === 'bigint') return fi(v);
      if (typeof v === 'number') return ff(v);
      throw new PrismError(name + ': expected number, got ' + typeOf(v));
    });
    const toI = x => { if (!Number.isFinite(x)) throw new PrismError('cannot convert ' + fmtFloat(x) + ' to int'); return BigInt(x); };
    m1('abs', v => (v < 0n ? -v : v), Math.abs, decAbs);
    m1('sqrt', v => Math.sqrt(Number(v)), Math.sqrt, decSqrt);
    m1('floor', v => v, x => toI(Math.floor(x)), d => decInt(d, 'floor'));
    m1('ceil', v => v, x => toI(Math.ceil(x)), d => decInt(d, 'ceil'));
    m1('round', v => v, x => toI(Math.round(x)), d => decInt(d, 'round'));
    { const r0 = g.vars.get('round').v.fn;
      g.vars.get('round').v = nat('round', a => {
        if (a.length < 2) return r0(a);
        if (a[0] instanceof Dec) { if (typeof a[1] !== 'bigint' || a[1] < 0n || a[1] > 1000n) throw new PrismError('round(x, n): n must be an int in 0..1000'); return decRoundDigits(a[0], Number(a[1])); }
        if (!isNum(a[0]) || typeof a[1] !== 'bigint' || a[1] < 0n || a[1] > 15n) throw new PrismError('round(x, n): x must be a number and n an int in 0..15');
        if (typeof a[0] === 'bigint') return a[0];
        const p = Math.pow(10, Number(a[1])), x = a[0];
        return Number.isFinite(x) ? Math.sign(x) * Math.floor(Math.abs(x) * p + 0.5) / p : x;
      }); }
    const mf = (name, f) => def(name, a => { if (a.length !== 1 || !isNum(a[0])) throw new PrismError(name + ': expected one number, got ' + (a.length === 1 ? typeOf(a[0]) : a.length + ' arguments')); return f(Number(a[0])); });
    mf('sin', Math.sin); mf('cos', Math.cos); mf('tan', Math.tan); mf('atan', Math.atan); mf('exp', Math.exp); mf('log2', Math.log2); mf('log10', Math.log10);
    def('log', a => { if (!a.length || a.length > 2 || !a.every(isNum)) throw new PrismError('log(x) or log(x, base) requires numbers'); return a.length === 1 ? Math.log(Number(a[0])) : Math.log(Number(a[0])) / Math.log(Number(a[1])); });
    def('atan2', a => { if (a.length !== 2 || !a.every(isNum)) throw new PrismError('atan2(y, x) requires two numbers'); return Math.atan2(Number(a[0]), Number(a[1])); });
    g.vars.set('pi', { v: Math.PI, mut: false });
    def('pow', a => {
      if ((a.length !== 2 && a.length !== 3) || !a.every(isNum)) throw new PrismError('pow(a, b) or pow(a, b, mod) requires numbers');
      if (a.length === 3) {
        if (!a.every(x => typeof x === 'bigint')) throw new PrismError('pow(a, b, mod) requires ints');
        let [b, e, m] = a; if (m === 0n) return new PError('division by zero'); if (e < 0n) throw new PrismError('pow(a, b, mod): b must be >= 0');
        let r = 1n; b = ((b % m) + m) % m;
        while (e > 0n) { if (e & 1n) r = (r * b) % m; e >>= 1n; b = (b * b) % m; }
        return ((r % m) + m) % m;
      }
      return self.arith('**', a[0], a[1], null);
    });
    def('gcd', a => {
      if (a.length !== 2 || !a.every(x => typeof x === 'bigint')) throw new PrismError('gcd(a, b) requires two ints');
      let [x, y] = a; if (x < 0n) x = -x; if (y < 0n) y = -y;
      while (y !== 0n) [x, y] = [y, x % y];
      return x;
    });
    def('divmod', a => {
      if (a.length !== 2 || !a.every(x => typeof x === 'bigint')) throw new PrismError('divmod(a, b) requires two ints');
      const [x, y] = a; if (y === 0n) return new PError('division by zero');
      let q = x / y; if ((x % y !== 0n) && ((x < 0n) !== (y < 0n))) q -= 1n;
      return [q, x - q * y];
    });
    def('sum', a => { const xs = a.length === 1 && (Array.isArray(a[0]) || a[0] instanceof Stream) ? a[0] : null; if (!xs) throw new PrismError('sum(xs) requires an array or stream, got ' + (a.length === 1 ? typeOf(a[0]) : a.length + ' arguments')); let s = 0n; for (const x of xs) s = self.add(s, x, null); return s; });
    def('ord', a => { if (typeof a[0] !== 'string' || sLen(a[0]) !== 1) throw new PrismError('ord(c) requires a one-character string'); return BigInt(a[0].codePointAt(0)); });
    def('chr', a => { if (typeof a[0] !== 'bigint' || a[0] < 0n || a[0] > 0x10FFFFn || (a[0] >= 0xD800n && a[0] <= 0xDFFFn)) throw new PrismError('chr(n) requires a valid code point'); return String.fromCodePoint(Number(a[0])); });
    const mm = (name, sign) => def(name, a => {
      const xs = a.length === 1 && (Array.isArray(a[0]) || a[0] instanceof Stream) ? Array.from(a[0]) : a;
      if (!xs.length) return null;
      if (xs.some(x => !isNum(x))) throw new PrismError(name + ': expected number, got ' + typeOf(xs.find(x => !isNum(x))));
      return xs.reduce((m, x) => (sign * self.order(x, m) > 0 ? x : m));
    });
    mm('min', -1); mm('max', 1);
    def('div', a => {
      if (a.length !== 2 || !isNum(a[0]) || !isNum(a[1])) throw new PrismError('div(a, b) requires two numbers');
      const [x, y] = a;
      if (typeof x === 'bigint' && typeof y === 'bigint') { if (y === 0n) return new PError('division by zero'); let q = x / y; if ((x % y !== 0n) && ((x < 0n) !== (y < 0n))) q -= 1n; return q; }
      if (Number(y) === 0) return new PError('division by zero');
      return toI(Math.floor(Number(x) / Number(y)));
    });
    def('int', a => {
      const v = a[0];
      if (typeof v === 'bigint') return v;
      if (v instanceof Dec) return decInt(v, 'trunc');
      if (typeof v === 'number') return Number.isFinite(v) ? BigInt(Math.trunc(v)) : new PError('cannot convert ' + fmtFloat(v) + ' to int');
      if (typeof v === 'string') {
        if (a.length > 1) {
          const b = a[1]; if (typeof b !== 'bigint' || b < 2n || b > 36n) throw new PrismError('int(s, base): base must be int in 2..36');
          const t = v.trim().toLowerCase(), m = /^([+-]?)([0-9a-z]+)$/.exec(t), base = Number(b);
          const ok = m && [...m[2]].every(ch => parseInt(ch, 36) < base);
          if (!ok) return new PError('"' + v + '" is not a valid int');
          let r = 0n; for (const ch of m[2]) r = r * b + BigInt(parseInt(ch, 36));
          return m[1] === '-' ? -r : r;
        }
        return parseIntStr(v);
      }
      if (typeof v === 'boolean') return v ? 1n : 0n;
      throw new PrismError('int(): cannot convert ' + typeOf(v));
    });
    def('float', a => {
      const v = a[0];
      if (DEC.prec > 0) { /* при включённой precision float — десятичное число */
        if (isNum(v)) return toDec(v);
        if (typeof v === 'string') { const d = decParse(v); return d === null ? parseFloatStr(v) : d; }
      }
      if (isNum(v)) return Number(v);
      if (typeof v === 'string') return parseFloatStr(v);
      throw new PrismError('float(): cannot convert ' + typeOf(v));
    });
    def('dec', a => {   /* dec(x) или dec(x, n): точное десятичное число, при n — округление до n знаков */
      if (!a.length || a.length > 2) throw new PrismError('dec(x) or dec(x, digits) takes one or two arguments');
      if (typeof a[0] !== 'string' && !isNum(a[0]) && typeof a[0] !== 'boolean') throw new PrismError('dec(): cannot convert ' + typeOf(a[0]));
      let d = typeof a[0] === 'string' ? decParse(a[0]) : toDec(a[0]);
      if (d === null) return new PError('"' + a[0] + '" is not a valid number');
      if (!(d instanceof Dec)) return d;
      if (a.length === 2) { if (typeof a[1] !== 'bigint' || a[1] < 0n || a[1] > 1000n) throw new PrismError('dec(x, digits): digits must be an int in 0..1000'); d = decRoundDigits(d, Number(a[1])); }
      return d;
    });
    def('precision', a => {   /* precision(n): n знаков после запятой для всех дробных чисел; 0 — обычный double; precision() — текущее значение */
      if (!a.length) return BigInt(DEC.prec);
      if (a.length !== 1 || typeof a[0] !== 'bigint' || a[0] < 0n || a[0] > 1000n) throw new PrismError('precision(n): n must be an int in 0..1000');
      DEC.prec = Number(a[0]); return null;
    });
    let seed = (self.opts.seed || 0x9e3779b9) >>> 0;
    def('random', () => { seed = (seed + 0x6D2B79F5) >>> 0; let t = seed; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; });
    /* есть ли ещё строка ввода; если её нет и ввод не закрыт (eof:false) - программа ждёт пользователя */
    const more = () => { if (self.stdinPos < self.stdin.length) return true; if (self.opts.eof === false) throw new NeedInput(); return false; };
    g.vars.set('time', { v: self.makeTime(), mut: false });

    /* ---- JSON, file, http: доступ даёт хост (opts.fs / opts.http), по умолчанию выключено; ошибки — значениями error(...) ---- */
    const jsonParse = text => {
      let i = 0; const n = text.length; let bad = null;
      const fail = m => { if (!bad) bad = 'invalid JSON at position ' + i + ': ' + m; return null; };
      const ws = () => { while (i < n && ' \n\t\r'.includes(text[i])) i++; };
      const val = depth => {
        ws(); if (bad) return null;
        if (depth > 200) return fail('nesting too deep');
        if (i >= n) return fail('unexpected end');
        const c = text[i];
        if (c === '{') {
          const o = {}; i++; ws(); if (text[i] === '}') { i++; return o; }
          for (;;) {
            ws(); if (text[i] !== '"') return fail('expected string key');
            const k = str1(); if (bad) return null; ws();
            if (text[i] !== ':') return fail("expected ':'"); i++;
            const v = val(depth + 1); if (bad) return null; o[k] = v; ws();
            if (text[i] === ',') { i++; continue; } if (text[i] === '}') { i++; return o; }
            return fail("expected ',' or '}'");
          }
        }
        if (c === '[') {
          const a = []; i++; ws(); if (text[i] === ']') { i++; return a; }
          for (;;) {
            const v = val(depth + 1); if (bad) return null; a.push(v); ws();
            if (text[i] === ',') { i++; continue; } if (text[i] === ']') { i++; return a; }
            return fail("expected ',' or ']'");
          }
        }
        if (c === '"') return str1();
        if (text.startsWith('true', i)) { i += 4; return true; }
        if (text.startsWith('false', i)) { i += 5; return false; }
        if (text.startsWith('null', i)) { i += 4; return null; }
        if (c === '-' || (c >= '0' && c <= '9')) {
          const m = /^-?\d+(\.\d+)?([eE][+-]?\d+)?/.exec(text.slice(i, i + 400));
          if (!m || /^-?\d+\.(?!\d)/.test(m[0])) return fail('bad number');
          const lit = m[0], isf = m[1] !== undefined || m[2] !== undefined; i += lit.length;
          if (!isf) return BigInt(lit);
          if (DEC.prec > 0) return decParse(lit);
          return parseFloat(lit);
        }
        return fail('unexpected character');
      };
      const str1 = () => {
        let out = ''; i++;
        for (;;) {
          if (i >= n) return fail('unterminated string');
          const c = text[i++];
          if (c === '"') return out;
          if (c < ' ') { i--; return fail('control character in string'); }
          if (c !== '\\') { out += c; continue; }
          if (i >= n) return fail('bad escape');
          const e = text[i++];
          if ('"\\/'.includes(e)) out += e;
          else if (e === 'b') out += '\b'; else if (e === 'f') out += '\f'; else if (e === 'n') out += '\n'; else if (e === 'r') out += '\r'; else if (e === 't') out += '\t';
          else if (e === 'u') { const h = text.slice(i, i + 4); if (!/^[0-9a-fA-F]{4}$/.test(h)) return fail('bad \\u escape'); i += 4; out += String.fromCharCode(parseInt(h, 16)); }
          else { i--; return fail('bad escape'); }
        }
      };
      const v = val(0); if (!bad) { ws(); if (i < n) fail('extra data after value'); }
      return bad ? new PError(bad) : v;
    };
    self.jsonParse = jsonParse;
    const jsonStr = (v, depth) => {
      if (depth > 200) throw new PError('json_str: nesting too deep (cyclic data?)');
      if (v === null) return 'null'; if (typeof v === 'boolean') return v ? 'true' : 'false';
      if (typeof v === 'bigint') return v.toString(); if (v instanceof Dec) return decStr(v);
      if (typeof v === 'number') { if (!Number.isFinite(v)) throw new PError('json_str: cannot encode inf/nan'); return fmtFloat(v); }
      if (typeof v === 'string') return JSON.stringify(v);
      if (Array.isArray(v)) return '[' + v.map(x => jsonStr(x, depth + 1)).join(',') + ']';
      if (isPlain(v)) return '{' + Object.keys(v).map(k => JSON.stringify(k) + ':' + jsonStr(v[k], depth + 1)).join(',') + '}';
      throw new PError('json_str: cannot encode ' + typeOf(v));
    };
    def('json_str', a => {
      if (a.length !== 1) throw new PrismError('json_str(x) takes one argument');
      try { return jsonStr(a[0], 0); } catch (e) { if (e instanceof PError) return e; throw e; }
    });
    const pathOk = p => { if (!p || /^[\/\\]/.test(p) || /^.:/.test(p) || p.includes('\0')) return false; return !p.split(/[\/\\]/).includes('..'); };
    const fsCall = (fn, a, op, nargs) => {
      if (a.length < 1 || typeof a[0] !== 'string') throw new PrismError(fn + ': path must be a string');
      const fs = self.opts.fs; if (!fs) return new PError('file access is disabled');
      if (!pathOk(a[0])) return new PError(fn + ': path "' + a[0] + '" is outside the allowed folder');
      try { const r = op(fs, a[0]); return r === undefined ? null : r; }
      catch (e) { if (e instanceof PrismError || e instanceof NeedInput) throw e; return new PError(fn + ': ' + (e && e.message || e) + ' ("' + a[0] + '")'); }
    };
    const linesOf = t => { if (t instanceof PError) return t; const r = t.split('\n'); if (r.length && r[r.length - 1] === '') r.pop(); return r.map(l => l.endsWith('\r') ? l.slice(0, -1) : l); };
    const wr = (fn, mode) => a => {
      if (a.length !== 2 || typeof a[1] !== 'string') throw new PrismError(fn + '(path, text): text must be a string');
      return fsCall(fn, a, (fs, p) => fs.write(p, a[1], mode));
    };
    const fileObj = {
      read: nat('file.read', a => fsCall('file.read', a, (fs, p) => fs.read(p))),
      lines: nat('file.lines', a => linesOf(fsCall('file.lines', a, (fs, p) => fs.read(p)))),
      write: nat('file.write', wr('file.write', 'w')),
      append: nat('file.append', wr('file.append', 'a')),
      write_lines: nat('file.write_lines', a => {
        if (a.length !== 2 || (!Array.isArray(a[1]) && !(a[1] instanceof Stream))) throw new PrismError('file.write_lines(path, lines): lines must be an array or stream');
        const text = Array.from(a[1]).map(x => (typeof x === 'string' ? x : repr(x)) + '\n').join('');
        return fsCall('file.write_lines', a, (fs, p) => fs.write(p, text, 'w'));
      }),
      exists: nat('file.exists', a => fsCall('file.exists', a, (fs, p) => !!fs.exists(p))),
      delete: nat('file.delete', a => fsCall('file.delete', a, (fs, p) => fs.remove(p))),
      list: nat('file.list', a => fsCall('file.list', a.length ? a : ['.'], (fs, p) => fs.list(p).slice().sort()))
    };
    g.vars.set('file', { v: fileObj, mut: false });
    const hdrStr = (h, fn) => {
      if (h === undefined || h === null) return {};
      if (!isPlain(h)) throw new PrismError(fn + ': headers must be an object like [name: "value"]');
      const o = {}; for (const k of Object.keys(h)) { if (typeof h[k] !== 'string') throw new PrismError(fn + ': header values must be strings'); o[k.replace(/_/g, '-')] = h[k]; }
      return o;
    };
    const httpDo = (fn, method, url, headers, body, timeout) => {
      if (!self.opts.http) return new PError('http is disabled');
      if (!/^https?:\/\//.test(url)) return new PError(fn + ': url must start with http:// or https://');
      let r;
      try { r = self.opts.http(method, url, headers, body, timeout); }
      catch (e) { if (e instanceof PrismError || e instanceof NeedInput) throw e; return new PError(fn + ': ' + (e && e.message || e)); }
      if (r.status >= 400) return new PError(fn + ': HTTP ' + r.status);
      const h = {}; for (const k of Object.keys(r.headers || {})) h[k.toLowerCase().replace(/-/g, '_')] = String(r.headers[k]);
      return { status: BigInt(r.status), ok: true, body: String(r.body), headers: h };
    };
    const nobody = (fn, method) => a => {
      if (a.length < 1 || a.length > 2 || typeof a[0] !== 'string') throw new PrismError(fn + '(url[, headers]): url must be a string');
      return httpDo(fn, method, a[0], hdrStr(a[1], fn), null, 30000);
    };
    const withbody = (fn, method) => a => {
      if (a.length < 2 || a.length > 3 || typeof a[0] !== 'string' || typeof a[1] !== 'string') throw new PrismError(fn + '(url, body[, headers]): url and body must be strings (use json_str(x) for objects)');
      return httpDo(fn, method, a[0], hdrStr(a[2], fn), a[1], 30000);
    };
    g.vars.set('http', { v: {
      get: nat('http.get', nobody('http.get', 'GET')), delete: nat('http.delete', nobody('http.delete', 'DELETE')),
      post: nat('http.post', withbody('http.post', 'POST')), put: nat('http.put', withbody('http.put', 'PUT')),
      request: nat('http.request', a => {
        if (a.length !== 1 || !isPlain(a[0])) throw new PrismError('http.request(options): options must be an object [method:, url:, headers:, body:, timeout:]');
        const o = a[0]; let method = 'GET', url = null, body = null, hv, timeout = 30000;
        for (const k of Object.keys(o)) {
          const v = o[k];
          if (k === 'method' && typeof v === 'string') method = v; else if (k === 'url' && typeof v === 'string') url = v;
          else if (k === 'body' && typeof v === 'string') body = v; else if (k === 'headers') hv = v;
          else if (k === 'timeout' && typeof v === 'bigint' && v > 0n) timeout = Number(v > 600000n ? 600000n : v);
          else throw new PrismError("http.request: bad option '" + k + "' (method, url, headers, body, timeout; strings, timeout int ms)");
        }
        if (url === null) throw new PrismError('http.request: url is required');
        return httpDo('http.request', method, url, hdrStr(hv, 'http.request'), body, timeout);
      })
    }, mut: false });
    g.vars.set('request', {
      v: {
        line: nat('line', () => (more() ? self.stdin[self.stdinPos++] : null)),
        lines: nat('lines', () => new Stream(function* () { while (more()) yield self.stdin[self.stdinPos++]; }, true)),
        words: nat('words', () => new Stream(function* () { while (more()) for (const w of self.stdin[self.stdinPos++].split(/\s+/)) if (w) yield w; }, true)),
        ints: nat('ints', () => new Stream(function* () {
          while (more()) for (const w of self.stdin[self.stdinPos++].split(/\s+/)) {
            if (!w) continue; const v = parseIntStr(w);
            if (v instanceof PError) throw new PrismError('request.ints(): invalid integer "' + w + '"');
            yield v;
          }
        }, true))
      }, mut: false
    });
  },

  /* быстрый вызов лямбды с одним параметром: окружение переиспользуется, пока его не захватило замыкание */
  fn1(f, n) {
    if (f instanceof Closure && !f.clauses && !f.async && !f.isBlock && f.params.length === 1 && f.params[0].k === 'id') {
      const pn = f.params[0].name, body = f.body, fenv = f.env;
      let env = null, busy = false;
      return x => {
        if (busy) return this.callFn(f, [x], false, n);
        if (env === null || env.cap) { env = new Env(fenv, false); env.vars.set(pn, { v: x, mut: true, param: true, frozenRef: false }); }
        else { const vm = env.vars; vm.clear(1); const b = vm.v[0]; b.v = x; b.mut = true; b.param = true; b.frozenRef = false; }
        busy = true;
        try { return this.ev(body, env); } finally { busy = false; }
      };
    }
    return x => this.callFn(f, [x], false, n);
  },
  pred1(f, n) {
    const g = this.fn1(f, n);
    return x => { const r = g(x); if (typeof r !== 'boolean') throw this.err('predicate must return bool, got ' + typeOf(r) + ' ' + repr(r), n); return r; };
  },
  pred(f, x, n) {
    const r = this.callFn(f, [x], false, n);
    if (typeof r !== 'boolean') throw this.err('predicate must return bool, got ' + typeOf(r) + ' ' + repr(r), n);
    return r;
  },
  cmpFor(key, n) {
    return (a, b) => {
      const x = key ? this.callFn(key, [a], false, n) : a, y = key ? this.callFn(key, [b], false, n) : b;
      const r = this.order(x, y);
      if (r === null) throw this.err('cannot compare ' + typeOf(x) + ' and ' + typeOf(y) + ' while sorting', n);
      return r;
    };
  },
  fromStream(f, x0) {
    const self = this;
    return new Stream(function* () { let x = x0; for (;;) { yield x; x = self.callFn(f, [x], false, null); } }, true);
  },

  /* o.f(args) для функции f из области видимости: то же, что f(o, args) */
  ufcs(o, name, n, env) {
    const b = env && env.lookup(name);
    if (!b || !(b.v instanceof Closure || b.v instanceof Native)) return null;
    const recvMut = n && n.obj ? this.isMutPath(n.obj, env) : true;
    return nat(name, a => this.callFn(b.v, [o, ...a], false, n, [recvMut]));
  },
  getMember(o, name, n, env) {
    const r = this.getMember0(o, name, n, env);
    if (r instanceof Native) {
      const NEED_IT = { map: 1, filter: 1, take_while: 1, skip_while: 1, zip: 1, find: 1, any: 1, all: 1, group_by: 1, reduce: 1, contains: 1, index_of: 1 };
      const NEED_STR = { contains: 1, starts_with: 1, ends_with: 1, index_of: 1, replace: 2 };
      const need = typeof o === 'string' ? NEED_STR[name] : (Array.isArray(o) || o instanceof Stream) ? (name === 'contains' || name === 'index_of' ? (Array.isArray(o) ? 1 : 0) : NEED_IT[name]) : 0;
      if (need) { const f = r.fn; return new Native(r.name, a => { if (a.length < need) throw this.err('.' + name + ': missing arguments', n); return f(a); }); }
    }
    return r;
  },
  getMember0(o, name, n, env) {
    if (o === null) throw this.err('cannot access ".' + name + '" of none', n);
    if (o instanceof Closure || o instanceof Native) {
      if (name === 'from') return nat('from', a => this.fromStream(o, a.length ? a[0] : null));
      throw this.err('function has only .from(value)', n);
    }
    if (o instanceof Stream) return this.iterMember(o, name, false, n, env) || (!ARR_ONLY.has(name) && this.ufcs(o, name, n, env)) || this.arrMember(Array.from(o), name, n, env, true);
    if (Array.isArray(o)) return this.iterMember(o, name, true, n, env) || this.arrMember(o, name, n, env, false);
    if (typeof o === 'string') return this.strMember(o, name, n, env);
    if (isNum(o)) {
      if (name === 'to_string') return nat(name, a => {
        if (a.length && typeof o === 'bigint') { if (typeof a[0] !== 'bigint' || a[0] < 2n || a[0] > 36n) throw this.err('to_string(base): base must be int in 2..36', n); return o.toString(Number(a[0])); }
        return str(o);
      });
      if (name === 'to_int') return nat(name, () => (typeof o === 'bigint' ? o : o instanceof Dec ? decInt(o, 'trunc') : Number.isFinite(o) ? BigInt(Math.trunc(o)) : new PError('cannot convert ' + fmtFloat(o) + ' to int')));
      if (name === 'to_float') return nat(name, () => (DEC.prec > 0 ? toDec(o) : Number(o)));
      const u = this.ufcs(o, name, n, env); if (u) return u;
      throw this.noMethod('number', name, NUM_METHODS, env, n);
    }
    if (typeof o === 'boolean') { const u = this.ufcs(o, name, n, env); if (u) return u; throw this.noMethod('bool', name, [], env, n); }
    if (o instanceof PError) { if (name === 'message') return o.message; throw this.err('error has only .message', n); }
    if (Object.prototype.hasOwnProperty.call(o, name)) return o[name];
    if (o instanceof TypeDef) throw this.err('type ' + o.name + ' has no member .' + name, n);
    switch (name) {
      case 'keys': return nat(name, () => Object.keys(o));
      case 'values': return nat(name, () => Object.values(o));
      case 'has': return nat(name, a => Object.prototype.hasOwnProperty.call(o, String(a[0])));
      case 'copy': return nat(name, () => tagLike(o, Object.assign({}, o)));
      case 'dcopy': return nat(name, () => deepCopy(o));
      case 'len': return nat(name, () => BigInt(Object.keys(o).length));
    }
    if (o[TYPE]) { const u = this.ufcs(o, name, n, env); if (u) return u; }
    return null;
  },

  /* методы, общие для массивов и потоков */
  iterMember(src, name, isArr, n, env) {
    const self = this, F = (f, a) => this.callFn(f, a, false, n);
    const st = !!(src instanceof Stream && src.stateful);
    /* массив даёт цепочку, которая идёт поэлементно; поток остаётся потоком */
    const wrap = g => (isArr || src instanceof ArrChain ? new ArrChain(g, st) : new Stream(g, st));
    const cnt = (k, what) => { if (typeof k !== 'bigint') throw self.err(what + '(n): n must be int, got ' + typeOf(k), n); return Number(k); };
    switch (name) {
      case 'map': return nat(name, a => { const g = self.fn1(a[0], n); return wrap(function* () { for (const x of src) { let r; try { r = g(x); } catch (e) { if (e instanceof LoopCtl) { if (e.ctl === 2) return; continue; } throw e; } yield r; } }); });
      case 'filter': return nat(name, a => { const g = self.pred1(a[0], n); return wrap(function* () { for (const x of src) { let r; try { r = g(x); } catch (e) { if (e instanceof LoopCtl) { if (e.ctl === 2) return; continue; } throw e; } if (r) yield x; } }); });
      case 'expand': case 'expand_bfs': case 'expand_once': case 'expand_bfs_once': return nat(name, a => {
        const once = name.endsWith('_once'), bfs = name.startsWith('expand_bfs');
        if (a.length < 1 || a.length > 2) throw self.err('.' + name + ': expected a function' + (!bfs ? ' (and an optional exit function)' : ''), n);
        if (bfs && a.length === 2) throw self.err('.' + name + ' takes one function', n);
        const seen = new Set(), fresh = x => { if (!once) return true; const k = repr(x); if (seen.has(k)) return false; seen.add(k); return true; };
        const kids = x => {
          const r = F(a[0], [x]);
          if (r === null) return [];
          if (Array.isArray(r) || r instanceof Stream) return r;
          throw self.err('.' + name + ': children function must return array, stream or none, got ' + typeOf(r) + ' ' + repr(r), n);
        };
        if (!bfs) return wrap(function* () {
          const stack = [{ it: src[Symbol.iterator](), node: undefined }];
          while (stack.length) {
            const top = stack[stack.length - 1], r = top.it.next();
            if (r.done) { stack.pop(); if (top.node !== undefined && a.length === 2) yield F(a[1], [top.node]); continue; }
            if (!fresh(r.value)) continue;
            yield r.value; stack.push({ it: kids(r.value)[Symbol.iterator](), node: r.value });
          }
        });
        return wrap(function* () {
          const q = []; for (const x of src) if (fresh(x)) q.push(x);
          for (let h = 0; h < q.length; h++) { yield q[h]; for (const c of kids(q[h])) if (fresh(c)) q.push(c); }
        });
      });
      case 'scan': return nat(name, a => {
        if (a.length < 1 || a.length > 2) throw self.err('.scan(init, f) or .scan(f)', n);
        return wrap(function* () { let acc, started = a.length === 2, f = a[a.length - 1]; if (started) acc = a[0]; for (const x of src) { if (!started) { acc = x; started = true; } else acc = F(f, [acc, x]); yield acc; } });
      });
      case 'flat_map': return nat(name, a => wrap(function* () { for (const x of src) { let r; try { r = F(a[0], [x]); } catch (e) { if (e instanceof LoopCtl) { if (e.ctl === 2) return; continue; } throw e; } if (Array.isArray(r) || r instanceof Stream) yield* r; else yield r; } }));
      case 'partition': return nat(name, a => { const y = [], no = []; for (const x of src) (self.pred(a[0], x, n) ? y : no).push(x); return [y, no]; });
      case 'min_by': case 'max_by': return nat(name, a => {
        let best, bk, first = true;
        for (const x of src) { const k = F(a[0], [x]); if (first) { best = x; bk = k; first = false; continue; } const c = self.order(k, bk); if (c === null) throw self.err('cannot compare ' + typeOf(k) + ' and ' + typeOf(bk) + ' in ' + name, n); if (name === 'min_by' ? c < 0 : c > 0) { best = x; bk = k; } }
        return first ? null : best;
      });
      case 'cycle': return nat(name, () => wrap(function* () { const buf = Array.from(src); if (!buf.length) return; for (;;) yield* buf; }));
      case 'repeat': return nat(name, a => wrap(function* () { const k = cnt(a[0], 'repeat'); const buf = Array.from(src); for (let i = 0; i < k; i++) yield* buf; }));
      case 'take': return nat(name, a => wrap(function* () { const k = cnt(a[0], 'take'); if (k <= 0) return; let i = 0; for (const x of src) { yield x; if (++i >= k) return; } }));
      case 'skip': return nat(name, a => wrap(function* () { const k = cnt(a[0], 'skip'); let i = 0; for (const x of src) { if (i++ >= k) yield x; } }));
      case 'take_while': return nat(name, a => wrap(function* () { for (const x of src) { if (!self.pred(a[0], x, n)) return; yield x; } }));
      case 'skip_while': return nat(name, a => wrap(function* () { let go = false; for (const x of src) { if (!go && !self.pred(a[0], x, n)) go = true; if (go) yield x; } }));
      case 'enumerate': return nat(name, () => wrap(function* () { let i = 0n; for (const x of src) yield [i++, x]; }));
      case 'flat': return nat(name, () => wrap(function* () { for (const x of src) { if (Array.isArray(x)) yield* x; else yield x; } }));
      case 'zip': return nat(name, a => wrap(function* () {
        const other = self.iterOf(a[0], n)[Symbol.iterator](); const mine = src[Symbol.iterator]();
        for (;;) { const p = mine.next(); if (p.done) return; const q = other.next(); if (q.done) return; yield [p.value, q.value]; }
      }));
      case 'par': return nat(name, () => {
        if (st) throw self.err('.par() cannot be used on a stateful stream', n);
        return wrap(function* () { yield* src; }); // в песочнице выполняется последовательно
      });
      case 'find': return nat(name, a => { for (const x of src) if (self.pred(a[0], x, n)) return x; return null; });
      case 'any': return nat(name, a => { for (const x of src) if (self.pred(a[0], x, n)) return true; return false; });
      case 'all': return nat(name, a => { for (const x of src) if (!self.pred(a[0], x, n)) return false; return true; });
      case 'count': return nat(name, a => { let c = 0n; for (const x of src) if (!a.length || self.pred(a[0], x, n)) c++; return c; });
      case 'first': return nat(name, () => { for (const x of src) return x; return null; });
      case 'to_list': return nat(name, () => Array.from(src));
      case 'sum': return nat(name, () => { let s = 0n; for (const x of src) s = typeof s === 'bigint' && typeof x === 'bigint' ? s + x : self.add(s, x, n); return s; });
      case 'reduce': return nat(name, a => {
        let acc, started = false, f;
        if (a.length >= 2) { acc = a[0]; f = a[1]; started = true; } else f = a[0];
        for (const x of src) { if (!started) { acc = x; started = true; } else acc = F(f, [acc, x]); }
        return started ? acc : null;
      });
      case 'sort': return nat(name, a => Array.from(src).sort(self.cmpFor(a[0], n)));
      case 'avg': return nat(name, () => { let s = 0n, c = 0; for (const x of src) { s = self.add(s, x, n); c++; } return c ? (s instanceof Dec || DEC.prec > 0 ? decArith('/', s, BigInt(c)) : Number(s) / c) : null; });
      case 'unique': return nat(name, () => wrap(function* () { const seen = []; for (const x of src) if (!seen.some(y => eq(x, y))) { seen.push(x); yield x; } }));
      case 'chunk': return nat(name, a => wrap(function* () { const k = cnt(a[0], 'chunk'); if (k < 1) throw self.err('chunk(n): n must be >= 1', n); let cur = []; for (const x of src) { cur.push(x); if (cur.length === k) { yield cur; cur = []; } } if (cur.length) yield cur; }));
      case 'window': return nat(name, a => wrap(function* () { const k = cnt(a[0], 'window'); if (k < 1) throw self.err('window(n): n must be >= 1', n); const buf = []; for (const x of src) { buf.push(x); if (buf.length > k) buf.shift(); if (buf.length === k) yield buf.slice(); } }));
      case 'group_by': return nat(name, a => { const o = {}; for (const x of src) { const k = str(F(a[0], [x])); if (!Object.prototype.hasOwnProperty.call(o, k)) o[k] = []; o[k].push(x); } return o; });
    }
    return null;
  },

  arrMember(arr, name, n, env, fromStream) {
    switch (name) {
      case 'copy': return nat(name, () => arr.slice()); // поверхностная копия: вложенные массивы и объекты остаются общими
      case 'dcopy': return nat(name, () => deepCopy(arr)); // глубокая копия
      case 'len': return nat(name, () => BigInt(arr.length));
      case 'last': return nat(name, () => arr.length ? arr[arr.length - 1] : null);
      case 'reverse': return nat(name, () => arr.slice().reverse());
      case 'contains': return nat(name, a => arr.some(x => eq(x, a[0])));
      case 'index_of': return nat(name, a => { const i = arr.findIndex(x => eq(x, a[0])); return i < 0 ? null : BigInt(i); });
      case 'join': return nat(name, a => arr.map(str).join(a.length ? str(a[0]) : ''));
      case 'min': return nat(name, () => this.globals.vars.get('min').v.fn([arr]));
      case 'max': return nat(name, () => this.globals.vars.get('max').v.fn([arr]));
      case 'pop':
        if (fromStream) throw this.err('.pop requires an array stored in a variable', n);
        return nat(name, a => {
          this.requireMutable(n.obj, env, n);
          if (!arr.length) throw this.err('.pop: array is empty', n);
          if (!a.length) return arr.pop();
          if (typeof a[0] !== 'bigint') throw this.err('index must be int', n);
          const i = Number(a[0]) < 0 ? arr.length + Number(a[0]) : Number(a[0]);
          if (i < 0 || i >= arr.length) throw this.err('index ' + a[0] + ' out of range for array of length ' + arr.length, n);
          return arr.splice(i, 1)[0];
        });
      case 'remove': case 'insert':
        if (fromStream) throw this.err('.' + name + ' requires an array stored in a variable', n);
        return nat(name, a => {
          this.requireMutable(n.obj, env, n);
          if (typeof a[0] !== 'bigint') throw this.err('index must be int', n);
          const i = Number(a[0]) < 0 ? arr.length + Number(a[0]) : Number(a[0]);
          if (name === 'remove') { if (i < 0 || i >= arr.length) throw this.err('index ' + a[0] + ' out of range for array of length ' + arr.length, n); return arr.splice(i, 1)[0]; }
          if (i < 0 || i > arr.length) throw this.err('index ' + a[0] + ' out of range for array of length ' + arr.length, n);
          arr.splice(i, 0, a[1]); return null;
        });
    }
    const u = this.ufcs(arr, name, n, env); if (u) return u;
    throw this.noMethod('array', name, ARR_METHODS, env, n);
  },

  strMember(s, name, n, env) {
    switch (name) {
      case 'len': return nat(name, () => BigInt(sLen(s)));
      case 'first': return nat(name, () => (s.length ? sAt(s, 0) : null));
      case 'last': return nat(name, () => (s.length ? sAt(s, sLen(s) - 1) : null));
      case 'skip': case 'take': return nat(name, a => {
        if (typeof a[0] !== 'bigint' || a[0] < 0n) throw this.err(name + '(n): n must be int >= 0', n);
        const k = Number(a[0]);
        return name === 'skip' ? sSlice(s, k) : sSlice(s, 0, k);
      });
      case 'remove': case 'insert': return nat(name, a => {   /* строки неизменяемы: новая строка */
        const ins = name === 'insert', cs = Array.from(s), L = cs.length;
        if (a.length !== (ins ? 2 : 1)) throw this.err('.' + name + ' takes ' + (ins ? '2 arguments (index, text)' : '1 argument (index)'), n);
        if (typeof a[0] !== 'bigint') throw this.err('index must be int', n);
        const i = Number(a[0]) < 0 ? L + Number(a[0]) : Number(a[0]);
        if (i < 0 || i > L || (!ins && i >= L)) throw this.err('index ' + a[0] + ' out of range for string of length ' + L, n);
        if (ins) cs.splice(i, 0, str(a[1])); else cs.splice(i, 1);
        return cs.join('');
      });
      case 'upper': return nat(name, () => s.toUpperCase());
      case 'lower': return nat(name, () => s.toLowerCase());
      case 'json': return nat(name, () => this.jsonParse(s));
      case 'trim': return nat(name, () => s.trim());
      case 'chars': return nat(name, () => Array.from(s));
      case 'lines': return nat(name, () => { const l = s.split(/\r?\n/); if (l.length && l[l.length - 1] === '') l.pop(); return l; });
      case 'split': return nat(name, a => (!a.length ? s.split(/\s+/).filter(x => x !== '') : a[0] === '' ? Array.from(s) : s.split(str(a[0]))));
      case 'contains': return nat(name, a => s.includes(str(a[0])));
      case 'starts_with': return nat(name, a => s.startsWith(str(a[0])));
      case 'ends_with': return nat(name, a => s.endsWith(str(a[0])));
      case 'index_of': return nat(name, a => { const i = s.indexOf(str(a[0])); return i < 0 ? null : BigInt(sLen(s.slice(0, i))); });
      case 'replace': return nat(name, a => s.split(str(a[0])).join(str(a[1])));
      case 'reverse': return nat(name, () => Array.from(s).reverse().join(''));
      case 'repeat': return nat(name, a => { if (typeof a[0] !== 'bigint' || a[0] < 0n) throw this.err('repeat(n): n must be int >= 0', n); if (Number(a[0]) * s.length > 50000000) throw this.err('string too large', n); return s.repeat(Number(a[0])); });
      case 'pad_left': case 'pad_right': return nat(name, a => {
        if (typeof a[0] !== 'bigint') throw this.err(name + '(n, c): n must be int', n);
        const c = a.length > 1 ? a[1] : ' ';
        if (typeof c !== 'string' || sLen(c) !== 1) throw this.err(name + '(n, c): c must be a one-character string', n);
        const k = Number(a[0]) - sLen(s); if (k <= 0) return s;
        return name === 'pad_left' ? c.repeat(k) + s : s + c.repeat(k);
      });
      case 'to_int': return nat(name, () => parseIntStr(s));
      case 'to_float': return nat(name, () => parseFloatStr(s));
      case 'to_number': return nat(name, () => { const t = s.trim(); return /^[+-]?\d+$/.test(t) ? BigInt(t) : parseFloatStr(s); }); // старое имя: int, если записано без точки
      case 'interpret': return nat(name, () => this.interpret(s, env, n));
    }
    const u = this.ufcs(s, name, n, env); if (u) return u;
    throw this.noMethod('string', name, STR_METHODS, env, n);
  },

  interpret(s, env, n) {
    let out = '', i = 0;
    while (i < s.length) {
      const c = s[i];
      if (c === '\\' && (s[i + 1] === '{' || s[i + 1] === '}')) { out += s[i + 1]; i += 2; continue; }
      if (c === '{') {
        let d = 1, j = i + 1;
        while (j < s.length && d > 0) { if (s[j] === '{') d++; else if (s[j] === '}') d--; j++; }
        if (d !== 0) throw this.err('unclosed "{" in interpolation', n);
        const src = s.slice(i + 1, j - 1);
        let v;
        try { v = this.ev(new Parser(this.lexSrc(src, false)).parseExprOnly(), env); }
        catch (e) { if (e instanceof PrismError) { e.message = 'in {' + src + '}: ' + e.message; if (e.line == null || e.kind === 'syntax') e.line = n && n.line; e.kind = 'runtime'; } throw e; }
        out += str(v); i = j; continue;
      }
      out += c; i++;
    }
    return out;
  }
});

function fmtError(e) {
  let msg;
  if (e instanceof PrismError) {
    const head = e.kind === 'syntax' ? 'SyntaxError' : 'RuntimeError';
    msg = head + (e.file ? ' in module "' + e.file + '"' : '') + (e.line != null ? ' (line ' + e.line + ')' : '') + ': ' + e.message;
  } else if (e instanceof RangeError) msg = 'RecursionError: maximum recursion depth exceeded';
  else msg = 'InternalError: ' + (e && e.message);
  return msg;
}
function prismRun(opts) {
  const it = new Interp(opts);
  try { it.runMain(opts.source); return { ok: true }; }
  catch (e) {
    if (e instanceof NeedInput) return { ok: false, needInput: true }; return { ok: false, error: fmtError(e), line: e instanceof PrismError && !e.file ? e.line : null }; }
}
if (typeof module !== 'undefined') module.exports = { prismRun };
