#include "../rtp_h265.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* Build a 12-byte RTP header into buf. Returns header length. */
static size_t rtp_hdr(uint8_t *buf, int marker, uint16_t seq, uint32_t ts)
{
    buf[0] = 0x80;                                  /* V=2, no P/X/CC */
    buf[1] = (uint8_t)((marker ? 0x80 : 0) | 97);   /* M + PT=97 */
    buf[2] = (uint8_t)(seq >> 8); buf[3] = (uint8_t)seq;
    buf[4] = (uint8_t)(ts >> 24); buf[5] = (uint8_t)(ts >> 16);
    buf[6] = (uint8_t)(ts >> 8);  buf[7] = (uint8_t)ts;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;        /* SSRC */
    return 12;
}

static const uint8_t START[4] = { 0, 0, 0, 1 };

/* A single-NAL-unit packet with the marker bit yields one Annex-B AU = start
 * code + the NAL bytes. */
static void test_single_nal_marker(void)
{
    rtp_h265_t r; rtp_h265_init(&r);
    uint8_t pkt[64]; size_t n = rtp_hdr(pkt, 1, 100, 9000);
    /* NAL: header type=1 (TRAIL_R): byte0=(1<<1)=0x02, byte1=0x01, then payload */
    uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB, 0xCC };
    memcpy(pkt + n, nal, sizeof nal); n += sizeof nal;

    const uint8_t *au; size_t au_len;
    int rc = rtp_h265_input(&r, pkt, n, &au, &au_len);
    assert(rc == 1);
    assert(au_len == 4 + sizeof nal);
    assert(memcmp(au, START, 4) == 0);
    assert(memcmp(au + 4, nal, sizeof nal) == 0);
    rtp_h265_free(&r);
}

/* A NAL split into FU fragments reassembles into one Annex-B NAL: the original
 * 2-byte NAL header (FuType substituted) + the concatenated fragment payloads. */
static void test_fu_fragmentation(void)
{
    rtp_h265_t r; rtp_h265_init(&r);
    const uint8_t *au; size_t au_len; int rc;

    /* Original NAL: type=19 (IDR_W_RADL) header 0x26 0x01, payload D1..D4. */
    /* FU PayloadHdr type=49: byte0=(49<<1)=0x62, byte1=0x01. */
    uint8_t s[64]; size_t n = rtp_hdr(s, 0, 100, 9000);
    s[n++] = 0x62; s[n++] = 0x01;        /* PayloadHdr (FU) */
    s[n++] = 0x80 | 19;                  /* FU header: S=1, FuType=19 */
    s[n++] = 0xD1; s[n++] = 0xD2;        /* first half of NAL payload */
    rc = rtp_h265_input(&r, s, n, &au, &au_len);
    assert(rc == 0);                     /* not complete yet */

    uint8_t e[64]; n = rtp_hdr(e, 1, 101, 9000);   /* marker = last packet of AU */
    e[n++] = 0x62; e[n++] = 0x01;
    e[n++] = 0x40 | 19;                  /* FU header: E=1, FuType=19 */
    e[n++] = 0xD3; e[n++] = 0xD4;        /* second half */
    rc = rtp_h265_input(&r, e, n, &au, &au_len);
    assert(rc == 1);

    uint8_t want[] = { 0,0,0,1, 0x26, 0x01, 0xD1, 0xD2, 0xD3, 0xD4 };
    assert(au_len == sizeof want);
    assert(memcmp(au, want, sizeof want) == 0);
    rtp_h265_free(&r);
}

/* A sequence-number gap inside an AU drops the whole AU; the next clean AU
 * (correct seq) is emitted normally — i.e. the depacketizer resyncs. */
static void test_loss_drops_then_resyncs(void)
{
    rtp_h265_t r; rtp_h265_init(&r);
    const uint8_t *au; size_t au_len; int rc;

    uint8_t p1[32]; size_t n = rtp_hdr(p1, 0, 100, 9000);
    p1[n++] = 0x02; p1[n++] = 0x01; p1[n++] = 0xAA;
    rc = rtp_h265_input(&r, p1, n, &au, &au_len); assert(rc == 0);

    uint8_t p2[32]; n = rtp_hdr(p2, 1, 102, 9000);   /* seq 101 lost; marker */
    p2[n++] = 0x02; p2[n++] = 0x01; p2[n++] = 0xBB;
    rc = rtp_h265_input(&r, p2, n, &au, &au_len);
    assert(rc == 0);                                 /* incomplete AU dropped */

    uint8_t p3[32]; n = rtp_hdr(p3, 1, 103, 9100);   /* fresh, in-sequence AU */
    p3[n++] = 0x02; p3[n++] = 0x01; p3[n++] = 0xCC;
    rc = rtp_h265_input(&r, p3, n, &au, &au_len);
    assert(rc == 1);                                 /* resynced */
    assert(au_len == 7 && au[6] == 0xCC);
    rtp_h265_free(&r);
}

/* An aggregation packet (AP) carries several NALs, each length-prefixed; all are
 * emitted, each with its own start code. */
static void test_ap_aggregation(void)
{
    rtp_h265_t r; rtp_h265_init(&r);
    const uint8_t *au; size_t au_len; int rc;

    uint8_t p[64]; size_t n = rtp_hdr(p, 1, 100, 9000);
    p[n++] = 0x60; p[n++] = 0x01;            /* PayloadHdr type=48 (AP) */
    p[n++] = 0x00; p[n++] = 0x03; p[n++] = 0x02; p[n++] = 0x01; p[n++] = 0xAA; /* NAL1 sz=3 */
    p[n++] = 0x00; p[n++] = 0x03; p[n++] = 0x02; p[n++] = 0x01; p[n++] = 0xBB; /* NAL2 sz=3 */
    rc = rtp_h265_input(&r, p, n, &au, &au_len);
    assert(rc == 1);
    uint8_t want[] = { 0,0,0,1, 0x02,0x01,0xAA, 0,0,0,1, 0x02,0x01,0xBB };
    assert(au_len == sizeof want);
    assert(memcmp(au, want, sizeof want) == 0);
    rtp_h265_free(&r);
}

int main(void)
{
    test_single_nal_marker();
    test_fu_fragmentation();
    test_loss_drops_then_resyncs();
    test_ap_aggregation();
    return 0;
}
