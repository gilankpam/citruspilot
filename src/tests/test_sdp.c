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
