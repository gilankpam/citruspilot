#!/bin/sh
# run-gs.sh — bring up the H618 wfb-ng FPV ground station end to end:
#   8812au monitor mode on the drone's channel
#     -> wfb_rx (de-FEC + decrypt) -> RTP udp:5600
#       -> wfbvid (Cedrus HW decode, direct-to-plane).
#
# Defaults are the working params from the 2026-06-17 live bring-up. Override per
# drone via env. To find LINK_ID/RADIO_PORT for a different drone: in monitor
# mode, `tcpdump -e` one frame; the SA after 57:42 is the channel_id (4 bytes BE),
# where channel_id = (LINK_ID << 8) + RADIO_PORT.
set -eu

IFACE="${IFACE:-wlxfc221c300582}"   # 8812au monitor interface (see: iw dev)
CHANNEL="${CHANNEL:-161}"           # drone wfb channel — prefer non-DFS 5G (driver blocks DFS)
RADIO_PORT="${RADIO_PORT:-0}"       # wfb radio port  (channel_id & 0xff)
LINK_ID="${LINK_ID:-7669206}"       # wfb link id     (channel_id >> 8)
KEY="${KEY:-/root/wfb-ng/gs.key}"   # gs.key matching the drone
SDP="${SDP:-/root/wfb-h265.sdp}"    # codec/payload (H265 PT97 here)
RX="${WFB_RX:-/root/wfb-ng/wfb_rx}"

echo "[gs] 8812au monitor mode on channel $CHANNEL ($IFACE)"
nmcli device set "$IFACE" managed no 2>/dev/null || true
ip link set "$IFACE" down
iw dev "$IFACE" set monitor otherbss
ip link set "$IFACE" up
iw dev "$IFACE" set channel "$CHANNEL" HT20

echo "[gs] tuning + clean slate"
sysctl -wq net.core.rmem_max=26214400 net.core.rmem_default=26214400 2>/dev/null || true
fuser -k 5600/udp 2>/dev/null || true; fuser -k /dev/video0 2>/dev/null || true
sleep 1

echo "[gs] wfb_rx: -p $RADIO_PORT -i $LINK_ID -K $KEY -> udp:5600"
"$RX" -p "$RADIO_PORT" -i "$LINK_ID" -u 5600 -K "$KEY" "$IFACE" >/tmp/wfbrx.log 2>&1 &
RX_PID=$!
trap 'kill $RX_PID 2>/dev/null' EXIT INT TERM
sleep 1

echo "[gs] player (Ctrl-C to stop; console restored on exit)"
/usr/local/bin/wfbplay "$SDP"     # not exec'd, so the trap stops wfb_rx too
