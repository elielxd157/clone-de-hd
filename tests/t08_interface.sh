#!/bin/bash
# caminho da interface grafica (erros/cancelamento sem fechar o programa) + extracao
. "$(dirname "$0")/lib.sh"; echo "== caminho da interface"
gcc -O1 -w -I"$ROOT" -o gc "$ROOT/tests/gui_core.c" "$ROOT/zstdlin.o" -lz -lpthread && gcc -O1 -w -I"$ROOT" -o gx "$ROOT/tests/gui_extract.c" "$ROOT/zstdlin.o" -lz -lpthread || { bad "compilacao"; finish; exit; }
head -c 64M /dev/urandom > d.img; printf 'label: dos\nstart=2048, type=7\n' | sfdisk -q d.img
head -c $((129024*512)) /dev/urandom > p.img; mkntfs -q -F -f p.img >/dev/null 2>&1; seq 1 99999 > t.txt; ntfscp -q -f p.img t.txt t.txt
dd if=p.img of=d.img bs=512 seek=2048 conv=notrunc status=none
HDCLONE_TEST_SNAP="1048576:$((129024*512)):$TMP/p.img" ./gc d.img i.hdc r.img > o.txt 2>&1
for t in "erro origem: ok=0" "cancelado:   ok=0" "backup:     ok=1" "verify:     ok=1" "restore:    ok=1" "clone:     ok=1" "clone cancelado: ok=0" "corrompida: ok=0"; do
  check "interface: $t" grep -q "$t" o.txt; done
check "interface: nenhum recurso fica aberto" bash -c "! grep 'recursos=' o.txt | grep -qv 'recursos=0'"
check "interface: .log fechado apos erro" grep -q "log ainda aberto? nao" o.txt
mkdir out; ./gx i.hdc out > x.txt 2>&1
check "extracao cancelada e refeita com a imagem aberta" grep -q "2a extracao.*cancelada=0.*erros=0" x.txt
finish
