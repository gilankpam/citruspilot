#!/bin/sh
# Stage 0 decode-only receiver test. Ingests the live RTP (synthetic loopback or
# real wfb-ng on the same UDP port) and HW-decodes on the Cedrus VPU with NO
# display, to isolate and measure the RTP front-end. Compares the two engines.
#
#   ENGINE=ffmpeg|gst  PORT=5600  SDP=/root/stream.sdp  DUR=18  ./test-decode.sh
#
# Findings (2026-06-17, H618, sample.mkv = 1080p30 HEVC, loopback):
#   * Both engines need a large UDP socket buffer (bursty RTP) -> rmem + buffer-size,
#     else heavy packet loss even on loopback.
#   * Start the RECEIVER before the source so it locks on the first IDR.
#   * ffmpeg pulls VPS/SPS/PPS from the SDP automatically; gst needs sprop-* in caps.
#   * ffmpeg ~111% of one core @ ~28 fps; gst ~90% @ ~25.8 fps, 0 drops.
#   * -> player uses the libav (ffmpeg) front-end; see docs/findings/stage0-player-pipe.md
set -eu
ENGINE="${ENGINE:-ffmpeg}"
PORT="${PORT:-5600}"
SDP="${SDP:-/root/stream.sdp}"
DUR="${DUR:-18}"
BUF="${BUF:-26214400}"           # 25 MiB socket buffer

sysctl -wq net.core.rmem_max="$BUF" net.core.rmem_default="$BUF" 2>/dev/null || true
fuser -k /dev/video0 2>/dev/null || true; sleep 1

if [ "$ENGINE" = ffmpeg ]; then
  exec timeout "$DUR" ffmpeg -hide_banner -loglevel info \
    -protocol_whitelist file,crypto,data,udp,rtp \
    -buffer_size "$BUF" -reorder_queue_size 2000 -max_delay 500000 \
    -hwaccel v4l2request -i "$SDP" -an -benchmark -f null -
else
  # gst rtph265depay needs the param-sets up front; lift them from the SDP fmtp line
  VPS=$(sed -n 's/.*sprop-vps=\([^;]*\).*/\1/p' "$SDP")
  SPS=$(sed -n 's/.*sprop-sps=\([^;]*\).*/\1/p' "$SDP")
  PPS=$(sed -n 's/.*sprop-pps=\([^;]*\).*/\1/p' "$SDP")
  CAPS="application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=96"
  [ -n "$VPS" ] && CAPS="$CAPS,sprop-vps=(string)\"$VPS\""
  [ -n "$SPS" ] && CAPS="$CAPS,sprop-sps=(string)\"$SPS\""
  [ -n "$PPS" ] && CAPS="$CAPS,sprop-pps=(string)\"$PPS\""
  exec timeout "$DUR" gst-launch-1.0 -v \
    udpsrc port="$PORT" buffer-size="$BUF" caps="$CAPS" \
    ! rtpjitterbuffer latency=200 ! rtph265depay ! h265parse ! v4l2slh265dec \
    ! fpsdisplaysink video-sink=fakesink text-overlay=false sync=false
fi
