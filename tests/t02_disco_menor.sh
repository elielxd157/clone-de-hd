#!/bin/bash
# restaurar em disco menor (reducao da ultima particao)
. "$(dirname "$0")/lib.sh"; echo "== disco menor"
has mkntfs && has mkfs.vfat && has ntfsresize || { skip "ntfs-3g/dosfstools ausentes"; finish; exit; }
mk(){ # $1 rotulo  $2 tipo da 2a particao (ntfs|fat32|logical)
  rm -f d.img; head -c 400M /dev/urandom > d.img
  if [ $2 = logical ]; then printf 'label: dos\nstart=2048, size=204800, type=7\nstart=206848, type=5\nstart=208896, type=7\n' | sfdisk -q d.img
  elif [ $1 = gpt ]; then printf 'label: gpt\nstart=2048, size=204800, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\nstart=206848, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n' | sfdisk -q d.img
  else T2=7; [ $2 = fat32 ] && T2=c; printf "label: dos\nstart=2048, size=204800, type=7\nstart=206848, type=$T2\n" | sfdisk -q d.img; fi
  read S Z < <(sfdisk -d d.img | grep -v "type=5" | grep start= | tail -1 | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/')
  head -c $((Z*512)) /dev/urandom > p2.img
  if [ $2 = fat32 ]; then mkfs.vfat -F 32 -s 1 p2.img >/dev/null; mcopy -i p2.img texto.txt ::texto.txt
  else mkntfs -q -F -f p2.img >/dev/null 2>&1; ntfscp -q -f p2.img texto.txt texto.txt; fi
  dd if=p2.img of=d.img bs=512 seek=$S conv=notrunc status=none; echo $S
}
seq 1 200000 > texto.txt
for cfg in "dos ntfs" "gpt ntfs" "dos logical" "dos fat32"; do
  set -- $cfg; S=$(mk $1 $2); $HD backup d.img i.hdc -s 0 >/dev/null 2>&1; head -c 270M /dev/urandom > r.img
  if $HD restore i.hdc r.img -y >/dev/null 2>&1; then
    read S2 Z2 < <(sfdisk -d r.img | grep -v "type=5" | grep start= | tail -1 | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/')
    dd if=r.img of=rp.img bs=512 skip=$S2 count=$Z2 status=none
    if [ $2 = fat32 ]; then check "$cfg: arquivo e fsck" bash -c "mtype -i rp.img ::texto.txt | cmp - texto.txt && fsck.vfat -n rp.img"
    else check "$cfg: NTFS reduzido consistente (ntfsresize)" ntfsresize -i -f rp.img
         cp rp.img rc.img; ntfsfix -d rc.img >/dev/null 2>&1; check "$cfg: arquivo intacto" bash -c "ntfscat rc.img texto.txt | cmp - texto.txt"; fi
    [ $1 = gpt ] && has sgdisk && check "$cfg: GPT valida" bash -c "sgdisk -v r.img | grep -q 'No problems'"
  else bad "$cfg: restauracao falhou"; fi
done
mk dos ntfs >/dev/null; $HD backup d.img i.hdc -s 0 >/dev/null 2>&1; head -c 150M /dev/urandom > r.img; md=$(md5sum < r.img)
check "nao cabe: avisa o minimo" bash -c "$HD restore i.hdc r.img -y 2>&1 | grep -q 'Disco minimo'"
check "nao cabe: destino intacto" test "$(md5sum < r.img)" = "$md"
finish
