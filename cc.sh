#!/bin/bash
set -eu

PLUGIN="libarchitsmbot_plugin.so"

CFLAGS=(-O3 -std=gnu11 -pedantic -Wall -Werror -march=native -fPIC -fdata-sections -ffunction-sections -s)
CFLAGS+=(-flto)
#CFLAGS+=(-S)
CFLAGS+=(-fgcse-las -fgcse-sm -fipa-pta -fivopts -fmodulo-sched -fmodulo-sched-allow-regmoves -frename-registers -ftree-loop-im -ftree-loop-ivcanon -funsafe-loop-optimizations -funswitch-loops -fweb)
LDFLAGS=(-Wl,-O3 -Wl,-flto -Wl,--as-needed -Wl,--gc-sections -Wl,--relax -Wl,--sort-common)
SRCFLAGS=(-DLINUX -DPIC -I../include -pthread)
STRIPFLAGS=(-s -R .note -R .comment -R .gnu.version -R .gnu.version_r)

#CFLAGS+=(-Wno-multichar) # PLS FIXME

TARGET="$(readlink "$0" || true)"
if [[ -z "$TARGET" ]]; then
	TARGET="$0"
fi

cd "$(dirname "$TARGET")"

gcc -c "${SRCFLAGS[@]}" "${CFLAGS[@]}" "${LDFLAGS[@]}" -o plugin.o plugin.c
strip "${STRIPFLAGS[@]}" plugin.o

#exit 0

gcc "${SRCFLAGS[@]}" "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$PLUGIN" -shared plugin.o
rm plugin.o
#strip "${STRIPFLAGS[@]}" "$PLUGIN"
mv "$PLUGIN" ../../plugins/
exit 0
