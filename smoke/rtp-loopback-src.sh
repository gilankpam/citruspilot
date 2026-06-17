#!/bin/sh
# Stage 0 synthetic RTP source. Streams a local HEVC/H.264 file as RTP to a UDP
# port (real-time paced, looping) and writes the matching SDP. Emulates the RTP
# that wfb-ng RX delivers to the ground station.
#
# Run in the background BEFORE the receiver-first test... actually: start the
# RECEIVER first (it binds the port), THEN this, so the stream begins on a clean
# VPS/SPS/PPS + IDR and the receiver locks immediately.
#
#   FILE=/root/sample.mkv HOST=127.0.0.1 PORT=5600 SDP=/root/stream.sdp ./rtp-loopback-src.sh
set -eu
FILE="${FILE:-/root/sample.mkv}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-5600}"
SDP="${SDP:-/root/stream.sdp}"
exec ffmpeg -hide_banner -loglevel warning -y \
    -re -stream_loop -1 -i "$FILE" \
    -an -c:v copy -f rtp -sdp_file "$SDP" "rtp://$HOST:$PORT"
