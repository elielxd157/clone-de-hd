#!/bin/bash
# setores defeituosos, retomada, espaco no destino, VSS simulado
. "$(dirname "$0")/lib.sh"; echo "== robustez"
head -c 200M /dev/urandom > src.img
fails(){ HDCLONE_TEST_BAD="$1" $HD backup src.img b.hdc -s 0 --raw $2 2>&1 | grep "leituras com falha" | awk '{print $NF}'; }
f1=$(fails "100000:1" ""); check "1 setor ruim: no maximo 3 falhas (foi $f1)" test "$f1" -le 3
f2=$(fails "200000:40960" ""); check "zona ruim de 20 MB: menos de 300 falhas (foi $f2)" test "$f2" -lt 300
f3=$(fails "200000:40960" "--rescue-all"); check "--rescue-all tenta tudo (foi $f3)" test "$f3" -gt 40000
rm -f r.img; $HD restore b.hdc r.img -y >/dev/null 2>&1
check "setores bons fora da zona ruim preservados" bash -c "cmp <(head -c 100000000 src.img) <(head -c 100000000 r.img)"
# retomada: interrompe com kill -9 e continua
head -c 150M /dev/urandom | base64 -w 0 > txt.img; truncate -s 200M txt.img
(HDCLONE_THREADS=1 $HD backup txt.img i.hdc --raw -c 19 -s 0 >/dev/null 2>&1 &); sleep 15; pkill -9 -f "hdclone backup txt.img"; sleep 1
$HD backup txt.img i.hdc --raw -s 0 -c 19 --resume > o.txt 2>&1
check "retomada: continuou de onde parou" grep -q "ja estao na imagem" o.txt
check "retomada: imagem vazia recomeca do zero" bash -c "rm -f e.hdc*; : > e.hdc; $HD backup src.img e.hdc --raw -s 0 --resume 2>&1 | grep -q 'comecando do inicio' && $HD verify e.hdc | grep -q 'Imagem OK'"
rm -f r2.img; $HD restore i.hdc r2.img -y >/dev/null 2>&1; check "retomada: resultado identico a origem" cmp txt.img r2.img
check "retomada: backup completo e recusado" bash -c "$HD backup txt.img i.hdc --raw -s 0 --resume 2>&1 | grep -q 'ja esta completo'"
# espaco no destino
if need_root; then mkdir pq; mount -t tmpfs -o size=20m tmpfs pq
  check "espaco insuficiente: para antes de copiar" bash -c "$HD backup src.img pq/x.hdc -s 0 --raw 2>&1 | grep -q 'espaco insuficiente'"; umount pq
else skip "espaco no destino (precisa root para montar tmpfs)"; fi
# VSS simulado: particao lida do "snapshot"
head -c 100M /dev/urandom > disk.img; printf 'label: dos\nstart=63, size=100000, type=7\n' | sfdisk -q disk.img 2>/dev/null
head -c $((100000*512)) /dev/urandom > s1.img; mkntfs -q -F -f -p 63 s1.img >/dev/null 2>&1
HDCLONE_TEST_SNAP="$((63*512)):$((100000*512)):$TMP/s1.img" $HD backup disk.img v.hdc --raw -s 0 >/dev/null 2>&1; rm -f o2.img; $HD restore v.hdc o2.img -y >/dev/null 2>&1
check "VSS simulado: particao vem do snapshot" bash -c "cmp s1.img <(dd if=o2.img bs=512 skip=63 count=100000 status=none)"
check "VSS simulado: resto vem do disco" bash -c "cmp <(head -c 32256 disk.img) <(head -c 32256 o2.img)"
finish
