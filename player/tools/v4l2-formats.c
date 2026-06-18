/* Enumerate the Cedrus stateless decoder's V4L2 formats on /dev/video0:
 * OUTPUT queue = coded (HEVC slice) formats it accepts, CAPTURE queue = decoded
 * pixel formats it emits. Tells us whether the driver can output LINEAR NV12
 * (scannable by the DE33 plane) or only a tiled/AFBC format.
 *
 * Build (cross): aarch64-...-gcc --sysroot=$S -I$S/usr/include v4l2-formats.c -o v4l2-formats
 * Run:   /tmp/v4l2-formats [/dev/videoN]                                          */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void enum_q(int fd, enum v4l2_buf_type type, const char *label) {
    printf("== %s ==\n", label);
    for (uint32_t i = 0; ; i++) {
        struct v4l2_fmtdesc fd0;
        memset(&fd0, 0, sizeof fd0);
        fd0.index = i;
        fd0.type = type;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fd0)) break;
        uint32_t pf = fd0.pixelformat;
        printf("   [%u] %.4s  (%s)%s\n", i, (char *)&pf, fd0.description,
               (fd0.flags & V4L2_FMT_FLAG_COMPRESSED) ? " COMPRESSED" : "");
    }
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(dev); return 1; }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (!ioctl(fd, VIDIOC_QUERYCAP, &cap))
        printf("driver=%s card=%s caps=%#x\n", cap.driver, cap.card, cap.capabilities);

    /* try multiplanar first (stateless m2m is _MPLANE), then singleplanar */
    enum_q(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  "OUTPUT_MPLANE (coded in)");
    enum_q(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE (decoded out)");
    enum_q(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,         "OUTPUT (coded in)");
    enum_q(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,        "CAPTURE (decoded out)");
    close(fd);
    return 0;
}
