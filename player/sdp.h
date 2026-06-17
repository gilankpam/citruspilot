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
