#!/bin/sh
# Builds engine.js (Node-usable engine) and ../docs/index.html (the self-contained playground).
# The standard library written in Prismflow is taken from ../c/lib/prelude.pr (shared with the C core).
cd "$(dirname "$0")" || exit 1
node -e "require('fs').writeFileSync('p35_prelude.js', 'const PRELUDE_SRC = ' + JSON.stringify(require('fs').readFileSync('../c/lib/prelude.pr','utf8')) + ';\n')"
cat p0_dec.js p1_parser.js p15_layout.js p2_interp.js p25_async.js p3_builtins.js p35_prelude.js > engine.js && node build.js
