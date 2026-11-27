#!/bin/bash
# preparar para outro hardware: registro SYSTEM de XP, 7 e 10 (conferido com hivex)
. "$(dirname "$0")/lib.sh"; echo "== preparar para outro hardware"
has hivexsh && has hivexget || { skip "hivex ausente (apt install libhivex-bin)"; finish; exit; }
python3 "$ROOT/tests/hive.py" minimal minimal.hiv
gen(){ cp minimal.hiv $1; { echo "add Select"; for cs in ControlSet001 ControlSet002; do echo "cd \\"; echo "add $cs"; echo "cd \\$cs"; echo "add Control"; echo "add Services"
  echo "cd \\$cs\\Control"; echo "add CriticalDeviceDatabase"; echo "cd CriticalDeviceDatabase"; [ $2 = xp ] && echo "add pci#cc_0101"
  echo "cd \\$cs\\Services"; for i in $(seq 1 1200); do echo "add Svc$i"; done
  for sv in atapi:0 intelide:4 pciide:3 $3; do n=${sv%:*}; v=${sv#*:}; echo "cd \\$cs\\Services"; echo "add $n"; echo "cd $n"; printf 'setval 1\nStart\ndword:%08x\n' $v; done
  if [ $2 = w10 ]; then for d in storahci stornvme; do echo "cd \\$cs\\Services\\$d"; echo "add StartOverride"; echo "cd StartOverride"; printf 'setval 1\n0\ndword:00000003\n'; done; fi
  done; echo commit; } | hivexsh -w $1 >/dev/null 2>&1; }
gen xp.hiv xp ""; gen w7.hiv w7 "msahci:3 iaStorV:3"; gen w10.hiv w10 "storahci:3 stornvme:3 iaStorV:3"
python3 "$ROOT/tests/hive.py" ri w10.hiv w10ri.hiv
check "hive com lista 'ri' valido para o hivex" bash -c "hivexget w10ri.hiv '\\ControlSet001\\Services\\storahci' Start | grep -q 3"
mkdisk(){ head -c 150M /dev/zero > p.img; mkfs.vfat -F 32 -s 1 p.img >/dev/null; mmd -i p.img ::/$2 ::/$2/$3 ::/$2/$3/config; mcopy -i p.img $1 ::/$2/$3/config/$4
  head -c 1M /dev/zero > d.img; cat p.img >> d.img; printf 'label: dos\nstart=2048, size=307200, type=c\n' | sfdisk -q d.img; }
gethive(){ part_out $1 rp.img; mcopy -o -i rp.img ::/$2 got.hiv; }
val(){ hivexget got.hiv "\\$1" "$2" 2>/dev/null; }
mkdisk w10ri.hiv Windows System32 SYSTEM; $HD backup d.img i.hdc -s 0 >/dev/null 2>&1; rm -f r.img
$HD restore i.hdc r.img -y --hwfix > o.txt 2>&1; gethive r.img Windows/System32/config/SYSTEM
check "Win10: 14 alteracoes" grep -q "14 alteracao" o.txt
check "Win10: storahci/stornvme Start=0 nos 2 ControlSets" bash -c "[ \$(hivexget got.hiv '\\ControlSet001\\Services\\storahci' Start) = 0 ] && [ \$(hivexget got.hiv '\\ControlSet002\\Services\\stornvme' Start) = 0 ]"
check "Win10: StartOverride zerado" bash -c "[ \$(hivexget got.hiv '\\ControlSet001\\Services\\storahci\\StartOverride' 0) = 0 ]"
check "Win10: so 14 bytes do registro mudaram" bash -c "[ \$(cmp -l w10ri.hiv got.hiv | wc -l) = 14 ]"
mkdisk w7.hiv Windows System32 SYSTEM; $HD hwfix d.img > o.txt 2>&1; gethive d.img Windows/System32/config/SYSTEM
check "Win7 (comando hwfix): msahci e iaStorV Start=0" bash -c "[ \$(hivexget got.hiv '\\ControlSet001\\Services\\msahci' Start) = 0 ] && [ \$(hivexget got.hiv '\\ControlSet001\\Services\\iaStorV' Start) = 0 ]"
check "segunda execucao: ja estava preparado" bash -c "$HD hwfix d.img | grep -q 'ja estava preparado'"
mkdisk xp.hiv WINDOWS system32 system; $HD hwfix d.img > o.txt 2>&1; gethive d.img WINDOWS/system32/config/system
check "XP (FAT32, pasta WINDOWS): intelide/pciide Start=0" bash -c "[ \$(hivexget got.hiv '\\ControlSet001\\Services\\intelide' Start) = 0 ] && [ \$(hivexget got.hiv '\\ControlSet001\\Services\\pciide' Start) = 0 ]"
check "XP: dica sobre modo AHCI" grep -q AHCI o.txt
head -c 20M /dev/zero > vazio.img; check "disco sem Windows: erro claro" bash -c "$HD hwfix vazio.img 2>&1 | grep -q 'nenhuma instalacao'"
finish
