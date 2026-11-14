// Reproduction for the dark-region color shift in a profile whose A2B/B2A
// pipeline mixes parametric curves and 1D LUTs (b2a_no_clut.icc style).
//
// Compares the local build (LOCAL_ prefix) against the reference build
// (REF_ prefix) over a dark-dense grid, in 8-bit quantization-level units.
#include "src/skcms_public.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
bool LOCALSK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool REFSK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool LOCALSK_skcms_Transform(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                             const skcms_ICCProfile*, void*, skcms_PixelFormat,
                             skcms_AlphaFormat, const skcms_ICCProfile*, size_t);
bool REFSK_skcms_Transform(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                           const skcms_ICCProfile*, void*, skcms_PixelFormat,
                           skcms_AlphaFormat, const skcms_ICCProfile*, size_t);
}

static uint8_t* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "profiles/fuzz/b2a_no_clut.icc";
    size_t len;
    uint8_t* buf = read_file(path, &len);
    if (!buf) { printf("cannot open %s\n", path); return 2; }
    static const int prio[] = {0, 1};
    skcms_ICCProfile lp, rp;
    if (!LOCALSK_skcms_ParseWithA2BPriority(buf, len, prio, 2, &lp) ||
        !REFSK_skcms_ParseWithA2BPriority(buf, len, prio, 2, &rp)) {
        printf("parse failed\n");
        return 2;
    }

    const int N = 40;
    long total = 0, dark = 0;
    double worst_abs = 0, worst_dark_abs = 0;
    double worst_lvl = 0, worst_dark_lvl = 0;
    float at[3] = {0, 0, 0};

    for (int i = 0; i <= N; i++)
    for (int j = 0; j <= N; j++)
    for (int k = 0; k <= N; k++) {
        float src[4] = { 0.15f * i / N, 0.15f * j / N, 0.15f * k / N, 1.0f };
        float lo[4], ro[4];
        if (!LOCALSK_skcms_Transform(src, skcms_PixelFormat_RGBA_ffff,
                                     skcms_AlphaFormat_Opaque, &lp,
                                     lo, skcms_PixelFormat_RGBA_ffff,
                                     skcms_AlphaFormat_Opaque, NULL, 1) ||
            !REFSK_skcms_Transform(src, skcms_PixelFormat_RGBA_ffff,
                                   skcms_AlphaFormat_Opaque, &rp,
                                   ro, skcms_PixelFormat_RGBA_ffff,
                                   skcms_AlphaFormat_Opaque, NULL, 1)) {
            printf("transform failed at pixel\n");
            return 2;
        }
        bool is_dark = src[0] < 0.03f && src[1] < 0.03f && src[2] < 0.03f;
        for (int c = 0; c < 3; c++) {
            double e = fabs((double)lo[c] - ro[c]);
            double lvl = e * 255.0;
            total++;
            if (e > worst_abs) { worst_abs = e; worst_lvl = lvl; }
            if (is_dark) {
                dark++;
                if (e > worst_dark_abs) {
                    worst_dark_abs = e;
                    worst_dark_lvl = lvl;
                    at[0] = src[0]; at[1] = src[1]; at[2] = src[2];
                }
            }
        }
    }

    printf("profile              : %s\n", path);
    printf("pixels/channel checks: %ld (dark: %ld)\n", total, dark);
    printf("worst abs delta      : %.6g  (%.4f 8-bit levels)\n", worst_abs, worst_lvl);
    printf("worst DARK delta     : %.6g  (%.4f 8-bit levels)\n", worst_dark_abs, worst_dark_lvl);
    printf("dark worst at rgb    : (%.5f, %.5f, %.5f)\n", at[0], at[1], at[2]);
    printf("one 8-bit level      : %.6g\n", 1.0 / 255.0);
    free(buf);
    return 0;
}
