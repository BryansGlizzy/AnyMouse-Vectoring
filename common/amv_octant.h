/*
 * AnyMouse octant vectoring math, shared by the ESP32-P4 dongle firmware and host tests.
 *
 * Same algorithm as scale_vector() in src/evdev_rt.c (README section 4):
 * atan2 -> octant position -> cosine C1 blend of adjacent weights -> sub-pixel
 * remainder diffusion. Weights are passed in, so the caller decides how they are
 * stored (atomics on the Linux engine, a seqlock snapshot on the dongle).
 *
 * NOTE: src/evdev_rt.c still carries its own copy. Switching it to this header is
 * planned (PLAN.md, M2) but is not done yet because it cannot be built or tested
 * off-Linux.
 */
#ifndef AMV_OCTANT_H
#define AMV_OCTANT_H

#include <math.h>
#include <stdint.h>

#define AMV_PI            3.14159265358979323846f
#define AMV_TWO_PI        (2.0f * AMV_PI)
#define AMV_INV_OCT_STEP  (4.0f / AMV_PI)

typedef struct {
    float rem_x;
    float rem_y;
} amv_octant_state_t;

/* Octant index: 0=E 1=NE 2=N 3=NW 4=W 5=SW 6=S 7=SE (screen Y is inverted). */
static inline void amv_octant_scale(const float w[8], amv_octant_state_t *st,
                                    int32_t raw_dx, int32_t raw_dy,
                                    int32_t *out_dx, int32_t *out_dy,
                                    uint8_t *out_octant)
{
    if (raw_dx == 0 && raw_dy == 0) {
        *out_dx = 0;
        *out_dy = 0;
        *out_octant = 0;
        return;
    }

    float f_dx = (float)raw_dx;
    float f_dy = (float)raw_dy;
    float theta = atan2f(-f_dy, f_dx);
    if (theta < 0.0f) {
        theta += AMV_TWO_PI;
    }

    float octant_pos = theta * AMV_INV_OCT_STEP;
    int k0 = ((int)octant_pos) & 7;
    int k1 = (k0 + 1) & 7;
    float t = octant_pos - (float)((int)octant_pos);
    *out_octant = (uint8_t)(((int)(octant_pos + 0.5f)) & 7);

    float smooth_t = (1.0f - cosf(t * AMV_PI)) * 0.5f;
    float scale = w[k0] * (1.0f - smooth_t) + w[k1] * smooth_t;

    float exact_x = f_dx * scale + st->rem_x;
    float exact_y = f_dy * scale + st->rem_y;
    int32_t ix = (int32_t)exact_x;
    int32_t iy = (int32_t)exact_y;
    st->rem_x = exact_x - (float)ix;
    st->rem_y = exact_y - (float)iy;
    *out_dx = ix;
    *out_dy = iy;
}

#endif /* AMV_OCTANT_H */
