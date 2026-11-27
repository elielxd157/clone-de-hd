#!/bin/bash
# backup / verificacao / restauracao, formato, indice, log, compatibilidade
. "$(dirname "$0")/lib.sh"; echo "== basico"
mk_ntfs_disk d.img 200
$HD backup d.img raw.hdc --raw -s 0 >/dev/null 2>&1; rm -f r.img; $HD restore raw.hdc r.img -y >/dev/null 2>&1
check "modo --raw: restauracao byte a byte" cmp d.img r.img
$HD backup d.img a.hdc -s 2 --verify > out.txt 2>&1
check "backup inteligente + --verify" grep -q "Imagem OK" out.txt
check "relatorio .log gravado com resultado" grep -q "Resultado: OK" a.hdc.log
check "imagem dividida em partes de 2 MB" test -f a.hdc.004
rm -f r2.img; $HD restore a.hdc r2.img -y >/dev/null 2>&1; part_out r2.img rp.img
check "restauracao: arquivo de texto" bash -c "ntfscat rp.img texto.txt | cmp - texto.txt"
check "restauracao: arquivo binario" bash -c "ntfscat rp.img dados.bin | cmp - dados.bin"
check "indice no fim da imagem e lido" bash -c "$HD ls a.hdc | grep -q 'indice da imagem lido'"
cp a.hdc.003 bak; printf 'XXXXXXXX' | dd of=a.hdc.003 bs=1 seek=100000 conv=notrunc status=none
check "imagem corrompida e detectada" bash -c "$HD verify a.hdc 2>&1 | grep -q 'CRC invalido'"
mv bak a.hdc.003; mv a.hdc.004 x
check "parte faltando e detectada" bash -c "$HD verify a.hdc 2>&1 | grep -q 'falta alguma parte'"
mv x a.hdc.004
$HD backup d.img c.hdc -s 0 --compat >/dev/null 2>&1
check "--compat gera imagem versao 1 (zlib)" bash -c "$HD info c.hdc | grep -q zlib"
check "--compat restaura igual" bash -c "rm -f r3.img; $HD restore c.hdc r3.img -y >/dev/null 2>&1 && cmp <(head -c 300000000 r2.img) <(head -c 300000000 r3.img)"
head -c 192M /dev/zero > alvo.img; printf 'ANTIGO' | dd of=alvo.img conv=notrunc status=none; printf 'XXXX' | dd of=c.hdc bs=1 seek=$(( $(stat -c %s c.hdc) / 2 )) conv=notrunc status=none
$HD restore c.hdc alvo.img -y --verify >/dev/null 2>&1
check "restore --verify nao toca no destino se a imagem estiver ruim" bash -c "head -c 6 alvo.img | grep -q ANTIGO"
finish
