#!/bin/bash
# Roda todos os testes. Uso: make test   (alguns testes precisam de root: sudo make test)
cd "$(dirname "$0")"; export RESULTS=$(mktemp); rc=0
for t in t0*.sh; do bash "$t" || rc=1; done
awk '{p+=$1; f+=$2; s+=$3} END {printf "\nTOTAL: %d ok, %d falhas, %d pulados\n", p, f, s}' "$RESULTS"; rm -f "$RESULTS"
exit $rc
