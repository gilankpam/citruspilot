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
build_and_run test_fit_rect    test_fit_rect.c
build_and_run test_stats       test_stats.c ../stats.c
build_and_run test_osd_render  test_osd_render.c ../osd_render.c
build_and_run test_rtp_h265    test_rtp_h265.c ../rtp_h265.c
echo "all host tests passed"
