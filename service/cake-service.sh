#!/bin/sh
# Persistent firmware hooks; runtime state/logging stay in RAM.
set -eu
base=/jffs/cake
bin="$base/runtime"
run=/tmp/cake-service
self="$base/cake-service.sh"
chain=CAKE_PERSIST
mkdir -p "$run"
umask 077
load_config() {
    . "$base/cake.conf"
    case "$ENABLED" in 0|1) ;; *) return 1;; esac
    for rate in "$DOWNLOAD_KBIT" "$UPLOAD_KBIT"; do
        case "$rate" in ''|*[!0-9]*) return 1;; esac
        [ "$rate" -ge 64 ] && [ "$rate" -le 100000 ] || return 1
    done
    case "$WAN_IF" in ''|*[!a-zA-Z0-9_.:-]*) return 1;; esac
    AUTORATE=${AUTORATE:-0}
    case "$AUTORATE" in 0|1) ;; *) return 1;; esac
}
lock() {
    for attempt in 1 2 3 4 5; do
        if mkdir "$run/lock" 2>/dev/null; then
            echo $$ > "$run/lock/pid"
            trap 'rm -f "$run/lock/pid"; rmdir "$run/lock"' EXIT
            trap 'exit 1' HUP INT TERM
            return 0
        fi
        owner=$(cat "$run/lock/pid" 2>/dev/null || :)
        case "$owner" in ''|*[!0-9]*) ;; *)
            if ! kill -0 "$owner" 2>/dev/null; then
                rm -f "$run/lock/pid"
                rmdir "$run/lock" 2>/dev/null || :
            fi;;
        esac
        sleep 1
    done
    echo 'Another lifecycle operation is running.' >&2
    return 1
}
log() { echo "$(date +%s) $*"; }
remove_hook() {
    rules=$(iptables-save -t mangle)
    while printf '%s\n' "$rules" | grep -Eq -- "-j $chain( |$)"; do
        old_if=$(printf '%s\n' "$rules" | sed -n "s/^-A PREROUTING -i \([^ ]*\) -j $chain *$/\1/p" | head -1)
        [ -n "$old_if" ] || return 1
        iptables -t mangle -D PREROUTING -i "$old_if" -j "$chain"
        rules=$(iptables-save -t mangle)
    done
}
cleanup() {
    [ -f "$run/owned" ] || return 0
    failed=0
    remove_hook || failed=1
    if iptables-save -t mangle | grep -q "^:$chain "; then
        iptables -t mangle -F "$chain" || failed=1
        iptables -t mangle -X "$chain" || failed=1
    fi
    old_if=$(cat "$run/wan" 2>/dev/null || echo "$WAN_IF")
    if "$bin/tc-cake" qdisc show dev "$old_if" 2>/dev/null | grep -q '^qdisc cake ca01: root '; then
        "$bin/tc-cake" qdisc del dev "$old_if" root || failed=1
    fi
    if grep -q '^imq ' /proc/modules; then
        ifconfig imq0 down || failed=1
        if "$bin/tc-cake" qdisc show dev imq0 | grep -q '^qdisc cake ca02: root '; then
            "$bin/tc-cake" qdisc del dev imq0 root || failed=1
        fi
    fi
    if grep -q '^xt_IMQ ' /proc/modules; then /sbin/rmmod xt_IMQ || failed=1; fi
    if grep -q '^imq ' /proc/modules; then /sbin/rmmod imq || failed=1; fi
    if grep -q '^sch_cake ' /proc/modules; then /sbin/rmmod sch_cake || failed=1; fi
    if [ "$failed" = 0 ]; then
        rm -f "$run/owned" "$run/applied-up" "$run/applied-down" "$run/wan"
        echo stopped > "$run/state"
    else
        echo cleanup-failed > "$run/state"
    fi
    return "$failed"
}
ensure() {
    load_config
    [ "$ENABLED" = 1 ] || { cleanup; return; }
    [ ! -e "$run/paused" ] || return 0
    [ "$(nvram get qos_enable)" = 0 ] || { log 'Native QoS enabled: refusing competing queue changes.'; return 1; }
    [ -d "/sys/class/net/$WAN_IF" ] || { echo waiting-wan > "$run/state"; return 0; }
    echo repairing > "$run/state"
    if [ ! -f "$run/owned" ]; then
        "$bin/preflight.sh"
        if grep -Eq '^(sch_cake|imq|xt_IMQ) ' /proc/modules ||
           grep -q 'imq0:' /proc/net/dev ||
           iptables-save | grep -q -- '-j IMQ' ||
           ip6tables-save | grep -q -- '-j IMQ' ||
           iptables-save -t mangle | grep -q "^:$chain "; then
            log 'Existing unowned CAKE/IMQ state: refusing adoption.'
            return 1
        fi
        echo owned > "$run/owned"
    fi
    echo "$WAN_IF" > "$run/wan"
    grep -q '^sch_cake ' /proc/modules || /sbin/insmod "$bin/sch_cake.ko"
    grep -q '^imq ' /proc/modules || /sbin/modprobe imq numdevs=1
    grep -q '^xt_IMQ ' /proc/modules || /sbin/modprobe xt_IMQ
    up=$("$bin/tc-cake" qdisc show dev "$WAN_IF")
    if printf '%s\n' "$up" | grep -q '^qdisc cake ca01: root '; then
        if [ "$(cat "$run/applied-up" 2>/dev/null || :)" != "$UPLOAD_KBIT" ]; then
            "$bin/tc-cake" qdisc change dev "$WAN_IF" root cake bandwidth "${UPLOAD_KBIT}kbit"
        fi
    else
        printf '%s\n' "$up" | grep -q '^qdisc pfifo_fast 0: root '
        "$bin/tc-cake" qdisc add dev "$WAN_IF" root handle ca01: cake bandwidth "${UPLOAD_KBIT}kbit" besteffort triple-isolate nat rtt 100ms memlimit 4mb
    fi
    echo "$UPLOAD_KBIT" > "$run/applied-up"
    down=$("$bin/tc-cake" qdisc show dev imq0)
    if printf '%s\n' "$down" | grep -q '^qdisc cake ca02: root '; then
        if [ "$(cat "$run/applied-down" 2>/dev/null || :)" != "$DOWNLOAD_KBIT" ]; then
            "$bin/tc-cake" qdisc change dev imq0 root cake bandwidth "${DOWNLOAD_KBIT}kbit"
        fi
    else
        # Newly created IMQ may show no queue until brought up.
        if [ -n "$down" ]; then printf '%s\n' "$down" | grep -q '^qdisc pfifo_fast 0: root '; fi
        "$bin/tc-cake" qdisc add dev imq0 root handle ca02: cake bandwidth "${DOWNLOAD_KBIT}kbit" besteffort triple-isolate nat ingress rtt 100ms memlimit 4mb
    fi
    echo "$DOWNLOAD_KBIT" > "$run/applied-down"
    ifconfig imq0 up
    if ! iptables-save -t mangle | grep -q "^:$chain "; then
        iptables -t mangle -N "$chain"
        iptables -t mangle -A "$chain" -j IMQ --todev 0
    fi
    rules=$(iptables-save -t mangle)
    if ! printf '%s\n' "$rules" | grep -q -- "^-A $chain -j IMQ --todev 0 *$"; then
        # Firmware -F can retain our chain but remove its target rule.
        if printf '%s\n' "$rules" | grep -q -- "^-A $chain "; then
            log 'Unexpected contents in owned IMQ chain.'; return 1
        fi
        iptables -t mangle -A "$chain" -j IMQ --todev 0
    fi
    if ! iptables-save -t mangle | grep -q -- "^-A PREROUTING -i $WAN_IF -j $chain *$"; then
        remove_hook
        iptables -t mangle -I PREROUTING 1 -i "$WAN_IF" -j "$chain"
    fi
    echo active > "$run/state"
}
status() {
    load_config
    echo "epoch=$(date +%s) enabled=$ENABLED down_kbit=$DOWNLOAD_KBIT up_kbit=$UPLOAD_KBIT wan=$WAN_IF state=$(cat "$run/state" 2>/dev/null || echo absent)"
    uptime
    grep '^cpu ' /proc/stat
    grep -E '^(MemFree|Slab):' /proc/meminfo
    grep -E '^(sch_cake|imq|xt_IMQ) ' /proc/modules || :
    "$bin/tc-cake" -s qdisc show dev "$WAN_IF" 2>/dev/null || :
    if grep -q '^imq ' /proc/modules; then "$bin/tc-cake" -s qdisc show dev imq0; fi
    iptables -t mangle -L "$chain" -v -n 2>/dev/null || :
}
worker_alive() {
    worker=$(cat "$run/worker.pid" 2>/dev/null || :)
    case "$worker" in ''|*[!0-9]*) return 1;; esac
    kill -0 "$worker" 2>/dev/null && tr '\000' ' ' < "/proc/$worker/cmdline" | grep -Fq "$self daemon"
}
# lte-autorate keeps its own pidfile; the conf rates are its static base
autorate() {
    [ "$AUTORATE" = 1 ] && [ -x "$bin/lte-autorate.sh" ] || return 0
    [ "$(cat "$run/state" 2>/dev/null || :)" = active ] || return 0
    p=$(cat /tmp/lte-autorate/pid 2>/dev/null || :)
    case "$p" in ''|*[!0-9]*) ;; *) if kill -0 "$p" 2>/dev/null; then return 0; fi;; esac
    set -f
    for o in ${AUTORATE_OPTS:-}; do
        n=${o%%=*}; v=${o#*=}
        case "$n" in ''|*[!A-Z_]*) log "AUTORATE_OPTS: bad name in $o"; set +f; return 0;; esac
        case "$v" in ''|*[!0-9]*) log "AUTORATE_OPTS: bad value in $o"; set +f; return 0;; esac
    done
    env ${AUTORATE_OPTS:-} DL_BASE=$DOWNLOAD_KBIT UL_BASE=$UPLOAD_KBIT nohup /bin/sh "$bin/lte-autorate.sh" run >> "$run/autorate.out" 2>&1 < /dev/null &
    set +f
}
case "${1:-status}" in
status) status;;
ensure) lock; ensure;;
start|event)
    mode=$1
    lock
    if [ "$mode" = start ]; then rm -f "$run/paused"; fi
    ensure
    if [ "$ENABLED" = 1 ] && [ ! -e "$run/paused" ] && ! worker_alive; then
        nohup /bin/sh "$self" daemon > "$run/daemon.log" 2>&1 < /dev/null &
        echo $! > "$run/worker.pid"
    fi
    status;;
stop)
    lock
    touch "$run/paused"
    if worker_alive; then kill "$worker"; fi
    if [ -x "$bin/lte-autorate.sh" ]; then /bin/sh "$bin/lte-autorate.sh" stop || :; fi
    load_config
    cleanup;;
daemon)
    cycle=0
    while [ ! -e "$run/paused" ]; do
        if [ -f "$run/daemon.log" ] && [ "$(wc -c < "$run/daemon.log")" -gt 524288 ]; then
            mv "$run/daemon.log" "$run/daemon.log.1"
            exec >> "$run/daemon.log" 2>&1
        fi
        /bin/sh "$self" ensure || log 'Ensure failed; retrying in 30 seconds.'
        if [ "$cycle" = 0 ]; then status; fi
        if load_config; then autorate; fi
        cycle=$(((cycle + 1) % 10))
        sleep 30
    done;;
*) echo 'Usage: cake-service.sh start|stop|ensure|event|status' >&2; exit 1;;
esac
