# DE33 VI-Plane Scaling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable hardware video-plane scaling on the H618 (DE33) via a one-line kernel quirk patch, then switch citrusplay from HDMI mode-retuning to aspect-fit plane scaling.

**Architecture:** Kernel: flip `scaler_mask` 0→0x7 (VI channels only) in `sun50i_planes.c`, persisted as a Buildroot `BR2_LINUX_KERNEL_PATCH` in the sbc-groundstations tree. Validation is staged: TEST_ONLY probe → real scaled test-pattern commit (visual) → player change. Player: pure `fit_rect()` aspect-fit helper (host-unit-tested), a TEST_ONLY capability probe with graceful fallback to the old retune path on unpatched kernels.

**Tech Stack:** C (kernel driver diff, libdrm atomic API, no new deps), Buildroot, busybox sh on the target.

**Spec:** `docs/superpowers/specs/2026-07-02-de33-plane-scaling-design.md`

## Global Constraints

- Two repos: **citruspilot** = `/home/gilankpam/Projects/poc/citruspilot` (git, branch `main`); **sbc-groundstations** = `/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam` (git; has UNRELATED dirty files — `git add` only the files this plan names, never `git add -A`).
- Target board (GS): `root@10.18.0.1`, busybox sh (no bash), SSH key auth. `/tmp` on the GS is tmpfs — re-copy tools after every reboot.
- `citrusplay` runs as a service and **holds DRM master**; any tool doing atomic commits (even TEST_ONLY) needs it stopped: `/etc/init.d/S99citruspilot stop` … `start`. **Always restart it** before leaving a task.
- Cross toolchain: `CC=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig/host/bin/aarch64-none-linux-gnu-gcc`, sysroot `.../output/orangepi_zero2w_defconfig/staging` (libdrm headers live directly in `staging/usr/include`).
- Kernel is `CONFIG_DRM_SUN4I=y` (built into `Image`); board boots extlinux from `/boot/Image`. Keep `/boot/Image.bak` for recovery (board network/SSH does not depend on DRM — recovery over SSH always possible).
- Kernel build tree: `.../output/orangepi_zero2w_defconfig/build/linux-custom` (6.18.35, extracted from the `linux-6.18.35-opi-sunxi` tarball; pristine copy of the same source in `build/linux-6.18.35`). Never run `linux-dirclean` in this plan (would discard the hand-applied patch mid-loop; the wiring in Task 1 covers clean rebuilds).
- Env-knob naming: `CITRUSPLAY_*`. New knob in this plan: `CITRUSPLAY_RETUNE` (default 0).

---

### Task 1: Kernel patch file + Buildroot wiring (sbc-groundstations repo)

**Files:**
- Create: `board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch` (in sbc-groundstations)
- Modify: `configs/orangepi_zero2w_defconfig` (in sbc-groundstations, after the `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE` line)
- Modify (build tree, NOT committed): `output/orangepi_zero2w_defconfig/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c`

**Interfaces:**
- Produces: a kernel where `sun8i_vi_layer_atomic_check()` accepts non-1:1 rects on VI channels 0–2 (scale range `SUN8I_VI_SCALER_SCALE_MIN..MAX`, i.e. up to 32× down / unlimited up within VSU limits). Consumed by Tasks 2–5.

