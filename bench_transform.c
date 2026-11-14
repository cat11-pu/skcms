/*
 * Copyright 2026 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Bench harness for the sRGB -> Display P3 RGBA_8888 pipeline
// (load_8888, tf_rgb, matrix_3x3, tf_rgb, store_8888).
//
// Prints best-of-N wall-clock ms for three image sizes plus a 64-bit
// FNV-1a hash of each output buffer so optimized builds can be checked
// for pixel-exact agreement with the reference build.

#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS
#endif

#define _POSIX_C_SOURCE 199309L

#include "src/skcms_public.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define expect(cond) if (!(cond)) exit(1)

static double now_ms(void) {
    struct timespec ts;
    expect(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static uint32_t rng_state = 0x243F6A88u;
static uint32_t next_u32(void) {  // xorshift32, deterministic across builds
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static uint64_t fnv1a64(const unsigned char* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

int main(int argc, char** argv) {
    int reps = 9;
    const char* dump_prefix = NULL;
    for (int i = 0; i < argc; i++) {
        if (0 == strcmp(argv[i], "-r")) { reps = atoi(argv[++i]); }
        if (0 == strcmp(argv[i], "-d")) { dump_prefix = argv[++i]; }
    }

    // sRGB -> Display P3 (same matrix as bench.c).
    skcms_ICCProfile src_profile = *skcms_sRGB_profile(),
                     dst_profile = *skcms_sRGB_profile();
    dst_profile.toXYZD50 = (skcms_Matrix3x3){{
        { 0.51512146f  , 0.29197692f , 0.15710449f},
        { 0.24119567f  , 0.6922454f  , 0.0665741f },
        {-0.0010375976f, 0.041885376f, 0.7840728f },
    }};

    const struct { int w, h; } sizes[] = {
        {1920, 1080},   //  ~2.1 MP
        {3840, 2160},   //  ~8.3 MP
        {4000, 2500},   // 10.0 MP
    };

    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
        size_t n = (size_t)sizes[s].w * sizes[s].h;
        uint8_t* src_px = malloc(n * 4);
        uint8_t* dst_px = malloc(n * 4);
        expect(src_px && dst_px);

        rng_state = 0x243F6A88u;
        for (size_t i = 0; i < n * 4; i++) {
            src_px[i] = (uint8_t)(next_u32() >> 24);
        }

        double best = 1e30;
        for (int r = 0; r < reps; r++) {
            memset(dst_px, 0, n * 4);
            double t0 = now_ms();
            expect(skcms_Transform(src_px, skcms_PixelFormat_RGBA_8888,
                                   skcms_AlphaFormat_Unpremul, &src_profile,
                                   dst_px, skcms_PixelFormat_RGBA_8888,
                                   skcms_AlphaFormat_Unpremul, &dst_profile,
                                   n));
            double t1 = now_ms();
            if (t1 - t0 < best) { best = t1 - t0; }
        }

        printf("%4dx%-4d %10zu px  %8.3f ms  (%6.3f ns/px)  hash=%016llx\n",
               sizes[s].w, sizes[s].h, n, best, best * 1e6 / (double)n,
               (unsigned long long)fnv1a64(dst_px, n * 4));

        if (dump_prefix) {
            char path[256];
            snprintf(path, sizeof(path), "%s_%zux%zu.raw", dump_prefix,
                     (size_t)sizes[s].w, (size_t)sizes[s].h);
            FILE* fp = fopen(path, "wb");
            expect(fp && fwrite(dst_px, 1, n * 4, fp) == n * 4);
            fclose(fp);
        }

        free(src_px);
        free(dst_px);
    }
    return 0;
}
