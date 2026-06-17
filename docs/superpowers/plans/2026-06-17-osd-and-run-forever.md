# OSD Overlay + Resilient Run-Forever Player — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an always-on system-stats OSD (CPU/mem/temp) composited above the zero-copy video, and turn the player into a resilient daemon that listens on a configurable UDP port forever, freezing the last frame on signal loss.

**Architecture:** New, isolated, host-testable modules (`sdp.h`, `stats.c`, `osd_render.c`) carry the pure logic; a DRM-bound `osd.c` owns the OSD overlay plane; `wfbvid.c` is refactored into a startup-modeset + run-forever decode loop. The OSD uses a single in-place ARGB dumb buffer on a dedicated overlay plane above the video — set up once at modeset, rewritten in place at 1 Hz, so the video page-flip path is untouched.

**Tech Stack:** C, libdrm (atomic KMS), libavformat/libavcodec/libavutil (v4l2request HW decode), `/proc` + `/sys` for stats. No new runtime dependencies (bitmap font baked in; Pillow used only as a one-time dev-host generator).

---

## Testing model (read first)

This is bare-metal C with no test framework. We split testing by what is verifiable where:

- **Host-side unit tests** (`player/tests/*.c`): pure logic — SDP composition, `/proc` parsing, glyph/box rendering into a memory buffer. Plain `cc` + `assert`, **runnable on any machine** (including the dev host, in-session). These get strict TDD: failing test → minimal code → passing test → commit.
- **On-board verification** (DRM plane selection, atomic commits, Cedrus decode, freeze-frame, reconnect): cannot run on the dev host. These tasks specify the exact build command and a **manual verification procedure on the Orange Pi Zero 2W**, driven by the existing `smoke/` synthetic-RTP harness (no radio needed). Steps marked **[BOARD]** require the board.

## File structure & interfaces

New files:
- `player/sdp.h` — `static inline int compose_sdp(char *out, size_t n, int port, int pt)`. Header-only.
- `player/stats.h` / `player/stats.c` — system-stats sampling. Pure parsers + a file-reading sampler.
- `player/font8x16.h` — `static const uint8_t font8x16[95][16]` (ASCII 0x20–0x7E). Generated, committed.
- `player/tools/gen_font8x16.py` — one-time dev-host generator for `font8x16.h`.
- `player/osd_render.h` / `player/osd_render.c` — pure ARGB8888 text/box rendering. No DRM.
- `player/osd.h` / `player/osd.c` — DRM-bound OSD overlay plane (dumb buffer + plane props). Uses `stats` + `osd_render`.
- `player/tests/test_sdp.c`, `test_stats.c`, `test_osd_render.c`, `run-tests.sh` — host unit tests.

Modified:
- `player/wfbvid.c` — refactored for run-forever + built-in SDP + `--port` + startup modeset + OSD wiring.
- `player/wfbplay` — `[--port N]` passthrough.
- `README.md` — build command (multi-file) + OSD/daemon usage.

Removed:
- `player/wfb-h265.sdp` — no longer used (SDP template now lives in code).

Build command after this plan (board):
```sh
cc player/wfbvid.c player/osd.c player/osd_render.c player/stats.c \
   -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
```

---

## Task 1: SDP template module (`sdp.h`)

**Files:**
- Create: `player/sdp.h`
- Test: `player/tests/test_sdp.c`, `player/tests/run-tests.sh`

- [ ] **Step 1: Write the failing test**

Create `player/tests/test_sdp.c`:
```c
#include "../sdp.h"
#include <assert.h>
#include <string.h>

int main(void) {
    char buf[512];
    int len = compose_sdp(buf, sizeof buf, 5600, 97);
    assert(len > 0);
    assert(strstr(buf, "m=video 5600 RTP/AVP 97") != NULL);
    assert(strstr(buf, "a=rtpmap:97 H265/90000") != NULL);

    /* a different port + payload type is substituted */
    assert(compose_sdp(buf, sizeof buf, 5602, 96) > 0);
    assert(strstr(buf, "m=video 5602 RTP/AVP 96") != NULL);

    /* too-small buffer reports failure rather than truncating silently */
    char small[8];
    assert(compose_sdp(small, sizeof small, 5600, 97) == -1);
    return 0;
}
```

Create `player/tests/run-tests.sh`:
```sh
#!/bin/sh
# Host-side unit tests for the pure CitrusPilot modules. Runs on any machine
# with a C compiler — no DRM, libav, or board required.
set -eu
cd "$(dirname "$0")"
CC="${CC:-cc}"
CFLAGS="-O2 -Wall -Wextra -std=c11"

build_and_run() {
    name="$1"; shift
    $CC $CFLAGS -o "/tmp/$name" "$@"
    "/tmp/$name"
    echo "PASS $name"
}

build_and_run test_sdp         test_sdp.c
build_and_run test_stats       test_stats.c ../stats.c
build_and_run test_osd_render  test_osd_render.c ../osd_render.c
echo "all host tests passed"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -O2 -Wall -std=c11 -o /tmp/test_sdp player/tests/test_sdp.c`
Expected: FAIL — `fatal error: ../sdp.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `player/sdp.h`:
```c
#ifndef CITRUSPILOT_SDP_H
#define CITRUSPILOT_SDP_H
#include <stdio.h>
#include <stddef.h>

/* Compose a minimal H.265 RTP SDP that listens on `port` with dynamic payload
 * type `pt`, into out[0..n). The codec parameter sets (VPS/SPS/PPS) are not in
 * the SDP — they arrive in-band on the first IDR. Returns the string length
 * (excluding the NUL terminator), or -1 if it did not fit. */
static inline int compose_sdp(char *out, size_t n, int port, int pt)
{
    int len = snprintf(out, n,
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=CitrusPilot\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video %d RTP/AVP %d\r\n"
        "a=rtpmap:%d H265/90000\r\n",
        port, pt, pt);
    if (len < 0 || (size_t)len >= n) return -1;
    return len;
}
#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -O2 -Wall -std=c11 -o /tmp/test_sdp player/tests/test_sdp.c && /tmp/test_sdp && echo OK`
Expected: PASS (prints `OK`).

- [ ] **Step 5: Commit**

```bash
chmod +x player/tests/run-tests.sh
git add player/sdp.h player/tests/test_sdp.c player/tests/run-tests.sh
git commit -m "player: add built-in SDP template (compose_sdp) + host test"
```

---

## Task 2: System-stats module (`stats.c`)

**Files:**
- Create: `player/stats.h`, `player/stats.c`
- Test: `player/tests/test_stats.c`

- [ ] **Step 1: Write the failing test**

Create `player/tests/test_stats.c`:
```c
#include "../stats.h"
#include <assert.h>

int main(void) {
    uint64_t total, idle;
    /* user nice system idle iowait irq softirq steal guest guest_nice */
    const char *st = "cpu  100 0 100 800 0 0 0 0 0 0\n"
                     "cpu0 50 0 50 400 0 0 0 0 0 0\n";
    assert(stats_parse_cpu(st, &total, &idle) == 0);
    assert(total == 1000);       /* sum of all fields */
    assert(idle == 800);         /* idle + iowait */
    assert(stats_parse_cpu("garbage\n", &total, &idle) == -1);

    int used, tot;
    const char *mi = "MemTotal:        1024000 kB\n"
                     "MemFree:           10000 kB\n"
                     "MemAvailable:     512000 kB\n";
    assert(stats_parse_mem(mi, &used, &tot) == 0);
    assert(tot == 1000);         /* 1024000 kB / 1024 = 1000 MiB */
    assert(used == 500);         /* (1024000-512000)/1024 = 500 MiB */
    assert(stats_parse_mem("MemTotal: 1 kB\n", &used, &tot) == -1); /* no MemAvailable */

    int c;
    assert(stats_parse_temp("48123\n", &c) == 0);
    assert(c == 48);
    assert(stats_parse_temp("oops", &c) == -1);
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -O2 -Wall -std=c11 -o /tmp/test_stats player/tests/test_stats.c player/stats.c`
Expected: FAIL — `fatal error: ../stats.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `player/stats.h`:
```c
#ifndef CITRUSPILOT_STATS_H
#define CITRUSPILOT_STATS_H
#include <stdint.h>

