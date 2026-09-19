// Spike 1: the exact-2x halving invariant.
//
// DownsampleCache.halve() builds a chain of exactly-2x reductions with vImage Lanczos, and
// TiledLayerRenderer depends on the result being position independent: a piece of a layer, rendered
// on its own with a margin and reduced the same way, must come out bit-identical to the same region
// of the whole layer reduced. TiledLayerRenderer.support(level:) is the margin it budgets for that
// -- 8 grid pixels at level 0, 16 << level above it.
//
// The question this answers: does a Lanczos-3 exactly-2x kernel fit inside that budget? The macOS
// build got the answer from vImage; on Linux we pick the kernel, so we have to measure it.
//
// Geometry mirrors DownsampleCache.halve exactly: pad by 8 transparent pixels on every side, scale
// the padded buffer by exactly 2x, clamp premultiplied ringing, crop back by pad/2.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD 8

static double sinc(double x) {
    if (fabs(x) < 1e-12) return 1.0;
    double p = M_PI * x;
    return sin(p) / p;
}
// Lanczos-3, the window vImage's kvImageHighQualityResampling uses for reductions.
static double lanczos3(double t) {
    if (t < 0) t = -t;
    if (t >= 3.0) return 0.0;
    return sinc(t) * sinc(t / 3.0);
}

// One axis of an exactly-2x reduction. Output pixel j is centred on source coordinate 2j + 1, and
// the reconstruction filter is stretched by the 2x factor, so a tap at source pixel i (centre
// i + 0.5) sits at t = ((i + 0.5) - (2j + 1)) / 2. Taps outside the buffer are simply absent --
// the transparent padding is real buffer, which is the point of padding in the first place.
#define TAPS 12  // |t| < 3 over a 2x stretch reaches 6 source pixels each side

typedef struct { int first, count; double w[TAPS + 2]; } Tap;

static void build_taps(int outCount, int inCount, Tap *taps) {
    for (int j = 0; j < outCount; ++j) {
        double centre = 2.0 * j + 1.0;
        int lo = (int)floor(centre - 6.0 + 0.5);
        int hi = (int)ceil(centre + 6.0 - 0.5);
        if (lo < 0) lo = 0;
        if (hi > inCount - 1) hi = inCount - 1;
        Tap *t = &taps[j];
        t->first = lo;
        t->count = hi - lo + 1;
        if (t->count > TAPS + 2) t->count = TAPS + 2;
        double sum = 0;
        for (int k = 0; k < t->count; ++k) {
            double w = lanczos3(((lo + k + 0.5) - centre) / 2.0);
            t->w[k] = w;
            sum += w;
        }
        // Normalise so a flat field stays flat, including where the kernel is clipped at the buffer
        // edge. Without this, the outermost rows darken.
        if (sum != 0) for (int k = 0; k < t->count; ++k) t->w[k] /= sum;
    }
}

// Exactly-2x reduction of a premultiplied RGBA8 buffer. `channels` is 4 for colour, 1 for an A8 mask.
static void reduce2x(const uint8_t *src, int sw, int sh, int sstride,
                     uint8_t *dst, int dstride, int channels) {
    int dw = sw / 2, dh = sh / 2;
    Tap *tx = malloc(sizeof(Tap) * (size_t)dw);
    Tap *ty = malloc(sizeof(Tap) * (size_t)dh);
    build_taps(dw, sw, tx);
    build_taps(dh, sh, ty);
    // Horizontal pass into a float scratch, then vertical -- separable, like any real implementation.
    double *mid = malloc(sizeof(double) * (size_t)dw * (size_t)sh * (size_t)channels);
    for (int y = 0; y < sh; ++y) {
        const uint8_t *row = src + (size_t)y * (size_t)sstride;
        for (int x = 0; x < dw; ++x) {
            const Tap *t = &tx[x];
            for (int c = 0; c < channels; ++c) {
                double sum = 0;
                for (int k = 0; k < t->count; ++k)
                    sum += t->w[k] * row[(size_t)(t->first + k) * (size_t)channels + (size_t)c];
                mid[((size_t)y * (size_t)dw + (size_t)x) * (size_t)channels + (size_t)c] = sum;
            }
        }
    }
    for (int y = 0; y < dh; ++y) {
        const Tap *t = &ty[y];
        uint8_t *out = dst + (size_t)y * (size_t)dstride;
        for (int x = 0; x < dw; ++x) {
            for (int c = 0; c < channels; ++c) {
                double sum = 0;
                for (int k = 0; k < t->count; ++k)
                    sum += t->w[k] * mid[((size_t)(t->first + k) * (size_t)dw + (size_t)x) * (size_t)channels + (size_t)c];
                double v = sum < 0 ? 0 : sum > 255 ? 255 : sum;
                out[(size_t)x * (size_t)channels + (size_t)c] = (uint8_t)lround(v);
            }
        }
    }
    free(mid); free(tx); free(ty);
}

static void clamp_premultiplied(uint8_t *rgba, size_t count) {
    for (size_t i = 0; i < count; ++i, rgba += 4) {
        uint8_t a = rgba[3];
        if (rgba[0] > a) rgba[0] = a;
        if (rgba[1] > a) rgba[1] = a;
        if (rgba[2] > a) rgba[2] = a;
    }
}

