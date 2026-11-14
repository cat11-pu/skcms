// Reproduction for color shift with mixed parametric-curve / LUT TRC profiles,
// most visible in the dark region.
//
// Build:
//   gcc -O2 -I. repro_mixed_trc.c skcms.cc src/skcms_TransformBaseline.c \
//       src/skcms_TransformHsw.c src/skcms_TransformSkx.c -lm -lstdc++
#include "src/skcms_public.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static double ref_eval_tf(const skcms_TransferFunction* tf, double x) {
    double sign = x < 0 ? -1.0 : 1.0;
    x = fabs(x);
    if (x < tf->d) {
        return sign * (tf->c * x + tf->f);
    }
    return sign * (pow(tf->a * x + tf->b, tf->g) + tf->e);
}

// Double precision reference for one curve (parametric or table), mirroring
// the ICC/skcms interpolation contract exactly.
static double ref_eval_curve(const skcms_Curve* curve, double x) {
    if (curve->table_entries == 0) {
        return ref_eval_tf(&curve->parametric, (float)x);
    }
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    double ix = x * (double)(curve->table_entries - 1);
    long lo = (long)ix;
    long hi = lo + 1;
    if (hi > (long)curve->table_entries - 1) hi = curve->table_entries - 1;
    double t = ix - (double)lo;
    double l, h;
    if (curve->table_8) {
        l = curve->table_8[lo] * (1.0 / 255.0);
        h = curve->table_8[hi] * (1.0 / 255.0);
    } else {
        uint16_t be_l, be_h;
        memcpy(&be_l, curve->table_16 + 2 * lo, 2);
        memcpy(&be_h, curve->table_16 + 2 * hi, 2);
        uint16_t le_l = ((be_l << 8) | (be_l >> 8)) & 0xffff;
        uint16_t le_h = ((be_h << 8) | (be_h >> 8)) & 0xffff;
        l = le_l * (1.0 / 65535.0);
        h = le_h * (1.0 / 65535.0);
    }
    return l + (h - l) * t;
}

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

int main(void) {
    const skcms_ICCProfile* srgb = skcms_sRGB_profile();
    const skcms_TransferFunction* srgb_tf = skcms_sRGB_TransferFunction();

    enum { N16 = 1024, N8 = 256 };
    static uint16_t tab16[N16];
    static uint8_t  tab8[N8];
    for (int i = 0; i < N16; i++) {
        float x = (float)i / (N16 - 1);
        tab16[i] = be16((uint16_t)lroundf(skcms_TransferFunction_eval(srgb_tf, x) * 65535.0f));
    }
    for (int i = 0; i < N8; i++) {
        float x = (float)i / (N8 - 1);
        tab8[i] = (uint8_t)lroundf(skcms_TransferFunction_eval(srgb_tf, x) * 255.0f);
    }

    // Mixed source profile:
    //   R = parametric sRGB, G = 16-bit LUT (sRGB), B = 8-bit LUT (sRGB).
    skcms_ICCProfile mixed = *srgb;
    mixed.has_trc = true;
    mixed.trc[0].table_entries = 0;
    mixed.trc[0].parametric = *srgb_tf;
    mixed.trc[1].table_entries = N16;
    mixed.trc[1].table_8 = NULL;
    mixed.trc[1].table_16 = (const uint8_t*)tab16;
    mixed.trc[2].table_entries = N8;
    mixed.trc[2].table_8 = tab8;
    mixed.trc[2].table_16 = NULL;

    double worst_abs = 0, worst_dark_abs = 0;
    double worst_q8 = 0, worst_dark_q8 = 0;   // error measured in 8-bit quantization levels
    long dark_pixels = 0, total = 0;
    float worst_rgb[3] = {0, 0, 0};

    // Dense grid, with emphasis on the dark region [0, 0.1].
    for (int i = 0; i <= 200; i++) {
        for (int j = 0; j <= 200; j++) {
            for (int k = 0; k <= 200; k++) {
                float src[3] = {
                    0.10f * i / 200.0f,
                    0.10f * j / 200.0f,
                    0.10f * k / 200.0f,
                };
                float got[3];
                if (!skcms_Transform(src, skcms_PixelFormat_RGB_fff,
                                     skcms_AlphaFormat_Opaque, &mixed,
                                     got, skcms_PixelFormat_RGB_fff,
                                     skcms_AlphaFormat_Opaque,
                                     skcms_XYZD50_profile(), 1)) {
                    printf("Transform failed\n");
                    return 1;
                }

                double lin[3] = {
                    ref_eval_curve(&mixed.trc[0], src[0]),
                    ref_eval_curve(&mixed.trc[1], src[1]),
                    ref_eval_curve(&mixed.trc[2], src[2]),
                };
                double ref[3];
                for (int r = 0; r < 3; r++) {
                    ref[r] = 0;
                    for (int c = 0; c < 3; c++) {
                        ref[r] += (double)mixed.toXYZD50.vals[r][c] * lin[c];
                    }
                }

                bool dark = src[0] < 0.02f && src[1] < 0.02f && src[2] < 0.02f;
                for (int r = 0; r < 3; r++) {
                    double e = fabs(got[r] - ref[r]);
                    double q = e * 255.0;
                    total++;
                    if (e > worst_abs) { worst_abs = e; worst_q8 = q; }
                    if (dark) {
                        dark_pixels++;
                        if (e > worst_dark_abs) {
                            worst_dark_abs = e;
                            worst_dark_q8 = q;
                            worst_rgb[0] = src[0];
                            worst_rgb[1] = src[1];
                            worst_rgb[2] = src[2];
                        }
                    }
                }
            }
        }
    }

    printf("pixels checked         : %ld (dark: %ld)\n", total, dark_pixels);
    printf("worst abs error        : %.3g  (%.4f 8-bit levels)\n", worst_abs, worst_q8);
    printf("worst DARK abs error   : %.3g  (%.4f 8-bit levels)\n", worst_dark_abs, worst_dark_q8);
    printf("dark worst at rgb      : (%.5f, %.5f, %.5f)\n",
           worst_rgb[0], worst_rgb[1], worst_rgb[2]);
    printf("one 8-bit level        : %.6g in linear/XYZ units\n", 1.0 / 255.0);
    return 0;
}
