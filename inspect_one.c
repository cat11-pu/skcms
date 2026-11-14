#include "src/skcms_public.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(n);
    size_t got = fread(b, 1, n, f);
    (void)got;
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "profiles/fuzz/b2a_no_clut.icc";
    size_t len;
    uint8_t* buf = read_file(path, &len);
    static const int prio[] = {0, 1};
    skcms_ICCProfile bp, rp;
    bool bok = BASESK_skcms_ParseWithA2BPriority(buf, len, prio, 2, &bp);
    bool rok = REFSK_skcms_ParseWithA2BPriority(buf, len, prio, 2, &rp);
    printf("parsed base=%d ref=%d\n", bok, rok);
    if (!bok || !rok) return 1;

    printf("base B2A in=%u out=%u  ref B2A in=%u out=%u\n",
           bp.B2A.input_channels, bp.B2A.output_channels,
           rp.B2A.input_channels, rp.B2A.output_channels);
    for (int i = 0; i < 3; i++) {
        printf("B2A.input_curves[%d] base entries=%u ref entries=%u\n",
               i, bp.B2A.input_curves[i].table_entries,
               rp.B2A.input_curves[i].table_entries);
    }
    for (int i = 0; i < 3; i++) {
        const skcms_Curve* c = &rp.B2A.input_curves[i];
        if (c->table_entries && c->table_16) {
            printf("ref input_curves[%d] raw16:", i);
            for (uint32_t k = 0; k < c->table_entries && k < 4; k++) {
                uint16_t be;
                __builtin_memcpy(&be, c->table_16 + 2*k, 2);
                uint16_t le = ((be << 8) | (be >> 8)) & 0xffff;
                printf(" %u", le);
            }
            printf("\n");
        }
    }

    float src[4] = { 0.1188f, 0.0f, 0.0625f, 1.0f };
    float bdst[4], rdst[4];
    bool b2 = BASESK_skcms_Transform(src, skcms_PixelFormat_RGBA_ffff,
                                     skcms_AlphaFormat_Opaque, &bp,
                                     bdst, skcms_PixelFormat_RGBA_ffff,
                                     skcms_AlphaFormat_Opaque, NULL, 1);
    bool r2 = REFSK_skcms_Transform(src, skcms_PixelFormat_RGBA_ffff,
                                    skcms_AlphaFormat_Opaque, &rp,
                                    rdst, skcms_PixelFormat_RGBA_ffff,
                                    skcms_AlphaFormat_Opaque, NULL, 1);
    printf("xform base=%d ref=%d\n", b2, r2);
    printf("base out = (%.6f, %.6f, %.6f)\n", bdst[0], bdst[1], bdst[2]);
    printf("ref  out = (%.6f, %.6f, %.6f)\n", rdst[0], rdst[1], rdst[2]);
    free(buf);
    return 0;
}
