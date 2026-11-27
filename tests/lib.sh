# Funcoes comuns dos testes do hdclone (Linux). Uso: . "$(dirname "$0")/lib.sh"
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HD="$ROOT/hdclone"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/hdtest.XXXXXX")
export LC_ALL=C.UTF-8 MTOOLS_SKIP_CHECK=1
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  ok     - $1"; PASS=$((PASS+1)); }
bad()  { echo "  FALHA  - $1"; FAIL=$((FAIL+1)); }
skip() { echo "  pulado - $1"; SKIP=$((SKIP+1)); }
check() { local d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
has() { command -v "$1" >/dev/null 2>&1; }
need_root() { [ "$(id -u)" = 0 ]; }
finish() { cd /; rm -rf "$TMP"; echo "  [$PASS ok, $FAIL falhas, $SKIP pulados]"; echo "$PASS $FAIL $SKIP" >> "${RESULTS:-/dev/null}"; [ "$FAIL" = 0 ]; }
cd "$TMP"
# disco de teste: MBR com particao NTFS (arquivos via ntfscp, sem precisar montar)
mk_ntfs_disk() { # $1 arquivo  $2 MB da particao
  head -c $(( $2 * 1048576 )) /dev/urandom > p.img; mkntfs -q -F -f p.img >/dev/null 2>&1
  seq 1 200000 > texto.txt; head -c 8M /dev/urandom > dados.bin
  ntfscp -q -f p.img texto.txt texto.txt; ntfscp -q -f p.img dados.bin dados.bin
  head -c 1M /dev/urandom > "$1"; cat p.img >> "$1"; rm p.img
  printf 'label: dos\nstart=2048, size=%d, type=7\n' $(( $2 * 2048 )) | sfdisk -q "$1"
}
part_out() { # extrai a particao 1 de $1 para $2
  local s z; s=$(sfdisk -d "$1" | grep start= | head -1 | sed 's/.*start= *\([0-9]*\).*/\1/')
  z=$(sfdisk -d "$1" | grep start= | head -1 | sed 's/.*size= *\([0-9]*\).*/\1/')
  dd if="$1" of="$2" bs=512 skip="$s" count="$z" status=none
}
