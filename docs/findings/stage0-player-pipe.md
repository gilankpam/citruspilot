# Stage 0 findings — player pipe (synthetic RTP)

**Date:** 2026-06-17 · **Board:** H618, kernel `6.18.35-current-sunxi64`,
ffmpeg 7.1.3 (v4l2request), gst 1.x with `v4l2slh265dec`.
**Source:** `sample.mkv` = Big Buck Bunny **1920×1080 / 30 fps / HEVC**, streamed
as RTP to `udp:127.0.0.1:5600` via `rtp-loopback-src.sh` (`-re -c:v copy`).

## Goal

De-risk the player and pick the RTP front-end **without** radio hardware:
prove `RTP/UDP → depay → Cedrus HW decode` works real-time and CPU-sane, and
compare libav (ffmpeg `v4l2request`) vs GStreamer (`v4l2slh265dec`).

## Result — both decode the live RTP; HW confirmed

`Using V4L2 media driver cedrus (6.18.35)` — Cedrus does the work (software
would peg all 4 cores; we see ~1 core).

| Engine | CPU (% of 1 core) | sustained fps | drops | param-sets |
|---|---|---|---|---|
| **ffmpeg / libav** | ~111% | ~28 | 0 | from the SDP automatically |
| **GStreamer** | ~90% | ~25.8 | 0 | needs `sprop-vps/sps/pps` in caps |

Same ballpark — gst's hoped-for multi-thread efficiency win did **not** appear.

## Decision: libav (ffmpeg) RTP/SDP front-end

Driven by integration cost, not performance (they tie):

- **Reuses the proven `drmvid` back-end verbatim** — same `avformat`+`avcodec`+
  `v4l2request` path. RTP = swap `avformat_open_input(file)` → open the SDP with
  `protocol_whitelist=file,udp,rtp` + buffering; the decode→DRM-plane half is
  untouched.
- **Param-sets come from the SDP** (clean) vs gst's `sprop-*`-in-caps (fragile —
  the base64 `==` padding has to be quoted, and it stalls silently without them).
- **Zero new dependencies** (no gst, no appsink→`avcodec_send_packet` glue).
- gst stays a **swappable fallback** if the real RF link's loss/reordering later
  needs its mature `rtpjitterbuffer` — the architecture isolates the front-end.

## Key learnings (carry into the player, task #15)

1. **UDP socket buffer is mandatory.** Default rmem → heavy loss *even on
   loopback* (the source bursts a frame's ~70 packets at once; decode jitter
   backs the socket up). Fix: `net.core.rmem_max` + `buffer_size`/`udpsrc
   buffer-size` ≈ 25 MiB → **0 loss**. Real wfb-ng is paced by the radio (gentler
   than this burst), so this is a conservative test.
2. **Receiver-first.** Bind the port before the stream starts so it locks on the
   first IDR; otherwise it waits for the next keyframe (long for default GOPs).
3. **Throughput to verify in the player:** both engines ran ~26-28 fps vs the
   30 fps source on this `-f null` decode-only test (the `average` includes the
   sync ramp). `drmvid` file-playback was smooth 30 fps, so this is likely the
   CLI/`-f null` + big `reorder_queue` overhead, not the VPU. **Open item:**
   confirm sustained 30 fps direct-to-plane and tune buffering for latency.
4. **For low latency** the player wants a *small* reorder window + small jitter
   target (this test used generous buffers to first prove correctness). Tune for
   latency once it plays.

## Gotchas hit (so we don't repeat them)

- `pkill -f "<pattern>"` over SSH matches the SSH command's own argv → kills the
  session (exit 255). Use resource-based kills: `fuser -k 5600/udp`,
  `fuser -k /dev/video0`.
- A stalled gst pipeline reaches PLAYING with **low CPU and no errors** — looks
  identical to "decoding efficiently." Always confirm with an objective frame
  count (here: an `identity` probe). The first "18% CPU" reading was a stall
  (0 frames), not efficiency.

## Reusable artifacts

- `smoke/rtp-loopback-src.sh` — synthetic RTP source (+ writes the SDP).
- `smoke/test-decode.sh` — `ENGINE=ffmpeg|gst` decode-only receiver; also points
  at the **real** wfb-ng port later (just set `PORT`, skip the source).
