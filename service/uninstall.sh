#!/bin/sh
# Stop CAKE, remove the hooks. Pass --purge to also remove /jffs/cake.
set -eu
base=/jffs/cake
[ ! -x "$base/cake-service.sh" ] || "$base/cake-service.sh" stop || :
for hook in services-start services-stop wan-start qos-start; do
    if [ -e "/jffs/scripts/$hook" ] && grep -Fq '/jffs/cake/cake-service.sh' "/jffs/scripts/$hook"; then
        rm "/jffs/scripts/$hook"
    fi
done
if [ -e /jffs/scripts/firewall-start ]; then
    sed '/# cake-2.6.22 begin/,/# cake-2.6.22 end/d' /jffs/scripts/firewall-start > /jffs/scripts/firewall-start.new
    cat /jffs/scripts/firewall-start.new > /jffs/scripts/firewall-start
    rm -f /jffs/scripts/firewall-start.new
fi
if [ "${1:-}" = --purge ]; then
    rm -rf "$base" /tmp/cake-service
    echo 'Removed /jffs/cake.'
else
    sed 's/^ENABLED=1$/ENABLED=0/' "$base/cake.conf" > "$base/cake.conf.new" 2>/dev/null && mv "$base/cake.conf.new" "$base/cake.conf" || :
    echo 'Hooks removed, CAKE disabled. Files remain in /jffs/cake (use --purge to remove).'
fi
