/* ===== Prism v2: перевод нового синтаксиса (отступы, [k: v], where/select) в токены ядра ===== */
const SYNTAX_DEFAULT = (typeof process !== 'undefined' && process.env && process.env.PRISM_SYNTAX) || 'v2';
const LOWWORDS = new Set(['where', 'select']);
const isLow = t => t && t.t === 'id' && LOWWORDS.has(t.v);
const OPEN = '([{', CLOSE = ')]}';
const endsExpr = t => !!t && (t.t === 'id' || t.t === 'num' || t.t === 'str' || t.t === 'istr' || (t.t === 'kw' && (t.v === 'true' || t.v === 'false' || t.v === 'none' || t.v === 'next' || t.v === 'stop')) || (t.t === 'op' && CLOSE.includes(t.v)));
const mk = (t, v, line, extra) => Object.assign({ t, v, line, col: 0, sp: true }, extra || {});

/* 1. скобки: [k: v] -> объект, [:] -> пустой объект, [a:, b:] -> шаблон; type P [x, y] и as [a, b] -> список полей */
/* фигурные скобки в Prism не нужны: сначала обычная синтаксическая ошибка, затем (только если похоже на объект) подсказка */
function strayBrace(out, i) {
  const t = out[i];
  let msg = 'unexpected character "' + t.v + '"';
  if (t.v === '{') {
    let d = 0, k = i + 1, hasKey = false;
    for (; k < out.length; k++) {
      const x = out[k]; if (x.t !== 'op') continue;
      if (OPEN.includes(x.v)) d++;
      else if (CLOSE.includes(x.v)) { if (d === 0) break; d--; }
      else if (d === 0 && x.v === ':') hasKey = true;
    }
    const closed = k < out.length && out[k].v === '}';
    const empty = closed && out.slice(i + 1, k).every(x => x.t === 'nl');
    if (hasKey) msg += '';
    else if (empty) msg += '';
  }
  return syntaxErr(msg, t.line);
}
function bracketPass(src) {
  const out = src.map(t => Object.assign({}, t));
  const match = new Array(out.length).fill(-1), st = [];
  out.forEach((t, i) => {
    if (t.t !== 'op') return;
    if (t.v === '{' || t.v === '}') throw strayBrace(out, i);
    if (OPEN.includes(t.v)) st.push(i);
    else if (CLOSE.includes(t.v)) { const j = st.pop(); if (j === undefined) throw syntaxErr('unmatched "' + t.v + '"', t.line); match[j] = i; match[i] = j; }
  });
  if (st.length) throw syntaxErr('unclosed "' + out[st[st.length - 1]].v + '"', out[st[st.length - 1]].line);
  for (let i = 0; i < out.length; i++) {
    const t = out[i];
    if (!(t.t === 'op' && t.v === '[')) continue;
    const p = out[i - 1], p2 = out[i - 2], j = match[i];
    if (p && p.t !== 'nl' && endsExpr(p) && !t.sp && !(p.t === 'id' && p2 && p2.t === 'id' && p2.v === 'type')) continue; // a[0]
    const fields = (p && p.t === 'kw' && p.v === 'as') || (p && p.t === 'id' && p2 && p2.t === 'id' && p2.v === 'type' && (i < 3 || out[i - 3].t === 'nl'));
    let obj = fields, empty = false;
    if (!obj) {
      if (j === i + 2 && out[i + 1].t === 'op' && out[i + 1].v === ':') { obj = true; empty = true; }
      else {
        let d = 0;
        for (let k = i + 1; k < j; k++) {
          const x = out[k]; if (x.t !== 'op') continue;
          if (OPEN.includes(x.v)) d++; else if (CLOSE.includes(x.v)) d--;
          else if (d === 0 && x.v === ':' && !x.adj) { obj = true; break; }
        }
      }
    }
    if (!obj) continue;
    out[i].v = '{'; out[i].gen = true; out[j].v = '}'; out[j].gen = true;
    if (empty) out[i + 1].del = true;
    else if (!fields) {
      let d = 0;
      for (let k = i + 1; k < j; k++) {
        const x = out[k]; if (x.t !== 'op') continue;
        if (OPEN.includes(x.v)) d++; else if (CLOSE.includes(x.v)) d--;
        else if (d === 0 && x.v === ':' && !x.adj) { const nx = out[k + 1]; if (k + 1 === j || (nx.t === 'op' && nx.v === ',') || nx.t === 'nl') x.del = true; }
      }
    }
  }
  return out.filter(t => !t.del);
}

