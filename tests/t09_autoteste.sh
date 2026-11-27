#!/bin/bash
# o proprio autoteste embutido (hdclone selftest) tem que passar
. "$(dirname "$0")/lib.sh"; echo "== autoteste embutido"
TMPDIR="$TMP" $HD selftest > o.txt 2>&1; rc=$?
check "hdclone selftest termina sem falhas" test $rc = 0
check "resumo sem falhas" grep -q "RESULTADO: .* 0 falha" o.txt
check "arquivos temporarios apagados" bash -c "! ls -d $TMP/hdclone-autoteste-* 2>/dev/null | grep -q ."
check "relatorio gravado" grep -q "RESULTADO" "$TMP/hdclone-autoteste.log"
finish
