// One-off precision measurement for skcms_XYZD50_to_linear().  Not built by
// default; compile with:
//   gcc -O2 -I. precision_xyz_to_linear.c skcms.cc src/skcms_TransformBaseline.c* ...
#include "src/skcms_public.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Double-precision reference: invert toXYZD50 in long double, apply in double.
static void reference(const skcms_ICCProfile* p, const float xyz[3], double rgb[3]) {
    long double m[3][3], inv[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = p->toXYZD50.vals[r][c];
    long double det = m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])
                    - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
                    + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            inv[r][c] = (m[(c+1)%3][(r+1)%3]*m[(c+2)%3][(r+2)%3]
                       - m[(c+1)%3][(r+2)%3]*m[(c+2)%3][(r+1)%3]) / det;
    for (int r = 0; r < 3; ++r) {
        rgb[r] = 0;
        for (int c = 0; c < 3; ++c)
            rgb[r] += (double)inv[r][c] * xyz[c];
    }
}

int main(void) {
    const skcms_ICCProfile* srgb = skcms_sRGB_profile();
    float rgb[3];

    // 1) Absolute error vs double-precision reference, in-gamut and extended.
    struct { const char* name; float lo, hi; } ranges[] = {
        { "in-gamut [0,1]^3     ", 0.0f, 1.0f },
        { "extended [-0.5,1.5]^3", -0.5f, 1.5f },
        { "HDR-ish  [0,4]^3     ", 0.0f, 4.0f },
    };
    for (int t = 0; t < 3; ++t) {
        double max_abs = 0, max_rel = 0;
        const int N = 40;
        for (int i = 0; i <= N; ++i)
            for (int j = 0; j <= N; ++j)
                for (int k = 0; k <= N; ++k) {
                    float xyz[3] = { ranges[t].lo + (ranges[t].hi - ranges[t].lo) * i / N,
                                     ranges[t].lo + (ranges[t].hi - ranges[t].lo) * j / N,
                                     ranges[t].lo + (ranges[t].hi - ranges[t].lo) * k / N };
                    double ref[3];
                    reference(srgb, xyz, ref);
                    skcms_XYZD50_to_linear(srgb, xyz, rgb);
                    for (int r = 0; r < 3; ++r) {
                        double err = fabs(rgb[r] - ref[r]);
                        if (err > max_abs) max_abs = err;
                        double rel = err / (fabs(ref[r]) > 1e-6 ? fabs(ref[r]) : 1e-6);
                        if (fabs(ref[r]) > 1e-3 && rel > max_rel) max_rel = rel;
                    }
                }
        printf("%s  max abs err %.3g  max rel err %.3g\n", ranges[t].name, max_abs, max_rel);
    }

    // 2) Round trip XYZ -> linear -> XYZ over extended range.
    {
        double max_abs = 0;
        const int N = 40;
        for (int i = 0; i <= N; ++i)
            for (int j = 0; j <= N; ++j)
                for (int k = 0; k <= N; ++k) {
                    float xyz[3] = { -0.5f + 2.0f * i / N,
                                     -0.5f + 2.0f * j / N,
                                     -0.5f + 2.0f * k / N };
                    skcms_XYZD50_to_linear(srgb, xyz, rgb);
                    for (int r = 0; r < 3; ++r) {
                        float back = srgb->toXYZD50.vals[r][0] * rgb[0]
                                   + srgb->toXYZD50.vals[r][1] * rgb[1]
                                   + srgb->toXYZD50.vals[r][2] * rgb[2];
                        double err = fabs(back - xyz[r]);
                        if (err > max_abs) max_abs = err;
                    }
                }
        printf("round trip [-0.5,1.5]^3  max abs err %.3g\n", max_abs);
    }

    // 3) Consistency with the full skcms_Transform path (XYZD50 -> linear sRGB).
    {
        skcms_ICCProfile linear_srgb = *skcms_sRGB_profile();
        skcms_SetTransferFunction(&linear_srgb, skcms_Identity_TransferFunction());
        double max_abs = 0;
        const int N = 16;
        for (int i = 0; i <= N; ++i)
            for (int j = 0; j <= N; ++j)
                for (int k = 0; k <= N; ++k) {
                    float src[4] = { -0.5f + 2.0f * i / N,
                                     -0.5f + 2.0f * j / N,
                                     -0.5f + 2.0f * k / N, 1.0f };
                    float dst[4];
                    skcms_Transform(src, skcms_PixelFormat_RGBA_ffff,
                                    skcms_AlphaFormat_Opaque, skcms_XYZD50_profile(),
                                    dst, skcms_PixelFormat_RGBA_ffff,
                                    skcms_AlphaFormat_Opaque, &linear_srgb, 1);
                    skcms_XYZD50_to_linear(srgb, src, rgb);
                    for (int r = 0; r < 3; ++r) {
                        double err = fabs(dst[r] - rgb[r]);
                        if (err > max_abs) max_abs = err;
                    }
                }
        printf("vs skcms_Transform      max abs err %.3g\n", max_abs);
    }

    // 4) White point and primaries.
    {
        float white[3];
        for (int r = 0; r < 3; ++r)
            white[r] = srgb->toXYZD50.vals[r][0] + srgb->toXYZD50.vals[r][1]
                     + srgb->toXYZD50.vals[r][2];
        skcms_XYZD50_to_linear(srgb, white, rgb);
        printf("D50 white (%.5f,%.5f,%.5f) -> (%.7f,%.7f,%.7f)\n",
               white[0], white[1], white[2], rgb[0], rgb[1], rgb[2]);
    }
    return 0;
}
