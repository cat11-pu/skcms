/*
 * Copyright 2026 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Randomized properties for skcms_Transform().
 *
 * Each generated group contains one curve and a fixed set of pixels.  The same
 * 500,000 groups are checked for all four properties:
 *
 *   - monotonicity,
 *   - white point preservation,
 *   - round-trip consistency,
 *   - no sign/order inversion in the dark end.
 *
 * LUT round trips are quantization-aware.  An 8-bit LUT cannot generally
 * recover the exact input near black (a gamma-4 curve maps several 8-bit input
 * codes to zero), so the LUT recovery bound includes the measured table error
 * and one table quantum.  The check still catches wrong indexing, clamping,
 * and non-invertible or non-monotonic tables.
 */

#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS
    #pragma warning(disable : 4127)  // conditional expression is constant
#endif

#include "src/skcms_public.h"
#include "property_tests.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
    #define PROPERTY_TRAP __debugbreak
#elif defined(__clang__)
    #define PROPERTY_TRAP __builtin_debugtrap
#else
    #define PROPERTY_TRAP __builtin_trap
#endif

#define PROPERTY_GROUPS 500000
#define GROUP_PIXELS 10
#define MAX_TABLE_ENTRIES 4096
#define MAX_TABLE16_BYTES (2 * MAX_TABLE_ENTRIES)

typedef enum {
    kInputDegenerate,
    kInputNegative,
    kInputOutOfRange,
    kInputLutBoundary,
    kInputTypical,
} InputClass;

typedef enum {
    kCurveIdentity,
    kCurveGamma,
    kCurvePowerToe,
    kCurveTable8,
    kCurveTable16,
} CurveKind;

typedef enum {
    kPropertyMonotonic,
    kPropertyWhite,
    kPropertyRoundTrip,
    kPropertyDark,
} Property;

typedef struct {
    uint64_t state;
} Rng;

typedef struct {
    uint64_t iteration;
    InputClass input_class;
    CurveKind kind;

    // For table curves this remains the normalized analytic curve that the
    // table samples.  It is also the destination curve in round-trip tests.
    skcms_TransferFunction tf;

    uint32_t table_entries;
    uint8_t table8[MAX_TABLE_ENTRIES];
    uint8_t table16[MAX_TABLE16_BYTES];  // big-endian, as stored in ICC profiles

    float x;
    float lo;
    float hi;
    float dark_scale;
} Case;

typedef struct {
    uint64_t groups;
    uint64_t degenerate;
    uint64_t negative;
    uint64_t out_of_range;
    uint64_t lut_boundary;
    uint64_t typical;
    uint64_t parametric;
    uint64_t table8;
    uint64_t table16;
    uint64_t digest;
} Stats;

static uint64_t rng_u64(Rng* rng) {
    uint64_t x = rng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->state = x;
    return x * UINT64_C(2685821657736338717);
}

static uint32_t rng_u32(Rng* rng) {
    return (uint32_t)(rng_u64(rng) >> 32);
}

static uint32_t rng_bounded(Rng* rng, uint32_t n) {
    return n ? rng_u32(rng) % n : 0;
}

static float rng_unit(Rng* rng) {
    return (float)(rng_u32(rng) >> 8) * (1.0f / 16777216.0f);
}

static bool finitef(float x) {
    return x * 0.0f == 0.0f;
}

static float minf(float x, float y) {
    return x < y ? x : y;
}

static float maxf(float x, float y) {
    return x > y ? x : y;
}

static float clamp01(float x) {
    return minf(maxf(x, 0.0f), 1.0f);
}

