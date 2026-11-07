#!/bin/sh
set -eu
directory=$(CDPATH= cd "$(dirname "$0")" && pwd)
if [ "$(uname -r)" != '2.6.22.19' ]; then
    echo 'Kernel does not match RT-N16 52E7.' >&2
    exit 1
fi
if [ "$(nvram get productid)" != 'RT-N16' ]; then
    echo 'Router model does not match RT-N16.' >&2
    exit 1
fi
if ! grep -Fq 'Linux version 2.6.22.19 (root@ASUS-AJ082535) (gcc version 4.2.4) #1 Sun Dec 19 19:11:41 MST 2021' /proc/version; then
    echo 'Kernel build does not match the verified 52E7 image.' >&2
    exit 1
fi
md5sum -c "$directory/image-libraries.md5"
"$directory/tc-cake" -V
echo 'Image compatibility preflight passed. See release evidence for completed hardware tests.'