- [ ] **Step 1: Edit the build-tree driver (this simultaneously prepares Task 2's rebuild)**

In `output/orangepi_zero2w_defconfig/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c`, first save a pristine copy, then change the quirks:

```bash
BR=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig
cp $BR/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c \
   $BR/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c.scaleorig
```

Then edit `sun50i_planes.c` — replace exactly this block (inside `sun50i_h616_planes_quirks`):

```c
	.cfg = {
		.de_type	= SUN8I_MIXER_DE33,
		/*
		 * TODO: All planes support scaling, but driver needs
		 * improvements to properly support it.
		 */
		.scaler_mask    = 0,
		.scanline_yuv	= 4096,
	},
```

with:

```c
	.cfg = {
		.de_type	= SUN8I_MIXER_DE33,
		/*
		 * VI channels (0-2) scale via the DE33 VSU path in
		 * sun8i_vi_scaler.c (validated on H618). TODO: UI channel
		 * scaling still needs driver improvements.
		 */
		.scaler_mask    = 0x7,
		.scanline_yuv	= 4096,
	},
```

- [ ] **Step 2: Generate the patch file from the diff**

```bash
BR=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig
GS=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam
mkdir -p $GS/board/orangepi/zero2w/linux-patches
diff -u --label a/drivers/gpu/drm/sun4i/sun50i_planes.c \
        --label b/drivers/gpu/drm/sun4i/sun50i_planes.c \
        $BR/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c.scaleorig \
        $BR/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c \
  > $GS/board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch
rm $BR/build/linux-custom/drivers/gpu/drm/sun4i/sun50i_planes.c.scaleorig
```

Then prepend a patch header (edit the file; keep the `---`/`+++` lines that follow):

```
drm/sun4i: de33: enable VI plane scaling

The DE33 quirks ship scaler_mask=0 ("TODO: All planes support scaling,
but driver needs improvements"), which makes sun8i_vi_layer_atomic_check()
reject any non-1:1 commit with ERANGE. The VI (video) path needs no
improvements: sun8i_vi_scaler.c is fully DE33-aware, and the VSU is
already enabled at 1:1 for every subsampled (NV12) frame. Enable the VI
channels (0-2); leave the unverified UI channels (6-8) at 1:1.

Validated on an Orange Pi Zero 2W (H618): NV12 1080p upscale/downscale
on the HDMI mixer's VI plane, kernel 6.18.35.

```

- [ ] **Step 3: Verify the patch applies to pristine source**

```bash
cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig/build/linux-6.18.35
patch -p1 --dry-run < ../../../../board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch
```

Expected: `checking file drivers/gpu/drm/sun4i/sun50i_planes.c` with no errors (dry-run only — the pristine tree stays pristine).

- [ ] **Step 4: Wire the patch dir into the defconfig**

In `configs/orangepi_zero2w_defconfig`, directly after the line
`BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="${BR2_EXTERNAL_OPENIPC_SBC_GS_PATH}/board/orangepi/zero2w/linux.config"`, add:

```
BR2_LINUX_KERNEL_PATCH="${BR2_EXTERNAL_OPENIPC_SBC_GS_PATH}/board/orangepi/zero2w/linux-patches"
```

(Same pattern as `configs/radxa_zero3_defconfig` line 22.)

- [ ] **Step 5: Commit (sbc-groundstations repo — only these two files)**

```bash
cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam
git add board/orangepi/zero2w/linux-patches/0001-drm-sun4i-de33-enable-vi-plane-scaling.patch \
        configs/orangepi_zero2w_defconfig
git commit -m "orangepi/zero2w: kernel patch to enable DE33 VI-plane scaling

scaler_mask 0 -> 0x7 (VI channels only) in sun50i_planes.c; wired via
BR2_LINUX_KERNEL_PATCH so it applies on top of the kernel tarball.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Rebuild kernel, deploy to the GS, re-run the TEST_ONLY probe

**Files:**
- No source changes (uses Task 1's edited build tree). Produces `output/orangepi_zero2w_defconfig/images/Image`.

**Interfaces:**
- Consumes: Task 1's edited `linux-custom` tree.
- Produces: patched kernel running on the GS; `scale-probe` (existing TEST_ONLY tool) reporting ACCEPTED for scaling. Gate for Tasks 3–5.

- [ ] **Step 1: Rebuild the kernel**

```bash
make -C /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig linux-rebuild
```

Expected: compiles `sun50i_planes.o`, relinks, ends with `Image` regenerated under `.../images/`. (~ minutes; do NOT run `linux-dirclean`.)

- [ ] **Step 2: Back up and deploy the kernel image**

```bash
ssh root@10.18.0.1 'cp /boot/Image /boot/Image.bak && ls -la /boot/'
scp -O /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig/images/Image root@10.18.0.1:/boot/Image
ssh root@10.18.0.1 'sync && reboot' || true
```

Expected: scp completes; ssh drops on reboot.

- [ ] **Step 3: Wait for the board and verify the new kernel booted**

```bash
sleep 25; for i in 1 2 3 4 5 6; do ssh -o ConnectTimeout=5 root@10.18.0.1 'uname -v; pidof citrusplay' && break; sleep 10; done
```

Expected: `uname -v` shows a build date of **today** (was `Fri Jun 19 …`); `pidof citrusplay` prints a pid (service auto-starts).
Recovery if the board never comes back: power-cycle; if still down, the SD card must be re-flashed or `Image.bak` restored via another host — flag to the user before proceeding.

- [ ] **Step 4: Re-run the TEST_ONLY scale probe**

The probe binary was in `/tmp` (tmpfs — gone after reboot); re-copy the aarch64 build (rebuild it if the scratchpad copy is gone: `$CC --sysroot=$STAGING -O2 -I$STAGING/usr/include src/tools/scale-probe.c -o scale-probe -L$STAGING/usr/lib -ldrm`).

```bash
scp -O <scale-probe-binary> root@10.18.0.1:/tmp/scale-probe
ssh root@10.18.0.1 '/etc/init.d/S99citruspilot stop; /tmp/scale-probe; rc=$?; /etc/init.d/S99citruspilot start; exit $rc'
```

Expected output flips vs the 2026-07-02 baseline:

```
  1:1 (control)                SRC 1920x1080 -> CRTC 1920x1080 : ACCEPTED
  upscale -> fullscreen        SRC 1920x1080 -> CRTC <mode>    : ACCEPTED
  downscale -> half            SRC 1920x1080 -> CRTC <mode/2>  : ACCEPTED
```

If still REJECTED (ERANGE): the running kernel is not the new one (check `uname -v`) or the patch didn't get compiled in — stop and re-investigate before any further task.

---

### Task 3: `scale-probe --commit` visual validation mode (+ Makefile wiring, commit the tool)

**Files:**
- Modify: `src/tools/scale-probe.c` (citruspilot repo — currently untracked; this task commits it)
- Modify: `Makefile:48-55` (add `scale-probe` to `TOOLS`)

**Interfaces:**
- Consumes: patched kernel on the GS (Task 2).
- Produces: `scale-probe --commit [seconds]` — real (non-TEST_ONLY) atomic commit of an NV12 test pattern scaled to fullscreen; operator visually confirms the VSU output. Gate for Tasks 4–5.

- [ ] **Step 1: Add the `--commit` mode to `src/tools/scale-probe.c`**

Add after the `make_nv12_fb()` function (reuse of its dumb-buffer creation requires returning the handle — extend it as shown):

```c
/* Like make_nv12_fb but also maps the buffer and paints a test pattern:
 * luma = horizontal gradient with a bright grid every 64 px, chroma = 8
 * vertical colour bars. Any scaling artefact (wrong pitch, chroma shift,
 * non-scaled crop) is obvious against this pattern. */
static uint32_t make_pattern_fb(int fd, int w, int h) {
    struct drm_mode_create_dumb cd = { .width = w, .height = h * 3 / 2, .bpp = 8 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 0; }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return 0; }
    uint8_t *p = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p == MAP_FAILED) { perror("mmap"); return 0; }
    static const uint8_t ub[8] = {128,  84, 255,   0, 170,  42, 212, 128};
    static const uint8_t vb[8] = {128, 255, 107, 148,   0, 201,  50,  21};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[y * cd.pitch + x] = (x % 64 < 2 || y % 64 < 2) ? 235 : 16 + (x * 219) / w;
    uint8_t *uv = p + cd.pitch * h;
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            int bar = (x * 2 * 8) / w;
            uv[y * cd.pitch + 2 * x]     = ub[bar];
            uv[y * cd.pitch + 2 * x + 1] = vb[bar];
        }
    munmap(p, cd.size);
    uint32_t handles[4] = { cd.handle, cd.handle, 0, 0 };
    uint32_t pitches[4] = { cd.pitch, cd.pitch, 0, 0 };
    uint32_t offsets[4] = { 0, cd.pitch * h, 0, 0 };
    uint32_t fb = 0;
    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_NV12, handles, pitches, offsets, &fb, 0))
        { perror("addfb2 pattern"); return 0; }
    return fb;
}
```

Add `#include <sys/mman.h>` and `#include <stdlib.h>` (for `atoi`) to the includes. In `main()`, change the signature to `int main(int argc, char **argv)` and add argument handling + the commit branch at the end (before `close(fd)`), after the existing TEST_ONLY block:

```c
    /* --commit [seconds]: REAL scaled commit of a test pattern, fullscreen.
     * Needs DRM master (stop citrusplay first) and an active CRTC. */
    int commit_secs = 0;
    if (argc > 1 && !strcmp(argv[1], "--commit"))
        commit_secs = argc > 2 ? atoi(argv[2]) : 5;

    if (commit_secs > 0) {
        if (mode_w <= 0 || mode_h <= 0) { fprintf(stderr, "CRTC inactive - start+stop citrusplay first\n"); return 1; }
        uint32_t pfb = make_pattern_fb(fd, sw, sh);
        if (!pfb) return 1;
        drmModeAtomicReq *r = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(r, plane, P_FB, pfb);
        drmModeAtomicAddProperty(r, plane, P_CRTC, crtc_id);
        drmModeAtomicAddProperty(r, plane, P_CX, 0);
        drmModeAtomicAddProperty(r, plane, P_CY, 0);
        drmModeAtomicAddProperty(r, plane, P_CW, mode_w);
        drmModeAtomicAddProperty(r, plane, P_CH, mode_h);
        drmModeAtomicAddProperty(r, plane, P_SX, 0);
        drmModeAtomicAddProperty(r, plane, P_SY, 0);
        drmModeAtomicAddProperty(r, plane, P_SW, (uint64_t)sw << 16);
        drmModeAtomicAddProperty(r, plane, P_SH, (uint64_t)sh << 16);
        int rc = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        printf("REAL commit SRC %dx%d -> CRTC %dx%d : %s\n", sw, sh, mode_w, mode_h,
               rc == 0 ? "OK - pattern should now FILL the screen" : strerror(errno));
        drmModeAtomicFree(r);
        if (rc == 0) sleep(commit_secs);
        return rc ? 1 : 0;
    }
```