/* Carries the previous CPU jiffie counters so load is a delta across samples. */
typedef struct {
    uint64_t prev_total, prev_idle;
    int have_prev;
} stats_t;

typedef struct {
    int cpu_pct;       /* 0..100 aggregate load, or -1 if unknown */
    int mem_used_mb;   /* MiB used, or -1 */
    int mem_total_mb;  /* MiB total */
    int temp_c;        /* hottest thermal zone in degrees C, or -1 */
} stats_sample_t;

/* Pure parsers (no I/O) — unit tested. Return 0 on success, -1 on failure. */
int stats_parse_cpu(const char *proc_stat, uint64_t *total, uint64_t *idle);
int stats_parse_mem(const char *proc_meminfo, int *used_mb, int *total_mb);
int stats_parse_temp(const char *temp_millideg, int *deg_c);

/* Reset the CPU delta state. */
void stats_init(stats_t *st);
/* Read /proc + /sys and fill `out`. Uses `st` to compute CPU load vs the prior
 * call (first call leaves cpu_pct == -1, since there is no delta yet). */
void stats_sample(stats_t *st, stats_sample_t *out);
#endif
```

Create `player/stats.c`:
```c
#define _GNU_SOURCE
#include "stats.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int stats_parse_cpu(const char *s, uint64_t *total, uint64_t *idle)
{
    const char *p = strstr(s, "cpu ");
    if (!p) return -1;
    p += 4;
    const char *nl = strchr(p, '\n');
    const char *end = nl ? nl : p + strlen(p);

    uint64_t v[10] = {0};
    int n = 0;
    const char *q = p;
    while (n < 10 && q < end) {
        char *e;
        unsigned long long x = strtoull(q, &e, 10);
        if (e == q) break;       /* no more digits on this line */
        v[n++] = x;
        q = e;
    }
    if (n < 4) return -1;        /* need at least user/nice/system/idle */

    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += v[i];
    *total = t;
    *idle  = v[3] + (n > 4 ? v[4] : 0);   /* idle + iowait */
    return 0;
}

static int find_kb(const char *s, const char *key, long *kb)
{
    const char *p = strstr(s, key);
    if (!p) return -1;
    *kb = strtol(p + strlen(key), NULL, 10);
    return 0;
}

int stats_parse_mem(const char *s, int *used_mb, int *total_mb)
{
    long tot, avail;
    if (find_kb(s, "MemTotal:", &tot)) return -1;
    if (find_kb(s, "MemAvailable:", &avail)) return -1;
    long used = tot - avail;
    if (used < 0) used = 0;
    *total_mb = (int)(tot / 1024);
    *used_mb  = (int)(used / 1024);
    return 0;
}

int stats_parse_temp(const char *s, int *deg_c)
{
    char *e;
    long milli = strtol(s, &e, 10);
    if (e == s) return -1;
    *deg_c = (int)(milli / 1000);
    return 0;
}

void stats_init(stats_t *st)
{
    st->prev_total = st->prev_idle = 0;
    st->have_prev = 0;
}

static char *slurp(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return NULL;
    buf[r] = '\0';
    return buf;
}

