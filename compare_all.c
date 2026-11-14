// Sweeps every test ICC profile and reports, per profile, the worst per-pixel
// delta of the pre-fix (OLD) and fixed (LOCAL) builds versus the reference (REF).
#include "src/skcms_public.h"
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint8_t* read_file(const char* p, size_t* len) {
    FILE* f = fopen(p, "rb");
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

static double worst_delta(parse_t pf, xform_t xf, const uint8_t* buf, size_t len,
                          const skcms_ICCProfile* rp, xform_t rxf) {
    static const int prio[] = {0, 1};
    skcms_ICCProfile p;
    if (!pf(buf, len, prio, 2, &p)) return -1.0;
    const int N = 16;
    double w = 0;
    for (int i = 0; i <= N; i++)
    for (int j = 0; j <= N; j++)
    for (int k = 0; k <= N; k++) {
        float s[4] = { 0.15f*i/N, 0.15f*j/N, 0.15f*k/N, 1.0f };
        float m[4], r[4];
        if (!xf(s, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, &p,
                m, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, NULL, 1))
            return -1.0;
        if (!rxf(s, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, rp,
                 r, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Opaque, NULL, 1))
            return -1.0;
        for (int c = 0; c < 3; c++) {
            double e = fabs((double)m[c] - (double)r[c]);
            if (e > w) w = e;
        }
    }
    return w;
}

static void collect(const char* dir, char*** files, int* n, int* cap) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".icc")) continue;
        if (*n == *cap) { *cap *= 2; *files = (char**)realloc(*files, *cap * sizeof(char*)); }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        (*files)[(*n)++] = strdup(path);
    }
    closedir(d);
}

int main(void) {
    char** files = NULL;
    int n = 0, cap = 32;
    files = (char**)malloc(cap * sizeof(char*));
    const char* dirs[] = {"profiles/misc", "profiles/mobile", "profiles/color.org",
                          "profiles/fuzz", "."};
    for (unsigned i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++)
        collect(dirs[i], &files, &n, &cap);

    static const int prio[] = {0, 1};
    int regressed = 0;
    double global_before = 0, global_after = 0;
    printf("%-52s %12s %12s\n", "profile", "before(lvl)", "after(lvl)");
    for (int fi = 0; fi < n; fi++) {
        size_t len;
        uint8_t* buf = read_file(files[fi], &len);
        if (!buf) continue;
        skcms_ICCProfile rp;
        if (!REFSK_skcms_ParseWithA2BPriority(buf, len, prio, 2, &rp)) { free(buf); continue; }
        double before = worst_delta(OLDSK_skcms_ParseWithA2BPriority, OLDSK_skcms_Transform,
                                    buf, len, &rp, REFSK_skcms_Transform);
        double after  = worst_delta(LOCALSK_skcms_ParseWithA2BPriority, LOCALSK_skcms_Transform,
                                    buf, len, &rp, REFSK_skcms_Transform);
        if (before > 1e-7 || after > 1e-7) {
            printf("%-52s %12.4f %12.4f\n", files[fi],
                   before < 0 ? -1.0 : before * 255.0,
                   after  < 0 ? -1.0 : after  * 255.0);
        }
        if (before > global_before) global_before = before;
        if (after > global_after) global_after = after;
        if (after > 1e-7) regressed++;
        free(buf);
    }
    printf("\nprofiles=%d  global worst BEFORE=%.6g (%.4f lvl)  AFTER=%.6g (%.4f lvl)  profiles>1e-7 after=%d\n",
           n, global_before, global_before * 255.0,
           global_after, global_after * 255.0, regressed);
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
    return 0;
}