static uint32_t float_bits(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

static float minus_1_ulp(float x) {
    uint32_t bits = float_bits(x);
    bits -= 1;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static void hash_u64(uint64_t* hash, uint64_t value) {
    *hash ^= value;
    *hash *= UINT64_C(1099511628211);
}

static void hash_float(uint64_t* hash, float value) {
    hash_u64(hash, float_bits(value));
}

static const char* input_class_name(InputClass cls) {
    switch (cls) {
        case kInputDegenerate: return "degenerate";
        case kInputNegative:   return "negative";
        case kInputOutOfRange: return "out-of-range";
        case kInputLutBoundary:return "lut-boundary";
        case kInputTypical:    return "typical";
    }
    return "unknown";
}

static const char* curve_kind_name(CurveKind kind) {
    switch (kind) {
        case kCurveIdentity: return "identity";
        case kCurveGamma:    return "gamma";
        case kCurvePowerToe: return "power-toe";
        case kCurveTable8:   return "table8";
        case kCurveTable16:  return "table16";
    }
    return "unknown";
}

static const char* property_name(Property property) {
    switch (property) {
        case kPropertyMonotonic: return "monotonicity";
        case kPropertyWhite:     return "white-point";
        case kPropertyRoundTrip: return "round-trip";
        case kPropertyDark:      return "dark-no-inversion";
    }
    return "unknown";
}

static bool is_table(CurveKind kind) {
    return kind == kCurveTable8 || kind == kCurveTable16;
}

static void set_identity(Case* c) {
    c->kind = kCurveIdentity;
    c->tf.g = 1.0f;
    c->tf.a = 1.0f;
    c->tf.b = 0.0f;
    c->tf.c = 0.0f;
    c->tf.d = 0.0f;
    c->tf.e = 0.0f;
    c->tf.f = 0.0f;
}

static void set_gamma(Case* c, float g) {
    c->kind = kCurveGamma;
    c->tf.g = g;
    c->tf.a = 1.0f;
    c->tf.b = 0.0f;
    c->tf.c = 0.0f;
    c->tf.d = 0.0f;
    c->tf.e = 0.0f;
    c->tf.f = 0.0f;
}

// A normalized, continuous power curve with a linear toe:
//   f(x) = x < d ? d^(g-1) * x : x^g
// It is odd-extended by skcms_TransferFunction_eval(), maps 0 -> 0 and
// 1 -> 1, and is monotonic for all g >= 0, d in [0,1].
static void set_power_toe(Case* c, float g, float d) {
    c->kind = kCurvePowerToe;
    c->tf.g = g;
    c->tf.a = 1.0f;
    c->tf.b = 0.0f;
    c->tf.d = clamp01(d);
    c->tf.c = c->tf.d > 0.0f ? powf(c->tf.d, g - 1.0f) : 0.0f;
    c->tf.e = 0.0f;
    c->tf.f = 0.0f;
}

static uint16_t table16_load(const Case* c, uint32_t i) {
    return (uint16_t)(((uint16_t)c->table16[2 * i] << 8) |
                       (uint16_t)c->table16[2 * i + 1]);
}

static void table16_store(Case* c, uint32_t i, uint16_t value) {
    c->table16[2 * i] = (uint8_t)(value >> 8);
    c->table16[2 * i + 1] = (uint8_t)(value & 0xff);
}

static void fill_table(Case* c) {
    uint32_t n = c->table_entries;
    if (n < 2) {
        n = 2;
        c->table_entries = n;
    }
    if (n > MAX_TABLE_ENTRIES) {
        n = MAX_TABLE_ENTRIES;
        c->table_entries = n;
    }

    for (uint32_t i = 0; i < n; ++i) {
        float x = (float)i / (float)(n - 1);
        float y = clamp01(skcms_TransferFunction_eval(&c->tf, x));
        if (c->kind == kCurveTable8) {
            c->table8[i] = (uint8_t)(y * 255.0f + 0.5f);
        } else {
            uint16_t value = (uint16_t)(y * 65535.0f + 0.5f);
            table16_store(c, i, value);
        }
    }

    // Keep the generated curve normalized even after quantization.
    if (c->kind == kCurveTable8) {
        c->table8[0] = 0;
        c->table8[n - 1] = 255;
    } else {
        table16_store(c, 0, 0);
        table16_store(c, n - 1, 65535);
    }
}

static float table_lookup(const Case* c, float x) {
    uint32_t n = c->table_entries;
    if (n < 2) {
        return 0.0f;
    }

    float ix = clamp01(x) * (float)(n - 1);
    uint32_t lo = (uint32_t)ix;
    if (lo >= n - 1) {
        lo = n - 2;
    }
    uint32_t hi = (uint32_t)minus_1_ulp(ix + 1.0f);
    if (hi >= n) {
        hi = n - 1;
    }
    float t = ix - (float)lo;

    float l, h;
    if (c->kind == kCurveTable8) {
        l = (float)c->table8[lo] * (1.0f / 255.0f);
        h = (float)c->table8[hi] * (1.0f / 255.0f);
    } else {
        l = (float)table16_load(c, lo) * (1.0f / 65535.0f);
        h = (float)table16_load(c, hi) * (1.0f / 65535.0f);
    }
    return l + (h - l) * t;
}

static uint32_t choose_table_entries(Rng* rng, InputClass cls) {
    static const uint32_t sizes[] = { 2, 3, 4, 5, 17, 257, 1024, 4096 };
    if (cls == kInputDegenerate) {
        return 2 + rng_bounded(rng, 2);
    }
    return sizes[rng_bounded(rng, (uint32_t)(sizeof(sizes) / sizeof(sizes[0])))];
}

static void choose_analytic_curve(Case* c, Rng* rng, bool degenerate) {
    float g = 0.125f + 3.875f * rng_unit(rng);
    float d = rng_unit(rng);

    if (degenerate) {
        switch (rng_bounded(rng, 5)) {
            case 0: set_identity(c); return;
            case 1: set_gamma(c, 1.0f); return;
            case 2: set_gamma(c, 0.5f); return;
            case 3: set_power_toe(c, g, 0.0f); return;
            default: set_power_toe(c, g, 1.0f); return;
        }
    }

    switch (rng_bounded(rng, 20)) {
        case 0:  set_identity(c); return;
        case 1:
        case 2:
        case 3:
        case 4:  set_gamma(c, g); return;
        default: break;
    }

    if (rng_bounded(rng, 20) == 0) {
        d = rng_bounded(rng, 2) ? 0.0f : 1.0f;
    }
    set_power_toe(c, g, d);
}

static InputClass class_for_iteration(uint64_t i) {
    uint32_t slot = (uint32_t)(i % 20);
    if (slot < 2) {
        return kInputDegenerate;    // 50,000
    }
    if (slot < 5) {
        return kInputNegative;      // 75,000
    }
    if (slot < 8) {
        return kInputOutOfRange;    // 75,000
    }
    if (slot < 12) {
        return kInputLutBoundary;   // 100,000
    }
    return kInputTypical;           // 200,000
}

static void set_ordered_pair(Case* c, float a, float b) {
    if (a > b) {
        float t = a;
        a = b;
        b = t;
    }
    if (a == b) {
        b = nextafterf(a, 2.0f);
    }
    c->lo = a;
    c->hi = b;
}

static void choose_pixels(Case* c, Rng* rng) {
    c->x = 0.0f;
    c->dark_scale = powf(10.0f, -6.0f + 4.0f * rng_unit(rng));

    switch (c->input_class) {
        case kInputDegenerate: {
            switch (rng_bounded(rng, 5)) {
                case 0:
                    set_ordered_pair(c, 0.0f, nextafterf(0.0f, 1.0f));
                    c->x = c->lo;
                    break;
                case 1:
                    set_ordered_pair(c, nextafterf(1.0f, 0.0f), 1.0f);
                    c->x = c->hi;
                    break;
                case 2:
                    set_ordered_pair(c, -nextafterf(0.0f, 1.0f),
                                        nextafterf(0.0f, 1.0f));
                    c->x = c->lo;
                    break;
                case 3:
                    set_ordered_pair(c, 0.0f, 1.0f);
                    c->x = rng_bounded(rng, 2) ? c->lo : c->hi;
                    break;
                default:
                    set_ordered_pair(c, -1.0f, 1.0f);
                    c->x = c->lo;
                    break;
            }
            break;
        }

        case kInputNegative: {
            float magnitude = powf(10.0f, -6.0f + 7.2f * rng_unit(rng));
            float low = -magnitude;
            float high = -magnitude * rng_unit(rng);
            set_ordered_pair(c, low, high);
            c->x = c->lo;
            break;
        }

        case kInputOutOfRange: {
            float low = 1.0f + 15.0f * rng_unit(rng);
            float high = low + 15.0f * rng_unit(rng);
            set_ordered_pair(c, low, high);
            c->x = c->lo;
            break;
        }

        case kInputLutBoundary: {
            if (is_table(c->kind)) {
                uint32_t n = c->table_entries;
                uint32_t index = rng_bounded(rng, n);
                float node = (float)index / (float)(n - 1);
                switch (rng_bounded(rng, 4)) {
                    case 0:
                        set_ordered_pair(c, node, nextafterf(node, 2.0f));
                        break;
                    case 1:
                        set_ordered_pair(c, nextafterf(node, -1.0f), node);
                        break;
                    case 2:
                        if (index + 1 < n) {
                            set_ordered_pair(c, node,
                                             (float)(index + 1) / (float)(n - 1));
                        } else {
                            set_ordered_pair(c, node, node);
                        }
                        break;
                    default:
                        set_ordered_pair(c, node, node);
                        break;
                }
                c->x = node;
                if (rng_bounded(rng, 2)) {
                    c->x = nextafterf(node, rng_bounded(rng, 2) ? 2.0f : -1.0f);
                }
            } else {
                float node = rng_bounded(rng, 3) == 0 ? 0.0f : 1.0f;
                set_ordered_pair(c, node, nextafterf(node, 2.0f));
                c->x = node;
            }
            break;
        }

        case kInputTypical:
        default: {
            float a = rng_unit(rng);
            float b = rng_unit(rng);
            set_ordered_pair(c, a, b);
            c->x = rng_bounded(rng, 2) ? c->lo : c->hi;
            break;
        }
    }
}

static void init_case(Case* c, Rng* rng, uint64_t iteration) {
    memset(c, 0, sizeof(*c));
    c->iteration = iteration;
    c->input_class = class_for_iteration(iteration);

    bool table = false;
    if (c->input_class == kInputLutBoundary) {
        table = true;
    } else if (c->input_class == kInputDegenerate) {
        table = rng_bounded(rng, 4) == 0;
    } else {
        table = rng_bounded(rng, 100) < 30;
    }

    bool degenerate_curve = c->input_class == kInputDegenerate;
    if (table) {
        CurveKind table_kind = rng_bounded(rng, 2) ? kCurveTable8 : kCurveTable16;
        c->table_entries = choose_table_entries(rng, c->input_class);
        choose_analytic_curve(c, rng, degenerate_curve);
        // choose_analytic_curve() selects a parametric representation.  Keep
        // its analytic function, but use the selected table representation.
        c->kind = table_kind;
        fill_table(c);
    } else {
        choose_analytic_curve(c, rng, degenerate_curve);
    }

    choose_pixels(c, rng);
}

static void make_case_profile(const Case* c, bool use_table, skcms_ICCProfile* p) {
    skcms_Init(p);
    if (use_table && is_table(c->kind)) {
        p->has_trc = true;
        for (int i = 0; i < 3; ++i) {
            p->trc[i].table_entries = c->table_entries;
            p->trc[i].table_8 = c->kind == kCurveTable8 ? c->table8 : NULL;
            p->trc[i].table_16 = c->kind == kCurveTable16 ? c->table16 : NULL;
        }
    } else {
        skcms_SetTransferFunction(p, &c->tf);
    }
    skcms_SetXYZD50(p, &skcms_XYZD50_profile()->toXYZD50);
}

static void build_inputs(const Case* c, float in[GROUP_PIXELS * 3]) {
    float dark[7] = {
        -4.0f * c->dark_scale,
        -2.0f * c->dark_scale,
        -1.0f * c->dark_scale,
         0.0f,
         1.0f * c->dark_scale,
         2.0f * c->dark_scale,
         4.0f * c->dark_scale,
    };
    float pixels[GROUP_PIXELS] = {
        c->lo, c->hi, 1.0f,
        dark[0], dark[1], dark[2], dark[3], dark[4], dark[5], dark[6],
    };

    for (int i = 0; i < GROUP_PIXELS; ++i) {
        in[3 * i + 0] = pixels[i];
        in[3 * i + 1] = pixels[i];
        in[3 * i + 2] = pixels[i];
    }
}

static bool run_case_outputs(const Case* c,
                             const float* in,
                             float* observed,
                             float* roundtrip) {
    skcms_ICCProfile src, dst;
    make_case_profile(c, true, &src);
    make_case_profile(c, false, &dst);

    if (!skcms_Transform(in, skcms_PixelFormat_RGB_fff,
                         skcms_AlphaFormat_Unpremul, &src,
                         observed, skcms_PixelFormat_RGB_fff,
                         skcms_AlphaFormat_Unpremul, skcms_XYZD50_profile(),
                         GROUP_PIXELS)) {
        return false;
    }
    return skcms_Transform(in, skcms_PixelFormat_RGB_fff,
                           skcms_AlphaFormat_Unpremul, &src,
                           roundtrip, skcms_PixelFormat_RGB_fff,
                           skcms_AlphaFormat_Unpremul, &dst,
                           GROUP_PIXELS);
}

static float table_recovery_tolerance(const Case* c,
                                      const skcms_TransferFunction* inv) {
    float measured = 0.0f;
    for (int i = 0; i < 33; ++i) {
        float x = (float)i / 32.0f;
        float y = table_lookup(c, x);
        float back = skcms_TransferFunction_eval(inv, y);
        measured = maxf(measured, fabsf(back - x));
    }

    float quantum = c->kind == kCurveTable8 ? 1.0f / 255.0f : 1.0f / 65535.0f;
    float low_quantum = fabsf(skcms_TransferFunction_eval(inv, quantum));
    float high_quantum = fabsf(1.0f - skcms_TransferFunction_eval(inv, 1.0f - quantum));
    float cell = 1.0f / (float)(c->table_entries - 1);
    return 3.0f * (measured + maxf(low_quantum, high_quantum) + cell) + 1.0e-3f;
}

static bool property_holds(Property property,
                           const Case* c,
                           const float* in,
                           const float* observed,
                           const float* roundtrip) {
    switch (property) {
        case kPropertyMonotonic:
            for (int channel = 0; channel < 3; ++channel) {
                float lo = observed[0 + channel];
                float hi = observed[3 + channel];
                // skcms intentionally uses approximate pow/log/exp.  Near the
                // top of the domain that can move the result by a few 1e-5
                // without reversing the curve; ignore that approximation noise
                // but still catch any real reversal.
                float slack = 4.0e-5f * maxf(1.0f, maxf(fabsf(lo), fabsf(hi)));
                if (!finitef(lo) || !finitef(hi) || lo > hi + slack) {
                    return false;
                }
            }
            return true;

        case kPropertyWhite:
            for (int channel = 0; channel < 3; ++channel) {
                float white = observed[2 * 3 + channel];
                if (!finitef(white) || fabsf(white - 1.0f) > 1.0e-5f) {
                    return false;
                }
            }
            return true;

        case kPropertyRoundTrip: {
            skcms_TransferFunction inv;
            bool table = is_table(c->kind);
            if (table && !skcms_TransferFunction_invert(&c->tf, &inv)) {
                return false;
            }

            float table_tol = table ? table_recovery_tolerance(c, &inv) : 0.0f;
            for (int pixel = 0; pixel < 2; ++pixel) {
                for (int channel = 0; channel < 3; ++channel) {
                    float x = in[3 * pixel + channel];
                    float actual = roundtrip[3 * pixel + channel];
                    float expected = x;
                    if (table) {
                        expected = skcms_TransferFunction_eval(&inv,
                                                               table_lookup(c, x));
                    }
                    if (!finitef(actual) || !finitef(expected)) {
                        return false;
                    }

                    float tol = 2.0e-3f * maxf(1.0f, fabsf(expected)) + 1.0e-5f;
                    if (fabsf(actual - expected) > tol) {
                        return false;
                    }

                    if (table) {
                        if (x >= 0.0f && x <= 1.0f) {
                            if (fabsf(actual - x) > table_tol) {
                                return false;
                            }
                        } else {
                            float clamped = clamp01(x);
                            if (fabsf(actual - clamped) >
                                2.0e-3f * maxf(1.0f, fabsf(clamped)) + 1.0e-5f) {
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        }

        case kPropertyDark: {
            float previous = 0.0f;
            for (int i = 3; i < GROUP_PIXELS; ++i) {
                float x = in[3 * i];
                float y = observed[3 * i];
                if (!finitef(y)) {
                    return false;
                }
                if (x < 0.0f && y > 0.0f) {
                    return false;
                }
                if (x > 0.0f && y < 0.0f) {
                    return false;
                }
                if (x == 0.0f && fabsf(y) > 1.0e-7f) {
                    return false;
                }
                if (i > 3 && y < previous) {
                    return false;
                }
                previous = y;
            }
            return true;
        }
    }
    return false;
}

static bool case_valid(const Case* c) {
    if (!finitef(c->tf.g) || !finitef(c->tf.a) || !finitef(c->tf.b) ||
        !finitef(c->tf.c) || !finitef(c->tf.d) || !finitef(c->tf.e) ||
        !finitef(c->tf.f)) {
        return false;
    }
    if (!finitef(c->x) || !finitef(c->lo) || !finitef(c->hi) ||
        !finitef(c->dark_scale) || c->lo > c->hi) {
        return false;
    }
    if (!skcms_TransferFunction_isSRGBish(&c->tf)) {
        return false;
    }
    if (fabsf(skcms_TransferFunction_eval(&c->tf, 0.0f)) > 1.0e-6f ||
        fabsf(skcms_TransferFunction_eval(&c->tf, 1.0f) - 1.0f) > 1.0e-6f) {
        return false;
    }

    if (!is_table(c->kind)) {
        return true;
    }
    if (c->table_entries < 2 || c->table_entries > MAX_TABLE_ENTRIES) {
        return false;
    }

    bool previous = false;
    float previous_value = 0.0f;
    for (uint32_t i = 0; i < c->table_entries; ++i) {
        float value = c->kind == kCurveTable8
                    ? (float)c->table8[i] * (1.0f / 255.0f)
                    : (float)table16_load(c, i) * (1.0f / 65535.0f);
        if (previous && value < previous_value) {
            return false;
        }
        previous = true;
        previous_value = value;
    }
    if (c->kind == kCurveTable8) {
        return c->table8[0] == 0 && c->table8[c->table_entries - 1] == 255;
    }
    return table16_load(c, 0) == 0 &&
           table16_load(c, c->table_entries - 1) == 65535;
}

static bool candidate_fails(Property property,
                            const Case* candidate,
                            const float* in,
                            float* observed,
                            float* roundtrip) {
    if (!case_valid(candidate)) {
        return false;
    }
    if (!run_case_outputs(candidate, in, observed, roundtrip)) {
        return false;
    }
    return !property_holds(property, candidate, in, observed, roundtrip);
}

static void print_float(FILE* fp, const char* name, float value) {
    fprintf(fp, " %s=%g(0x%08x)", name, (double)value,
            (unsigned)float_bits(value));
}

static uint64_t table_digest(const Case* c) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!is_table(c->kind)) {
        return hash;
    }
    if (c->kind == kCurveTable8) {
        for (uint32_t i = 0; i < c->table_entries; ++i) {
            hash_u64(&hash, c->table8[i]);
        }
    } else {
        for (uint32_t i = 0; i < c->table_entries; ++i) {
            hash_u64(&hash, table16_load(c, i));
        }
    }
    return hash;
}

static void print_case(FILE* fp, const char* label, const Case* c) {
    fprintf(fp, "%s: iteration=%llu class=%s curve=%s",
            label, (unsigned long long)c->iteration,
            input_class_name(c->input_class), curve_kind_name(c->kind));
    print_float(fp, "x", c->x);
    print_float(fp, "lo", c->lo);
    print_float(fp, "hi", c->hi);
    print_float(fp, "dark", c->dark_scale);
    fprintf(fp, "\n");
    fprintf(fp, "  tf:");
    print_float(fp, "g", c->tf.g);
    print_float(fp, "a", c->tf.a);
    print_float(fp, "b", c->tf.b);
    print_float(fp, "c", c->tf.c);
    print_float(fp, "d", c->tf.d);
    print_float(fp, "e", c->tf.e);
    print_float(fp, "f", c->tf.f);
    fprintf(fp, "\n");
    if (is_table(c->kind)) {
        fprintf(fp, "  table entries=%u digest=0x%016llx",
                c->table_entries, (unsigned long long)table_digest(c));
        uint32_t n = c->table_entries;
        if (n <= 8) {
            fprintf(fp, " values=[");
            for (uint32_t i = 0; i < n; ++i) {
                unsigned value = c->kind == kCurveTable8
                               ? c->table8[i] : table16_load(c, i);
                fprintf(fp, "%s%u", i ? "," : "", value);
            }
            fprintf(fp, "]");
        }
        fprintf(fp, "\n");
    }
}

static bool try_replace(Property property,
                        Case* best,
                        const Case* candidate) {
    float in[GROUP_PIXELS * 3];
    float observed[GROUP_PIXELS * 3];
    float roundtrip[GROUP_PIXELS * 3];
    build_inputs(candidate, in);
    if (!candidate_fails(property, candidate, in, observed, roundtrip)) {
        return false;
    }
    *best = *candidate;
    return true;
}

// Shrinks to a one-minimal counterexample with respect to the mutations below:
// no single mutation in this set still fails the property.  The mutations
// simplify inputs toward 0/1, halve dark magnitudes, reduce LUT size, and
// simplify the analytic curve toward identity.
static void shrink_failure(Property property, Case* c) {
    static const float kCurveValues[] = {
        0.0f, 1.0f, 0.5f, 0.125f, 0.25f, 2.0f, 4.0f,
    };
    static const float kPointValues[] = {
        -16.0f, -1.0f, -1.0e-7f, 0.0f, 1.0e-7f, 1.0f, 16.0f,
    };

    for (int round = 0; round < 128; ++round) {
        bool progress = false;

        {
            Case candidate = *c;
            set_identity(&candidate);
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }
        }

        if (is_table(c->kind)) {
            uint32_t sizes[] = { 2, 3, c->table_entries / 2, c->table_entries - 1 };
            for (uint32_t i = 0; i < (uint32_t)(sizeof(sizes) / sizeof(sizes[0])); ++i) {
                if (sizes[i] < 2 || sizes[i] >= c->table_entries) {
                    continue;
                }
                Case candidate = *c;
                candidate.table_entries = sizes[i];
                fill_table(&candidate);
                if (try_replace(property, c, &candidate)) {
                    progress = true;
                }
            }
        } else {
            for (uint32_t i = 0;
                 i < (uint32_t)(sizeof(kCurveValues) / sizeof(kCurveValues[0]));
                 ++i) {
                Case candidate = *c;
                float g = kCurveValues[i];
                if (g != 0.0f) {
                    set_gamma(&candidate, g);
                    if (try_replace(property, c, &candidate)) {
                        progress = true;
                    }
                }
                for (int d_index = 0; d_index < 2; ++d_index) {
                    candidate = *c;
                    set_power_toe(&candidate, g > 0.0f ? g : c->tf.g,
                                  d_index ? 1.0f : 0.0f);
                    if (try_replace(property, c, &candidate)) {
                        progress = true;
                    }
                }
            }
        }

        for (uint32_t i = 0;
             i < (uint32_t)(sizeof(kPointValues) / sizeof(kPointValues[0]));
             ++i) {
            Case candidate = *c;
            candidate.x = kPointValues[i];
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }

            candidate = *c;
            candidate.lo = kPointValues[i];
            if (candidate.lo > candidate.hi) {
                candidate.hi = candidate.lo;
            }
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }

            candidate = *c;
            candidate.hi = kPointValues[i];
            if (candidate.hi < candidate.lo) {
                candidate.lo = candidate.hi;
            }
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }
        }

        {
            Case candidate = *c;
            candidate.x *= 0.5f;
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }

            candidate = *c;
            candidate.lo *= 0.5f;
            candidate.hi *= 0.5f;
            set_ordered_pair(&candidate, candidate.lo, candidate.hi);
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }

            candidate = *c;
            candidate.dark_scale *= 0.5f;
            if (try_replace(property, c, &candidate)) {
                progress = true;
            }
        }

        if (!progress) {
            break;
        }
    }
}

