#!/bin/sh
# Replace the runtime and service script of an existing install; cake.conf is kept.
set -eu
cd "$(dirname "$0")"
base=/jffs/cake
[ -d "$base" ] || { echo 'Not installed; use install.sh.' >&2; exit 1; }
md5sum -c runtime.md5
./preflight.sh
"$base/cake-service.sh" stop
for name in sch_cake.ko q_cake.so tc tc-cake preflight.sh image-libraries.md5; do
    cp -p "$name" "$base/runtime/$name"
done
cp cake-service.sh "$base/cake-service.sh.new"
chmod 0700 "$base/cake-service.sh.new"
mv "$base/cake-service.sh.new" "$base/cake-service.sh"
chmod 0700 "$base/runtime/preflight.sh" "$base/runtime/tc-cake" "$base/runtime/tc"
chown -R 0:0 "$base"
"$base/cake-service.sh" start
"$base/cake-service.sh" status
echo 'Updated; shaping was paused briefly.'
