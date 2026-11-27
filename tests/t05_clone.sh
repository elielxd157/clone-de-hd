#!/bin/bash
# clone disco a disco: mesmo tamanho, --raw, destino menor, GPT maior, erros
. "$(dirname "$0")/lib.sh"; echo "== clone"
seq 1 200000 > texto.txt
head -c 300M /dev/urandom > s.img; printf 'label: dos\nstart=2048, size=204800, type=c\nstart=206848, size=405504, type=7\n' | sfdisk -q s.img
head -c 100M /dev/zero > f.img; mkfs.vfat -F 32 -s 1 f.img >/dev/null; mcopy -i f.img texto.txt ::/
head -c $((405504*512)) /dev/urandom > n.img; mkntfs -q -F -f n.img >/dev/null 2>&1; ntfscp -q -f n.img texto.txt texto.txt
dd if=f.img of=s.img bs=512 seek=2048 conv=notrunc status=none; dd if=n.img of=s.img bs=512 seek=206848 conv=notrunc status=none
head -c 300M /dev/urandom > t1.img; $HD clone s.img t1.img -y --verify > o.txt 2>&1
check "clone mesmo tamanho + --verify" grep -q "conferido" o.txt
dd if=t1.img of=a.img bs=512 skip=2048 count=204800 status=none; dd if=t1.img of=b.img bs=512 skip=206848 count=405504 status=none
check "clone: FAT32 integro" bash -c "mtype -i a.img ::texto.txt | cmp - texto.txt && fsck.vfat -n a.img"
check "clone: NTFS integro" bash -c "ntfscat b.img texto.txt | cmp - texto.txt"
rm -f t2.img; $HD clone s.img t2.img -y --raw >/dev/null 2>&1; check "clone --raw byte a byte" cmp s.img t2.img
head -c 260M /dev/urandom > t3.img; $HD clone s.img t3.img -y --verify > o.txt 2>&1
check "clone para disco menor" grep -q "reduzida de" o.txt
Z=$(sfdisk -d t3.img | grep start= | tail -1 | sed 's/.*size= *\([0-9]*\).*/\1/'); dd if=t3.img of=c.img bs=512 skip=206848 count=$Z status=none
check "clone menor: NTFS consistente" ntfsresize -i -f c.img
check "mesmo disco: recusado" bash -c "$HD clone s.img s.img -y 2>&1 | grep -q 'mesmo disco'"
head -c 100M /dev/urandom > t4.img; md=$(md5sum < t4.img); $HD clone s.img t4.img -y >/dev/null 2>&1
check "destino pequeno demais: intacto" test "$(md5sum < t4.img)" = "$md"
finish
