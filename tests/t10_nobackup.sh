#!/bin/bash
# lista "nao fazer backup" do Windows (FilesNotToBackup): leitura do registro e medicao
. "$(dirname "$0")/lib.sh"; echo "== lista 'nao fazer backup' do Windows"
has hivexsh || { skip "hivex ausente (apt install libhivex-bin)"; finish; exit; }
python3 "$ROOT/tests/hive.py" minimal sys.hiv
msz(){ python3 -c 'import sys; b=b"".join(s.encode("utf-16-le")+b"\0\0" for s in sys.argv[1:])+b"\0\0"; print(",".join("%02x"%c for c in b))' "$@"; }
{ echo "add Select"; echo "cd Select"; printf 'setval 1\nCurrent\ndword:00000001\n'
  echo "cd \\"; echo "add ControlSet001"; echo "cd ControlSet001"; echo "add Control"; echo "cd Control"; echo "add BackupRestore"
  echo "cd BackupRestore"; echo "add FilesNotToBackup"; echo "cd FilesNotToBackup"
  printf 'setval 4\nTemporary Files\nhex:7:%s\nLogs\nhex:7:%s\nRepetido\nhex:7:%s\nPaging File\nhex:7:%s\n' \
    "$(msz '%TEMP%\* /s' '%Foo%\x.dat')" "$(msz '%systemroot%\Logs\*.etl')" "$(msz 'C:\Windows\Temp\* /s')" "$(msz '\Pagefile.sys')"
  echo commit; } | hivexsh -w sys.hiv >/dev/null 2>&1
check "registro de teste com valores REG_MULTI_SZ" bash -c "hivexsh sys.hiv <<<'cd \\ControlSet001\\Control\\BackupRestore\\FilesNotToBackup
lsval' | grep -q Paging"
head -c 150M /dev/zero > p.img; mkfs.vfat -F 32 -s 1 p.img >/dev/null
mmd -i p.img ::/Windows ::/Windows/System32 ::/Windows/System32/config ::/Windows/Temp ::/Windows/Temp/sub ::/Windows/Logs \
  ::/Users ::/Users/joao ::/Users/joao/AppData ::/Users/joao/AppData/Local ::/Users/joao/AppData/Local/Temp
mcopy -i p.img sys.hiv ::/Windows/System32/config/SYSTEM
put(){ head -c $2 /dev/urandom > f.bin; mcopy -i p.img f.bin "::/$1"; }
put Windows/Temp/a.tmp 1048576; put Windows/Temp/sub/b.tmp 524288; put Users/joao/AppData/Local/Temp/x.dat 2097152
put Windows/Logs/rastro.etl 102400; put Windows/Logs/manter.log 300000; put pagefile.sys 3145728; put x.dat 50000
head -c 1M /dev/zero > d.img; cat p.img >> d.img; printf 'label: dos\nstart=2048, size=307200, type=c\n' | sfdisk -q d.img 2>/dev/null
md5_0=$(md5sum d.img)
$HD nobackup d.img > o.txt 2>&1; cat o.txt
check "Temp (Windows e usuario, com subpastas): 3 arquivos" grep -qE "Temporary Files +3\.5 MB +3 arquivo" o.txt
check "mascara *.etl so pega o .etl" grep -qE "Logs +100(\.0)? KB +1 arquivo" o.txt
check "arquivo repetido em dois itens conta uma vez so" bash -c "! grep -q Repetido o.txt"
check "pagefile marcado como ja excluido" grep -qE "Paging File .*ja deixa de fora" o.txt
check "total: 4 arquivos" grep -q "em 4 arquivo" o.txt
check "disco nao foi alterado" bash -c "[ \"\$(md5sum d.img)\" = '$md5_0' ]"
head -c 20M /dev/zero > vazio.img; check "disco sem Windows: erro claro" bash -c "$HD nobackup vazio.img 2>&1 | grep -q 'nenhuma instalacao'"
finish