(The plane is left attached on exit; restarting citrusplay does a full `ALLOW_MODESET` startup commit that reclaims everything.)

- [ ] **Step 2: Add scale-probe to the Makefile tools target**

In `Makefile`, change line 48 `TOOLS := plane-formats plane-probe v4l2-formats` to:

```make
TOOLS := plane-formats plane-probe v4l2-formats scale-probe
```

and add after the `plane-probe` rule:

```make
$(SRC)/tools/scale-probe: $(SRC)/tools/scale-probe.c
	$(CC) $(ALLCFLAGS) $< -o $@ $(LDFLAGS) -ldrm
```

- [ ] **Step 3: Cross-build and verify it compiles clean**

```bash
cd /home/gilankpam/Projects/poc/citruspilot
BR=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig
make tools SYSROOT=$BR/staging CC=$BR/host/bin/aarch64-none-linux-gnu-gcc
```

Expected: builds `src/tools/scale-probe` (aarch64) with no warnings.

- [ ] **Step 4: Run on the GS — visual confirmation (operator required)**

```bash
scp -O src/tools/scale-probe root@10.18.0.1:/tmp/scale-probe
ssh root@10.18.0.1 '/etc/init.d/S99citruspilot stop; /tmp/scale-probe --commit 8; rc=$?; /etc/init.d/S99citruspilot start; exit $rc'
```

Expected: tool prints `REAL commit SRC 1920x1080 -> CRTC <mode> : OK`. **Ask the user to confirm on the physical screen:** gradient+grid+colour bars fill the whole screen (the pattern is 1080p, the mode may differ), grid lines straight, bars uniform, no tearing/garbage. Do not proceed to Task 4 without this confirmation.

- [ ] **Step 5: Commit (citruspilot repo)**

