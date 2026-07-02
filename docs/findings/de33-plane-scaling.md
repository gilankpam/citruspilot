# DE33 VI-plane scaling: driver gap, kernel patch, HW-scaled validation

Date: 2026-07-02. Board: Orange Pi Zero 2W (H618), DE33 mixer, VI plane id 33,
CRTC id 47. Tool: `src/tools/scale-probe.c`.

## Investigation

The DE33 video (VI) overlay plane (id 33, see `docs/findings/osd-plane-probe.md`)
rejected every non-1:1 atomic commit on the stock kernel. `scale-probe`
(`TEST_ONLY` commits at three source/dest rect pairs) on the **baseline
kernel** (`uname -v` = `#1 SMP PREEMPT Fri Jun 19 13:41:49 UTC 2026`):

```
VI plane=33  crtc=47 (idx 0)  active mode=1920x1280
TEST_ONLY commits (no visible change):
  1:1 (control)                SRC 1920x1080 -> CRTC 1920x1080 : ACCEPTED
  upscale -> fullscreen        SRC 1920x1080 -> CRTC 1920x1280 : REJECTED (ERANGE)
  downscale -> half            SRC 1920x1080 -> CRTC 960x640 : REJECTED (ERANGE)
```

1:1 always accepted; any up- or down-scale rejected with `ERANGE`, consistent
across repeated runs.

### Root cause

Mainline `drivers/gpu/drm/sun4i/sun50i_planes.c` ships the H616/H618 DE33
mixer quirks with:

```c
.cfg = {
    .de_type    = SUN8I_MIXER_DE33,
    /*
     * TODO: All planes support scaling, but driver needs
     * improvements to properly support it.
     */
    .scaler_mask    = 0,
    .scanline_yuv   = 4096,
},
```

(checked at kernel 6.18.35). `sun8i_vi_layer_atomic_check()` reads
`scaler_mask` per-channel and, when the bit is clear, forces
`DRM_PLANE_NO_SCALING` on that plane's scaling-range check — so any commit
where `src` and `crtc` rects differ fails atomic check with `ERANGE`. This is
a **driver gap, not a hardware limit**: the DE33 VSU (video scaling unit) is
already active at 1:1 for every NV12 frame (chroma upsampling requires it),
and `sun8i_vi_scaler.c` is fully DE33-aware — the TODO comment itself says
"all planes support scaling." Corroborating evidence: an out-of-tree upstream
v7 series for this driver ships `scaler_mask = 0xf` (all channels) instead of
`0`, i.e. this is a known-conservative default, not a hardware constraint.

## Patch

`sbc-groundstations-gilankpam` commit `6160ebc` —
`board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch`,
wired via `BR2_LINUX_KERNEL_PATCH` in `configs/orangepi_zero2w_defconfig`:

```diff
--- a/drivers/gpu/drm/sun4i/sun50i_planes.c
+++ b/drivers/gpu/drm/sun4i/sun50i_planes.c
@@ -172,10 +172,11 @@
 	.cfg = {
 		.de_type	= SUN8I_MIXER_DE33,
 		/*
-		 * TODO: All planes support scaling, but driver needs
-		 * improvements to properly support it.
+		 * VI channels (0-2) scale via the DE33 VSU path in
+		 * sun8i_vi_scaler.c (validated on H618). TODO: UI channel
+		 * scaling still needs driver improvements.
 		 */
-		.scaler_mask    = 0,
+		.scaler_mask    = 0x7,
 		.scanline_yuv	= 4096,
 	},
 };
```

`0x7` enables only the **VI (video) channels 0-2** — the ones `citrusplay`
actually drives, and the only path validated end-to-end here. The **UI
channels (6-8)**, used for cursor/OSD-style planes elsewhere in the DE33, are
deliberately left at 1:1 since they weren't exercised by this validation.

## Staged validation

### Stage 1 — probe, unpatched vs. patched kernel

Baseline (unpatched, above): 1:1 ACCEPTED, up/down-scale REJECTED (ERANGE).

After deploying the patched kernel and confirming it actually booted
(`uname -v` = `#2 SMP PREEMPT Thu Jul  2 16:04:13 UTC 2026`):

```
scale-probe on new kernel: 1:1 ACCEPTED, upscale 1920x1080->1920x1280 ACCEPTED,
downscale ->960x640 ACCEPTED
```

All three cases ACCEPTED — the mask-0x7 patch removes the `ERANGE` rejection
for the VI plane.

### Stage 2 — real (non-TEST_ONLY) scaled commit, visual pattern

`scale-probe --commit` builds an NV12 test pattern (luma grid + chroma bars)
and does a **real** atomic commit (not `TEST_ONLY`) of a 1920x1080 source
scaled up to the full 1920x1280 CRTC:

```
VI plane=33  crtc=47 (idx 0)  active mode=1920x1280
TEST_ONLY commits (no visible change):
  1:1 (control)                SRC 1920x1080 -> CRTC 1920x1080 : ACCEPTED
  upscale -> fullscreen        SRC 1920x1080 -> CRTC 1920x1280 : ACCEPTED
  downscale -> half            SRC 1920x1080 -> CRTC 960x640 : ACCEPTED
REAL commit SRC 1920x1080 -> CRTC 1920x1280 : OK - pattern should now FILL the screen
commit rc=0
```

**OPERATOR CONFIRMED**: pattern filled the whole 1920x1280 screen, grid
straight, bars uniform, no artifacts.
This is the first real (non-probe) HW-scaled commit on this plane — proof the
mask-0x7 patch scales actual pixel content correctly, not just that atomic
check passes.