/* 2. слова where / select: A where B select C -> (A).filter(B).map(C); ведущая .имя в B и C = _.имя */
function wherePass(src) {
  const a = src.slice();
  for (let guard = 0; guard < 5000; guard++) {
    const i = a.findIndex(isLow); if (i < 0) break;
    const kw = a[i];
    let s = i, d = 0;
    for (let k = i - 1; k >= 0; k--) {
      const x = a[k];
      if (x.t === 'op' && CLOSE.includes(x.v)) { d++; s = k; continue; }
      if (x.t === 'op' && OPEN.includes(x.v)) { if (d === 0) { s = k + 1; break; } d--; s = k; continue; }
      if (d > 0) { s = k; continue; }
      if (x.t === 'nl') { const nx = a[k + 1]; if (nx && (isLow(nx) || (nx.t === 'op' && (nx.v === '.' || nx.v === '::')))) { s = k + 1; continue; } s = k + 1; break; }
      if (x.t === 'op' && (ASSIGN_OPS.has(x.v) || x.v === '->' || x.v === ',')) { s = k + 1; break; }
      if (x.t === 'kw' && (x.v === 'return' || x.v === 'else' || x.v === 'is')) { s = k + 1; break; }
      s = k;
    }
    while (a[s] && a[s].t === 'nl') s++;
    if (s >= i) throw syntaxErr('expected expression before ' + kw.v, kw.line);
    let e = a.length; d = 0;
    for (let k = i + 1; k < a.length; k++) {
      const x = a[k];
      if (x.t === 'eof') { e = k; break; }
      if (x.t === 'op' && OPEN.includes(x.v)) { d++; continue; }
      if (x.t === 'op' && CLOSE.includes(x.v)) { if (d === 0) { e = k; break; } d--; continue; }
      if (d === 0 && (x.t === 'nl' || isLow(x) || (x.t === 'op' && (x.v === '::' || x.v === ',')))) { e = k; break; }
    }
    const r = a.slice(i + 1, e);
    if (!r.length) throw syntaxErr('expected expression after ' + kw.v, kw.line);
    const rr = [];
    r.forEach((x, m) => {
      if (x.t === 'op' && x.v === '[' && r[m + 1] && r[m + 1].t === 'str' && r[m + 2] && r[m + 2].t === 'op' && r[m + 2].v === ']') { // ["имя"] = _["имя"]
        const pv = r[m - 1];
        if (!pv || (pv.t === 'op' && !CLOSE.includes(pv.v)) || (pv.t === 'kw' && ['and', 'or', 'not', 'is', 'in'].includes(pv.v))) rr.push(mk('id', '_', x.line));
      } else if (x.t === 'op' && x.v === '.' && r[m + 1] && r[m + 1].t === 'id') {
        const pv = r[m - 1];
        if (!pv || (pv.t === 'op' && !CLOSE.includes(pv.v)) || (pv.t === 'kw' && ['and', 'or', 'not', 'is', 'in'].includes(pv.v))) rr.push(mk('id', '_', x.line));
      }
      rr.push(x);
    });
    const L = a.slice(s, i);
    a.splice(s, e - s, mk('op', '(', L[0].line, { col: L[0].col }), ...L, mk('op', ')', kw.line), mk('op', '.', kw.line), mk('id', kw.v === 'where' ? 'filter' : 'map', kw.line), mk('op', '(', kw.line), ...rr, mk('op', ')', kw.line));
  }
  return a;
}