void stats_sample(stats_t *st, stats_sample_t *out)
{
    char buf[8192];
    out->cpu_pct = out->mem_used_mb = out->mem_total_mb = out->temp_c = -1;

    if (slurp("/proc/stat", buf, sizeof buf)) {
        uint64_t total, idle;
        if (stats_parse_cpu(buf, &total, &idle) == 0) {
            if (st->have_prev && total > st->prev_total) {
                uint64_t dt = total - st->prev_total;
                uint64_t di = idle  - st->prev_idle;
                out->cpu_pct = (int)((100 * (dt - di)) / dt);
            }
            st->prev_total = total;
            st->prev_idle  = idle;
            st->have_prev  = 1;
        }
    }

    if (slurp("/proc/meminfo", buf, sizeof buf))
        stats_parse_mem(buf, &out->mem_used_mb, &out->mem_total_mb);

    for (int z = 0; z < 16; z++) {
        char path[64], tb[32];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        if (slurp(path, tb, sizeof tb)) {
            int c;
            if (stats_parse_temp(tb, &c) == 0 && c > out->temp_c)
                out->temp_c = c;
        }
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -O2 -Wall -std=c11 -o /tmp/test_stats player/tests/test_stats.c player/stats.c && /tmp/test_stats && echo OK`
Expected: PASS (prints `OK`).

- [ ] **Step 5: Commit**

```bash
git add player/stats.h player/stats.c player/tests/test_stats.c
git commit -m "player: add system-stats sampling (cpu/mem/temp) + host test"
```

---

## Task 3: Baked-in bitmap font (`font8x16.h`)

We do not hand-author pixel data. A one-time dev-host generator rasterizes ASCII into an 8×16 array using a ubiquitous monospace TTF; its output is committed so the board never needs Python.

**Files:**
- Create: `player/tools/gen_font8x16.py`
- Create (generated): `player/font8x16.h`

- [ ] **Step 1: Write the generator**

Create `player/tools/gen_font8x16.py`:
```python
#!/usr/bin/env python3
"""Generate player/font8x16.h: an 8x16 bitmap for ASCII 0x20..0x7E.

One-time dev-host step (needs Pillow + a DejaVu mono TTF). Output is committed;
the board/runtime never runs this. Each glyph is 16 bytes (one per row); bit
0x80 is the leftmost column.

    pip install pillow      # or: apt-get install python3-pil fonts-dejavu-core
    python3 player/tools/gen_font8x16.py
"""
import os
from PIL import Image, ImageFont, ImageDraw

W, H = 8, 16
CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/Library/Fonts/DejaVuSansMono.ttf",
]
path = next((p for p in CANDIDATES if os.path.exists(p)), None)
if not path:
    raise SystemExit("DejaVuSansMono.ttf not found; install fonts-dejavu-core")

font = ImageFont.truetype(path, 14)
rows = []
for code in range(0x20, 0x7F):
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text((0, -1), chr(code), fill=255, font=font)
    px = img.load()
    glyph = []
    for y in range(H):
        bits = 0
        for x in range(W):
            if px[x, y] > 96:
                bits |= 0x80 >> x
        glyph.append(bits)
    rows.append((code, glyph))

with open(os.path.join(os.path.dirname(__file__), "..", "font8x16.h"), "w") as f:
    f.write("#ifndef CITRUSPILOT_FONT8X16_H\n#define CITRUSPILOT_FONT8X16_H\n")
    f.write("#include <stdint.h>\n")
    f.write("/* Generated by tools/gen_font8x16.py. ASCII 0x20..0x7E, 8x16, MSB=left. */\n")
    f.write("static const uint8_t font8x16[95][16] = {\n")
    for code, glyph in rows:
        body = ",".join("0x%02x" % b for b in glyph)
        f.write("    { %s }, /* 0x%02x '%s' */\n"
                % (body, code, chr(code) if code != 0x20 else " "))
    f.write("};\n#endif\n")
print("wrote player/font8x16.h")
```

- [ ] **Step 2: Run the generator**

Run: `python3 player/tools/gen_font8x16.py`
Expected: prints `wrote player/font8x16.h`. If Pillow/DejaVu missing, install with `apt-get install python3-pil fonts-dejavu-core` (or `pip install pillow`) and rerun.

- [ ] **Step 3: Sanity-check the output**

Run: `head -6 player/font8x16.h && grep -c '{ 0x' player/font8x16.h`
Expected: header guard present and the count is `95` (one row per printable ASCII char).

- [ ] **Step 4: Commit**

```bash
git add player/tools/gen_font8x16.py player/font8x16.h
git commit -m "player: add baked 8x16 ASCII bitmap font + generator"
```

---

## Task 4: Pure ARGB text renderer (`osd_render.c`)

**Files:**
- Create: `player/osd_render.h`, `player/osd_render.c`
- Test: `player/tests/test_osd_render.c`

- [ ] **Step 1: Write the failing test**

Create `player/tests/test_osd_render.c`:
```c
#include "../osd_render.h"
#include "../font8x16.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    int w, h;
    osd_box_size(12, 3, 2, 4, &w, &h);    /* cols, lines, scale, pad */
    assert(w == 12 * 8 * 2 + 2 * 4);
    assert(h == 3 * 16 * 2 + 2 * 4);

    uint32_t bg = OSD_ARGB(0x80, 0, 0, 0);
    uint32_t fg = OSD_ARGB(0xff, 0xff, 0xff, 0xff);
    uint32_t *buf = calloc((size_t)w * h, sizeof *buf);
    assert(buf);

    osd_fill(buf, w, h, w, bg);
    assert(buf[0] == bg);
    assert(buf[(size_t)w * h - 1] == bg);

    /* draw_text advances 8*scale px per glyph and returns the new x */
    int x2 = osd_draw_text(buf, w, h, w, 4, 4, "AB", fg, 2);
    assert(x2 == 4 + 2 * (8 * 2));

    /* the font actually has ink for a printable glyph */
    int ink = 0;
    for (int i = 0; i < 16; i++) ink += font8x16['A' - 0x20][i];
    assert(ink > 0);

    /* drawing past the right edge must not crash (clipping) */
    osd_draw_char(buf, w, h, w, w - 2, h - 2, 'X', fg, 3);

    free(buf);
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -O2 -Wall -std=c11 -o /tmp/test_osd_render player/tests/test_osd_render.c player/osd_render.c`
Expected: FAIL — `fatal error: ../osd_render.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `player/osd_render.h`:
```c
#ifndef CITRUSPILOT_OSD_RENDER_H
#define CITRUSPILOT_OSD_RENDER_H
#include <stdint.h>
#include <stddef.h>

/* ARGB8888 little-endian: 0xAARRGGBB packed in a uint32_t. */
#define OSD_ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
     ((uint32_t)(g) << 8)  | (uint32_t)(b))

/* All functions take a pixel buffer with `stride_px` uint32_t per row. */
void osd_fill(uint32_t *buf, int w, int h, int stride_px, uint32_t argb);

/* Blit one 8x16 glyph at (x,y), integer-scaled, in `color`. Font-bit-0 pixels
 * are left untouched (transparent). Clipped to the buffer. */
void osd_draw_char(uint32_t *buf, int w, int h, int stride_px,
                   int x, int y, char c, uint32_t color, int scale);

/* Draw a NUL-terminated string; advances 8*scale px per char. Returns the x
 * just past the last glyph. */
int osd_draw_text(uint32_t *buf, int w, int h, int stride_px,
                  int x, int y, const char *s, uint32_t color, int scale);

/* Pixel size for `nlines` rows of `cols` chars at `scale`, with `pad` px on
 * every side. */
void osd_box_size(int cols, int nlines, int scale, int pad, int *w, int *h);
#endif
```

Create `player/osd_render.c`:
```c
#include "osd_render.h"
#include "font8x16.h"

void osd_fill(uint32_t *buf, int w, int h, int stride_px, uint32_t argb)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            buf[(size_t)y * stride_px + x] = argb;
}

void osd_draw_char(uint32_t *buf, int w, int h, int stride_px,
                   int x, int y, char c, uint32_t color, int scale)
{
    unsigned uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7e) return;          /* blank for non-printable */
    const uint8_t *g = font8x16[uc - 0x20];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    buf[(size_t)py * stride_px + px] = color;
                }
        }
    }
}

int osd_draw_text(uint32_t *buf, int w, int h, int stride_px,
                  int x, int y, const char *s, uint32_t color, int scale)
{
    for (; *s; s++) {
        osd_draw_char(buf, w, h, stride_px, x, y, *s, color, scale);
        x += 8 * scale;
    }
    return x;
}

void osd_box_size(int cols, int nlines, int scale, int pad, int *w, int *h)
{
    *w = cols * 8 * scale + 2 * pad;
    *h = nlines * 16 * scale + 2 * pad;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -O2 -Wall -std=c11 -o /tmp/test_osd_render player/tests/test_osd_render.c player/osd_render.c && /tmp/test_osd_render && echo OK`
Expected: PASS (prints `OK`).

- [ ] **Step 5: Run the whole host suite + commit**

Run: `player/tests/run-tests.sh`
Expected: `PASS test_sdp`, `PASS test_stats`, `PASS test_osd_render`, `all host tests passed`.

```bash
git add player/osd_render.h player/osd_render.c player/tests/test_osd_render.c
git commit -m "player: add pure ARGB bitmap-text renderer + host test"
```

---

## Task 5: [BOARD] DRM plane-capability probe + findings

Confirms a usable alpha overlay plane exists above the video, and records the zpos behaviour the OSD wiring depends on. Runs on the Orange Pi Zero 2W.

**Files:**
- Create: `player/tools/plane-probe.c`
- Create: `docs/findings/osd-plane-probe.md`

- [ ] **Step 1: Write the probe**

Create `player/tools/plane-probe.c`:
```c
/* Enumerate DRM planes on the active CRTC: type, formats, zpos range. Tells us
 * whether a non-primary plane supports an alpha format (ARGB8888/ABGR8888) and
 * can sit above the video — the prerequisite for the OSD overlay.
 *
 * Build: cc plane-probe.c -o /tmp/plane-probe $(pkg-config --cflags --libs libdrm)
 * Run:   /tmp/plane-probe                                                       */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static const char *type_name(int fd, uint32_t plane_id) {
    drmModeObjectProperties *p =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    const char *r = "?";
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr && !strcmp(pr->name, "type")) {
            switch (p->prop_values[i]) {
                case DRM_PLANE_TYPE_PRIMARY: r = "PRIMARY"; break;
                case DRM_PLANE_TYPE_OVERLAY: r = "OVERLAY"; break;
                case DRM_PLANE_TYPE_CURSOR:  r = "CURSOR";  break;
            }
        }
        if (pr) drmModeFreeProperty(pr);
    }
    if (p) drmModeFreeObjectProperties(p);
    return r;
}

static void print_zpos(int fd, uint32_t plane_id) {
    drmModeObjectProperties *p =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr && !strcmp(pr->name, "zpos")) {
            printf("    zpos=%llu", (unsigned long long)p->prop_values[i]);
            if (pr->count_values == 2)
                printf(" range=[%lld..%lld]%s",
                       (long long)pr->values[0], (long long)pr->values[1],
                       (pr->flags & DRM_MODE_PROP_IMMUTABLE) ? " IMMUTABLE" : " mutable");
            printf("\n");
        }
        if (pr) drmModeFreeProperty(pr);
    }
    if (p) drmModeFreeObjectProperties(p);
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card0"); return 1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        printf("plane %u  type=%s  possible_crtcs=0x%x\n",
               pl->plane_id, type_name(fd, pl->plane_id), pl->possible_crtcs);
        printf("    formats:");
        int has_argb = 0, has_abgr = 0, has_nv12 = 0;
        for (uint32_t f = 0; f < pl->count_formats; f++) {
            uint32_t fmt = pl->formats[f];
            printf(" %.4s", (char *)&fmt);
            if (fmt == DRM_FORMAT_ARGB8888) has_argb = 1;
            if (fmt == DRM_FORMAT_ABGR8888) has_abgr = 1;
            if (fmt == DRM_FORMAT_NV12)     has_nv12 = 1;
        }
        printf("\n    alpha=%s%s  nv12=%s\n",
               has_argb ? "ARGB8888 " : "", has_abgr ? "ABGR8888" : (has_argb ? "" : "none"),
               has_nv12 ? "yes" : "no");
        print_zpos(fd, pl->plane_id);
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    close(fd);
    return 0;
}
```

- [ ] **Step 2: [BOARD] Build and run the probe**

Run on the board:
```sh
cc player/tools/plane-probe.c -o /tmp/plane-probe $(pkg-config --cflags --libs libdrm)
/tmp/plane-probe
```
Expected: a list of planes. Confirm there is **at least one `OVERLAY` plane, distinct from the NV12 video overlay, that lists `alpha=ARGB8888`** and whose zpos can be ordered above the video. Note whether zpos is `mutable` or `IMMUTABLE`.

- [ ] **Step 3: Record findings**

Create `docs/findings/osd-plane-probe.md` with the probe output and these conclusions, filling the bracketed values from the run:
```markdown
# OSD plane-capability probe (H618 DE33)

Date: 2026-06-17. Tool: player/tools/plane-probe.c.

## Result
- Video (NV12) overlay plane id: [____]
- OSD candidate (ARGB8888 overlay) plane id: [____]
- Alpha format used: [ARGB8888 / ABGR8888]
- zpos: [mutable, range a..b / IMMUTABLE — natural order is primary<overlayN<...]

## Decision
- [ ] A usable alpha plane exists above the video -> proceed with the OSD overlay
      plane (Tasks 7-8).
- [ ] zpos is mutable -> assign primary(min) < video < osd(max).
- [ ] zpos is immutable -> rely on natural plane order; OSD plane id chosen so it
      stacks above the video plane id.
- [ ] No alpha plane at all -> OSD degrades off in wfbvid (osd == NULL); video
      unaffected. (Approach B / margins deferred.)

## Raw output
```
[paste /tmp/plane-probe output here]
```
```

- [ ] **Step 4: Commit**

```bash
git add player/tools/plane-probe.c docs/findings/osd-plane-probe.md
git commit -m "player: add DRM plane probe + OSD plane findings"
```

---

## Task 6: [BOARD] Refactor wfbvid into the run-forever daemon (no OSD yet)

Replaces deferred-modeset + exit-on-error with: startup modeset (preferred mode, black primary), built-in SDP on a `--port`, an interruptible run-forever loop that freezes the last frame and reconnects. This task adds **no** OSD; the player still works (black until video, then live video, surviving stream drops).

**Files:**
- Modify: `player/wfbvid.c`

- [ ] **Step 1: Add includes and replace globals/options**

In `player/wfbvid.c`, after the existing `#include <libavutil/pixdesc.h>` (line ~41) add:
```c
#include <time.h>
#include "sdp.h"
```

Replace the options/globals block (the lines from `static int opt_enc, opt_range, opt_nv21, opt_debug;` through the `on_flip` definition, lines ~57–63) with:
```c
static int opt_enc, opt_range, opt_nv21, opt_debug;
static int opt_port, opt_pt;
static uint64_t video_zpos = 1;              /* set from vplane zmax at runtime */

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

/* Monotonic clock + a deadline used by the libav interrupt callback so a
 * blocking read wakes ~4x/s during a stall (to honor Ctrl-C and, later, refresh
 * the OSD) without tearing down the input. */
static int64_t wake_deadline_ms;
static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int interrupt_cb(void *p)
{
    (void)p;
    if (!running) return 1;
    if (wake_deadline_ms && now_ms() >= wake_deadline_ms) return 1;
    return 0;
}
static void nsleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static volatile int flip_pending;
static void on_flip(int fd, unsigned seq, unsigned s, unsigned us, unsigned crtc, void *d) { flip_pending = 0; }
```

- [ ] **Step 2: Replace `pick_mode` with `pick_preferred_mode`**

Replace the entire `pick_mode` function (lines ~196–208) with:
```c
/* Pick the connector's preferred mode (fallback 1080p, then first). Chosen once
 * at startup; video is centred 1:1 within it (the DE33 VI scaler can't upscale). */
static drmModeConnector *g_conn;
static void pick_preferred_mode(void)
{
    int mi = -1;
    for (int i = 0; i < g_conn->count_modes; i++)
        if (g_conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mi = i; break; }
    if (mi < 0) for (int i = 0; i < g_conn->count_modes; i++)
        if (g_conn->modes[i].hdisplay == 1920 && g_conn->modes[i].vdisplay == 1080) { mi = i; break; }
    if (mi < 0) mi = 0;
    mode = g_conn->modes[mi];
    drmModeCreatePropertyBlob(drm_fd, &mode, sizeof(mode), &mode_blob);
    LOG("startup mode %s (%dx%d)", mode.name, mode.hdisplay, mode.vdisplay);
}
```
Note: this removes the old `static drmModeConnector *g_conn;` line that sat just above `pick_mode`; it is re-declared here, so delete the original declaration (line ~195) to avoid a duplicate.

- [ ] **Step 3: Replace `modeset` with `startup_modeset` + `commit_video_plane`**

Replace the entire `modeset` function (lines ~141–162) with:
```c
/* Startup modeset: CRTC on, black primary full-screen. Video + OSD planes are
 * attached later (video on the first frame). Brings a screen up immediately so
 * the player shows something even before any signal. */
static void startup_modeset(uint32_t pfb)
{
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, conn_id, CON_CRTC, crtc_id);
    drmModeAtomicAddProperty(r, crtc_id, C_MODE, mode_blob);
    drmModeAtomicAddProperty(r, crtc_id, C_ACTIVE, 1);
    add_plane(r, pplane_id, Pp_FB, Pp_CRTC, Pp_CX, Pp_CY, Pp_CW, Pp_CH, Pp_SX, Pp_SY, Pp_SW, Pp_SH,
              pfb, 0, 0, mode.hdisplay, mode.vdisplay, mode.hdisplay, mode.vdisplay);
    if (Pp_ZPOS) drmModeAtomicAddProperty(r, pplane_id, Pp_ZPOS, 0);
    /* (OSD plane is added here in Task 8.) */
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        DIE("startup modeset: %s", strerror(errno));
    drmModeAtomicFree(r);
}

/* Bring up the video overlay plane, centred 1:1 within the active mode. Called
 * once, on the first decoded frame. */
static void commit_video_plane(uint32_t vfb, int vw, int vh)
{
    int dx = (mode.hdisplay - vw) / 2, dy = (mode.vdisplay - vh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    add_plane(r, vplane_id, Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH,
              vfb, dx, dy, vw, vh, vw, vh);
    if (Vp_ZPOS)   drmModeAtomicAddProperty(r, vplane_id, Vp_ZPOS, video_zpos);
    if (Vp_CENC)   drmModeAtomicAddProperty(r, vplane_id, Vp_CENC, opt_enc);
    if (Vp_CRANGE) drmModeAtomicAddProperty(r, vplane_id, Vp_CRANGE, opt_range);
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        LOG("video plane commit: %s", strerror(errno));
    drmModeAtomicFree(r);
}
```

- [ ] **Step 4: Add the input open/close helpers**

Immediately before `int main(` (line ~224) insert:
```c
typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *cc;
    AVBufferRef     *hw;
    int vs;
} input_t;

/* Open the live RTP/SDP input + a low-delay Cedrus decoder. Returns 0, or -1
 * (caller retries). Does not require any packets to have arrived yet. */
static int open_input(input_t *in, const char *sdp_path, const char *bufsz)
{
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return -1;
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = NULL;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "protocol_whitelist", "file,crypto,data,udp,rtp", 0);
    av_dict_set(&opts, "buffer_size", bufsz, 0);
    av_dict_set(&opts, "reorder_queue_size", "256", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    if (avformat_open_input(&fmt, sdp_path, NULL, &opts) < 0) { av_dict_free(&opts); return -1; }
    av_dict_free(&opts);
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return -1; }

    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) { avformat_close_input(&fmt); return -1; }
    AVStream *st = fmt->streams[vs];

    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, st->codecpar);
    cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    AVBufferRef *hw = NULL;
    if (av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_V4L2REQUEST, NULL, NULL, 0) < 0) {
        avcodec_free_context(&cc); avformat_close_input(&fmt); return -1;
    }
    cc->hw_device_ctx = av_buffer_ref(hw);
    cc->get_format = get_drm_prime;
    cc->extra_hw_frames = 8;
    if (avcodec_open2(cc, dec, NULL) < 0) {
        av_buffer_unref(&hw); avcodec_free_context(&cc); avformat_close_input(&fmt); return -1;
    }
    in->fmt = fmt; in->cc = cc; in->hw = hw; in->vs = vs;
    LOG("input open: codec %s", avcodec_get_name(st->codecpar->codec_id));
    return 0;
}

/* Tear down input + decoder. Does NOT touch DRM state or any held frame, so the
 * last image stays frozen on screen across a reconnect. */
static void close_input(input_t *in)
{
    if (in->cc)  avcodec_free_context(&in->cc);
    if (in->hw)  av_buffer_unref(&in->hw);
    if (in->fmt) avformat_close_input(&in->fmt);
    in->vs = -1;
}
```

- [ ] **Step 5: Replace the body of `main` from input setup through cleanup**

Replace everything from the comment `// ---- open the live RTP/SDP input ...` (line ~301) down to `return 0;` (line ~384) with:
```c
    // ---- option parsing ----
    opt_port = 5600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) opt_port = atoi(argv[++i]);
    }
    opt_pt = getenv("WFBVID_PT") ? atoi(getenv("WFBVID_PT")) : 97;

    // ---- bring a screen up immediately (preferred mode, black primary) ----
    pick_preferred_mode();
    uint32_t pfb = make_black_primary();
    video_zpos = vplane_zmax;        /* video at top for now; lowered in Task 8 */
    startup_modeset(pfb);

    // ---- compose the built-in SDP onto a temp file ----
    char sdptext[512];
    int sdplen = compose_sdp(sdptext, sizeof sdptext, opt_port, opt_pt);
    if (sdplen < 0) DIE("compose_sdp overflow");
    char sdppath[] = "/tmp/citruspilot-XXXXXX";
    int sfd = mkstemp(sdppath);
    if (sfd < 0) DIE("mkstemp: %s", strerror(errno));
    if (write(sfd, sdptext, sdplen) != sdplen) DIE("write sdp");
    close(sfd);
    LOG("listening for H.265 RTP on udp:%d (PT %d)", opt_port, opt_pt);

    // ---- run forever: connect, decode, present; freeze + reconnect on drop ----
    input_t in = { .vs = -1 };
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    AVFrame  *held[2] = { NULL, NULL };
    uint32_t  heldfb[2] = { 0, 0 };
    int slot = 0, video_up = 0, warned_sw = 0;
    long shown = 0;

    wake_deadline_ms = now_ms() + 2000;
    while (running && open_input(&in, sdppath, bufsz) < 0) {
        LOG("waiting for stream on udp:%d ...", opt_port);
        nsleep_ms(500);
        wake_deadline_ms = now_ms() + 2000;
    }

    while (running) {
        /* (Task 8: if (osd) osd_tick(osd); here, self-throttled to 1 Hz.) */
        wake_deadline_ms = now_ms() + 250;
        int r = av_read_frame(in.fmt, pkt);
        if (r >= 0) {
            if (pkt->stream_index == in.vs && avcodec_send_packet(in.cc, pkt) == 0) {
                while (running && avcodec_receive_frame(in.cc, frame) == 0) {
                    if (frame->format != AV_PIX_FMT_DRM_PRIME) {
                        if (!warned_sw) { LOG("not hardware-decoded (got %s) — skipping",
                                              av_get_pix_fmt_name(frame->format)); warned_sw = 1; }
                        av_frame_unref(frame); continue;
                    }
                    uint32_t fb = fb_from_frame(frame);
                    if (!fb) { av_frame_unref(frame); continue; }
                    if (!video_up) {
                        commit_video_plane(fb, frame->width, frame->height);
                        video_up = 1; LOG("playing.");
                    } else {
                        drain_flip(); flip(fb);
                    }
                    shown++;
                    if (held[slot]) { drmModeRmFB(drm_fd, heldfb[slot]); av_frame_free(&held[slot]); }
                    held[slot] = frame; heldfb[slot] = fb; slot ^= 1;
                    frame = av_frame_alloc();
                }
            }
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        if (!running) break;
        if (r == AVERROR_EXIT) continue;          /* periodic wake — keep waiting */

        /* a genuine input error: freeze last frame, recycle input, reconnect */
        LOG("input error (%s) — reconnecting", av_err2str(r));
        close_input(&in);
        wake_deadline_ms = now_ms() + 2000;
        while (running && open_input(&in, sdppath, bufsz) < 0) {
            nsleep_ms(500);
            wake_deadline_ms = now_ms() + 2000;
        }
    }

    drain_flip();
    LOG("presented %ld frames", shown);
    for (int i = 0; i < 2; i++) if (held[i]) { drmModeRmFB(drm_fd, heldfb[i]); av_frame_free(&held[i]); }
    if (pfb) drmModeRmFB(drm_fd, pfb);
    av_frame_free(&frame); av_packet_free(&pkt);
    close_input(&in);
    unlink(sdppath);
    drmDropMaster(drm_fd);
    close(drm_fd);
    return 0;
}
```
Note: the old `main` read `path = argv[1]` and required `argc >= 2`. Remove the old `if (argc < 2) DIE(...)` and `const char *path = argv[1];` lines (~226–227); the new option parsing replaces them. Keep the existing `opt_enc/opt_range/opt_nv21/opt_debug/bufsz` env reads and signal setup.

- [ ] **Step 6: [BOARD] Build**

Run on the board:
```sh
cc player/wfbvid.c player/stats.c player/osd_render.c \
   -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
```
Expected: compiles clean (no warnings). (`stats.c`/`osd_render.c` are linked now so the build command is stable for later tasks, even though `wfbvid.c` doesn't call them yet.)

- [ ] **Step 7: [BOARD] Verify cold-start + play + freeze + reconnect**

Using the `smoke/` harness on the board (two shells), with `FILE` set to a local HEVC sample:
```sh
# shell A: start the player FIRST (cold start, no source yet)
wfbplay        # after Task 9; until then: wfbvid --port 5600 directly on a free VT
```
Expected A: screen goes black immediately (startup modeset), log `waiting for stream on udp:5600 ...`, no crash.
```sh
# shell B: start the synthetic RTP source on the same port
FILE=/root/sample.mkv PORT=5600 SDP=/root/stream.sdp smoke/rtp-loopback-src.sh
```
Expected A: `input open: codec hevc`, then `playing.`, live video centred on screen.
```sh
# shell B: Ctrl-C the source  -> then restart it
```
Expected A: on kill, the **last frame stays frozen** (no black, no crash), log shows reconnect attempts; on restart, video resumes within ~1–2 s (re-syncs on the next IDR). Ctrl-C in shell A exits cleanly and restores the console.

- [ ] **Step 8: Commit**

```bash
git add player/wfbvid.c
git commit -m "player: run-forever daemon — built-in SDP, --port, startup modeset, freeze+reconnect"
```

---

## Task 7: [BOARD] DRM-bound OSD module (`osd.c`)

The overlay-plane OSD: an ARGB8888 dumb buffer rendered in place at 1 Hz.

**Files:**
- Create: `player/osd.h`, `player/osd.c`

- [ ] **Step 1: Write `osd.h`**

Create `player/osd.h`:
```c
#ifndef CITRUSPILOT_OSD_H
#define CITRUSPILOT_OSD_H
#include <stdint.h>
#include <xf86drmMode.h>

/* Plane property ids the OSD needs to attach itself to an atomic commit. */
typedef struct {
    uint32_t fb_id, crtc_id, crtc_x, crtc_y, crtc_w, crtc_h,
             src_x, src_y, src_w, src_h, zpos;
} osd_plane_props;

typedef struct osd osd_t;

/* Create an OSD on `plane_id`: allocate an ARGB8888 dumb buffer sized to the HUD
 * box, place it `margin` px in from the top-left of the screen, assign `zpos_val`
 * (above the video), render the first frame. Returns NULL on any failure — the
 * caller then simply runs without an OSD. */
osd_t *osd_create(int drm_fd, uint32_t crtc_id, uint32_t plane_id,
                  const osd_plane_props *props,
                  int screen_w, int screen_h, int margin, int scale,
                  uint64_t zpos_val);

/* Re-sample stats and re-render the buffer in place if >= 1 s since last render.
 * Returns 1 if it re-rendered, else 0. No atomic commit needed (in-place buffer). */
int osd_tick(osd_t *o);

/* Add the OSD plane's props to an atomic request. With `initial` != 0 it adds
 * CRTC_ID/position/zpos (for the startup modeset); otherwise only FB_ID. */
void osd_add_to_commit(osd_t *o, drmModeAtomicReq *r, int initial);

void osd_destroy(osd_t *o);
#endif
```

- [ ] **Step 2: Write `osd.c`**

Create `player/osd.c`:
```c
#define _GNU_SOURCE
#include "osd.h"
#include "osd_render.h"
#include "stats.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#define OSD_COLS  18
#define OSD_LINES 3
#define OSD_PAD   4

struct osd {
    int drm_fd;
    uint32_t plane_id, crtc_id, fb_id, handle;
    osd_plane_props props;
    uint32_t *map;
    size_t map_size;
    int box_w, box_h, x, y, scale, stride_px;
    uint64_t zpos;
    stats_t st;
    int64_t last_ms;
    int rendered_once;
};

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void render(osd_t *o)
{
    stats_sample_t s;
    stats_sample(&o->st, &s);

    osd_fill(o->map, o->box_w, o->box_h, o->stride_px, OSD_ARGB(0x80, 0, 0, 0));

    char l[OSD_LINES][32];
    if (s.cpu_pct >= 0) snprintf(l[0], sizeof l[0], "CPU  %d%%", s.cpu_pct);
    else                snprintf(l[0], sizeof l[0], "CPU  --");
    if (s.mem_used_mb >= 0) snprintf(l[1], sizeof l[1], "MEM  %d/%d MB", s.mem_used_mb, s.mem_total_mb);
    else                    snprintf(l[1], sizeof l[1], "MEM  --");
    if (s.temp_c >= 0) snprintf(l[2], sizeof l[2], "TEMP %d C", s.temp_c);
    else               snprintf(l[2], sizeof l[2], "TEMP --");

    uint32_t fg = OSD_ARGB(0xff, 0xff, 0xff, 0xff);
    for (int i = 0; i < OSD_LINES; i++)
        osd_draw_text(o->map, o->box_w, o->box_h, o->stride_px,
                      OSD_PAD, OSD_PAD + i * 16 * o->scale, l[i], fg, o->scale);
}

osd_t *osd_create(int drm_fd, uint32_t crtc_id, uint32_t plane_id,
                  const osd_plane_props *props,
                  int screen_w, int screen_h, int margin, int scale,
                  uint64_t zpos_val)
{
    osd_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->drm_fd = drm_fd; o->crtc_id = crtc_id; o->plane_id = plane_id;
    o->props = *props; o->scale = scale; o->zpos = zpos_val;

    osd_box_size(OSD_COLS, OSD_LINES, scale, OSD_PAD, &o->box_w, &o->box_h);
    o->x = margin; o->y = margin;
    if (o->x + o->box_w > screen_w) o->x = screen_w - o->box_w;
    if (o->y + o->box_h > screen_h) o->y = screen_h - o->box_h;
    if (o->x < 0) o->x = 0;
    if (o->y < 0) o->y = 0;

    struct drm_mode_create_dumb cd = { .width = o->box_w, .height = o->box_h, .bpp = 32 };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { free(o); return NULL; }
    o->handle = cd.handle; o->stride_px = cd.pitch / 4; o->map_size = cd.size;

    uint32_t handles[4] = { cd.handle }, pitches[4] = { cd.pitch }, offsets[4] = { 0 };
    if (drmModeAddFB2(drm_fd, o->box_w, o->box_h, DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets, &o->fb_id, 0))
        goto fail_dumb;

    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
        goto fail_fb;
    o->map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, md.offset);
    if (o->map == MAP_FAILED)
        goto fail_fb;

    stats_init(&o->st);
    render(o);
    o->last_ms = mono_ms();
    o->rendered_once = 1;
    return o;

fail_fb:
    drmModeRmFB(drm_fd, o->fb_id);
fail_dumb:
    {
        struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    free(o);
    return NULL;
}

int osd_tick(osd_t *o)
{
    if (!o) return 0;
    int64_t now = mono_ms();
    if (o->rendered_once && now - o->last_ms < 1000) return 0;
    render(o);
    o->last_ms = now;
    return 1;
}

void osd_add_to_commit(osd_t *o, drmModeAtomicReq *r, int initial)
{
    if (!o) return;
    drmModeAtomicAddProperty(r, o->plane_id, o->props.fb_id, o->fb_id);
    if (initial) {
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_id, o->crtc_id);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_x, o->x);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_y, o->y);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_w, o->box_w);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_h, o->box_h);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_x, 0);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_y, 0);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_w, (uint64_t)o->box_w << 16);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_h, (uint64_t)o->box_h << 16);
        if (o->props.zpos) drmModeAtomicAddProperty(r, o->plane_id, o->props.zpos, o->zpos);
    }
}

void osd_destroy(osd_t *o)
{
    if (!o) return;
    if (o->map && o->map != MAP_FAILED) munmap(o->map, o->map_size);
    if (o->fb_id) drmModeRmFB(o->drm_fd, o->fb_id);
    struct drm_mode_destroy_dumb dd = { .handle = o->handle };
    if (o->handle) drmIoctl(o->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    free(o);
}
```

- [ ] **Step 3: [BOARD] Compile-check the module in isolation**

Run on the board (object-only, proves it builds against the real DRM headers):
```sh
cc -c player/osd.c -o /tmp/osd.o $(pkg-config --cflags libdrm)
echo "osd.c compiles: $?"
```
Expected: `osd.c compiles: 0`, no warnings.

- [ ] **Step 4: Commit**

```bash
git add player/osd.h player/osd.c
git commit -m "player: add DRM-bound OSD overlay module (in-place ARGB plane)"
```

---

## Task 8: [BOARD] Wire the OSD into wfbvid

Select the OSD plane during enumeration, place the video below it in zpos, create the OSD at startup, attach it to the startup modeset, and tick it once per loop iteration.

**Files:**
- Modify: `player/wfbvid.c`

- [ ] **Step 1: Add the OSD include, globals, and options**

After `#include "sdp.h"` (added in Task 6) add:
```c
#include "osd.h"
```
After the `video_zpos` global (added in Task 6) add:
```c
static uint32_t osd_plane_id;
static osd_plane_props Op;          /* OSD plane prop ids */
static uint64_t osd_zpos_max = 1;
static osd_t *osd;
static int opt_osd, opt_osd_scale;
```

- [ ] **Step 2: Select the OSD plane during enumeration**

In `main`, inside the plane-enumeration loop (the `for` over `pr->count_planes`, lines ~258–267), the body currently classifies primary vs NV12 overlay. Replace that loop body with:
```c
        drmModePlane *pl = drmModeGetPlane(drm_fd, pr->planes[i]);
        if (!(pl->possible_crtcs & (1 << crtc_idx))) { drmModeFreePlane(pl); continue; }
        int has_nv12 = 0, has_argb = 0;
        for (uint32_t f = 0; f < pl->count_formats; f++) {
            if (pl->formats[f] == DRM_FORMAT_NV12)     has_nv12 = 1;
            if (pl->formats[f] == DRM_FORMAT_ARGB8888) has_argb = 1;
        }
        uint64_t type = prop_val(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type");
        if (type == DRM_PLANE_TYPE_PRIMARY && !pplane_id) pplane_id = pl->plane_id;
        if (has_nv12 && type != DRM_PLANE_TYPE_PRIMARY && !vplane_id) vplane_id = pl->plane_id;
        else if (has_argb && type != DRM_PLANE_TYPE_PRIMARY && !osd_plane_id
                 && pl->plane_id != vplane_id) osd_plane_id = pl->plane_id;
        drmModeFreePlane(pl);
```
Note: this relies on the NV12 video plane being found before (or in the same pass as) the OSD plane; the `pl->plane_id != vplane_id` guard keeps them distinct. If `DRM_FORMAT_ARGB8888` isn't declared, ensure `#include <drm_fourcc.h>` is present (it already is).

- [ ] **Step 3: Query the OSD plane's props + zpos range**

After the `PP(Vp, vplane_id); PP(Pp, pplane_id);` line (~286) and the existing vplane zpos-range block (~289–299), add:
```c
    if (osd_plane_id) {
        Op.fb_id   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
        Op.crtc_id = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        Op.crtc_x  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        Op.crtc_y  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        Op.crtc_w  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        Op.crtc_h  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
        Op.src_x   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
        Op.src_y   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        Op.src_w   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
        Op.src_h   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
        Op.zpos    = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "zpos");
        drmModeObjectProperties *op = drmModeObjectGetProperties(drm_fd, osd_plane_id, DRM_MODE_OBJECT_PLANE);
        for (uint32_t i = 0; op && i < op->count_props; i++) {
            drmModePropertyRes *prp = drmModeGetProperty(drm_fd, op->props[i]);
            if (prp) {
                if (!strcmp(prp->name, "zpos") && prp->count_values == 2) osd_zpos_max = prp->values[1];
                drmModeFreeProperty(prp);
            }
        }
        if (op) drmModeFreeObjectProperties(op);
    }
```

- [ ] **Step 4: Set zpos ordering + create the OSD at startup**

In `main`, replace the Task-6 lines:
```c
    pick_preferred_mode();
    uint32_t pfb = make_black_primary();
    video_zpos = vplane_zmax;        /* video at top for now; lowered in Task 8 */
    startup_modeset(pfb);
```
with:
```c
    opt_osd       = getenv("WFBVID_OSD")       ? atoi(getenv("WFBVID_OSD"))       : 1;
    opt_osd_scale = getenv("WFBVID_OSD_SCALE") ? atoi(getenv("WFBVID_OSD_SCALE")) : 2;

    pick_preferred_mode();
    uint32_t pfb = make_black_primary();

    /* Order planes: primary(0) < video < osd(top). With a mutable zpos we put
     * the OSD at its max and the video just below; otherwise natural plane order
     * (per the probe) already stacks the OSD plane above the video. */
    if (osd_plane_id && opt_osd) {
        video_zpos = (osd_zpos_max > 0) ? osd_zpos_max - 1 : 0;
        if (video_zpos > vplane_zmax) video_zpos = vplane_zmax;
        osd = osd_create(drm_fd, crtc_id, osd_plane_id, &Op,
                         mode.hdisplay, mode.vdisplay, 16, opt_osd_scale, osd_zpos_max);
        if (!osd) LOG("OSD disabled (osd_create failed) — video unaffected");
    } else {
        video_zpos = vplane_zmax;
        if (opt_osd && !osd_plane_id) LOG("OSD disabled (no ARGB overlay plane) — video unaffected");
    }
    startup_modeset(pfb);
```

- [ ] **Step 5: Attach the OSD to the startup modeset**

In `startup_modeset` (Task 6), replace the line:
```c
    /* (OSD plane is added here in Task 8.) */
```
with:
```c
    osd_add_to_commit(osd, r, 1);
```

- [ ] **Step 6: Tick the OSD each loop iteration**

In the main `while (running)` loop, replace the line:
```c
        /* (Task 8: if (osd) osd_tick(osd); here, self-throttled to 1 Hz.) */
```
with:
```c
        osd_tick(osd);     /* self-throttled to 1 Hz; in-place buffer, no commit */
```

- [ ] **Step 7: Destroy the OSD on exit**

In the cleanup block at the end of `main`, after `close_input(&in);` add:
```c
    osd_destroy(osd);
```

- [ ] **Step 8: [BOARD] Build and verify the OSD**

Build (full multi-file command):
```sh
cc player/wfbvid.c player/osd.c player/osd_render.c player/stats.c \
   -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
```
Verify with the `smoke/` source running (as in Task 6):
- The HUD appears **top-left, above the video**, on a translucent black box: `CPU`, `MEM`, `TEMP` lines.
- Values update ~once per second and are plausible vs `top`, `free -m`, and `cat /sys/class/thermal/thermal_zone*/temp` (divide by 1000).
- Kill the RTP source → video freezes but the **OSD keeps updating** (liveness).
- `WFBVID_OSD=0 wfbvid --port 5600` → no OSD, video unchanged.
- `WFBVID_OSD_SCALE=3 wfbvid --port 5600` → larger glyphs.

- [ ] **Step 9: [BOARD] No-regression check on the video path**

With the source running, compare against the pre-OSD baseline:
```sh
# count frames over ~20 s and watch CPU; OSD on vs WFBVID_OSD=0 should match
WFBVID_OSD=0 timeout 20 wfbvid --port 5600 2>&1 | tail -1   # "presented N frames"
timeout 20 wfbvid --port 5600 2>&1 | tail -1                # OSD on
```
Expected: frame counts within noise of each other; `top` shows no meaningful CPU increase from the OSD (it renders ~1×/s into a small buffer).

- [ ] **Step 10: Commit**

```bash
git add player/wfbvid.c
git commit -m "player: wire OSD overlay into wfbvid (plane select, zpos, create, tick)"
```

---

## Task 9: Launcher + repo cleanup + docs

**Files:**
- Modify: `player/wfbplay`
- Remove: `player/wfb-h265.sdp`
- Modify: `README.md`

- [ ] **Step 1: Update `wfbplay` to a `--port` passthrough**

Replace the body of `player/wfbplay` (the `SDP=...`/`BIN=...` vars and the final invocation) so it no longer takes an SDP. New `player/wfbplay`:
```sh
#!/bin/sh
# wfbplay — run wfbvid on a free console for the live wfb-ng RTP link.
# Frees the console (stops getty + unbinds fbcon so they don't fight the modeset),
# raises the UDP socket buffer (bursty RTP), frees the decoder, plays, and
# restores the console on exit.
#
#   wfbplay [--port N]      (default port 5600)
#
# Pair with an external receiver, e.g.:
#   wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface> &
#   wfbplay --port 5600
set -u
BIN="${WFBVID:-/usr/local/bin/wfbvid}"

restore() {
    echo 1 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true
    systemctl start getty@tty1 2>/dev/null || true
}
trap restore EXIT INT TERM

systemctl stop getty@tty1 2>/dev/null || true
echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true   # stop fbcon repainting the primary plane
sysctl -wq net.core.rmem_max=26214400 net.core.rmem_default=26214400 2>/dev/null || true
fuser -k /dev/video0 2>/dev/null || true
sleep 1

"$BIN" "$@"     # not exec'd, so the restore trap runs on exit
```

- [ ] **Step 2: Remove the now-unused SDP file**

Run: `git rm player/wfb-h265.sdp`
Expected: file staged for deletion. (The player composes its SDP internally now.)

- [ ] **Step 3: Update the README build + usage**

In `README.md`, replace the build/run code block under "Build & run" so the build compiles all four sources and the run uses `--port`:
```sh
# build (on the board)
cc player/wfbvid.c player/osd.c player/osd_render.c player/stats.c \
   -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
cp player/wfbplay /usr/local/bin/

# upstream (external): wfb_rx de-FECs the drone link -> RTP udp:5600
wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface> &

# play: listens forever on udp:5600, HUD overlay on, console restored on exit
wfbplay --port 5600
```
Then change the env-knobs sentence to add the OSD knobs:
```
Env knobs: `WFBVID_OSD` (1=on, default), `WFBVID_OSD_SCALE` (glyph scale, default 2),
`WFBVID_PT` (RTP payload type, default 97), `WFBVID_NV21` (default 1, DE33 chroma
workaround), `WFBVID_BUFSIZE`, `WFBVID_ENC`/`WFBVID_RANGE`.
```
And in the Status table, change the OSD row to:
```
| OSD overlay (system: CPU/mem/temp) + run-forever daemon | ✅ v1 |
```

- [ ] **Step 4: Run host tests + commit**

Run: `player/tests/run-tests.sh`
Expected: `all host tests passed`.

```bash
git add player/wfbplay README.md
git commit -m "player: wfbplay --port passthrough; drop unused SDP; doc OSD + daemon"
```

---

## Task 10: [BOARD] End-to-end validation pass

Final acceptance against the spec's testing section. No code changes — observe and record.

- [ ] **Step 1: [BOARD] Full scenario run**

On the board, drive the player with the `smoke/` harness and walk the spec checklist:
1. **Build** — the four-file command compiles clean.
2. **Cold start, no source** — `wfbplay --port 5600` → black + live OSD immediately.
3. **Lock** — start `smoke/rtp-loopback-src.sh` → `playing.`, video centred, OSD above it.
4. **OSD correctness** — CPU/MEM/TEMP plausible vs `top`/`free -m`/thermal, ~1 Hz.
5. **Mid-stream drop** — kill source → last frame frozen, OSD still ticking → restart → re-sync.
6. **Port arg** — `wfbplay --port 5602` with the source on 5602 works; default (no arg) is 5600.
7. **OSD off** — `WFBVID_OSD=0` → no OSD, video unaffected.
8. **No regression** — frame count + CPU match the OSD-off baseline.

- [ ] **Step 2: [BOARD] Record the result**

Append a short "OSD + run-forever — validated" section to `docs/findings/stage0-player-pipe.md` (or a new `docs/findings/osd-run-forever.md`) noting: board, kernel, which alpha plane/zpos the OSD landed on, observed CPU delta, and any deviations.

- [ ] **Step 3: Commit**

```bash
git add docs/findings/
git commit -m "docs: OSD + run-forever end-to-end validation findings"
```

---

## Self-review

**1. Spec coverage** (each spec section → task):
- System-stats OSD (CPU/mem/temp) → Tasks 2 (sampling), 4 (render), 7 (plane), 8 (wire). ✓
- Dedicated ARGB plane above video, HW blend → Tasks 5 (probe), 7–8. ✓
- Baked 8×16 font, no deps → Task 3. ✓
- One-commit-per-flip / tear-free → **deviation, documented below**. ✓
- Plane probe + graceful fallback (no plane → OSD off) → Task 5; fallback coded in Task 8 Step 4. ✓
- `wfbvid [--port N]`, built-in SDP, default 5600 → Task 6. ✓
- Run forever / interruptible reads / reconnect → Task 6. ✓
- Freeze last frame across teardown → Task 6 Step 4–5 (`close_input` leaves `held[]`). ✓
- Modeset at startup → Task 6 Step 3. ✓
- `wfbplay` update, remove `wfb-h265.sdp`, README/build → Task 9. ✓
- `WFBVID_OSD` / `WFBVID_OSD_SCALE` / `WFBVID_PT` knobs → Tasks 6, 8. ✓
- Testing (probe, build, OSD visible, cold start, mid-stream drop, port, no-regression) → Tasks 5–10. ✓

**Deviation from spec (intentional, lower-risk):** The spec proposed folding the OSD FB swap into the video page-flip commit (with double buffering) to be tear-free. This plan instead uses a **single in-place OSD buffer** updated at 1 Hz — no per-frame OSD commit, no second atomic commit to coordinate with the non-blocking video flip (so no `EBUSY` risk). At 1 Hz on a small buffer any intra-OSD tearing is imperceptible. If tearing is ever visible, double-buffer + combined commit is a contained follow-up. This also removes the spec's "re-modeset on resolution change" need: the startup preferred mode is fixed and video is centred 1:1 within it (matching the original centring behaviour).

**2. Placeholder scan:** No "TBD/TODO/handle edge cases". The only bracketed fills are in the *findings template* (Task 5 Step 3), which is data to record on the board, not code. ✓

**3. Type/name consistency:** `compose_sdp`, `stats_t`/`stats_sample_t`/`stats_parse_*`/`stats_sample`/`stats_init`, `osd_t`/`osd_create`/`osd_tick`/`osd_add_to_commit`/`osd_destroy`/`osd_plane_props`, `osd_fill`/`osd_draw_char`/`osd_draw_text`/`osd_box_size`/`OSD_ARGB`, and wfbvid helpers (`open_input`/`close_input`/`input_t`/`startup_modeset`/`commit_video_plane`/`pick_preferred_mode`/`interrupt_cb`/`now_ms`/`nsleep_ms`) are used consistently across tasks. `osd_create` signature in Task 7 matches its call in Task 8 Step 4. `osd_plane_props` field names match `Op.*` usage and `osd_add_to_commit`. ✓
