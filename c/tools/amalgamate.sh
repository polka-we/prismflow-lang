#!/bin/sh
# Собирает src/prism.c из src/parts/*.c (порядок: a b c d e f).
cd "$(dirname "$0")/.." || exit 1
out=src/prism.c
: > "$out"
for f in a_core a2_big a3_dec b_lex c_parse d_interp d3_vm d2_sched d4_io d5_prelude e_builtins f_api; do
  echo "/* ===== $f ===== */" >> "$out"
  cat "src/parts/$f.c" >> "$out"
done
echo "собрано $out"
