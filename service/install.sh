#!/bin/sh
# Run on the router from the unpacked release directory. Installs /jffs/cake and the boot/WAN/firewall hooks.
set -eu
package=$(CDPATH= cd "$(dirname "$0")" && pwd)
base=/jffs/cake
cd "$package"
md5sum -c runtime.md5
./preflight.sh
[ "$(nvram get jffs2_scripts)" = 1 ] && [ "$(nvram get jffs2_on)" = 1 ] || { echo 'Enable JFFS and custom scripts first.' >&2; exit 1; }
[ ! -e "$base" ] && [ ! -e /tmp/cake-service ] || { echo "$base exists; use update.sh." >&2; exit 1; }
[ "$(nvram get qos_enable)" = 0 ] || { echo 'Disable native QoS first.' >&2; exit 1; }
for hook in services-start services-stop wan-start qos-start; do
    [ ! -e "/jffs/scripts/$hook" ] || { echo "Existing $hook needs a manual merge." >&2; exit 1; }
done
umask 077
mkdir -p "$base/runtime" "$base/backups"
if [ -e /jffs/scripts/firewall-start ]; then
    cp -p /jffs/scripts/firewall-start "$base/backups/firewall-start.before"
else
    mkdir -p /jffs/scripts
    printf '#!/bin/sh\n' > /jffs/scripts/firewall-start
    chmod 0700 /jffs/scripts/firewall-start
    : > "$base/backups/firewall-start.absent"
fi
for name in sch_cake.ko q_cake.so tc tc-cake preflight.sh image-libraries.md5; do
    cp -p "$name" "$base/runtime/$name"
done
cp -p cake-service.sh "$base/cake-service.sh"
cp -p uninstall.sh "$base/uninstall.sh"
cp -p cake.conf.example "$base/cake.conf"
chmod 0700 "$base/cake-service.sh" "$base/runtime/preflight.sh" "$base/runtime/tc-cake" "$base/runtime/tc"
chmod 0600 "$base/cake.conf"
chown -R 0:0 "$base"
complete=0
rollback() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$complete" = 0 ]; then
        echo 'Installation failed; disabling hooks and cleaning owned state.' >&2
        "$base/cake-service.sh" stop || :
        sed 's/^ENABLED=1$/ENABLED=0/' "$base/cake.conf" > "$base/cake.conf.disabled"
        mv "$base/cake.conf.disabled" "$base/cake.conf"
        sed '/# cake-2.6.22 begin/,/# cake-2.6.22 end/d' /jffs/scripts/firewall-start > /jffs/scripts/firewall-start.new
        cat /jffs/scripts/firewall-start.new > /jffs/scripts/firewall-start
        rm -f /jffs/scripts/firewall-start.new
        for hook in services-start services-stop wan-start qos-start; do
            if cmp -s "hooks/$hook" "/jffs/scripts/$hook"; then rm "/jffs/scripts/$hook"; fi
        done
    fi
    exit "$result"
}
trap rollback EXIT
trap 'exit 1' HUP INT TERM
for hook in services-start services-stop wan-start qos-start; do
    cp -p "hooks/$hook" "/jffs/scripts/$hook"
    chmod 0700 "/jffs/scripts/$hook"
    chown 0:0 "/jffs/scripts/$hook"
done
cat hooks/firewall-start.append >> /jffs/scripts/firewall-start
"$base/cake-service.sh" start
# Repeated events must keep counters and not duplicate rules or workers.
"$base/cake-service.sh" event
"$base/cake-service.sh" status
complete=1
echo 'Installed and running. Edit /jffs/cake/cake.conf for your rates; changes apply within 30 seconds.'