```bash
cd /home/gilankpam/Projects/poc/citruspilot
git add src/tools/scale-probe.c Makefile
git commit -m "tools: scale-probe — DE33 plane-scaling probe (+ --commit visual test)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: `fit_rect()` aspect-fit helper (TDD, host)

**Files:**
- Create: `src/fit_rect.h`
- Create: `src/tests/test_fit_rect.c`
- Modify: `src/tests/run-tests.sh:16-19` (add the test)

**Interfaces:**
- Produces (consumed by Task 5):

```c
typedef struct { int x, y, w, h; } fit_rect_t;
static inline fit_rect_t fit_rect(int src_w, int src_h, int mode_w, int mode_h);
/* aspect-preserving largest fit, centred; x/y/w/h rounded DOWN to even;
 * any non-positive input -> all-zero rect (caller treats as "don't scale") */
```

- [ ] **Step 1: Write the failing test — `src/tests/test_fit_rect.c`**

```c
#include "../fit_rect.h"
#include <assert.h>

int main(void) {
    fit_rect_t r;

    /* same aspect, upscale to exact fit */
    r = fit_rect(1280, 720, 1920, 1080);
    assert(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);

    /* same aspect, downscale (4k stream into 1080p mode) */
    r = fit_rect(3840, 2160, 1920, 1080);
    assert(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);

    /* already exact */
    r = fit_rect(1920, 1080, 1920, 1080);
    assert(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);

    /* letterbox: 16:9 into a taller 3:2 mode (the GS panel's 1920x1280) */
    r = fit_rect(1920, 1080, 1920, 1280);
    assert(r.x == 0 && r.y == 100 && r.w == 1920 && r.h == 1080);

    /* pillarbox: 4:3 into 16:9 */
    r = fit_rect(640, 480, 1920, 1080);
    assert(r.x == 240 && r.y == 0 && r.w == 1440 && r.h == 1080);

    /* portrait mode, odd results round DOWN to even */
    r = fit_rect(1920, 1080, 1080, 1920);
    assert(r.w == 1080 && r.h == 606);       /* 607 -> 606 */
    assert(r.x == 0 && r.y == 656);          /* 657 -> 656 */

    /* degenerate inputs -> zero rect */
    r = fit_rect(0, 1080, 1920, 1080);
    assert(r.x == 0 && r.y == 0 && r.w == 0 && r.h == 0);
    r = fit_rect(1920, 1080, 0, 0);
    assert(r.w == 0 && r.h == 0);
    return 0;
}
```

Add to `src/tests/run-tests.sh` after the `test_sdp` line:

```sh
build_and_run test_fit_rect    test_fit_rect.c
```

- [ ] **Step 2: Run — verify it fails**

Run: `src/tests/run-tests.sh`
Expected: FAIL at compile — `../fit_rect.h: No such file or directory`.

- [ ] **Step 3: Implement `src/fit_rect.h`**

```c
/* fit_rect — aspect-preserving fit of a video into a display mode.
 * Pure/header-only so the host unit tests build it without DRM/libav. */
#ifndef FIT_RECT_H
#define FIT_RECT_H

typedef struct { int x, y, w, h; } fit_rect_t;

/* Largest src_w:src_h rectangle that fits mode_w x mode_h, centred
 * (letterbox/pillarbox). x/y/w/h are rounded DOWN to even — NV12 chroma is
 * 2x2-subsampled and even coords keep the DE33 blender happy. Non-positive
 * input yields the zero rect (caller: treat as "cannot scale"). */
static inline fit_rect_t fit_rect(int src_w, int src_h, int mode_w, int mode_h)
{
    fit_rect_t r = {0, 0, 0, 0};
    if (src_w <= 0 || src_h <= 0 || mode_w <= 0 || mode_h <= 0)
        return r;
    long long sw_mh = (long long)src_w * mode_h;
    long long sh_mw = (long long)src_h * mode_w;
    if (sw_mh >= sh_mw) {              /* source is wider: full width, letterbox */
        r.w = mode_w;
        r.h = (int)(sh_mw / src_w);
    } else {                           /* source is taller: full height, pillarbox */
        r.h = mode_h;
        r.w = (int)(sw_mh / src_h);
    }
    r.w &= ~1;
    r.h &= ~1;
    if (r.w <= 0 || r.h <= 0)
        return (fit_rect_t){0, 0, 0, 0};
    r.x = ((mode_w - r.w) / 2) & ~1;
    r.y = ((mode_h - r.h) / 2) & ~1;
    return r;
}

