#!/bin/sh
# Synthetic RTP source for testing the CitrusPilot player WITHOUT the radio.
# Streams a local file as H.265/RTP to a UDP port, real-time paced, looping.
# Emulates the RTP that wfb_rx delivers to the ground station.
#
# The player uses a built-in SDP with NO out-of-band sprop param sets — it
# expects the drone to send VPS/SPS/PPS *in-band* before each IDR. So this
# source must do the same. The default MODE=emulate RE-ENCODES with
# repeat-headers=1 so the param sets ride in-band (drone-like). Use MODE=copy
# only if $FILE already carries in-band param sets at each keyframe (a plain
# `-c:v copy` of an mkv does NOT — its param sets live in the container's
# extradata, which ffmpeg would otherwise emit only as SDP sprop).
#
# Start the RECEIVER (player) first so it locks on the first IDR.
#
# NOTE: software HEVC encoding on the H618 is slow (~5 fps @ 540p). That's fine
# for validating the player pipeline + OSD + freeze/reconnect; real 1080p60
# throughput is only exercised by the live drone link.
#
#   FILE=/root/sample.mkv PORT=5600 PT=97 MODE=emulate SIZE=960x540 ./rtp-loopback-src.sh
set -eu
FILE="${FILE:-/root/sample.mkv}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-5600}"
PT="${PT:-97}"             # match the player's WFBVID_PT (default 97)
MODE="${MODE:-emulate}"    # emulate (re-encode, in-band headers) | copy
SIZE="${SIZE:-960x540}"    # downscale so the software encoder can keep up
BITRATE="${BITRATE:-3M}"

if [ "$MODE" = copy ]; then
    exec ffmpeg -hide_banner -loglevel warning -y \
        -re -stream_loop -1 -i "$FILE" \
        -an -c:v copy -payload_type "$PT" -f rtp "rtp://$HOST:$PORT"
else
    exec ffmpeg -hide_banner -loglevel warning -y \
        -re -stream_loop -1 -i "$FILE" \
        -an -c:v libx265 -preset ultrafast \
        -x265-params "repeat-headers=1:keyint=30" \
        -b:v "$BITRATE" -s "$SIZE" \
        -payload_type "$PT" -f rtp "rtp://$HOST:$PORT"
fi
