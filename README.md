# cake-2.6.22

The CAKE queue discipline (`sch_cake`) backported to Linux 2.6.22, with `tc` support and a
persistence service, for the ASUS RT-N16 running the Asuswrt-Merlin LTS fork.

CAKE has been in mainline Linux only since 4.19 (2018). I know of no earlier port to a 2.6.22 kernel. This one is a loadable module built against the unmodified vendor kernel
(no reflash, no kernel replacement), a plugin that teaches the firmware's 2006-era `tc` the
`cake` qdisc, and a service that keeps it running across reboots and WAN events. It shapes
real traffic on a real router and has run for days without a fault. It is a backport of one
upstream snapshot, not a new design; the CAKE algorithm is the work of its authors (see Provenance).

## Tested on

| | |
|---|---|
| Router | ASUS RT-N16, Broadcom BCM4718, MIPS32R2 little-endian, 480 MHz |
| Firmware | Asuswrt-Merlin LTS fork `374.43_52E7j9527` (SHA-256 `5124a59de12d6c01edbe9c8c6ea7f3560f494cf56be0965bc1d4c15159bced8f`) |
| Kernel | Linux 2.6.22.19 (`#1 Sun Dec 19 19:11:41 MST 2021`), GCC 4.2.4, uClibc |
| Userspace | BusyBox 1.25.1, iproute2-ss060323 `tc`, Dropbear |
| WAN | `ppp0` (LTE modem over USB) |

Nothing else is tested. The preflight script refuses to run on any other model, kernel build or
set of firmware libraries.

## How it works

- **Egress** (upload): CAKE is the root qdisc on the WAN device (`ppp0`, handle `ca01:`).
- **Ingress** (download): the firmware has no IFB, so inbound IPv4 packets are redirected to an
  IMQ device. An iptables rule in a private chain in `mangle/PREROUTING` (`-i ppp0 -j IMQ --todev 0`)
  sends them to `imq0`, which carries CAKE in `ingress` mode (handle `ca02:`).
- Options: `besteffort triple-isolate nat rtt 100ms memlimit 4mb` (plus `ingress` on `imq0`).
  Rates come from `cake.conf`.
- `tc` support: the firmware `tc` has no `cake` module. `q_cake.so` is loaded into it with
  `LD_PRELOAD` by the `tc-cake` wrapper; the stock `tc` binary is not modified.
- Backport changes: replacement of the post-2.6.22 APIs CAKE uses (flow dissector, conntrack
  lookup, qdisc ops, netlink attributes), and a fix for a bug in the vendor IMQ driver (its
  queued-drop destructor omits the netfilter hook module reference, which pins the module).
  See `patches/sch_cake-2.6.22.patch`.

## Install

Needs JFFS and custom scripts enabled, native QoS disabled (`qos_enable=0`), and no existing
`services-start`, `services-stop`, `wan-start` or `qos-start` in `/jffs/scripts` (the installer
stops rather than overwrite them; `firewall-start` is extended, with a backup).

1. Download the release archive and `SHA256SUMS` from the GitHub Release; verify with
   `shasum -a 256 -c SHA256SUMS`.
2. Copy to the router (the old Dropbear needs legacy SCP):
   `scp -O cake-2.6.22-v1.0.0.tar.gz admin@<router>:/tmp/`
3. On the router: `cd /tmp && tar -xzf cake-2.6.22-v1.0.0.tar.gz && cd cake-2.6.22-v1.0.0`,
   then `./preflight.sh` (stop if it fails; never force the module), then `./install.sh`.
4. The installer starts CAKE with the example rates (4000 down / 2000 up kbit/s on `ppp0`). Edit
   `/jffs/cake/cake.conf` to your link (`service/cake.conf.example`); changes apply within 30 seconds.
   Check with `/jffs/cake/cake-service.sh status`.

`update.sh` replaces the runtime and service script and keeps `cake.conf`; shaping pauses briefly.

## Persistence

`cake-service.sh` is installed in `/jffs/cake` with the config and tested binaries. Boot
(`services-start`), WAN up (`wan-start`), firewall rebuild (`firewall-start`) and QoS start
(`qos-start`) call a serialized, idempotent `event`/`start`; `services-stop` cleans up. A
30-second repair loop covers missed or overlapping events (the firmware's late firewall flush
can empty the IMQ chain; the service recreates its rule). Locks, logs and state are in RAM
(`/tmp/cake-service/`, `/tmp/cake-service-hooks.log`); only config, binaries and scripts are on JFFS.
The service refuses to adopt CAKE/IMQ state it did not create, and refuses to run while native QoS is on.

| Command | |
|---|---|
| `cake-service.sh status` | queue state and counters, read only |
| `cake-service.sh start` | start or resume |
| `cake-service.sh stop` | pause and clean up until next boot (`ENABLED=0` disables permanently) |

### Optional: rate controller

With `AUTORATE=1` in `cake.conf`, the service also starts `/jffs/cake/runtime/lte-autorate.sh`
if that file exists, using `DOWNLOAD_KBIT`/`UPLOAD_KBIT` as its base rates, and stops it on `stop`.
Extra controller settings go into `AUTORATE_OPTS`, e.g. `AUTORATE_OPTS="HIGH_MS=50 DL_MIN=1500"`
(`NAME=digits` tokens only; anything else is logged and the controller is not started).
The controller is a separate project and is not included:
<https://github.com/timemaster5/lte-autorate>. Without the file, or with `AUTORATE=0`, nothing changes.

## Uninstall