#endif
```

- [ ] **Step 4: Run — verify it passes**

Run: `src/tests/run-tests.sh`
Expected: `PASS test_fit_rect` … `all host tests passed`.

- [ ] **Step 5: Commit**

```bash
cd /home/gilankpam/Projects/poc/citruspilot
git add src/fit_rect.h src/tests/test_fit_rect.c src/tests/run-tests.sh
git commit -m "player: fit_rect aspect-fit helper (host-tested)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: citrusplay — scale-to-fullscreen with retune fallback

**Files:**
- Modify: `src/citrusplay.c` (env block ~415, `commit_video_plane` 217–231, first-frame branch 621–626, header comment 11–21)

**Interfaces:**
- Consumes: `fit_rect_t fit_rect(int,int,int,int)` from `src/fit_rect.h` (Task 4); patched kernel (Task 2).
- Produces: default behavior = stay in startup mode + hardware aspect-fit scaling; `CITRUSPLAY_RETUNE=1` or kernel rejection (TEST_ONLY probe) = old retune+centre path.

- [ ] **Step 1: Add include, globals, env knob**

After `#include "osd_render.h"` add:

```c
#include "fit_rect.h"
```

Next to the other `opt_*` globals (line ~77) add:

```c
static int opt_retune;              /* CITRUSPLAY_RETUNE=1: force HDMI mode retune (old behavior) */
```

In `main()`'s env block (after the `opt_debug` line, ~418):

```c
    opt_retune = getenv("CITRUSPLAY_RETUNE") ? atoi(getenv("CITRUSPLAY_RETUNE")) : 0;
```

- [ ] **Step 2: Generalise `commit_video_plane` and add the capability probe**

Replace the whole `commit_video_plane()` (lines 215–231) with:

```c
/* Bring up / reconfigure the video overlay plane: SRC = the full vw x vh
 * frame, CRTC = rect d (either an aspect-fit scaled rect or a centred 1:1
 * one). Called on the first decoded frame and on stream-size changes. */
static void commit_video_plane(uint32_t vfb, int vw, int vh, fit_rect_t d)
{
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    add_plane(r, vplane_id, Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH,
              vfb, d.x, d.y, d.w, d.h, vw, vh);
    if (Vp_ZPOS)   drmModeAtomicAddProperty(r, vplane_id, Vp_ZPOS, video_zpos);
    if (Vp_CENC)   drmModeAtomicAddProperty(r, vplane_id, Vp_CENC, opt_enc);
    if (Vp_CRANGE) drmModeAtomicAddProperty(r, vplane_id, Vp_CRANGE, opt_range);
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        LOG("video plane commit: %s", strerror(errno));
    drmModeAtomicFree(r);
}

/* Centred 1:1 rect — the pre-scaling behavior (crops when the mode is
 * smaller than the video; the kernel clips the plane to the CRTC). */
static fit_rect_t centred_1to1(int vw, int vh)
{
    fit_rect_t d = { (mode.hdisplay - vw) / 2, (mode.vdisplay - vh) / 2, vw, vh };
    if (d.x < 0) d.x = 0;
    if (d.y < 0) d.y = 0;
    return d;
}

/* Will the kernel scale the video plane to rect d? TEST_ONLY commit — no
 * visible effect. An unpatched DE33 kernel rejects non-1:1 with ERANGE;
 * then we fall back to retuning the HDMI mode as before. */
static int probe_plane_scaling(uint32_t vfb, int vw, int vh, fit_rect_t d)
{
    if (d.w == vw && d.h == vh) return 1;     /* 1:1 — nothing to prove */
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    add_plane(r, vplane_id, Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH,
              vfb, d.x, d.y, d.w, d.h, vw, vh);
    int rc = drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
    if (rc) LOG("plane scaling unavailable (%s) — falling back to mode retune", strerror(errno));
    drmModeAtomicFree(r);
    return rc == 0;
}
```

