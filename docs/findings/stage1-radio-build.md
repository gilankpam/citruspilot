# Stage 1 findings — build the radio stack

**Date:** 2026-06-17 · **Board:** H618, `6.18.35-current-sunxi64`, Armbian/Trixie.
**Result:** ✅ both halves build + run on the H618.

## Outcome

| Piece | Result |
|---|---|
| **rtl8812au** (svpcom master / v5.2.20) | builds clean on **6.18** (no cfg80211 API breakage), loads — `88XXau_wfb.ko` (note the `_wfb` suffix), installed to `/lib/modules/$K/.../`, auto-loads via USB alias |
| **wfb-ng** (gilankpam @ swfec) | `wfb_rx`/`wfb_tx`/`wfb_keygen`/`wfb_tx_cmd`/`wfb_tun` build with **NEON FEC**; `wfb_rx` runs |

Scripts: `groundstation/build-rtl8812au.sh`, `groundstation/build-wfb-ng.sh`.

The driver loading (`insmod` OK, no vermagic error) also confirms the **P47b7
headers matched the running kernel**.

## The detour (cost most of the time — worth recording)

1. **SD-card corruption.** A single ext4 inode (`#27205`) had a bad checksum
   (`EFSBADCRC`). When `apt`/`dpkg` read it, ext4 aborted the journal and
   **remounted root read-only** — surfacing first as a confusing `apt` **SIGILL**.
   Fix: reboot → the **initramfs e2fsck** repaired the inode → `clean`, rw. The
   card may be marginal; if errors recur, reflash a fresh one. Nothing
   irreplaceable lives on the board (kernel/headers debs + sources are off-board).

2. **Self-inflicted apt breakage.** While chasing the SIGILL I `rm`'d
   `/var/lib/apt/lists` and ran interrupted updates. Partial lists made core
   `-dev` packages (`zlib1g-dev`, `xml-core`, `libsystemd-dev`) look
   *"not available"* / *"version skew / wants to downgrade systemd"* — all bogus.
   **One clean `apt-get update` fixed everything.** Lesson: stale apt lists
   masquerade as dependency/version-skew errors; rebuild the lists before
   believing them.

3. **Kernel headers must be `apt install`ed, not `dpkg -i`'d.** Bare `dpkg -i`
   left the package *unconfigured*, so its postinst never compiled the host
   tools → `scripts/basic/fixdep: not found` at module build. `apt-get install
   ./headers.deb` runs the postinst (builds fixdep + modpost). `apt-mark hold`
   it afterwards — apt's solver removed it once mid-conflict, emptying the tree.

## Stage 2/3 (live bring-up) — exact RX invocation

`wfb_rx`'s own usage gives the shape:
```
wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface>
```
de-FECs the drone's stream → RTP on `udp:5600` → the player. Needs: the 8812au in
monitor mode on the drone's channel, and the **drone-matching `gs.key`** + radio
port.
