# DE33 VI-Plane Scaling — Design

**Date:** 2026-07-02
**Goal:** citrusplay shows the stream full-screen in the sink's startup HDMI mode by
scaling the video plane in hardware, instead of retuning the HDMI output mode to
match the stream.

## Background

The DE33 display engine on the H618 rejects any plane commit where the source and
destination rects differ (`ERANGE`), so citrusplay currently retunes the HDMI mode
to the stream size (`retune_mode`). Investigation (validated live on the GS,
2026-07-02, with `src/tools/scale-probe.c`) showed this is **not a hardware
limit**: mainline `drivers/gpu/drm/sun4i/sun50i_planes.c` ships the H616/H618 DE33
quirks with `.scaler_mask = 0` and the comment *"TODO: All planes support scaling,
but driver needs improvements to properly support it."* With the mask empty,
`sun8i_vi_layer_atomic_check()` passes `DRM_PLANE_NO_SCALING` to
`drm_atomic_helper_check_plane_state()`, which returns `-ERANGE` for any non-1:1
rect.

Evidence the VI scaler path works on DE33 in this tree (kernel 6.18.35 tarball,
`linux-6.18.35-opi-sunxi`):

- `sun8i_vi_scaler.c` is fully DE33-aware (`sun8i_vi_scaler_base()` handles
  `SUN8I_MIXER_DE33`: unit base `0x4000 + 0x20000 * channel` inside the planes
  regmap, `max_register 0x17fffc`).
- For subsampled formats (NV12), `sun8i_vi_layer_update_coord()` **already
  enables the VSU at 1:1** on every frame (chroma must be scaled to luma size) —
  so the full DE33 VSU programming path (steps, phases, coefficient tables) is
  exercised and working on the live GS today.
- Upstream's earlier DE33 series (Ryan Walklin, up to v7) shipped
  `scaler_mask = 0xf`, and the v11 `vi_scaler` DE33 patch got a Reviewed-by; no
  mailing-list discussion documents a concrete defect behind the TODO.

Channel layout (H616/H618, `UI_PLANE_OFFSET = 6`): VI channels are 0 (mixer 0 —
HDMI video plane), 1, 2 (mixer 1); UI channels are 6, 7 (mixer 0 primary/OSD), 8.
`layer->channel` is the physical channel; `scaler_mask` is checked as
`BIT(layer->channel)`.

## Decisions (user-confirmed)

1. **Scope: VI channels only** — `scaler_mask = 0x7` (channels 0,1,2). UI planes
   stay 1:1; the DE33 UI-scaler path is unverified and citrusplay doesn't need it.
2. **Persistence: buildroot patch dir** — patch file in the sbc-groundstations
   tree, applied by Buildroot on top of the kernel tarball.
3. **Player policy: scale-to-fullscreen replaces retune** — stay in the sink's
   startup (EDID-preferred) mode, aspect-fit the video; retune kept only as
   fallback/env override.

## Component 1: kernel patch

Single functional change in `drivers/gpu/drm/sun4i/sun50i_planes.c`,
`sun50i_h616_planes_quirks`:

```c
.scaler_mask = 0x7,   /* VI channels only; UI scaling still TODO */
```

Comment updated accordingly. No other driver change: `atomic_check` then grants
`SUN8I_VI_SCALER_SCALE_MIN..MAX` and the existing `update_coord()` VSU path does
the rest.

**Persistence:** `board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch`
in `/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam`, wired via
`BR2_LINUX_KERNEL_PATCH="${BR2_EXTERNAL_OPENIPC_SBC_GS_PATH}/board/orangepi/zero2w/linux-patches"`
in `configs/orangepi_zero2w_defconfig` (same pattern as `board/radxa/zero3`).
Committed to that repo (kernel is `CONFIG_DRM_SUN4I=y`, built into `Image`).

## Component 2: kernel validation (gate before player work)

Fast loop: apply the patch to `output/orangepi_zero2w_defconfig/build/linux-custom`,
`make linux-rebuild`, back up `/boot/Image` on the GS (extlinux boot), scp the new
`Image`, reboot.

1. Rerun `scale-probe` (TEST_ONLY): upscale + downscale must flip REJECTED→ACCEPTED.
2. New `--commit` mode in `src/tools/scale-probe.c`: real atomic commit of a
   generated NV12 test pattern scaled to fullscreen for a few seconds (stop
   citrusplay first — it holds DRM master). Operator visually confirms actual
   scaling and image integrity; the tool reports commit rc. This surfaces any
   undocumented VSU gotcha *before* citrusplay depends on scaling.

## Component 3: citrusplay scale-to-fullscreen

- New pure function `fit_rect(src_w, src_h, mode_w, mode_h) -> dst rect`:
  aspect-preserving letterbox/pillarbox, centered, even-aligned. Host unit test in
  `src/tests/` (existing harness).
- First decoded frame (and mid-stream resolution change): commit SRC = full frame
  → CRTC = fit_rect. No mode switch; the screen stays in the startup mode brought
  up at launch.
- **Fallback:** before the first scaled commit, run it as `TEST_ONLY`. If rejected
  (unpatched kernel → `ERANGE`), revert to today's behavior (retune to matching
  EDID mode, else centered 1:1). `CITRUSPLAY_RETUNE=1` forces the old behavior.
- OSD/primary plane untouched (draws at mode size already).
- `retune_mode()` code stays; only its default invocation changes.

## Error handling

- Probe/commit failures reported per-case with errno (existing scale-probe style).
- citrusplay: TEST_ONLY probe result cached once per stream-size; on unexpected
  commit failure at runtime, log and fall back to retune path rather than exit.
- Kernel deploy risk: `/boot/Image.bak` kept on the GS; extlinux entry unchanged —
  recovery = restore backup over SSH (board stays reachable; DRM is not needed
  for boot/network).

## Testing

| Stage | Test | Pass criterion |
|---|---|---|
| Kernel | scale-probe TEST_ONLY | up/downscale ACCEPTED |
| Kernel | scale-probe --commit (visual) | pattern fills screen, no artifacts |
| Player | host unit test fit_rect | letterbox/pillarbox/exact/degenerate cases |
| Player | GS loopback or live stream | 1080p stream fullscreen in startup mode, no mode switch, OSD intact, no regressions in stats |
| Fallback | run new citrusplay on old kernel (pre-reboot) | falls back to retune, still plays |

## Out of scope

- UI-plane scaling (separate TODO upstream).
- Upstreaming the kernel change (worth doing later; needs broader testing than one
  board).
- Any change to the radio stack or decode path.
