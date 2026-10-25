/* Host-side checks of common/amv_octant.h and the protocol header's size asserts.
 * Build and run:  cc -std=c11 -Wall -Wextra -Icommon firmware/esp32p4-dongle/tests/test_octant.c -lm -o /tmp/test_octant && /tmp/test_octant */
#include <stdio.h>
#include <stdlib.h>
#include "amv_octant.h"
#include "amv_dongle_proto.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    float one[8] = {1,1,1,1,1,1,1,1};
    amv_octant_state_t st = {0};
    int32_t ox, oy; uint8_t oc;

    /* weight 1.0 everywhere is the identity, with no remainder left behind */
    for (int dx = -50; dx <= 50; dx += 7)
        for (int dy = -50; dy <= 50; dy += 5) {
            amv_octant_scale(one, &st, dx, dy, &ox, &oy, &oc);
            CHECK(ox == dx && oy == dy, "identity %d,%d -> %d,%d", dx, dy, ox, oy);
        }

    /* the eight compass directions land in their own octant */
    int dirs[8][2] = {{10,0},{10,-10},{0,-10},{-10,-10},{-10,0},{-10,10},{0,10},{10,10}};   /* screen Y is inverted */
    for (int k = 0; k < 8; k++) {
        amv_octant_scale(one, &st, dirs[k][0], dirs[k][1], &ox, &oy, &oc);
        CHECK(oc == k, "direction %d tagged octant %d", k, oc);
    }

    /* sub-pixel remainder: 1000 counts at 0.25 must come out as exactly 250, never lost to truncation */
    float quarter[8] = {.25f,.25f,.25f,.25f,.25f,.25f,.25f,.25f};
    st = (amv_octant_state_t){0};
    long sx = 0, sy = 0;
    for (int i = 0; i < 1000; i++) { amv_octant_scale(quarter, &st, 1, 1, &ox, &oy, &oc); sx += ox; sy += oy; }
    CHECK(sx == 250 && sy == 250, "remainder diffusion: got %ld,%ld want 250,250", sx, sy);

    /* anisotropy: boosting N (octant 2) only changes vertical-up motion */
    float vb[8] = {1,1,1.5f,1,1,1,1,1};
    st = (amv_octant_state_t){0};
    amv_octant_scale(vb, &st, 0, -100, &ox, &oy, &oc);
    CHECK(oy == -150 && ox == 0, "north boost: %d,%d", ox, oy);
    st = (amv_octant_state_t){0};
    amv_octant_scale(vb, &st, 100, 0, &ox, &oy, &oc);
    CHECK(ox == 100, "east unaffected: %d", ox);

    /* the boundary between octants is continuous (no jump) */
    float w[8] = {1,2,1,2,1,2,1,2};
    int32_t prev = 0;
    for (int deg = 0; deg <= 90; deg++) {
        float th = deg * AMV_PI / 180.0f;
        st = (amv_octant_state_t){0};
        amv_octant_scale(w, &st, (int32_t)(10000 * cosf(th)), (int32_t)(-10000 * sinf(th)), &ox, &oy, &oc);
        int32_t mag = (int32_t)sqrtf((float)ox * ox + (float)oy * oy);
        if (deg) CHECK(abs(mag - prev) < 1500, "jump at %d deg: %d -> %d", deg, prev, mag);
        prev = mag;
    }

    printf(fails ? "%d FAILED\n" : "octant tests passed\n", fails);
    return fails != 0;
}
