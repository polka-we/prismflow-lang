#!/bin/sh
# Запуск: ./tests/run.sh [путь/к/prism]. Сравнивает вывод с *.expected (stdin берётся из *.stdin).
# tests/io/*.pr запускаются с --files во временной пустой папке.
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-./prism}
fail=0; total=0
for f in examples/*.pr tests/cases/*.pr tests/parity/*.pr tests/io/*.pr; do
  b=${f%.pr}; total=$((total+1))
  case "$f" in tests/parity/*|tests/io/*) ERR='2>&1';; *) ERR='2>/dev/null';; esac
  IN=/dev/null; [ -f "$b.stdin" ] && IN="$b.stdin"
  FL=""; case "$f" in tests/io/*) TD=$(mktemp -d); FL="--files $TD";; esac
  eval "\"$BIN\" $FL \"$f\" < $IN > /tmp/prism_out.$$ $ERR"
  [ -n "$FL" ] && rm -rf "$TD"
  if cmp -s /tmp/prism_out.$$ "$b.expected"; then echo "ok   $f"; else echo "FAIL $f"; fail=$((fail+1)); fi
done
rm -f /tmp/prism_out.$$
echo "$((total-fail)) из $total прошли"
[ $fail -eq 0 ]