`sh uninstall.sh` (from the release directory, or the copy in `/jffs/cake`): stops the service,
removes the hooks and the `firewall-start` block, sets `ENABLED=0`. `sh uninstall.sh --purge` also
removes `/jffs/cake`. The firewall-start backup is `/jffs/cake/backups/firewall-start.before`.

## Tested

One RT-N16 on an LTE link (rates vary with the radio and background traffic, so the figures are
indicative, not a controlled benchmark).

- **Latency under load**, 15 pings during one finite transfer, ms avg / max. Without CAKE:
  download 63.5 / 434, upload 32.7 / 64. With CAKE: download shaped to 2, 4, 6 Mbit/s
  42.7 / 92, 42.1 / 95, 43.5 / 203; upload shaped to 1 and 2 Mbit/s 36.4 / 55, 30.8 / 51;
  4 down + 2 up together 35.1 / 112. No packet loss in any stage. Goodput of a single
  transfer was 1.1 to 3.8 Mbit/s down and 0.8 to 1.3 Mbit/s up at those settings.
- **CPU**: window-average CPU busy 7 to 11 % across those stages. Softirq (where CAKE and IMQ run)
  in 300 one-second samples with 2 to 3.9 Mbit/s of download: median 4 %, p95 11 %, max 21 %. Over the multi-day run the 5-minute average
  was 1.1 % median, 10.9 % at p95, 36.8 % at the worst sample.
- **Multi-day run**: 34.8 hours, 418 five-minute samples at 4 Mbit/s down / 2 Mbit/s up
  (IPv4 download): 8.9 GB down, 1.9 GB up, upload queue drops 0.16 % of packets. No reboot,
  kernel error or OOM; free memory flat at 68 to 70 MB; the IMQ reference count returned to zero
  repeatedly. The module file is 312 KB but loads about 40 KB (debug sections are not allocated).
- **Reboot persistence**: after two planned reboots the module, both queues and the IMQ rule
  came back with no manual step and no repair errors; one later unplanned full reboot recovered
  the same way. A forced WAN reconnect, separate from reboot, was not exercised on its own; the
  `wan-start` hook and the repair loop are installed.

## Limits

- This CPU is not expected to shape much more than about 15 Mbit/s. CPU above about 6 Mbit/s
  was not tested; the 15 Mbit/s figure is an estimate, not a measurement.
- Ingress shaping is **IPv4 only**: the IMQ rule is an iptables rule. IPv6 is not redirected.
- Only the firmware, kernel and libraries listed above are supported.
- `install.sh`, `update.sh` and `uninstall.sh` follow the install that was done and verified by hand
  on the test router, but the scripts themselves have not yet been run on hardware.
- Not tested: multi-client NAT fairness, fragments and GSO paths beyond normal traffic, repeated
  unload/reload under load, sustained rates above 4 Mbit/s.

### Known traps

- Native QoS must stay off; the service will not start with `qos_enable=1`.
- No web UI integration; the firmware's QoS page is not changed.
- IMQ passes GSO packets unsplit (its `nf_info` ownership is not reference-counted). IPv6
  address keys are folded to 32 bits; fragments have no ports. The vendor `HZ=100` watchdog
  path is untested on hardware.
- A mismatch in preflight means a different image: stop, do not force `insmod`.
- If the WAN device is not `ppp0`, set `WAN_IF`; the config is re-read every 30 seconds.
- BusyBox shell arithmetic is 32-bit; keep that in mind for any script that sums byte counters.
- `scp` to this router needs `-O`.

## Build

Needs the Asuswrt-Merlin LTS tree prepared for the RT-N16 (kernel configured, `utsrelease.h`
present) and its GCC 4.2.4 mipsel/uClibc toolchain (`tools/brcm/K26/hndtools-mipsel-uclibc-4.2.4`)
on `PATH`:

```sh
KDIR=/path/to/asuswrt-merlin/release/src-rt/linux/linux-2.6 \
CROSS_COMPILE=mipsel-linux- ./build/build.sh
```

Output goes to `out/`. The released binaries were built this way. The firmware sources are at
<https://github.com/john9527/asuswrt-merlin> (commit `9c7e0011e3efa3f8437ea01621a0da1de217ec01`
was used).

## Layout

```
src/sch_cake/   backported module source (upstream files + merlin_compat.h, merlin_imq.h)
src/tc/         q_cake_legacy.c tc plugin, tc_plugin_abi.h, tc-cake wrapper
patches/        sch_cake-2.6.22.patch, the diff against upstream
build/          build.sh
service/        cake-service.sh, hooks, install/update/uninstall, preflight, cake.conf.example
dist/           (not in git) release assets: sch_cake.ko, tc, q_cake.so, tc-cake, SHA256SUMS,
                cake-2.6.22-v1.0.0.tar.gz (the router package)
```

## Provenance and licence

Based on the out-of-tree CAKE module <https://github.com/dtaht/sch_cake>, commit
`53ff94d4b58071307084d47a7937afbe71e40070`; `patches/sch_cake-2.6.22.patch` applies cleanly to it.
CAKE is by Jonathan Morton, Toke Høiland-Jørgensen, Dave Täht, Sebastian Moeller, Kevin
Darbyshire-Bryant and Ryan Mounce; upstream copyright headers are kept. `tc` in the release is the
firmware's own iproute2 binary, unmodified, shipped so the package is self-consistent.

The module is Dual BSD/GPL (upstream `GPL-2.0 OR BSD-3-Clause`); everything else is GPL-2.0. See `LICENSE`.
