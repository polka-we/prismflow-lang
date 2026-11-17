#!/bin/sh
# Runs the shared parity tests (../c/tests/parity) on the JavaScript engine and compares with the expected output
# produced by the C core. Usage: web/tests/parity.sh   (build the engine first: web/mk_engine.sh)
cd "$(dirname "$0")/../.." || exit 1
fail=0; total=0
for f in c/tests/parity/*.pr; do
  b=${f%.pr}
  case "$f" in *time*|*par_*) continue;; esac
  total=$((total+1))
  IN=/dev/null; [ -f "$b.stdin" ] && IN="$b.stdin"
  # time-dependent and thread tests are not comparable
  node web/run.js "$f" "$IN" > /tmp/pf_js.$$ 2>&1
  if cmp -s /tmp/pf_js.$$ "$b.expected"; then :; else echo "DIFF $f"; fail=$((fail+1)); fi
done
rm -f /tmp/pf_js.$$
echo "JS engine: $((total-fail)) of $total parity tests match"
[ $fail -eq 0 ]