### Stage 3 — live stream, `citrusplay` scale-to-fit + retune fallback

`citrusplay` was extended to compute an aspect-fit rect (`src/fit_rect.h`),
try a `TEST_ONLY` probe commit at that rect (`probe_plane_scaling()` in
`src/citrusplay.c`), and either HW-scale into it or fall back to the old
retune-HDMI-mode / centre-1:1 behavior if the probe reports `ERANGE` or
`CITRUSPLAY_RETUNE=1` is set.

Scaled run (default behavior, live 1280x720 wfb_rx stream into the
1920x1280 startup mode):

```
citrusplay: startup mode 1920x1280 (1920x1280)
citrusplay: playing 1280x720 -> 1920x1080+0+100 in 1920x1280 (HW scaled).
```

debugfs plane[33] state confirms the scaled commit (src != crtc-pos, CRTC
mode unchanged from startup — no retune happened):

```
plane[33]: plane-0
	crtc=crtc-0
	fb=53
	format=NV12 little-endian (0x3231564e)
	size=1280x720
	crtc-pos=1920x1080+0+100
	src-pos=1280.000000x720.000000+0.000000+0.000000
	rotation=1
	normalized-zpos=1
```
```
crtc[47]: crtc-0
	enable=1
	active=1
	mode_changed=0
	mode: "1920x1280": 60 164820 1920 1968 2000 2050 1280 1283 1289 1340 0x48 0x9
```

**OPERATOR CONFIRMED**: live 720p stream HW-upscaled fullscreen-width on the
1920x1280 panel, correct aspect, OSD intact, no artifacts, smooth.

Fallback run (`CITRUSPLAY_RETUNE=1`, same stream) — exercises the same
fallback path an unpatched kernel would take on `ERANGE`:

```
citrusplay: retuned HDMI to 1280x720 (1280x720@60) to match the 1280x720 stream
citrusplay: playing 1280x720 -> 1280x720+0+0 in 1280x720 (1:1).
```

debugfs confirms 1:1 with a retuned CRTC mode (`crtc-pos == src-pos`,
`mode: "1280x720"` instead of the 1920x1280 startup mode) — the fallback
correctly bypasses HW scaling and reproduces the pre-patch behavior on
demand.

## Persistence & deploy notes

- Kernel patch lives in the **Buildroot external tree**, not this repo:
  `sbc-groundstations-gilankpam` commit `6160ebc` adds
  `board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch`
  and wires it via `BR2_LINUX_KERNEL_PATCH` in
  `configs/orangepi_zero2w_defconfig`, so it reapplies automatically on any
  future `linux-custom` rebuild from a clean tree.
- **Squashfs/overlay boot gotcha (future kernel work must account for this):**
  this board's root is an **overlayfs** — lower = read-only SquashFS
  (`/dev/mmcblk0p1`, bind-mounted at `/rom`), upper = ext4 `/overlay`
  (`/dev/mmcblk0p2`, persistent). **U-Boot's extlinux boot reads
  `/boot/Image` directly off the raw `mmcblk0p1` SquashFS partition** — it has
  no concept of Linux's overlayfs. So `scp`-ing a new `Image` over the live
  `/boot/Image` only ever writes the overlay's copy-on-write upper layer; it
  is visible to userspace but **U-Boot still boots the old kernel baked into
  the SquashFS**, silently. Confirmed by `dd`-reading the partition's magic
  bytes (`hsqs`) and observing `/rom/boot/Image`'s mtime stay unchanged after
  the overlay write. **Deploying a new kernel requires rebuilding/reflashing
  partition 1 (the SquashFS rootfs image) itself** — not just copying a file
  onto the running root — via a full Buildroot image rebuild and a supervised
  reflash (or an SD-card swap). This was the actual deploy path used to get
  the patched kernel running (`uname -v` before/after: `#1 ... Jun 19` →
  `#2 ... Jul 2`).
- Runtime fallback (`CITRUSPLAY_RETUNE=1`, or automatic on `ERANGE` from
  `probe_plane_scaling()`) means `citrusplay` degrades gracefully on any board
  or kernel that doesn't carry this patch — it is not a hard dependency for
  the player to function, only for the fullscreen-fill behavior.

## Future work

- `probe_plane_scaling()` TEST_ONLY omits `ALLOW_MODESET` (real commit includes
  it) — could false-negative on kernels that treat scaler enable as a mode
  change; safe degradation (falls back to retune), but worth revisiting if
  other kernels are targeted.
- Spec promised "on unexpected runtime commit failure, fall back to retune";
  current code logs and continues (matches pre-change behavior). Implement or
  amend the spec.
- `citrusplay` logs "(HW scaled)" even for an exact-fit 1:1 commit (probe
  short-circuit) — cosmetic.
- After a fallback retune, a later successful scale fits the retuned mode, not
  the original startup mode (mutable `mode` global; narrow transient-failure
  edge case).
- scale-probe: `--commit` seconds arg accepts garbage/negative silently
  (degrades to TEST_ONLY-only); non-ERANGE rejections print without separator
  (`REJECTEDPermission denied`); fd/FBs not released on exit paths (fine for
  run-once tool).
- fit_rect: no negative-input test cases (guard covers them by inspection).
- Upstream the kernel patch (needs broader testing than one board; UI-channel
  scaling remains TODO upstream).