/* 3. отступы -> блоки */
function layoutPass(src) {
  const lines = []; let cur = [], depth = 0;
  for (const t of src) {
    if (t.t === 'eof') break;
    if (t.t === 'nl' && depth === 0) { if (cur.length) lines.push(cur); cur = []; continue; }
    cur.push(t);
    if (t.t === 'op') { if (OPEN.includes(t.v)) depth++; else if (CLOSE.includes(t.v)) depth--; }
  }
  if (cur.length) lines.push(cur);
  const out = [], stack = [];
  const nl = ln => mk('nl', undefined, ln);
  const isHeader = L => {
    if (L.length < 2 || L[0].t !== 'id' || L[L.length - 1].t !== 'op' || L[L.length - 1].v !== '=') return false;
    let d = 0;
    for (let k = 1; k < L.length - 1; k++) {
      const x = L[k];
      if (x.t === 'op') { if (OPEN.includes(x.v)) d++; else if (CLOSE.includes(x.v)) d--; else if (d === 0 && (ASSIGN_OPS.has(x.v) || x.v === '.' || x.v === '::' || x.v === '->')) return false; }
    }
    return true;
  };
  const isForeachTail = L => {
    let k = L.length - 1;
    while (k >= 0 && (L[k].t === 'id' || (L[k].t === 'op' && L[k].v === ','))) k--;
    return k >= 0 && L[k].t === 'op' && L[k].v === '::';
  };
  for (let li = 0; li < lines.length; li++) {
    const L = lines[li].slice(), I = L[0].col, ln = L[0].line;
    while (stack.length && I < stack[stack.length - 1].body) { stack.pop(); out.push(mk('op', '}', ln, { gen: true })); out.push(nl(ln)); }
    for (let k = 0; k < L.length; k++) { // xs::[par] a[,b] -> тело (в одну строку)
      if (!(L[k].t === 'op' && L[k].v === '::')) continue;
      let e = k + 1;
      while (e < L.length && (L[e].t === 'id' || (L[e].t === 'op' && L[e].v === ','))) e++;
      if (e > k + 1 && e < L.length && L[e].t === 'op' && L[e].v === '->' && (e - k > 2 || L[k + 1].v === 'par' || L[e - 1].t === 'id' && e - 1 > k + 1)) {
        L.splice(e, 1, mk('op', '{', ln, { gen: true })); L.push(mk('op', '}', ln, { gen: true })); break;
      }
    }
    const last = L[L.length - 1], next = lines[li + 1];
    let open = false;
    if (next && next[0].col > I) {
      if (last.t === 'op' && last.v === '->') open = true;
      else if (last.t === 'kw' && last.v === 'match') open = true;
      else if (isForeachTail(L)) open = true;
      else if (isHeader(L)) { L.pop(); open = true; }
    }
    out.push(...L);
    if (open) { out.push(mk('op', '{', last.line, { gen: true })); stack.push({ body: next[0].col }); }
    out.push(nl(last.line));
  }
  const ln = out.length ? out[out.length - 1].line : 1;
  while (stack.length) { stack.pop(); out.push(mk('op', '}', ln, { gen: true })); out.push(nl(ln)); }
  out.push({ t: 'eof', line: ln });
  return out;
}

/* 4. многострочная лямбда внутри скобок: f(x ->⏎    тело⏎) — тело идёт до сдвига влево или до закрывающей скобки */
function lambdaBlockPass(src) {
  const a = src.slice();
  let depth = 0;
  for (let i = 0; i < a.length; i++) {
    const t = a[i];
    if (t.t === 'op') { if (OPEN.includes(t.v)) depth++; else if (CLOSE.includes(t.v)) depth--; }
    if (!(depth > 0 && t.t === 'op' && t.v === '->' && a[i + 1] && a[i + 1].t === 'nl')) continue;
    let b = i + 1; while (a[b] && a[b].t === 'nl') b++;
    if (!a[b] || a[b].t === 'eof' || a[b].col === undefined) continue;
    const bodyCol = a[b].col;
    let d = 0, e = -1;
    for (let k = b; k < a.length; k++) {
      const x = a[k];
      if (x.t === 'eof') { e = k; break; }
      if (x.t === 'op' && OPEN.includes(x.v)) { d++; continue; }
      if (x.t === 'op' && CLOSE.includes(x.v)) { if (d === 0) { e = k; break; } d--; continue; }
      if (d === 0 && x.t === 'nl') { let m = k + 1; while (a[m] && a[m].t === 'nl') m++; if (!a[m] || a[m].t === 'eof' || a[m].col < bodyCol || (a[m].t === 'op' && CLOSE.includes(a[m].v))) { e = k; break; } }
    }
    if (e < 0) continue;
    const ln = t.line;
    const body = a.slice(b, e);
    while (body.length && body[body.length - 1].t === 'nl') body.pop();
    body.push({ t: 'eof', line: ln });
    const inner = layoutPass(lambdaBlockPass(body));
    inner.pop();
    a.splice(i + 1, e - (i + 1), mk('op', '{', ln, { gen: true }), ...inner, mk('op', '}', ln, { gen: true }));
    depth = depth; // скобки внутри уже сбалансированы
  }
  return a;
}

function toV1(toks, opts) {
  let t = wherePass(bracketPass(toks));
  if (!opts || opts.layout !== false) t = layoutPass(lambdaBlockPass(t));
  return t;
}
