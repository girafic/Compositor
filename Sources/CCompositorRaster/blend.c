#include "raster.h"

#include <math.h>

// Blending, per PDF 32000-1 section 11.3.5 — the same specification CoreGraphics,
// CoreImage and the W3C compositing spec all implement.
//
// One deliberate difference from CoreGraphics: its Color Dodge and Color Burn ignore the
// source's alpha, so a soft brush in those modes comes out with a hard edge. The macOS app
// already routes around that through CoreImage (see Attic/appkit/SeparableBlend.swift,
// whose comment says "Core Graphics gets these two wrong"). Implementing them to spec here
// means that detour can be deleted rather than reproduced.

// MARK: - Separable blend functions, on unpremultiplied components in 0...1

static float blend_multiply(float b, float s) { return b * s; }
static float blend_screen(float b, float s) { return b + s - b * s; }

static float blend_hard_light(float b, float s) {
    return s <= 0.5f ? blend_multiply(b, 2.0f * s) : blend_screen(b, 2.0f * s - 1.0f);
}

static float blend_soft_light(float b, float s) {
    if (s <= 0.5f) return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : sqrtf(b);
    return b + (2.0f * s - 1.0f) * (d - b);
}

static float blend_color_dodge(float b, float s) {
    if (b <= 0.0f) return 0.0f;
    if (s >= 1.0f) return 1.0f;
    float value = b / (1.0f - s);
    return value > 1.0f ? 1.0f : value;
}

static float blend_color_burn(float b, float s) {
    if (b >= 1.0f) return 1.0f;
    if (s <= 0.0f) return 0.0f;
    float value = (1.0f - b) / s;
    return 1.0f - (value > 1.0f ? 1.0f : value);
}

static float blend_separable(raster_blend mode, float b, float s) {
    switch (mode) {
    case RASTER_BLEND_MULTIPLY: return blend_multiply(b, s);
    case RASTER_BLEND_SCREEN: return blend_screen(b, s);
    case RASTER_BLEND_OVERLAY: return blend_hard_light(s, b);  // HardLight with the arguments swapped
    case RASTER_BLEND_DARKEN: return b < s ? b : s;
    case RASTER_BLEND_LIGHTEN: return b > s ? b : s;
    case RASTER_BLEND_COLOR_DODGE: return blend_color_dodge(b, s);
    case RASTER_BLEND_COLOR_BURN: return blend_color_burn(b, s);
    case RASTER_BLEND_SOFT_LIGHT: return blend_soft_light(b, s);
    case RASTER_BLEND_HARD_LIGHT: return blend_hard_light(b, s);
    case RASTER_BLEND_DIFFERENCE: return fabsf(b - s);
    case RASTER_BLEND_EXCLUSION: return b + s - 2.0f * b * s;
    default: return s;  // Normal, and the Porter-Duff modes handled before we get here
    }
}

// MARK: - Non-separable blend functions (Hue, Saturation, Color, Luminosity)

typedef struct { float r, g, b; } rgb;

static float lum(rgb c) { return 0.30f * c.r + 0.59f * c.g + 0.11f * c.b; }

