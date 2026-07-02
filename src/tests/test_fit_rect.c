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
