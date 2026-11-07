#!/bin/sh
# Cross-build sch_cake.ko and q_cake.so with the Asuswrt-Merlin LTS mipsel toolchain.
#   KDIR=<prepared Merlin kernel tree, release/src-rt/linux/linux-2.6>
#   CROSS_COMPILE=<toolchain prefix, default mipsel-linux->  (GCC 4.2.4 hndtools-mipsel-uclibc)
# Output: ./out/sch_cake.ko ./out/q_cake.so
set -eu
here=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
: "${KDIR:?Set KDIR to the prepared Merlin 2.6.22.19 kernel tree}"
prefix=${CROSS_COMPILE:-mipsel-linux-}
grep -Fq '"2.6.22.19"' "$KDIR/include/linux/utsrelease.h" || { echo 'KDIR is not a 2.6.22.19 tree.' >&2; exit 1; }
mkdir -p "$here/out"
make -C "$KDIR" ARCH=mips CROSS_COMPILE="$prefix" HOSTCFLAGS='-O2 -fcommon' M="$here/src/sch_cake" modules
cp "$here/src/sch_cake/sch_cake.ko" "$here/out/sch_cake.ko"
"${prefix}gcc" -Os -Wall -Wextra -Werror -fPIC -shared -I"$here/src" -I"$here/src/tc" \
    "$here/src/tc/q_cake_legacy.c" -o "$here/out/q_cake.so"
echo "Built $here/out/sch_cake.ko and $here/out/q_cake.so"
