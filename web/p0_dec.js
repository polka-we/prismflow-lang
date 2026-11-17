/* ===== десятичные числа произвольной точности (float с точностью, которую задаёт precision) ===== */
const DEC = { prec: 0 };   /* 0 — float это обычный double; n > 0 — дробные числа десятичные, n знаков после запятой */
class Dec {                /* значение = m / 10^s, s >= 0 */
  constructor(m, s) { this.m = m; this.s = s; }
  valueOf() { return decToNumber(this); }
  toString() { return decStr(this); }
}
const decP = () => (DEC.prec > 0 ? DEC.prec : 30);
const POW10 = [1n];
const p10 = k => { while (POW10.length <= k) POW10.push(POW10[POW10.length - 1] * 10n); return POW10[k]; };
function decParse(txt) {
  const m = /^([+-]?)(\d*)(?:\.(\d*))?(?:[eE]([+-]?\d+))?$/.exec(String(txt).trim());
  if (!m || (m[2] === '' && (m[3] === undefined || m[3] === ''))) return null;
  const frac = m[3] || '';
  let mant = BigInt((m[2] + frac) || '0'), s = frac.length - (m[4] ? Number(m[4]) : 0);
  if (s < 0) { mant *= p10(-s); s = 0; }
  return new Dec(m[1] === '-' ? -mant : mant, s);
}
function decFromNumber(x) { return Number.isFinite(x) ? decParse(String(x)) : x; }
function toDec(v) {
  if (v instanceof Dec) return v;
  if (typeof v === 'bigint') return new Dec(v, 0);
  if (typeof v === 'boolean') return new Dec(v ? 1n : 0n, 0);
  if (typeof v === 'number') return decFromNumber(v);
  return v;
}
function decRound(d, P) {
  if (d.s <= P) return d;
  const div = p10(d.s - P); let m = d.m; const neg = m < 0n; if (neg) m = -m;
  let q = m / div; if (2n * (m % div) >= div) q += 1n;
  return new Dec(neg ? -q : q, P);
}
function decAlign(a, b) { const s = Math.max(a.s, b.s); return [a.m * p10(s - a.s), b.m * p10(s - b.s), s]; }
function decCmp(a, b) { const [x, y] = decAlign(a, b); return x < y ? -1 : x > y ? 1 : 0; }
function decNeg(a) { return new Dec(-a.m, a.s); }
function decAbs(a) { return a.m < 0n ? new Dec(-a.m, a.s) : a; }
function decDiv(a, b) {
  if (b.m === 0n) return new PError('division by zero');
  const P = decP();
  let num = a.m * p10(P + 1 + b.s), den = b.m * p10(a.s);
  const neg = (num < 0n) !== (den < 0n); if (num < 0n) num = -num; if (den < 0n) den = -den;
  const q = num / den;
  return decRound(new Dec(neg ? -q : q, P + 1), P);
}
function decArith(op, a, b) {
  const x = toDec(a), y = toDec(b);
  if (!(x instanceof Dec) || !(y instanceof Dec)) { /* inf/nan: обычная арифметика */
    const u = Number(a), v = Number(b);
    switch (op) { case '+': return u + v; case '-': return u - v; case '*': return u * v; case '/': return v === 0 ? new PError('division by zero') : u / v; case '%': return v === 0 ? new PError('division by zero') : ((u % v) + v) % v; default: return Math.pow(u, v); }
  }
  const P = decP();
  switch (op) {
    case '+': { const [p, q, s] = decAlign(x, y); return decRound(new Dec(p + q, s), P); }
    case '-': { const [p, q, s] = decAlign(x, y); return decRound(new Dec(p - q, s), P); }
    case '*': return decRound(new Dec(x.m * y.m, x.s + y.s), P);
    case '/': return decDiv(x, y);
    case '%': { const [p, q, s] = decAlign(x, y); if (q === 0n) return new PError('division by zero'); return new Dec(((p % q) + q) % q, s); }
    case '**': {
      if (y.s === 0 || y.m % p10(y.s) === 0n) {   /* целая степень */
        let e = y.m / p10(y.s); const neg = e < 0n; if (neg) e = -e;
        if (e > 100000n) throw new PrismError('exponent too large');
        let res = new Dec(1n, 0), base = x;
        while (e > 0n) { if (e & 1n) res = decRound(new Dec(res.m * base.m, res.s + base.s), P); e >>= 1n; if (e) base = decRound(new Dec(base.m * base.m, base.s + base.s), P); }
        return neg ? decDiv(new Dec(1n, 0), res) : res;
      }
      const r = Math.pow(decToNumber(x), decToNumber(y));
      return decFromNumber(r);
    }
  }
}
function decStr(d) {
  let m = d.m; const neg = m < 0n; if (neg) m = -m;
  let ds = m.toString(); if (ds.length <= d.s) ds = '0'.repeat(d.s - ds.length + 1) + ds;
  const ip = ds.slice(0, ds.length - d.s); let fp = ds.slice(ds.length - d.s).replace(/0+$/, '');
  return (neg ? '-' : '') + ip + '.' + (fp || '0');
}
function decToNumber(d) { return parseFloat(decStr(d)); }
function decInt(d, mode) {   /* floor | ceil | trunc | round(half away from zero) -> BigInt */
  const div = p10(d.s); let q = d.m / div; const r = d.m % div;   /* усечение к нулю */
  if (r === 0n) return q;
  if (mode === 'trunc') return q;
  if (mode === 'floor') return d.m < 0n ? q - 1n : q;
  if (mode === 'ceil') return d.m < 0n ? q : q + 1n;
  const ar = r < 0n ? -r : r;
  return 2n * ar >= div ? (d.m < 0n ? q - 1n : q + 1n) : q;
}
function decRoundDigits(d, n) {   /* округление до n знаков (в сторону от нуля при .5) */
  if (d.s <= n) return d;
  return decRound(d, n);
}
function isqrt(n) { if (n < 2n) return n; let x = 1n << BigInt((n.toString(2).length + 1) >> 1); for (;;) { const y = (x + n / x) >> 1n; if (y >= x) return x; x = y; } }
function decSqrt(d) {
  if (d.m < 0n) return NaN;
  const P = decP(), Q = P + 1;
  if (d.s > 2 * Q) d = decRound(d, 2 * Q);
  const r = isqrt(d.m * p10(2 * Q - d.s));
  return decRound(new Dec(r, Q), P);
}