static void report_failure(Property property, const Case* original) {
    Case shrunk = *original;
    shrink_failure(property, &shrunk);

    fprintf(stderr, "random property failure: %s\n", property_name(property));
    print_case(stderr, "original", original);
    print_case(stderr, "minimal", &shrunk);
    fflush(stderr);
    PROPERTY_TRAP();
}

static void hash_case(uint64_t* hash,
                      const Case* c,
                      const float* in,
                      const float* observed,
                      const float* roundtrip) {
    hash_u64(hash, c->iteration);
    hash_u64(hash, (uint64_t)c->input_class);
    hash_u64(hash, (uint64_t)c->kind);
    hash_float(hash, c->tf.g);
    hash_float(hash, c->tf.a);
    hash_float(hash, c->tf.b);
    hash_float(hash, c->tf.c);
    hash_float(hash, c->tf.d);
    hash_float(hash, c->tf.e);
    hash_float(hash, c->tf.f);
    hash_u64(hash, c->table_entries);
    hash_float(hash, c->x);
    hash_float(hash, c->lo);
    hash_float(hash, c->hi);
    hash_float(hash, c->dark_scale);
    for (int i = 0; i < GROUP_PIXELS * 3; ++i) {
        hash_float(hash, in[i]);
        hash_float(hash, observed[i]);
        hash_float(hash, roundtrip[i]);
    }
    if (is_table(c->kind)) {
        hash_u64(hash, table_digest(c));
    }
}