// One halving, geometry identical to DownsampleCache.halve for the colour case.
// Returns a newly allocated (w+1)/2 x (h+1)/2 buffer, tightly packed.
static uint8_t *halve(const uint8_t *src, int w, int h, int stride, int *outW, int *outH) {
    int dw = (w + 1) / 2, dh = (h + 1) / 2;
    int pw = dw * 2 + PAD * 2, ph = dh * 2 + PAD * 2;
    uint8_t *padded = calloc((size_t)pw * (size_t)ph * 4, 1);
    for (int y = 0; y < h; ++y)
        memcpy(padded + ((size_t)(y + PAD) * (size_t)pw + PAD) * 4,
               src + (size_t)y * (size_t)stride, (size_t)w * 4);

    int rw = pw / 2, rh = ph / 2;
    uint8_t *reduced = malloc((size_t)rw * (size_t)rh * 4);
    reduce2x(padded, pw, ph, pw * 4, reduced, rw * 4, 4);
    clamp_premultiplied(reduced, (size_t)rw * (size_t)rh);

    uint8_t *out = malloc((size_t)dw * (size_t)dh * 4);
    for (int y = 0; y < dh; ++y)
        memcpy(out + (size_t)y * (size_t)dw * 4,
               reduced + ((size_t)(y + PAD / 2) * (size_t)rw + PAD / 2) * 4, (size_t)dw * 4);
    free(padded); free(reduced);
    *outW = dw; *outH = dh;
    return out;
}

static uint8_t *halveTimes(const uint8_t *src, int w, int h, int stride, int levels, int *outW, int *outH) {
    int cw = w, ch = h, cstride = stride;
    uint8_t *current = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; ++y)
        memcpy(current + (size_t)y * (size_t)w * 4, src + (size_t)y * (size_t)stride, (size_t)w * 4);
    cstride = w * 4;
    for (int l = 0; l < levels; ++l) {
        int nw, nh;
        uint8_t *next = halve(current, cw, ch, cstride, &nw, &nh);
        free(current);
        current = next; cw = nw; ch = nh; cstride = nw * 4;
    }
    *outW = cw; *outH = ch;
    return current;
}

// TiledLayerRenderer.support(level:)
static int support(int level) { return level == 0 ? 8 : (16 << level); }

static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

int main(void) {
    // Big enough that the piece can grow a margin larger than any level needs without running off
    // the edge -- otherwise the harness, not the kernel, decides the answer.
    const int W = 3072, H = 3072;
    uint8_t *whole = malloc((size_t)W * (size_t)H * 4);
    uint32_t seed = 12345;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *p = whole + ((size_t)y * (size_t)W + (size_t)x) * 4;
            // High-frequency content plus varying alpha: the hardest case for a wide kernel, and
            // the one where premultiplied ringing actually shows up.
            uint32_t r = rnd(&seed);
            uint8_t a = (uint8_t)(128 + (r & 127));
            p[3] = a;
            p[0] = (uint8_t)(((r >> 8) & 255) * a / 255);
            p[1] = (uint8_t)(((r >> 16) & 255) * a / 255);
            p[2] = (uint8_t)(((r >> 24) & 255) * a / 255);
        }
    }

    printf("Lanczos-3 exactly-2x halving: margin needed for a piece to match the whole\n");
    printf("%-7s %-10s %-14s %-10s %s\n", "level", "support()", "margin needed", "headroom", "verdict");

    int worstOk = 1;
    for (int level = 1; level <= 6; ++level) {
        int lw, lh;
        uint8_t *reference = halveTimes(whole, W, H, W * 4, level, &lw, &lh);

        // An interior piece of the grid, aligned to 2^level so level-k pixels line up.
        int cell = 256;
        int px = 1280, py = 1280;             // piece origin in source pixels, multiple of 2^6
        int pwid = cell, phgt = cell;

        // A piece has to start on a level-k boundary, so the margin steps in units of 2^k.
        int step = 1 << level;
        int needed = -1;
        for (int m = 0; m <= 1280; m += step) {
            int rx = px - m, ry = py - m;
            int rw = pwid + 2 * m, rh = phgt + 2 * m;
            if (rx < 0 || ry < 0 || rx + rw > W || ry + rh > H) break;

            uint8_t *region = malloc((size_t)rw * (size_t)rh * 4);
            for (int y = 0; y < rh; ++y)
                memcpy(region + (size_t)y * (size_t)rw * 4,
                       whole + ((size_t)(ry + y) * (size_t)W + (size_t)rx) * 4, (size_t)rw * 4);

            int qw, qh;
            uint8_t *piece = halveTimes(region, rw, rh, rw * 4, level, &qw, &qh);

            // Compare the piece's interior against the same region of the whole.
            int ix = px >> level, iy = py >> level;
            int iw = pwid >> level, ih = phgt >> level;
            int ox = (rx >> level), oy = (ry >> level);
            int diff = 0;
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x)
                    for (int c = 0; c < 4; ++c) {
                        int a = reference[(((size_t)(iy + y) * (size_t)lw) + (size_t)(ix + x)) * 4 + (size_t)c];
                        int b = piece[(((size_t)(iy + y - oy) * (size_t)qw) + (size_t)(ix + x - ox)) * 4 + (size_t)c];
                        int d = abs(a - b);
                        if (d > diff) diff = d;
                    }
            free(region); free(piece);
            if (diff == 0) { needed = m; break; }
        }
        free(reference);

        int budget = support(level);
        int ok = needed >= 0 && needed <= budget;
        if (!ok) worstOk = 0;
        if (needed < 0) printf("%-7d %-10d %-14s %-10s %s\n", level, budget, ">1024", "-", "FAIL");
        else printf("%-7d %-10d %-14d %-9.1fx %s\n", level, budget, needed,
                    (double)budget / (needed ? needed : 1), ok ? "fits" : "EXCEEDS BUDGET");
    }
    free(whole);
    printf("\n%s\n", worstOk ? "RESULT: Lanczos-3 fits inside TiledLayerRenderer.support(level:) at every level."
                             : "RESULT: budget exceeded -- support(level:) would have to grow.");
    return 0;
}