- [ ] **Step 3: Rewrite the first-frame / size-change branch**

Replace lines 621–626 (`if (!video_up || ...) { ... }` body) with:

```c
            if (!video_up || pending->width != cur_w || pending->height != cur_h) {
                drain_flip();   /* no flip may be in flight across a reconfigure */
                fit_rect_t d = fit_rect(pending->width, pending->height,
                                        mode.hdisplay, mode.vdisplay);
                int scaled = !opt_retune && d.w > 0 &&
                             probe_plane_scaling(pending_fb, pending->width, pending->height, d);
                if (!scaled) {   /* forced or kernel can't scale: old behavior */
                    retune_mode(pending->width, pending->height, &pfb);
                    d = centred_1to1(pending->width, pending->height);
                }
                commit_video_plane(pending_fb, pending->width, pending->height, d);
                cur_w = pending->width; cur_h = pending->height;
                video_up = 1;
                LOG("playing %dx%d -> %dx%d%+d%+d in %s%s.", cur_w, cur_h,
                    d.w, d.h, d.x, d.y, mode.name, scaled ? " (HW scaled)" : " (1:1)");
            } else {
```

(The `else { drain_flip(); flip(pending_fb); }` and the rest of the block stay as they are.)

- [ ] **Step 4: Update the file-header comment and the stale 1:1 notes**

In the header block: line 11–14, replace

```
//   * RUN FOREVER — the screen is brought up at startup (preferred mode, black
//     primary); the video plane appears on the first decoded frame, at which
//     point the HDMI mode is retuned to MATCH the stream size (DE33 is 1:1, no
//     scaler — see retune_mode). On a stream drop the last frame stays FROZEN
```

with

```
//   * RUN FOREVER — the screen is brought up at startup (preferred mode, black
//     primary); the video plane appears on the first decoded frame, HW-scaled
//     to aspect-fit the mode (kernel scaler_mask patch; falls back to retuning
//     the HDMI mode to the stream size on unpatched 1:1-only kernels, or set
//     CITRUSPLAY_RETUNE=1 to force that). On a stream drop the frame stays FROZEN
```

and line 20–21, replace

```
// Requires kernel patch 0099 (NV12 on the DE33 VI plane) + linear-NV12 Cedrus,
// and scans 1:1 (the DE33 VI scaler can't upscale), centring sub-mode video.
```

with

```
// Requires kernel patch 0099 (NV12 on the DE33 VI plane) + linear-NV12 Cedrus;
// full-screen scaling additionally needs the DE33 scaler_mask kernel patch.
```

Also update the comment above the size-change branch (lines 615–620): drop "(and the HDMI mode)" wording? No — it still applies in fallback; instead just change the last sentence to "Re-running the full setup re-fits the plane (and retunes the mode in fallback)."

- [ ] **Step 5: Build both native-host-check and cross; run host tests**

```bash
cd /home/gilankpam/Projects/poc/citruspilot
src/tests/run-tests.sh
BR=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig
make SYSROOT=$BR/staging CC=$BR/host/bin/aarch64-none-linux-gnu-gcc
```

Expected: all host tests pass; `citrusplay` cross-builds with no new warnings.

- [ ] **Step 6: Deploy to the GS and validate end-to-end**

```bash
ssh root@10.18.0.1 '/etc/init.d/S99citruspilot stop'
scp -O citrusplay root@10.18.0.1:/usr/bin/citrusplay
ssh root@10.18.0.1 '/etc/init.d/S99citruspilot start; sleep 3; pidof citrusplay'
```

With a stream feeding udp:5600 (live drone, or `smoke/` loopback with `/root/sample_inband.mp4` per `smoke/test-player-loopback.sh`), verify **objectively via debugfs** (works without seeing the screen):