static Stats run_suite(uint64_t seed) {
    Stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.digest = UINT64_C(1469598103934665603) ^ seed;

    Rng rng = { seed ? seed : UINT64_C(1) };
    for (uint64_t iteration = 0; iteration < PROPERTY_GROUPS; ++iteration) {
        Case c;
        init_case(&c, &rng, iteration);

        float in[GROUP_PIXELS * 3];
        float observed[GROUP_PIXELS * 3];
        float roundtrip[GROUP_PIXELS * 3];
        build_inputs(&c, in);
        if (!run_case_outputs(&c, in, observed, roundtrip)) {
            fprintf(stderr, "skcms_Transform() unexpectedly failed at iteration %llu\n",
                    (unsigned long long)iteration);
            print_case(stderr, "case", &c);
            fflush(stderr);
            PROPERTY_TRAP();
        }

        hash_case(&stats.digest, &c, in, observed, roundtrip);
        stats.groups++;
        switch (c.input_class) {
            case kInputDegenerate:  stats.degenerate++;   break;
            case kInputNegative:    stats.negative++;     break;
            case kInputOutOfRange:  stats.out_of_range++; break;
            case kInputLutBoundary: stats.lut_boundary++; break;
            case kInputTypical:     stats.typical++;      break;
        }
        switch (c.kind) {
            case kCurveIdentity:
            case kCurveGamma:
            case kCurvePowerToe: stats.parametric++; break;
            case kCurveTable8:   stats.table8++;     break;
            case kCurveTable16:  stats.table16++;    break;
        }

        static const Property kProperties[] = {
            kPropertyMonotonic,
            kPropertyWhite,
            kPropertyRoundTrip,
            kPropertyDark,
        };
        for (uint32_t i = 0;
             i < (uint32_t)(sizeof(kProperties) / sizeof(kProperties[0]));
             ++i) {
            if (!property_holds(kProperties[i], &c, in, observed, roundtrip)) {
                report_failure(kProperties[i], &c);
            }
        }
    }
    return stats;
}

