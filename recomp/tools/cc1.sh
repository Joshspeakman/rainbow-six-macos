#!/usr/bin/env bash
# cc1.sh <outdir> <file.c> : compile one generated file (called in parallel by build.sh)
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FLAGS=()
if [[ -f "$1/compiler-flags.txt" ]]; then read -ra FLAGS < "$1/compiler-flags.txt"; fi
exec "${CC:-clang}" "${FLAGS[@]}" -c -O2 -fPIC -gline-tables-only -ffp-contract=off -fno-strict-aliasing -I "$HERE/runtime" -I "$1" -o "$1/obj/$(basename "$2" .c).o" "$2"
