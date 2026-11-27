#!/bin/bash
# recuperar arquivos: NTFS e FAT32, busca, nomes acentuados, arquivo fragmentado
. "$(dirname "$0")/lib.sh"; echo "== recuperar arquivos"
# FAT32 (mtools, sem montar nada)
mkdir src; for i in $(seq 1 300); do echo "arquivo $i" > src/f$i.txt; done; echo "acento ok" > "src/Relatório Ção.txt"
head -c 40M /dev/zero > p.img; mkfs.vfat -F 32 -s 1 -n DADOS p.img >/dev/null 2>&1
mmd -i p.img ::/Docs "::/Docs/Relatórios"; mcopy -i p.img src/f*.txt ::/Docs/; mcopy -i p.img "src/Relatório Ção.txt" "::/Docs/Relatórios/"
for i in $(seq 1 1000); do head -c 16384 /dev/urandom > t$i; done; mcopy -i p.img t* ::/ 2>/dev/null
for i in $(seq 1 2 1000); do mdel -i p.img ::/t$i; done; head -c 6M /dev/urandom > frag.bin; mcopy -i p.img frag.bin ::/
head -c 1M /dev/zero > d.img; cat p.img >> d.img; printf 'label: dos\nstart=2048, size=81920, type=c\n' | sfdisk -q d.img
$HD backup d.img f.hdc -s 0 >/dev/null 2>&1
check "FAT32: lista particao com rotulo" bash -c "$HD ls f.hdc | grep -q 'FAT32.*DADOS'"
check "FAT32: pasta com 300 arquivos" bash -c "$HD ls f.hdc 1 Docs | grep -q '301 itens'"
check "FAT32: busca com curinga e acento" bash -c "$HD find f.hdc 1 'relat*' | grep -q 'Relatório Ção.txt'"
$HD extract f.hdc 1 / out >/dev/null 2>&1
check "FAT32: arquivo fragmentado identico" cmp out/particao/frag.bin frag.bin
check "FAT32: nome com acento identico" cmp "out/particao/Docs/Relatórios/Relatório Ção.txt" "src/Relatório Ção.txt"
check "FAT32: 300 arquivos" bash -c "diff <(ls src/f*.txt | xargs -n1 basename | sort) <(ls out/particao/Docs/f*.txt | xargs -n1 basename | sort)"
# NTFS com pastas (precisa montar com ntfs-3g)
if need_root && has ntfs-3g && [ -e /dev/fuse ]; then
  mkdir m; head -c 200M /dev/zero > n.img; mkntfs -q -F -f n.img >/dev/null 2>&1; ntfs-3g n.img m
  mkdir -p m/Users/Ana; cp src/f*.txt m/Users/Ana/; head -c 20M /dev/urandom > m/grande.bin; cp m/grande.bin .
  (printf 'HIBR'; head -c 8M /dev/urandom) > m/hiberfil.sys; umount m
  head -c 1M /dev/zero > d2.img; cat n.img >> d2.img; printf 'label: dos\nstart=2048, size=409600, type=7\n' | sfdisk -q d2.img
  $HD backup d2.img n.hdc -s 9 > b.txt 2>&1
  check "NTFS: hiberfil.sys deixado de fora" grep -q "hiberfil.sys" b.txt
  $HD extract n.hdc 1 / o2 >/dev/null 2>&1
  check "NTFS: pasta extraida identica" diff -r src/ o2/particao/Users/Ana/ -x 'Relat*' -x 'frag.bin'
  check "NTFS: arquivo de 20 MB identico" cmp o2/particao/grande.bin grande.bin
  check "NTFS: busca" bash -c "$HD find n.hdc 1 'f29?.txt' | grep -q '10 encontrado'"
  rm -f r.img; $HD restore n.hdc r.img -y >/dev/null 2>&1; part_out r.img rp.img; ntfs-3g -o ro rp.img m
  check "NTFS: hiberfil restaurado comeca com zeros" bash -c "[ \$(head -c 4096 m/hiberfil.sys | tr -d '\\0' | wc -c) = 0 ]"; umount m
else skip "NTFS com pastas (precisa root + FUSE)"; fi
finish
