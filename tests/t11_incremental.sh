#!/bin/bash
# backup incremental: cadeia completa -> inc1 -> inc2, restauracao, arquivos, erros
. "$(dirname "$0")/lib.sh"; echo "== backup incremental"
mk_ntfs_disk d.img 200
part_back(){ dd if=p.img of=d.img bs=512 seek=2048 conv=notrunc status=none; }
part_out d.img p.img; head -c 40M /dev/urandom > estavel.bin; ntfscp -q -f p.img estavel.bin estavel.bin; part_back
mkdir -p bk
$HD backup d.img bk/full.hdc -s 3 >/dev/null 2>&1; cp d.img d0.img
# 1a alteracao: arquivo novo de 6 MB + 1 MB alterado dentro do dados.bin
part_out d.img p.img; head -c 6M /dev/urandom > novo.bin; ntfscp -q -f p.img novo.bin novo.bin
head -c 8M /dev/urandom > dados.bin; ntfscp -q -f p.img dados.bin dados.bin; part_back; cp d.img d1.img
$HD backup d.img bk/inc1.hdc --base bk/full.hdc -s 3 --verify > o1.txt 2>&1; cat o1.txt | grep -E "Incremental|Imagem:|Imagem OK|erro" | sed 's/^/        /'
full=$(du -cb bk/full.hdc* | tail -1 | cut -f1); inc=$(du -cb bk/inc1.hdc* | tail -1 | cut -f1)
check "incremental + verificacao (le os blocos iguais da base)" grep -q "Imagem OK" o1.txt
check "incremental bem menor que o completo ($((inc/1048576)) MB x $((full/1048576)) MB)" test $inc -lt $((full / 2))
check "info mostra de qual imagem depende" bash -c "$HD info bk/inc1.hdc | grep -q 'desde \"full.hdc\"'"
# 2a alteracao: so um arquivo pequeno
part_out d.img p.img; seq 1 5000 > pequeno.txt; ntfscp -q -f p.img pequeno.txt pequeno.txt; part_back; cp d.img d2.img
$HD backup d.img bk/inc2.hdc --base bk/inc1.hdc -s 0 > o2.txt 2>&1
inc2=$(stat -c %s bk/inc2.hdc)
check "incremental do incremental: so a mudanca pequena ($((inc2/1024)) KB)" test $inc2 -lt 3000000
rm -f r.img; $HD restore bk/inc2.hdc r.img -y --verify >/dev/null 2>&1; part_out r.img rp.img
check "restaurar inc2: arquivo do completo" bash -c "ntfscat rp.img texto.txt | cmp - texto.txt"
check "restaurar inc2: alteracoes do inc1" bash -c "ntfscat rp.img dados.bin | cmp - dados.bin && ntfscat rp.img novo.bin | cmp - novo.bin"
check "restaurar inc2: alteracao do inc2" bash -c "ntfscat rp.img pequeno.txt | cmp - pequeno.txt"
check "restaurar inc2: disco identico ao estado 2" bash -c "$HD backup d2.img x.hdc -s 0 >/dev/null 2>&1; rm -f rx.img; $HD restore x.hdc rx.img -y >/dev/null 2>&1; cmp r.img rx.img"
rm -f r.img; $HD restore bk/inc1.hdc r.img -y >/dev/null 2>&1; part_out r.img rp.img
check "restaurar inc1: estado 1 (sem o pequeno.txt)" bash -c "ntfscat rp.img novo.bin | cmp - novo.bin && ! ntfscat rp.img pequeno.txt >/dev/null 2>&1"
check "recuperar arquivo de dentro do incremental" bash -c "rm -rf ex; $HD extract bk/inc2.hdc 1 novo.bin ex >/dev/null 2>&1 && cmp ex/novo.bin novo.bin"
head -c 190M /dev/zero > m.img; $HD restore bk/inc2.hdc m.img -y > om.txt 2>&1; part_out m.img mp.img; ntfsfix -d mp.img >/dev/null 2>&1
check "restaurar incremental em disco menor" bash -c "grep -q 'sera reduzida' om.txt && ntfscat mp.img estavel.bin | cmp - estavel.bin && ntfscat mp.img pequeno.txt | cmp - pequeno.txt"
# erros
mv bk/full.hdc bk/x.hdc
check "falta a base: erro claro" bash -c "$HD restore bk/inc2.hdc r.img -y 2>&1 | grep -q 'falta a imagem base \"full.hdc\"'"
mv bk/x.hdc bk/full.hdc
$HD backup d0.img outra.hdc -s 0 >/dev/null 2>&1
cp -r bk bk_save; rm bk/full.hdc*; cp outra.hdc bk/full.hdc
check "base substituida por outro backup: erro claro" bash -c "$HD verify bk/inc1.hdc 2>&1 | grep -q 'nao e a base usada'"
rm -rf bk; mv bk_save bk
check "cadeia restaurada volta a funcionar" bash -c "$HD verify bk/inc2.hdc 2>&1 | grep -q 'Imagem OK'"
check "incremental com o mesmo nome da base: recusado" bash -c "$HD backup d.img bk/full.hdc --base bk/full.hdc 2>&1 | grep -q 'nome diferente'"
mkdir -p outra; check "incremental em outra pasta: recusado" bash -c "$HD backup d.img outra/i.hdc --base bk/full.hdc 2>&1 | grep -q 'mesma pasta'"
cp bk/full.hdc* .; python3 - <<'PY'
import struct, zlib, glob
n = max(glob.glob('full.hdc*'), key=lambda x: int(x.rsplit('.', 1)[1]) if x[-1].isdigit() else 1)
f = open(n, 'r+b'); f.seek(-64, 2); t = bytearray(f.read(64)); struct.pack_into('<I', t, 56, 0)
struct.pack_into('<I', t, 60, zlib.crc32(bytes(t[:60]))); f.seek(-64, 2); f.write(t)
PY
check "base sem tabela de blocos (versao anterior): erro claro" bash -c "$HD backup d.img semtab.hdc --base full.hdc 2>&1 | grep -q 'backup completo'"
head -c 100M /dev/zero > pq.img; check "base de outro disco: recusado" bash -c "$HD backup pq.img bk/q.hdc --base bk/full.hdc 2>&1 | grep -q 'outro disco'"
check "base intacta depois de tudo" bash -c "$HD verify bk/full.hdc 2>&1 | grep -q 'Imagem OK'"
finish
