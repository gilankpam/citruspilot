#!/bin/sh
# Build the wfb-ng (swfec fork) ground-station binaries on the H618.
#
# VALIDATED 2026-06-17: wfb_rx / wfb_tx / wfb_keygen / wfb_tx_cmd / wfb_tun build
# clean (NEON-accelerated FEC) from gilankpam/wfb-ng @ swfec, kernel 6.18 / Trixie.
#
# Only wfb_rx is needed for the *video* ground station. We use the standalone
# wfb_rx binary (not the Python wfb-server), so no python3 packaging deps are
# required. wfb_tun (mavlink/IP tunnel) is optional and needs libevent-dev.
#
# Source: clone from GitHub if the repo is reachable/public; otherwise transfer a
# git bundle from a dev box:
#   (dev)   git -C wfb-ng bundle create wfb-swfec.bundle swfec ; scp it to /root
#   (board) git clone -b swfec /root/wfb-swfec.bundle wfb-ng
set -eu
SRC="${SRC:-https://github.com/gilankpam/wfb-ng.git}"   # or /root/wfb-swfec.bundle
BRANCH="${BRANCH:-swfec}"

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    git build-essential libpcap-dev libsodium-dev libevent-dev python3

cd /root
rm -rf wfb-ng
git clone -b "$BRANCH" "$SRC" wfb-ng
cd wfb-ng
make all_bin            # -> wfb_rx wfb_tx wfb_keygen wfb_tx_cmd wfb_tun

echo "built: $(ls wfb_rx wfb_tx wfb_keygen wfb_tx_cmd wfb_tun 2>/dev/null | tr '\n' ' ')"
echo
echo "Ground-station video RX (Stage 3), standalone:"
echo "  wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface>"
echo "  ^ de-FECs the drone's stream and forwards RTP to udp:5600 -> player"