static bool same_stats(const Stats* a, const Stats* b) {
    return a->groups == b->groups &&
           a->degenerate == b->degenerate &&
           a->negative == b->negative &&
           a->out_of_range == b->out_of_range &&
           a->lut_boundary == b->lut_boundary &&
           a->typical == b->typical &&
           a->parametric == b->parametric &&
           a->table8 == b->table8 &&
           a->table16 == b->table16 &&
           a->digest == b->digest;
}

void skcms_RunRandomPropertyTests(void) {
    const uint64_t seed = UINT64_C(0x534b434d5350524f);  // "SKCMSPRO"
    Stats first = run_suite(seed);
    Stats second = run_suite(seed);

    if (!same_stats(&first, &second)) {
        fprintf(stderr,
                "random property tests were not bit-identical across two runs\n"
                "  run 1 digest=0x%016llx\n"
                "  run 2 digest=0x%016llx\n",
                (unsigned long long)first.digest,
                (unsigned long long)second.digest);
        fflush(stderr);
        PROPERTY_TRAP();
    }

    fprintf(stderr,
            "random properties: %llu groups per property "
            "(%llu property checks total)\n"
            "  inputs: degenerate=%llu negative=%llu out-of-range=%llu "
            "lut-boundary=%llu typical=%llu\n"
            "  curves: parametric=%llu table8=%llu table16=%llu\n"
            "  deterministic digest: 0x%016llx\n",
            (unsigned long long)first.groups,
            (unsigned long long)first.groups * 4,
            (unsigned long long)first.degenerate,
            (unsigned long long)first.negative,
            (unsigned long long)first.out_of_range,
            (unsigned long long)first.lut_boundary,
            (unsigned long long)first.typical,
            (unsigned long long)first.parametric,
            (unsigned long long)first.table8,
            (unsigned long long)first.table16,
            (unsigned long long)first.digest);
    fflush(stderr);
}
