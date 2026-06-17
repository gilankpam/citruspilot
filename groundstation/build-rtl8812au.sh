#!/bin/sh
# Build + install the svpcom rtl8812au (monitor mode + injection) driver on the
# H618 ground station, against the patched mainline 6.18 kernel.
#
# VALIDATED 2026-06-17 on 6.18.35-current-sunxi64: builds clean (no cfg80211 API
# breakage — only harmless sprintf -Wrestrict warnings) and loads.
# NOTE: the svpcom *wfb* fork names the module 88XXau_wfb.ko (not 88XXau.ko).
#
# Hard-won prereqs / gotchas:
#   * Run a CLEAN `apt-get update` FIRST. Partial/stale apt lists make core -dev
#     packages (zlib1g-dev, xml-core, libsystemd-dev) look "not available" and
#     break every install with confusing "version skew" / "no candidate" errors.
#   * Install the matching linux-headers deb via `apt-get install ./xxx.deb`
#     (resolves deps so the postinst can compile the host tools fixdep/modpost),
#     NOT bare `dpkg -i` (leaves it unconfigured -> "fixdep: not found" at build).
#   * apt-mark hold the headers so apt's solver can't drop them mid-resolution.
set -eu

HEADERS_DEB="${HEADERS_DEB:-/root/linux-headers-P47b7.deb}"   # matching the running kernel
SRC_URL="${SRC_URL:-https://github.com/svpcom/rtl8812au/archive/refs/heads/master.tar.gz}"
K="$(uname -r)"

echo "== clean apt update (mandatory — see header) =="
apt-get update

echo "== build deps =="
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential git bc flex bison libelf-dev

echo "== matching kernel headers (postinst builds fixdep/modpost) =="
apt-get install -y "$HEADERS_DEB"
apt-mark hold "$(dpkg-deb -f "$HEADERS_DEB" Package)"
[ -x "/lib/modules/$K/build/scripts/mod/modpost" ] \
    || ( cd "/usr/src/linux-headers-$K" && make ARCH=arm64 modules_prepare )

echo "== fetch + build (native arm64; default PLATFORM_I386_PC uses SUBARCH=arm64) =="
cd /root
wget -qO rtl8812au.tar.gz "$SRC_URL"
rm -rf rtl8812au-*/ ; tar xzf rtl8812au.tar.gz
cd rtl8812au-*/
make -j"$(nproc)"        # -> 88XXau_wfb.ko
make install             # -> /lib/modules/$K/kernel/drivers/net/wireless/ + depmod

echo
echo "installed: $(modinfo 88XXau_wfb 2>/dev/null | sed -n 's/^filename: *//p')"
echo "load: modprobe 88XXau_wfb   (udev auto-loads via the USB alias when the 8812au is plugged in)"
