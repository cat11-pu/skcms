// Before/after comparison against the reference implementation.
// OLDSK_ = pre-fix baseline, LOCALSK_ = fixed build, REFSK_ = upstream reference.
#include "src/skcms_public.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
bool OLDSK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool LOCALSK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool REFSK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool OLDSK_skcms_Transform(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                           const skcms_ICCProfile*, void*, skcms_PixelFormat,
                           skcms_AlphaFormat, const skcms_ICCProfile*, size_t);
bool LOCALSK_skcms_Transform(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                             const skcms_ICCProfile*, void*, skcms_PixelFormat,
                             skcms_AlphaFormat, const skcms_ICCProfile*, size_t);
bool REFSK_skcms_Transform(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                           const skcms_ICCProfile*, void*, skcms_PixelFormat,
                           skcms_AlphaFormat, const skcms_ICCProfile*, size_t);
}

typedef bool (*parse_t)(const void*, size_t, const int*, int, skcms_ICCProfile*);
typedef bool (*xform_t)(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                        const skcms_ICCProfile*, void*, skcms_PixelFormat,
                        skcms_AlphaFormat, const skcms_ICCProfile*, size_t);

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

static double measure(parse_t pf, xform_t xf, const uint8_t* buf, size_t len,
                      const skcms_ICCProfile* refprof, xform_t refxf,
                      long* dark_out) {
    static const int prio[] = {0, 1};
    skcms_ICCProfile p;
    if (!pf(buf, len, prio, 2, &p)) return -1.0;
    const int N = 40;
    double worst = 0;
    long dark = 0;
    for (int i = 0; i <= N; i++)
    for (int j = 0; j <= N; j++)
    for (int k = 0; k <= N; k++) {
        float src[4] = { 0.15f*i/N, 0.15f*j/N, 0.15f*k/N, 1.0f };
        float mine[4], ref[4];
        if (!xf(src, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, &p,
                mine, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, NULL, 1))
            return -1.0;
        if (!refxf(src, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, refprof,
                   ref, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, NULL, 1))
            return -1.0;
        bool is_dark = src[0] < 0.03f && src[1] < 0.03f && src[2] < 0.03f;
        if (is_dark) dark++;
        for (int c = 0; c < 3; c++) {
            double e = fabs((double)mine[c] - (double)ref[c]);
            if (e > worst) worst = e;
        }
    }
    if (dark_out) *dark_out = dark;
    return worst;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "profiles/fuzz/b2a_no_clut.icc";
    size_t len;
    uint8_t* buf = read_file(path, &len);
    if (!buf) { printf("cannot open %s\n", path); return 2; }
    static const int prio[] = {0, 1};
    skcms_ICCProfile refprof;
    if (!REFSK_skcms_ParseWithA2BPriority(buf, len, prio, 2, &refprof)) {
        printf("ref parse failed\n"); return 2;
    }
    long dark = 0;
    double before = measure(OLDSK_skcms_ParseWithA2BPriority, OLDSK_skcms_Transform,
                            buf, len, &refprof, REFSK_skcms_Transform, &dark);
    double after  = measure(LOCALSK_skcms_ParseWithA2BPriority, LOCALSK_skcms_Transform,
                            buf, len, &refprof, REFSK_skcms_Transform, NULL);
    printf("profile           : %s\n", path);
    printf("dark pixels/grid  : %ld\n", dark);
    printf("BEFORE fix max |d|: %.6g  (%.4f 8-bit levels)\n", before, before * 255.0);
    printf("AFTER  fix max |d|: %.6g  (%.4f 8-bit levels)\n", after, after * 255.0);
    printf("one 8-bit level   : %.6g\n", 1.0/255.0);
    free(buf);
    return 0;
}
