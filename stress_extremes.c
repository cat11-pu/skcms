// Stress test: extreme / NaN / Inf / out-of-range pixels through every parsed
// profile in both directions. Must never crash; transforms are allowed to
// return false for unsupported combinations.
#include "src/skcms_public.h"
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    const skcms_ICCProfile* srgb = skcms_sRGB_profile();
    float nanv = nanf("");
    float infv = INFINITY;
    float extremes[][4] = {
        {0, 0, 0, 1}, {1, 1, 1, 1}, {-1, -1, -1, 1}, {2, 2, 2, 1},
        {1e-30f, 1e-30f, 1e-30f, 1}, {1e30f, 1e30f, 1e30f, 1},
        {nanv, 0, 0, 1}, {infv, 0, 0, 1}, {-infv, 0.5f, 0.5f, 1},
        {0.0001f, 0.0002f, 0.0003f, 1},
    };
    int checked = 0;
    for (int fi = 0; fi < n; fi++) {
        size_t len;
        uint8_t* buf = read_file(files[fi], &len);
        if (!buf) continue;
        skcms_ICCProfile p;
        if (!skcms_Parse(buf, len, &p)) { free(buf); continue; }
        for (unsigned e = 0; e < sizeof(extremes)/sizeof(extremes[0]); e++) {
            float dst[4];
            (void)skcms_Transform(extremes[e], skcms_PixelFormat_RGBA_ffff,
                                  skcms_AlphaFormat_Opaque, &p,
                                  dst, skcms_PixelFormat_RGBA_ffff,
                                  skcms_AlphaFormat_Opaque, srgb, 1);
            (void)skcms_Transform(extremes[e], skcms_PixelFormat_RGBA_ffff,
                                  skcms_AlphaFormat_Opaque, srgb,
                                  dst, skcms_PixelFormat_RGBA_ffff,
                                  skcms_AlphaFormat_Opaque, &p, 1);
            checked++;
        }
        free(buf);
    }
    printf("extreme transforms attempted: %d across %d profiles, no crash\n", checked, n);
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
    return 0;
}
