#!/bin/sh
# lib/prelude.pr -> src/parts/d5_prelude.c (строковая константа PRELUDE_SRC)
cd "$(dirname "$0")/.." || exit 1
awk 'BEGIN { print "/* сгенерировано tools/gen_prelude.sh из lib/prelude.pr — не править вручную */"; print "static const char PRELUDE_SRC[] =" }
{ gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); print "\"" $0 "\\n\"" }
END { print ";" }' lib/prelude.pr > src/parts/d5_prelude.c
echo "собрано src/parts/d5_prelude.c"
