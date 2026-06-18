#include "rtp_h265.h"
#include <stdlib.h>
#include <string.h>

void rtp_h265_init(rtp_h265_t *r) { memset(r, 0, sizeof *r); }
void rtp_h265_free(rtp_h265_t *r) { free(r->au); memset(r, 0, sizeof *r); }

static int ensure(rtp_h265_t *r, size_t extra)
{
    if (r->au_len + extra <= r->au_cap) return 0;
    size_t cap = r->au_cap ? r->au_cap : 8192;
    while (cap < r->au_len + extra) cap *= 2;
    uint8_t *p = realloc(r->au, cap);
    if (!p) return -1;
    r->au = p; r->au_cap = cap;
    return 0;
}

static int append(rtp_h265_t *r, const uint8_t *d, size_t n)
{
    if (ensure(r, n)) return -1;
    memcpy(r->au + r->au_len, d, n);
    r->au_len += n;
    return 0;
}

/* Append one NAL unit as Annex-B (4-byte start code + NAL). */
static int append_nal(rtp_h265_t *r, const uint8_t *nal, size_t n)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    if (append(r, sc, 4)) return -1;
    return append(r, nal, n);
}

int rtp_h265_input(rtp_h265_t *r, const uint8_t *pkt, size_t len,
                   const uint8_t **au, size_t *au_len)
{
    if (len < 12 + 2) return -1;                 /* RTP header + a NAL header */
    int marker      = pkt[1] & 0x80;
    uint16_t seq    = (uint16_t)((pkt[2] << 8) | pkt[3]);
    const uint8_t *pl = pkt + 12;
    size_t pl_len   = len - 12;

    /* Start a fresh AU if the previous packet completed one. */
    if (r->emitted) { r->au_len = 0; r->corrupt = 0; r->emitted = 0; r->in_fu = 0; }

    /* Sequence-gap detection: any hole taints the AU under assembly. */
    if (r->have_seq && seq != r->exp_seq) r->corrupt = 1;
    r->exp_seq = (uint16_t)(seq + 1);
    r->have_seq = 1;

    int type = (pl[0] >> 1) & 0x3f;
    if (type <= 47) {                            /* single NAL unit */
        if (append_nal(r, pl, pl_len)) return -1;
    } else if (type == 49) {                     /* fragmentation unit (FU) */
        if (pl_len < 3) return -1;
        int start = pl[2] & 0x80, end = pl[2] & 0x40, futype = pl[2] & 0x3f;
        if (start) {
            /* Rebuild the original 2-byte NAL header: keep F bit + LayerId high
             * bit from the payload header, substitute the fragmented NAL type. */
            uint8_t nh[2] = { (uint8_t)((pl[0] & 0x81) | (futype << 1)), pl[1] };
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            if (append(r, sc, 4) || append(r, nh, 2) || append(r, pl + 3, pl_len - 3))
                return -1;
            r->in_fu = 1;
        } else if (!r->in_fu) {
            r->corrupt = 1;                      /* lost the FU start fragment */
        } else if (append(r, pl + 3, pl_len - 3)) {
            return -1;
        }
        if (end) r->in_fu = 0;
    } else if (type == 48) {                     /* aggregation packet (AP) */
        size_t off = 2;                          /* skip the 2-byte payload header */
        while (off + 2 <= pl_len) {
            size_t nsz = (size_t)((pl[off] << 8) | pl[off + 1]);
            off += 2;
            if (nsz == 0 || off + nsz > pl_len) { r->corrupt = 1; break; }
            if (append_nal(r, pl + off, nsz)) return -1;
            off += nsz;
        }
    } else {
        return -1;                               /* PACI: not used by our encoder */
    }

    if (marker) {
        r->emitted = 1;
        if (r->corrupt) return 0;                /* incomplete frame: drop it */
        *au = r->au;
        *au_len = r->au_len;
        return 1;
    }
    return 0;
}