```bash
ssh root@10.18.0.1 'cat /sys/kernel/debug/dri/*/state | sed -n "/plane\[33/,/plane\[39/p"'
```

Expected: the VI plane shows `src` = stream size (e.g. `1920x1080`) and `crtc` = the fit rect **differing from src** (e.g. `1920x1080+0+100` in a 1920x1280 mode — or full-mode when aspect matches), while the CRTC mode is still the **startup** mode (no retune). Also check the player log line: `playing 1920x1080 -> WxH+X+Y in <mode> (HW scaled).`
**Ask the user to visually confirm** fullscreen video with correct aspect and no artifacts.

- [ ] **Step 7: Validate the fallback path**

```bash
ssh root@10.18.0.1 '/etc/init.d/S99citruspilot stop; CITRUSPLAY_RETUNE=1 /usr/bin/citrusplay --port 5600 >/tmp/retune.log 2>&1 & sleep 15; grep -i "retuned\|playing" /tmp/retune.log; cat /sys/kernel/debug/dri/*/state | grep -A3 "crtc\["; kill $(pidof citrusplay); sleep 1; /etc/init.d/S99citruspilot start'
```

Expected: with the env knob the old behavior returns — log shows `retuned HDMI to …` (when the sink offers the stream mode) and the plane is 1:1. (This also stands in for the unpatched-kernel ERANGE fallback, which exercises the same code path via `probe_plane_scaling` returning 0.)

- [ ] **Step 8: Commit**

```bash
cd /home/gilankpam/Projects/poc/citruspilot
git add src/citrusplay.c
git commit -m "player: HW-scale the video plane to fit the mode (drop retune by default)

Uses the DE33 scaler_mask kernel patch; TEST_ONLY-probes scaling per
stream size and falls back to the old HDMI mode retune when the kernel
is 1:1-only or CITRUSPLAY_RETUNE=1.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Documentation

**Files:**
- Modify: `README.md` (Limitations section + env knobs line ~56-59)
- Create: `docs/findings/de33-plane-scaling.md`

**Interfaces:** none (docs only).

- [ ] **Step 1: Rewrite the README limitation bullet**

Replace the whole `- **No video scaling (DE33 VI plane is 1:1 only).** …` bullet with:

```markdown
- **Video scaling needs a patched kernel (DE33 ships 1:1-only).** Mainline
  disables the DE33 plane scaler (`sun50i_planes.c` sets `.scaler_mask = 0`,
  "TODO: All planes support scaling, but driver needs improvements") so any
  non-1:1 commit fails with `ERANGE` — a **driver gap, not a hardware limit**
  (probed live with `src/tools/scale-probe.c`). The sbc-groundstations image
  carries a one-line kernel patch enabling the VI (video) channels
  (`scaler_mask = 0x7`, validated on the H618 VSU), and `citrusplay` then
  HW-scales the stream to aspect-fit the sink's startup mode — no HDMI mode
  switch. On an unpatched kernel (probed per stream size with a `TEST_ONLY`
  commit), or with `CITRUSPLAY_RETUNE=1`, it falls back to the old behavior:
  retune the HDMI output to the EDID mode matching the stream (see
  `retune_mode` in `src/citrusplay.c`), else centre the frame 1:1.
```

Add `CITRUSPLAY_RETUNE` to the env-knob list (line ~56): `CITRUSPLAY_RETUNE (1=force HDMI mode retune instead of HW scaling, default 0)`.

- [ ] **Step 2: Write `docs/findings/de33-plane-scaling.md`**

Content: the investigation (scale-probe ERANGE baseline → `scaler_mask=0` TODO root cause → mask 0x7 patch), evidence (VSU already active 1:1 for NV12; upstream v7 series shipped 0xf), validation results from Tasks 2/3/5 (paste the actual probe output and debugfs plane state), and the buildroot persistence wiring. Written after the on-board results exist — include real outputs, not placeholders.

- [ ] **Step 3: Commit**

```bash
cd /home/gilankpam/Projects/poc/citruspilot
git add README.md docs/findings/de33-plane-scaling.md
git commit -m "docs: DE33 plane scaling — README + findings

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
