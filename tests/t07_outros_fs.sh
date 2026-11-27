#!/bin/bash
# copia inteligente de ext2/3/4 e exFAT; pontos de restauracao gravados como zeros
. "$(dirname "$0")/lib.sh"; echo "== ext4 / exFAT / pontos de restauracao"
mkdir src; for i in $(seq 1 60); do head -c $((RANDOM*10)) /dev/urandom > src/f$i.bin; done; head -c 15M /dev/urandom > src/grande.bin
if has mkfs.ext4 && has debugfs; then for opt in "" "-t ext2" "-b 1024 -g 4096"; do
  head -c 300M /dev/urandom > p.img; mkfs.ext4 -q -F -E nodiscard $opt p.img >/dev/null 2>&1
  { echo "mkdir d"; for f in src/*; do echo "write $f d/$(basename $f)"; done; } | debugfs -w p.img >/dev/null 2>&1
  head -c 1M /dev/zero > d.img; cat p.img >> d.img; printf 'label: dos\nstart=2048, size=614400, type=83\n' | sfdisk -q d.img
  $HD backup d.img e.hdc -s 0 >/dev/null 2>&1; sz=$(stat -c %s e.hdc)
  check "ext [${opt:-padrao}]: imagem pequena ($((sz/1048576)) MB de 300)" test $sz -lt 120000000
  head -c 301M /dev/urandom > r.img; $HD restore e.hdc r.img -y >/dev/null 2>&1; part_out r.img rp.img
  check "ext [${opt:-padrao}]: e2fsck limpo" e2fsck -fn rp.img
  okf=1; for f in src/*; do debugfs -R "dump d/$(basename $f) o.tmp" rp.img >/dev/null 2>&1; cmp -s o.tmp $f || okf=0; done
  check "ext [${opt:-padrao}]: arquivos identicos" test $okf = 1
done; else skip "e2fsprogs ausente"; fi
if need_root && has mkfs.exfat && has mount.exfat-fuse && losetup -f >/dev/null 2>&1; then
  mkdir m; head -c 200M /dev/urandom > p.img; mkfs.exfat p.img >/dev/null 2>&1; L=$(losetup -f --show p.img); mount.exfat-fuse $L m >/dev/null 2>&1; cp -r src m/; sync; umount m; losetup -d $L
  head -c 1M /dev/zero > d.img; cat p.img >> d.img; printf 'label: dos\nstart=2048, size=409600, type=7\n' | sfdisk -q d.img
  $HD backup d.img x.hdc -s 0 >/dev/null 2>&1; check "exFAT: imagem pequena" test $(stat -c %s x.hdc) -lt 80000000
  head -c 201M /dev/urandom > r.img; $HD restore x.hdc r.img -y >/dev/null 2>&1; part_out r.img rp.img
  check "exFAT: fsck limpo" bash -c "fsck.exfat -n rp.img | grep -q clean"
  L=$(losetup -f --show rp.img); mount.exfat-fuse -o ro $L m >/dev/null 2>&1; check "exFAT: arquivos identicos" diff -r src m/src; umount m; losetup -d $L
else skip "exFAT (precisa root, loop e exfat-fuse)"; fi
if need_root && has ntfs-3g && [ -e /dev/fuse ]; then
  mkdir -p m; head -c 200M /dev/zero > n.img; mkntfs -q -F -f n.img >/dev/null 2>&1; ntfs-3g n.img m
  mkdir -p "m/System Volume Information" m/Windows; G="{3808876b-c176-4e48-b7ae-04046e6cc752}"
  head -c 30M /dev/urandom > "m/System Volume Information/{1111}$G"; head -c 20M /dev/urandom > m/Windows/MEMORY.DMP; cp src/grande.bin m/; umount m
  head -c 1M /dev/zero > d.img; cat n.img >> d.img; printf 'label: dos\nstart=2048, size=409600, type=7\n' | sfdisk -q d.img
  $HD backup d.img a.hdc -s 0 >/dev/null 2>&1; $HD backup d.img z.hdc -s 0 --skip-restore-points >/dev/null 2>&1
  check "pontos de restauracao: imagem ao menos 45 MB menor" test $(( $(stat -c %s a.hdc) - $(stat -c %s z.hdc) )) -gt 47000000
  rm -f r.img; $HD restore z.hdc r.img -y >/dev/null 2>&1; part_out r.img rp.img; ntfs-3g -o ro rp.img m
  check "arquivos descartados existem, zerados" bash -c "[ \$(stat -c %s m/Windows/MEMORY.DMP) = 20971520 ] && [ \$(tr -d '\\0' < m/Windows/MEMORY.DMP | wc -c) = 0 ]"
  check "outros arquivos intactos" cmp m/grande.bin src/grande.bin; umount m
  check "NTFS consistente" bash -c "ntfsfix -n rp.img | grep -q successfully"
else skip "pontos de restauracao (precisa root + FUSE)"; fi
finish
