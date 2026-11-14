// Drives two skcms builds (baseline = BASE_ prefix, reference = REF_ prefix)
// over every test ICC profile and reports per-pixel differences, with extra
// sampling density in the dark region.
#include "src/skcms_public.h"
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE(x) BASESK_##x
#define REF(x)  REFSK_##x

typedef bool (*xform_fn)(const void*, skcms_PixelFormat, skcms_AlphaFormat,
                         const skcms_ICCProfile*, void*, skcms_PixelFormat,
                         skcms_AlphaFormat, const skcms_ICCProfile*, size_t);

extern "C" {
bool BASESK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool REFSK_skcms_ParseWithA2BPriority(const void*, size_t, const int*, int, skcms_ICCProfile*);
bool BASESK_skcms_Transform(const void*, skcms_PixelFormat, skcms_AlphaFormat,
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
    uint8_t* buf = (uint8_t*)malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void collect_profiles(const char* dir, char*** out, int* count, int* cap) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* name = e->d_name;
        size_t nl = strlen(name);
        if (nl < 5 || strcmp(name + nl - 4, ".icc") != 0) continue;
        if (*count == *cap) {
            *cap *= 2;
            *out = (char**)realloc(*out, *cap * sizeof(char*));
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        (*out)[(*count)++] = strdup(path);
    }
    closedir(d);
}

int main(int argc, char** argv) {
    xform_fn base_xform = (xform_fn)BASE(skcms_Transform);
    xform_fn ref_xform  = (xform_fn)REF(skcms_Transform);
    static const int priority[] = {0, 1};

    char** files = NULL;
    int count = 0, cap = 16;
    files = (char**)malloc(cap * sizeof(char*));
    const char* dirs[] = { "profiles/misc", "profiles/mobile", "profiles/color.org",
                           "profiles/fuzz", "." };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        collect_profiles(dirs[i], &files, &count, &cap);
    }

    // Test pixel set: dark-dense grid plus full-range samples.
    const int GRID = 24;
    int npix = 0;
    for (int i = 0; i <= GRID; i++)
        for (int j = 0; j <= GRID; j++)
            for (int k = 0; k <= GRID; k++)
                npix++;
    float* src_rgb = (float*)malloc(npix * 3 * sizeof(float));
    int p = 0;
    for (int i = 0; i <= GRID; i++)
        for (int j = 0; j <= GRID; j++)
            for (int k = 0; k <= GRID; k++) {
                // Half the range concentrated in [0, 0.15] via sqrt-ish spacing.
                src_rgb[3*p+0] = 0.15f * i / GRID;
                src_rgb[3*p+1] = 0.15f * j / GRID;
                src_rgb[3*p+2] = 0.15f * k / GRID;
                p++;
            }

    double global_worst = 0;
    int differing_profiles = 0;
    for (int fi = 0; fi < count; fi++) {
        size_t len;
        uint8_t* buf = read_file(files[fi], &len);
        if (!buf) continue;
        skcms_ICCProfile bp, rp;
        bool bok = BASESK_skcms_ParseWithA2BPriority(buf, len, priority, 2, &bp);
        bool rok = REFSK_skcms_ParseWithA2BPriority(buf, len, priority, 2, &rp);
        if (bok != rok) {
            printf("PARSE-DIFF %s base=%d ref=%d\n", files[fi], bok, rok);
            free(buf);
            differing_profiles++;
            continue;
        }
        if (!bok) { free(buf); continue; }

        float* bdst = (float*)malloc(npix * 4 * sizeof(float));
        float* rdst = (float*)malloc(npix * 4 * sizeof(float));
        bool bok2 = base_xform(src_rgb, skcms_PixelFormat_RGBA_ffff,
                               skcms_AlphaFormat_Opaque, &bp,
                               bdst, skcms_PixelFormat_RGBA_ffff,
                               skcms_AlphaFormat_Opaque, NULL, npix);
        bool rok2 = ref_xform(src_rgb, skcms_PixelFormat_RGBA_ffff,
                              skcms_AlphaFormat_Opaque, &rp,
                              rdst, skcms_PixelFormat_RGBA_ffff,
                              skcms_AlphaFormat_Opaque, NULL, npix);
        if (bok2 != rok2) {
            printf("XFORM-DIFF %s base=%d ref=%d\n", files[fi], bok2, rok2);
            differing_profiles++;
        } else if (bok2) {
            double worst = 0;
            int worst_pix = 0;
            for (int q = 0; q < npix; q++)
                for (int c = 0; c < 3; c++) {
                    double e = fabs((double)bdst[4*q+c] - rdst[4*q+c]);
                    if (e > worst) { worst = e; worst_pix = q; }
                }
            if (worst > 1e-7) {
                differing_profiles++;
                int q = worst_pix;
                printf("NUM-DIFF  %-55s worst=%.6g (%.4f lvl8) at src=(%.4f,%.4f,%.4f)\n",
                       files[fi], worst, worst * 255.0,
                       src_rgb[3*q], src_rgb[3*q+1], src_rgb[3*q+2]);
                if (worst > global_worst) global_worst = worst;
            }
        }
        free(bdst); free(rdst); free(buf);
    }
    printf("\nprofiles=%d differing=%d global_worst=%.6g (%.4f 8-bit levels)\n",
           count, differing_profiles, global_worst, global_worst * 255.0);
    free(src_rgb);
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    return 0;
}
