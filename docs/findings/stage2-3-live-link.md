# Stage 2 + 3 findings — monitor-mode + the live drone link

**Date:** 2026-06-17 · **Board:** H618 · **Adapter:** RTL8812AU (0bda:8812) ·
**Drone:** OpenIPC VTX, H.265 **1080p60**, wfb-ng (swfec). **Result:** ✅
glass-to-glass — live video HW-decoded, direct-to-plane.

## Stage 2 — 8812au monitor mode

- Adapter enumerated as `wlxfc221c300582`, claimed by **our** `rtl88xxau_wfb`
  driver (the mainline `rtw88_8812au` was also loaded but did not own it).
- **Regulatory was the snag.** The out-of-tree driver self-manages its domain
  (`country 98`, `rtw_country_code` garbage) and **ignores `iw reg set`**. Its
  domain blocks **DFS** channels.
  - **Channel 132 (DFS) → rejected** (`channel is disabled`).
  - **Channel 161 (5805 MHz, UNII-3, non-DFS) → works.** Lesson: use a non-DFS
    5 GHz channel for the link.
- Bring-up that works:
  ```
  iw dev $IFACE set monitor otherbss ; ip link set $IFACE up
  iw dev $IFACE set channel 161 HT20
  ```
- Sniff confirmed the drone: 778k frames/8s, `SA 57:42:…` (the `57:42`="WB"
  marker), −58 dBm signal / −79 dBm noise (~21 dB SNR), MCS5/20 MHz.

## Stage 3 — the live link

**1. Radio-port / link-id mismatch (wfb_rx saw 0 packets).**
wfb-ng's MAC = `0x57 0x42` + **`channel_id`** (4 bytes BE), where
`channel_id = (link_id << 8) + radio_port` (`src/wifibroadcast.hpp`; the rx BPF
filter is `ether[0x0a:2]==0x5742 && ether[0x0c:4]==channel_id`). wfb_rx defaults
`link_id=0`, so it filtered for `channel_id=0` and matched nothing. Decoding the
captured `57:42:75:05:d6:00`:
```
channel_id = 0x7505d600  ->  radio_port = 0x00 = 0 ,  link_id = 0x7505d6 = 7669206
```
→ `wfb_rx -p 0 -i 7669206 …` and it received the stream. (To find these for a new
drone: capture one frame in monitor mode and read the bytes after `57:42`.)

**2. Key.** `gs.key` matching the drone (found on the dev box, all copies
identical) → wfb_rx established a `SESSION` and **decrypted RTP to udp:5600**.
The ~290 "Unable to decrypt" at startup are the pre-session packets — normal.

**3. Codec / payload type.** Read off the live RTP: **payload type 97**, first
payload byte `0x62` → H265 NAL type **49 (FU)** = HEVC. The SDP
(`m=video 5600 RTP/AVP 97 ; rtpmap:97 H265/90000`) matches as-is.

**4. Glass-to-glass.** `wfb_rx … -u 5600` → `wfbvid stream.sdp`: Cedrus decode,
auto-detected **1080p**, modeset 1:1, **presented 1144 frames (~57 fps)** — the
drone is 1080p60 and the H618 keeps up. The `PPS id out of range` /
`undecodable NALU` flood at the top is the mid-stream join before the first IDR;
the player skips it and locks on at the first keyframe.

## Gotchas

- **Display looked like "just the console"** because the test auto-killed the
  player after N seconds and *restored the console* — by the time you looked, the
  login prompt was back. Running the player **persistently** showed the video.
  Lesson: for a visual check, leave it up; don't auto-restore.
- The `pkill -f "wfb_rx…"` cleanup matches the SSH command's own argv (self-kill,
  exit 255). Use `pkill -x wfb_rx` / `fuser -k /dev/video0`.

## Known remaining (not blockers)

- **Colour is off.** DE33 chroma + the drone's colour space. Tunable via
  `WFBVID_NV21` (0/1), `WFBVID_ENC` (0=601,1=709,2=2020), `WFBVID_RANGE`
  (0=limited,1=full) — sweep these to find the drone's combo (camera likely
  full-range or BT.601). Not a pipeline fault.
- **Startup warm-up noise** — put the drone's `sprop-vps/sps/pps` in the SDP so it
  locks on the first IDR without the PPS-error flood.
- **Latency** — not yet measured; design is low-latency (no pacing, low-delay).
- **Persistence** — currently manual; a systemd unit would auto-start the GS.
