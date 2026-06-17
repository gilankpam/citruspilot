# H618 wfb-ng FPV ground station — bring-up, start to working

This is the full story of turning a stock Orange Pi Zero 2W (Allwinner **H618**)
into a low-latency **wfb-ng FPV ground station + hardware video player** — the
sunxi analog of `PixelPilot_rk` — and the recipe to reproduce it.

**Result (2026-06-17):** live **1080p60 H.265** from an OpenIPC drone, over a
wfb-ng radio link, **hardware-decoded on the Cedrus VPU and scanned
direct-to-plane** on the H618, CPU mostly idle. (Colour tuning is the one
remaining polish item — see the end.)

```
drone (OpenIPC VTX, H.265 1080p60)
  → RTP → wfb-ng TX (swfec) → 8812au  ))) RF ch161 (((  8812au on H618
    → wfb_rx (de-FEC + decrypt) → RTP udp:5600
      → wfbvid: libav RTP/SDP → Cedrus (v4l2request) → DRM NV12 overlay plane
```

The decode+display back-end was already proven from a file in
[`h618-mainline-video`](../../h618-mainline-video) (`drmvid`); this project adds
the radio ingress and the live RTP front-end.

---

## The four stages (what we actually did)

### Stage 0 — player pipe, no radio (`smoke/`, `docs/findings/stage0-…`)
Synthesized H.265 RTP on loopback and proved `RTP → depay → Cedrus` works
real-time on the H618, comparing two front-ends:
- **libav (ffmpeg/SDP)** and **GStreamer** tied on CPU/fps (~1 core, ~26-28 fps).
- **Chose libav** — it reuses `drmvid`'s exact path, pulls codec/params from the
  SDP, and adds no dependencies. Key lesson: a **large UDP socket buffer** is
  mandatory (bursty RTP drops packets even on loopback) and start the receiver
  **before** the stream so it locks on the first IDR.

### Stage 1 — build the radio software on the board (`groundstation/`, `…stage1-…`)
- **rtl8812au** (svpcom master, v5.2.20) → `88XXau_wfb.ko`, builds clean on **6.18**
  (no cfg80211 API breakage) and loads.
- **wfb-ng (swfec)** → `wfb_rx`/`wfb_tx`/`wfb_keygen` with NEON FEC.
- **The detour:** the board's SD card had a single corrupted ext4 inode that
  flipped root read-only mid-build (surfacing as a baffling apt **SIGILL**). A
  reboot's initramfs `fsck` fixed it. Most of the follow-on apt pain was
  self-inflicted — `rm`'d apt lists during debugging make core `-dev` packages
  look "version-skewed/not available"; **one clean `apt-get update` fixed it.**
  Also: install the kernel-headers deb via `apt-get install ./deb` (not `dpkg -i`)
  so its postinst builds `fixdep`/`modpost`, then `apt-mark hold` it.

### Stage 2 — 8812au monitor mode (`docs/findings/stage2-3-…`)
The adapter (`wlxfc…`) is owned by our `rtl88xxau_wfb`. The driver **self-manages
its regulatory** (`country 98`) and **ignores `iw reg set`**, blocking **DFS**
channels — so the original **channel 132 (DFS) was rejected**; we moved the drone
to **161 (5805 MHz, non-DFS)** and it worked. Sniffing confirmed the drone
(`57:42…`="WB", −58 dBm, ~21 dB SNR).

### Stage 3 — the live link (`docs/findings/stage2-3-…`)
Three things to line up, all decodable from the air or found locally:
1. **Radio port + link id.** wfb-ng MAC = `57:42` + `channel_id` (BE), and
   `channel_id = (link_id<<8) + radio_port`. The captured `57:42:75:05:d6:00` →
   **radio_port 0, link_id 7669206**. wfb_rx defaults link_id 0 (so it saw
   nothing); `-i 7669206` fixed reception.
2. **Key.** The drone's `gs.key` → wfb_rx decrypts → RTP on udp:5600.
3. **Codec/PT.** Read off the live stream: **H.265, payload type 97**.

Then `wfb_rx … -u 5600` → `wfbvid stream.sdp` → **1080p60 on the plane**.

---

## Reproduce it (blank board → working)

**Prereqs:** the patched mainline 6.18 kernel (NV12 DE33 overlay, patch `0099`)
from `h618-mainline-video`, and the v4l2request ffmpeg. Then on the board:

```sh
# 1. radio software (driver + wfb-ng)
groundstation/build-rtl8812au.sh        # -> 88XXau_wfb.ko (loads on plug-in)
groundstation/build-wfb-ng.sh           # -> wfb_rx / wfb_tx / wfb_keygen

# 2. player
cc player/wfbvid.c -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
cp player/wfbplay /usr/local/bin/ ; cp player/wfb-h265.sdp /root/

# 3. the drone's gs.key
cp gs.key /root/wfb-ng/gs.key

# 4. bring it all up (set CHANNEL / LINK_ID / RADIO_PORT for your drone)
CHANNEL=161 LINK_ID=7669206 RADIO_PORT=0 groundstation/run-gs.sh
```

To get `LINK_ID`/`RADIO_PORT` for a different drone: put the adapter in monitor
mode on the channel, `tcpdump -i <iface> -e -c1`, read the bytes after `57:42`
(that's `channel_id`), then `radio_port = channel_id & 0xff`,
`link_id = channel_id >> 8`. Confirm the codec/PT from the live RTP and update the
SDP if it isn't H.265/97.

---

## Known remaining (none are blockers)

- **Colour is off.** DE33 chroma swap + the drone's colour space. Sweep
  `WFBVID_NV21` (0/1) × `WFBVID_ENC` (0=601, 1=709, 2=2020) × `WFBVID_RANGE`
  (0=limited, 1=full) to find the drone's combo (its camera is likely full-range
  or BT.601). Pure tuning, not a pipeline fault.
- **Startup warm-up noise** — add the drone's `sprop-vps/sps/pps` to the SDP so the
  player locks on the first IDR without the `PPS id out of range` flood.
- **Latency** — not yet measured (design is low-latency: no pacing, low-delay
  decode, minimal jitter buffer).
- **Persistence** — `run-gs.sh` is manual; a systemd unit would auto-start the GS
  on boot and re-launch on adapter/stream events.
- **SD card** — had one corruption event; if it recurs, reflash (everything is
  reproducible from local artifacts).