static rgb clip_color(rgb c) {
    float l = lum(c);
    float n = fminf(c.r, fminf(c.g, c.b));
    float x = fmaxf(c.r, fmaxf(c.g, c.b));
    if (n < 0.0f && l != n) {
        float k = l / (l - n);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    if (x > 1.0f && x != l) {
        float k = (1.0f - l) / (x - l);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    return c;
}

static rgb set_lum(rgb c, float l) {
    float d = l - lum(c);
    c.r += d;
    c.g += d;
    c.b += d;
    return clip_color(c);
}

static float sat(rgb c) {
    return fmaxf(c.r, fmaxf(c.g, c.b)) - fminf(c.r, fminf(c.g, c.b));
}

// The spec's SetSat: stretch the middle component between 0 and s, zero the others.
static rgb set_sat(rgb c, float s) {
    float *components[3] = { &c.r, &c.g, &c.b };
    // Sort pointers so mn/md/mx address the smallest, middle and largest components.
    for (int i = 0; i < 2; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (*components[j] < *components[i]) {
                float *swap = components[i];
                components[i] = components[j];
                components[j] = swap;
            }
    float *mn = components[0], *md = components[1], *mx = components[2];
    if (*mx > *mn) {
        *md = (*md - *mn) * s / (*mx - *mn);
        *mx = s;
    } else {
        *md = *mx = 0.0f;
    }
    *mn = 0.0f;
    return c;
}

static rgb blend_nonseparable(raster_blend mode, rgb b, rgb s) {
    switch (mode) {
    case RASTER_BLEND_HUE: return set_lum(set_sat(s, sat(b)), lum(b));
    case RASTER_BLEND_SATURATION: return set_lum(set_sat(b, sat(s)), lum(b));
    case RASTER_BLEND_COLOR: return set_lum(s, lum(b));
    case RASTER_BLEND_LUMINOSITY: return set_lum(b, lum(s));
    default: return s;
    }
}

static bool is_nonseparable(raster_blend mode) {
    return mode >= RASTER_BLEND_HUE && mode <= RASTER_BLEND_LUMINOSITY;
}

bool raster_blend_supported(raster_blend mode) {
    switch (mode) {
    case RASTER_BLEND_NORMAL:
    case RASTER_BLEND_MULTIPLY:
    case RASTER_BLEND_SCREEN:
    case RASTER_BLEND_OVERLAY:
    case RASTER_BLEND_DARKEN:
    case RASTER_BLEND_LIGHTEN:
    case RASTER_BLEND_COLOR_DODGE:
    case RASTER_BLEND_COLOR_BURN:
    case RASTER_BLEND_SOFT_LIGHT:
    case RASTER_BLEND_HARD_LIGHT:
    case RASTER_BLEND_DIFFERENCE:
    case RASTER_BLEND_EXCLUSION:
    case RASTER_BLEND_HUE:
    case RASTER_BLEND_SATURATION:
    case RASTER_BLEND_COLOR:
    case RASTER_BLEND_LUMINOSITY:
    case RASTER_BLEND_CLEAR:
    case RASTER_BLEND_COPY:
    case RASTER_BLEND_DESTINATION_OUT:
        return true;
    default:
        return false;
    }
}

// MARK: - Compositing

static inline uint8_t to_byte(float value) {
    if (!(value > 0.0f)) return 0;  // also catches NaN
    if (value >= 1.0f) return 255;
    return (uint8_t)lrintf(value * 255.0f);
}

void raster_blend_rgba(uint8_t *dst, const uint8_t src[4], raster_blend mode,
                       uint8_t alpha, uint8_t coverage) {
    // The graphics state's alpha and the clip's coverage both scale the source, exactly as
    // setAlpha and a mask clip do. A premultiplied pixel scales whole.
    float k = (alpha / 255.0f) * (coverage / 255.0f);
    if (k <= 0.0f && mode != RASTER_BLEND_COPY && mode != RASTER_BLEND_CLEAR) return;

    float br = dst[0] / 255.0f, bg = dst[1] / 255.0f, bb = dst[2] / 255.0f, ba = dst[3] / 255.0f;
    float sr = src[0] / 255.0f * k, sg = src[1] / 255.0f * k, sb = src[2] / 255.0f * k;
    float sa = src[3] / 255.0f * k;

    switch (mode) {
    case RASTER_BLEND_CLEAR: {
        float keep = 1.0f - k;
        dst[0] = to_byte(br * keep);
        dst[1] = to_byte(bg * keep);
        dst[2] = to_byte(bb * keep);
        dst[3] = to_byte(ba * keep);
        return;
    }
    case RASTER_BLEND_COPY: {
        // Not an unconditional store: CoreGraphics weights Copy by the clip, so a partially
        // covered pixel is a lerp. Clone Stamp draws through a soft coverage clip in this
        // mode and depends on it.
        float keep = 1.0f - k;
        dst[0] = to_byte(src[0] / 255.0f * k + br * keep);
        dst[1] = to_byte(src[1] / 255.0f * k + bg * keep);
        dst[2] = to_byte(src[2] / 255.0f * k + bb * keep);
        dst[3] = to_byte(src[3] / 255.0f * k + ba * keep);
        return;
    }
    case RASTER_BLEND_DESTINATION_OUT: {
        // The eraser. Premultiplied, so scaling alpha scales the colour with it.
        float keep = 1.0f - sa;
        dst[0] = to_byte(br * keep);
        dst[1] = to_byte(bg * keep);
        dst[2] = to_byte(bb * keep);
        dst[3] = to_byte(ba * keep);
        return;
    }
    default:
        break;
    }

    // co = (1 - as)*cb + (1 - ab)*cs + as*ab*B(Cb, Cs), on premultiplied values, with B
    // evaluated on unpremultiplied ones.
    float both = sa * ba;
    float mixed[3];
    if (both > 0.0f) {
        float ur = br / ba, ug = bg / ba, ub = bb / ba;
        float vr = sr / sa, vg = sg / sa, vb = sb / sa;
        if (is_nonseparable(mode)) {
            rgb blended = blend_nonseparable(mode, (rgb){ ur, ug, ub }, (rgb){ vr, vg, vb });
            mixed[0] = blended.r;
            mixed[1] = blended.g;
            mixed[2] = blended.b;
        } else {
            mixed[0] = blend_separable(mode, ur, vr);
            mixed[1] = blend_separable(mode, ug, vg);
            mixed[2] = blend_separable(mode, ub, vb);
        }
    } else {
        mixed[0] = mixed[1] = mixed[2] = 0.0f;
    }

    float keepSource = 1.0f - ba, keepBackdrop = 1.0f - sa;
    dst[0] = to_byte(keepBackdrop * br + keepSource * sr + both * mixed[0]);
    dst[1] = to_byte(keepBackdrop * bg + keepSource * sg + both * mixed[1]);
    dst[2] = to_byte(keepBackdrop * bb + keepSource * sb + both * mixed[2]);
    dst[3] = to_byte(sa + ba * keepBackdrop);
}

void raster_blend_gray(uint8_t *dst, uint8_t src, raster_blend mode,
                       uint8_t alpha, uint8_t coverage) {
    // A GRAY8 surface is opaque: there is no alpha to composite, so the result is the
    // backdrop moved towards the blended value by alpha x coverage.
    float k = (alpha / 255.0f) * (coverage / 255.0f);
    if (k <= 0.0f) return;

    float b = dst[0] / 255.0f, s = src / 255.0f;
    float value;
    switch (mode) {
    case RASTER_BLEND_CLEAR:
        value = 0.0f;  // clearing a gray plane writes black, not transparency
        break;
    case RASTER_BLEND_COPY:
    case RASTER_BLEND_NORMAL:
        value = s;
        break;
    case RASTER_BLEND_DESTINATION_OUT:
        value = 0.0f;
        k *= s;  // erase in proportion to the source
        break;
    // On a single channel the non-separable modes degenerate: saturation is always zero and
    // luminosity is the sample itself.
    case RASTER_BLEND_HUE:
    case RASTER_BLEND_SATURATION:
    case RASTER_BLEND_COLOR:
        value = b;
        break;
    case RASTER_BLEND_LUMINOSITY:
        value = s;
        break;
    default:
        value = blend_separable(mode, b, s);
        break;
    }
    dst[0] = to_byte(b + (value - b) * k);
}

// MARK: - Runs

void raster_blend_row_rgba(uint8_t *dst, const uint8_t *src, size_t count, raster_blend mode,
                           uint8_t alpha, const uint8_t *coverage) {
    for (size_t i = 0; i < count; ++i)
        raster_blend_rgba(dst + i * 4, src + i * 4, mode, alpha, coverage ? coverage[i] : 255);
}

void raster_blend_row_gray(uint8_t *dst, const uint8_t *src, size_t count, raster_blend mode,
                           uint8_t alpha, const uint8_t *coverage) {
    for (size_t i = 0; i < count; ++i)
        raster_blend_gray(dst + i, src[i], mode, alpha, coverage ? coverage[i] : 255);
}

void raster_fill_row_rgba(uint8_t *dst, const uint8_t src[4], size_t count, raster_blend mode,
                          uint8_t alpha, const uint8_t *coverage) {
    for (size_t i = 0; i < count; ++i)
        raster_blend_rgba(dst + i * 4, src, mode, alpha, coverage ? coverage[i] : 255);
}

void raster_fill_row_gray(uint8_t *dst, uint8_t src, size_t count, raster_blend mode,
                          uint8_t alpha, const uint8_t *coverage) {
    for (size_t i = 0; i < count; ++i)
        raster_blend_gray(dst + i, src, mode, alpha, coverage ? coverage[i] : 255);
}
