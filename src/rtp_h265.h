#ifndef CITRUSPILOT_RTP_H265_H
#define CITRUSPILOT_RTP_H265_H
#include <stdint.h>
#include <stddef.h>

/* Stateful H.265 (RFC 7798) RTP depacketizer. Feed it whole RTP packets (the UDP
 * payload); it reassembles NAL units — single, AP-aggregated, or FU-fragmented —
 * into Annex-B access units (each NAL prefixed with a 00 00 00 01 start code).
 *
 * An access unit completes on the RTP marker bit (last packet of a frame). On a
 * sequence-number gap anywhere inside an access unit, the whole AU is dropped —
 * so the stateless decoder is never handed a frame with a hole in it. */

typedef struct {
    uint8_t *au;          /* access-unit buffer being assembled (Annex-B) */
    size_t   au_len;
    size_t   au_cap;
    uint16_t exp_seq;     /* next expected RTP sequence number */
    int      have_seq;
    int      corrupt;     /* current AU lost a packet -> drop it on completion */
    int      emitted;     /* previous call completed an AU -> reset before reuse */
    int      in_fu;       /* mid-reassembly of a fragmented (FU) NAL unit */
} rtp_h265_t;

void rtp_h265_init(rtp_h265_t *r);
void rtp_h265_free(rtp_h265_t *r);

/* Feed one RTP packet. Returns 1 and points au and au_len at a completed Annex-B
 * access unit (valid until the next call) when the marker bit closes a clean AU.
 * Returns 0 when more packets are needed or the completed AU was dropped due to
 * loss. Returns -1 for a malformed/ignored packet. */
int rtp_h265_input(rtp_h265_t *r, const uint8_t *pkt, size_t len,
                   const uint8_t **au, size_t *au_len);

#endif
