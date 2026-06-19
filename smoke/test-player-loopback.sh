#!/bin/sh
# test-player-loopback.sh — integration test for the citrusvid HW-decode path.
# RUNS ON THE GS. Loops a clean, zero-loss in-band H.265 file in as RTP and
# asserts citrusvid actually HW-decodes and PRESENTS frames to the plane.
#
# This is the regression test for the RTP -> stateless-Cedrus stall: without the
# in-decoder HEVC parser, citrusvid gets 0 DRM_PRIME frames out of a *clean* stream
# ("Failed waiting on capture buffer", "presented 0 frames"). With the parser it
# presents a continuous stream. Pass criterion: it prints "playing." and
# "presented N frames" with N greater than a threshold.
#
#   SAMPLE=/root/sample_inband.mp4 MIN_FRAMES=30 DUR=14 ./test-player-loopback.sh
#
# Stage the sample once from the dev host (GS has no encoder):
#   ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 10 -c:v libx265 \
#     -pix_fmt yuv420p -x265-params keyint=30:bframes=0:repeat-headers=1 \
#     sample_inband.mp4 && scp -O sample_inband.mp4 root@10.18.0.1:/root/
set -u
SAMPLE="${SAMPLE:-/root/sample_inband.mp4}"
BIN="${CITRUSVID:-/usr/local/bin/citrusvid}"
PORT="${PORT:-5600}"
PT="${PT:-97}"
DUR="${DUR:-14}"
MIN_FRAMES="${MIN_FRAMES:-30}"
LOG=/tmp/test-citrusvid.log

[ -f "$SAMPLE" ] || { echo "FAIL: sample $SAMPLE not present (stage it first, see header)"; exit 2; }

# console prep (mirror citrusplay) so the modeset is not fought by fbcon
systemctl stop getty@tty1 2>/dev/null || true
echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true
sysctl -wq net.core.rmem_max=26214400 net.core.rmem_default=26214400 2>/dev/null || true
fuser -k /dev/video0 2>/dev/null || true
fuser -k "$PORT"/udp 2>/dev/null || true
sleep 1

: > "$LOG"
"$BIN" --port "$PORT" >"$LOG" 2>&1 &
VID=$!
sleep 1
# clean in-band stream looped in as RTP (no re-encode; file carries in-band headers)
ffmpeg -hide_banner -loglevel warning -re -stream_loop -1 -i "$SAMPLE" \
    -an -c:v copy -payload_type "$PT" -f rtp "rtp://127.0.0.1:$PORT" >/tmp/test-src.log 2>&1 &
SRC=$!

sleep "$DUR"
kill -INT "$VID" 2>/dev/null; sleep 2; kill -KILL "$VID" "$SRC" 2>/dev/null

# restore console
echo 1 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true
systemctl start getty@tty1 2>/dev/null || true

PRESENTED=$(sed -n 's/.*presented \([0-9]*\) frames.*/\1/p' "$LOG" | tail -1)
PRESENTED="${PRESENTED:-0}"
PLAYING=$(grep -c "playing." "$LOG")
STALL=$(grep -c "Failed waiting on capture buffer" "$LOG")

echo "--- result: playing=$PLAYING presented=$PRESENTED stall_markers=$STALL (min=$MIN_FRAMES) ---"
if [ "$PLAYING" -ge 1 ] && [ "$PRESENTED" -ge "$MIN_FRAMES" ]; then
    echo "PASS"
    exit 0
fi
echo "FAIL: citrusvid did not present a healthy frame stream"
echo "--- last log lines ---"
grep -vE "PPS id out of range|Skipping invalid undecodable" "$LOG" | tail -8
exit 1
