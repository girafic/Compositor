// Property test for region.c against a brute-force bool[G][G] bitmap.
//
// Everything the header promises is checked against the bitmap: the four set operations,
// bounds, contains, is_empty, the accessors, copy, and intersect_rect. On top of that,
// every region that leaves the engine is run through a canonical-form checker, because the
// canonical form is what makes `bounds` exact and what makes two regions covering the same
// pixels compare equal rect-for-rect.
//
// Coordinates are kept in [0, G) so the bitmap can be indexed directly; a separate pass
// uses negative coordinates to prove nothing in the engine assumes non-negative input.

#include "raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G 32

static int failures = 0;
static int checks = 0;

static void fail(const char *what, unsigned seed) {
    fprintf(stderr, "FAIL [seed %u]: %s\n", seed, what);
    ++failures;
}

// MARK: - Bitmap oracle

typedef struct { bool on[G][G]; } bitmap;  // on[y][x]

static void bitmap_clear(bitmap *m) { memset(m, 0, sizeof(*m)); }

static void bitmap_add_rect(bitmap *m, raster_rect r) {
    for (int32_t y = r.y0; y < r.y1; ++y)
        for (int32_t x = r.x0; x < r.x1; ++x)
            if (x >= 0 && x < G && y >= 0 && y < G) m->on[y][x] = true;
}

static void bitmap_xor_rect(bitmap *m, raster_rect r) {
    for (int32_t y = r.y0; y < r.y1; ++y)
        for (int32_t x = r.x0; x < r.x1; ++x)
            if (x >= 0 && x < G && y >= 0 && y < G) m->on[y][x] = !m->on[y][x];
}

typedef enum { OP_UNION, OP_INTERSECT, OP_SUBTRACT, OP_XOR } op;

static void bitmap_combine(bitmap *out, const bitmap *a, const bitmap *b, op o) {
    for (int y = 0; y < G; ++y)
        for (int x = 0; x < G; ++x) {
            bool ia = a->on[y][x], ib = b->on[y][x];
            switch (o) {
            case OP_UNION: out->on[y][x] = ia || ib; break;
            case OP_INTERSECT: out->on[y][x] = ia && ib; break;
            case OP_SUBTRACT: out->on[y][x] = ia && !ib; break;
            case OP_XOR: out->on[y][x] = ia != ib; break;
            }
        }
}

// MARK: - Checks

// Renders the region into a bitmap by walking its rectangles, so a mismatch is a
// disagreement about *pixels* and not about how the pixels are chopped into rectangles.
static void render(const raster_region *region, bitmap *out) {
    bitmap_clear(out);
    size_t n = raster_region_count(region);
    for (size_t i = 0; i < n; ++i) bitmap_add_rect(out, raster_region_rect(region, i));
}

static bool bitmaps_equal(const bitmap *a, const bitmap *b) {
    return memcmp(a, b, sizeof(bitmap)) == 0;
}

static void dump(const char *label, const bitmap *m) {
    fprintf(stderr, "  %s:\n", label);
    for (int y = 0; y < G; ++y) {
        fprintf(stderr, "   ");
        for (int x = 0; x < G; ++x) fputc(m->on[y][x] ? '#' : '.', stderr);
        fputc('\n', stderr);
    }
}

// The canonical form from raster.h, checked literally:
//   - no empty rectangles;
//   - sorted by y0 then x0;
//   - rectangles sharing a y0 share a y1 and do not touch or overlap in x;
//   - two vertically adjacent bands never have identical x-interval lists.
static void check_canonical(const raster_region *region, const char *what, unsigned seed) {
    ++checks;
    size_t n = raster_region_count(region);
    size_t i = 0;
    size_t previousStart = 0, previousCount = 0;
    bool havePrevious = false;

    while (i < n) {
        raster_rect first = raster_region_rect(region, i);
        if (raster_rect_is_empty(first)) {
            fprintf(stderr, "  empty rect at %zu\n", i);
            fail(what, seed);
            return;
        }
        // Gather the band sharing this y0.
        size_t start = i, count = 0;
        int32_t previousX1 = 0;
        while (i < n) {
            raster_rect r = raster_region_rect(region, i);
            if (r.y0 != first.y0) break;
            if (r.y1 != first.y1) {
                fprintf(stderr, "  rect %zu shares y0=%d but not y1 (%d vs %d)\n",
                        i, r.y0, r.y1, first.y1);
                fail(what, seed);
                return;
            }
            if (count > 0 && r.x0 <= previousX1) {
                fprintf(stderr, "  rects %zu,%zu touch or overlap in x (%d <= %d)\n",
                        i - 1, i, r.x0, previousX1);
                fail(what, seed);
                return;
            }
            previousX1 = r.x1;
            ++count;
            ++i;
        }
        if (i < n) {
            raster_rect next = raster_region_rect(region, i);
            if (next.y0 < first.y1) {
                fprintf(stderr, "  band at y=%d overlaps the next band at y=%d\n",
                        first.y0, next.y0);
                fail(what, seed);
                return;
            }
        }
        // Adjacent identical bands must have been merged.
        if (havePrevious && previousCount == count) {
            raster_rect previousFirst = raster_region_rect(region, previousStart);
            if (previousFirst.y1 == first.y0) {
                bool identical = true;
                for (size_t k = 0; k < count; ++k) {
                    raster_rect p = raster_region_rect(region, previousStart + k);
                    raster_rect c = raster_region_rect(region, start + k);
                    if (p.x0 != c.x0 || p.x1 != c.x1) { identical = false; break; }
                }
                if (identical) {
                    fprintf(stderr, "  bands at y=%d and y=%d are identical and adjacent\n",
                            previousFirst.y0, first.y0);
                    fail(what, seed);
                    return;
                }
            }
        }
        previousStart = start;
        previousCount = count;
        havePrevious = true;
    }
}

// bounds, contains and is_empty against the bitmap.
static void check_queries(const raster_region *region, const bitmap *m,
                          const char *what, unsigned seed) {
    ++checks;
    bool anyPixel = false;
    raster_rect want = { 0, 0, 0, 0 };
    bool haveWant = false;
    for (int y = 0; y < G; ++y)
        for (int x = 0; x < G; ++x)
            if (m->on[y][x]) {
                anyPixel = true;
                if (!haveWant) { want = (raster_rect){ x, y, x + 1, y + 1 }; haveWant = true; }
                else {
                    if (x < want.x0) want.x0 = x;
                    if (y < want.y0) want.y0 = y;
                    if (x + 1 > want.x1) want.x1 = x + 1;
                    if (y + 1 > want.y1) want.y1 = y + 1;
                }
            }

    if (raster_region_is_empty(region) != !anyPixel) {
        fprintf(stderr, "  is_empty says %d, bitmap says %d\n",
                raster_region_is_empty(region), !anyPixel);
        fail(what, seed);
    }

    raster_rect got = raster_region_bounds(region);
    if (got.x0 != want.x0 || got.y0 != want.y0 || got.x1 != want.x1 || got.y1 != want.y1) {
        fprintf(stderr, "  bounds (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
                got.x0, got.y0, got.x1, got.y1, want.x0, want.y0, want.x1, want.y1);
        fail(what, seed);
    }

    // `contains` is probed one pixel outside the grid on every side too, so an
    // out-of-range coordinate has to answer false rather than run off the rect list.
    for (int y = -1; y <= G; ++y)
        for (int x = -1; x <= G; ++x) {
            bool inside = x >= 0 && x < G && y >= 0 && y < G && m->on[y][x];
            if (raster_region_contains(region, x, y) != inside) {
                fprintf(stderr, "  contains(%d,%d) says %d, bitmap says %d\n",
                        x, y, raster_region_contains(region, x, y), inside);
                fail(what, seed);
                return;
            }
        }
}

static void check_pixels(const raster_region *region, const bitmap *want,
                         const char *what, unsigned seed) {
    ++checks;
    bitmap got;
    render(region, &got);
    if (!bitmaps_equal(&got, want)) {
        fail(what, seed);
        dump("got", &got);
        dump("want", want);
    }
}

static void check_all(const raster_region *region, const bitmap *want,
                      const char *what, unsigned seed) {
    check_pixels(region, want, what, seed);
    check_canonical(region, what, seed);
    check_queries(region, want, what, seed);
}

// MARK: - Random input

static unsigned rngState = 1;
static unsigned next_random(void) {
    rngState = rngState * 1664525u + 1013904223u;
    return rngState >> 8;
}

static raster_rect random_rect(void) {
    int32_t x0 = (int32_t)(next_random() % G);
    int32_t y0 = (int32_t)(next_random() % G);
    // Widths lean small: many small rectangles produce far more bands, and bands are where
    // the merging logic lives. A handful of wide ones still show up.
    int32_t w = (int32_t)(next_random() % 9) + 1;
    int32_t h = (int32_t)(next_random() % 9) + 1;
    if (next_random() % 8 == 0) { w = (int32_t)(next_random() % G) + 1; }
    if (next_random() % 8 == 0) { h = (int32_t)(next_random() % G) + 1; }
    int32_t x1 = x0 + w, y1 = y0 + h;
    if (x1 > G) x1 = G;
    if (y1 > G) y1 = G;
    return (raster_rect){ x0, y0, x1, y1 };
}

// Builds a region as the union of `n` random rectangles, with the matching bitmap.
// The union path is therefore exercised on every single construction.
static raster_region *random_region(int n, bitmap *m, raster_rect *rects) {
    bitmap_clear(m);
    raster_region *region = raster_region_create();
    for (int i = 0; i < n; ++i) {
        raster_rect r = random_rect();
        if (rects) rects[i] = r;
        bitmap_add_rect(m, r);
        raster_region *piece = raster_region_create_rect(r);
        raster_region *merged = raster_region_union(region, piece);
        raster_region_destroy(piece);
        raster_region_destroy(region);
        region = merged;
    }
    return region;
}

// MARK: - Cases

static void test_empty_and_degenerate(void) {
    bitmap m;
    bitmap_clear(&m);

    raster_region *empty = raster_region_create();
    check_all(empty, &m, "empty region", 0);
    if (raster_region_count(empty) != 0) fail("empty count", 0);

    // An empty rectangle, in each of the three ways it can be empty.
    raster_rect degenerate[] = {
        { 5, 5, 5, 10 }, { 5, 5, 10, 5 }, { 10, 10, 5, 5 }, { 0, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(degenerate) / sizeof(degenerate[0]); ++i) {
        if (!raster_rect_is_empty(degenerate[i])) fail("rect_is_empty", (unsigned)i);
        raster_region *r = raster_region_create_rect(degenerate[i]);
        check_all(r, &m, "region from empty rect", (unsigned)i);
        raster_region_destroy(r);
    }

    // Out-of-range index and NULL are defined, not crashes.
    raster_rect zero = raster_region_rect(empty, 0);
    if (!raster_rect_is_empty(zero)) fail("rect(out of range)", 0);
    if (!raster_region_is_empty(NULL)) fail("is_empty(NULL)", 0);
    if (raster_region_count(NULL) != 0) fail("count(NULL)", 0);
    if (raster_region_contains(NULL, 0, 0)) fail("contains(NULL)", 0);
    raster_rect nullBounds = raster_region_bounds(NULL);
    if (!raster_rect_is_empty(nullBounds)) fail("bounds(NULL)", 0);
    raster_region_destroy(NULL);

    // NULL as an operand behaves as the empty region.
    raster_region *one = raster_region_create_rect((raster_rect){ 2, 3, 8, 9 });
    struct { const char *name; raster_region *(*fn)(const raster_region *, const raster_region *); bool keepsA; } ops[] = {
        { "union", raster_region_union, true },
        { "intersect", raster_region_intersect, false },
        { "subtract", raster_region_subtract, true },
        { "xor", raster_region_xor, true },
    };
    bitmap full;
    bitmap_clear(&full);
    bitmap_add_rect(&full, (raster_rect){ 2, 3, 8, 9 });
    for (size_t i = 0; i < 4; ++i) {
        raster_region *r = ops[i].fn(one, NULL);
        check_all(r, ops[i].keepsA ? &full : &m, "op with NULL b", (unsigned)i);
        raster_region_destroy(r);

        raster_region *l = ops[i].fn(NULL, one);
        // a=NULL: union and xor keep b, intersect and subtract are empty.
        bool keepsB = (i == 0 || i == 3);
        check_all(l, keepsB ? &full : &m, "op with NULL a", (unsigned)i);
        raster_region_destroy(l);
    }
    raster_region_destroy(one);
    raster_region_destroy(empty);
}

static void test_single_rect(void) {
    for (unsigned seed = 0; seed < 200; ++seed) {
        rngState = seed + 1;
        raster_rect r = random_rect();
        if (raster_rect_is_empty(r)) continue;
        bitmap m;
        bitmap_clear(&m);
        bitmap_add_rect(&m, r);
        raster_region *region = raster_region_create_rect(r);
        check_all(region, &m, "single rect", seed);
        if (raster_region_count(region) != 1) fail("single rect count != 1", seed);

        raster_region *copy = raster_region_copy(region);
        check_all(copy, &m, "copy of single rect", seed);
        raster_region_destroy(copy);
        raster_region_destroy(region);
    }
}

static void test_operations(int rectCount, int iterations) {
    for (unsigned seed = 1; seed <= (unsigned)iterations; ++seed) {
        rngState = seed * 2654435761u + rectCount;
        bitmap ma, mb;
        raster_region *a = random_region(rectCount, &ma, NULL);
        raster_region *b = random_region(rectCount, &mb, NULL);

        check_all(a, &ma, "built region a", seed);
        check_all(b, &mb, "built region b", seed);

        struct { const char *name; raster_region *(*fn)(const raster_region *, const raster_region *); op o; } ops[] = {
            { "union", raster_region_union, OP_UNION },
            { "intersect", raster_region_intersect, OP_INTERSECT },
            { "subtract", raster_region_subtract, OP_SUBTRACT },
            { "xor", raster_region_xor, OP_XOR },
        };
        for (size_t i = 0; i < 4; ++i) {
            bitmap want;
            bitmap_combine(&want, &ma, &mb, ops[i].o);
            raster_region *r = ops[i].fn(a, b);
            check_all(r, &want, ops[i].name, seed);
            raster_region_destroy(r);
        }

        // Identity and annihilation laws, which catch aliasing between the two operands.
        raster_region *self = raster_region_intersect(a, a);
        check_all(self, &ma, "a intersect a", seed);
        raster_region_destroy(self);

        raster_region *selfUnion = raster_region_union(a, a);
        check_all(selfUnion, &ma, "a union a", seed);
        raster_region_destroy(selfUnion);

        bitmap none;
        bitmap_clear(&none);
        raster_region *selfDiff = raster_region_subtract(a, a);
        check_all(selfDiff, &none, "a subtract a", seed);
        raster_region_destroy(selfDiff);

        raster_region *selfXor = raster_region_xor(a, a);
        check_all(selfXor, &none, "a xor a", seed);
        raster_region_destroy(selfXor);

        // intersect_rect must agree with intersect against a one-rect region.
        raster_rect clip = random_rect();
        bitmap mc;
        bitmap_clear(&mc);
        bitmap_add_rect(&mc, clip);
        bitmap wantClip;
        bitmap_combine(&wantClip, &ma, &mc, OP_INTERSECT);
        raster_region *clipped = raster_region_intersect_rect(a, clip);
        check_all(clipped, &wantClip, "intersect_rect", seed);
        raster_region_destroy(clipped);

        raster_region_destroy(a);
        raster_region_destroy(b);
    }
}

// Canonical form has to mean *one* representation per pixel set. Building the same set of
// rectangles in a different order must produce a byte-identical rectangle list, or the
// clip stack cannot compare two clips for equality.
static void test_order_independence(void) {
    for (unsigned seed = 1; seed <= 400; ++seed) {
        rngState = seed * 40503u + 7;
        enum { N = 7 };
        raster_rect rects[N];
        bitmap m;
        raster_region *forward = random_region(N, &m, rects);

        raster_region *backward = raster_region_create();
        for (int i = N - 1; i >= 0; --i) {
            raster_region *piece = raster_region_create_rect(rects[i]);
            raster_region *merged = raster_region_union(backward, piece);
            raster_region_destroy(piece);
            raster_region_destroy(backward);
            backward = merged;
        }

        ++checks;
        size_t n = raster_region_count(forward);
        if (raster_region_count(backward) != n) {
            fprintf(stderr, "  %zu rects forward, %zu backward\n",
                    n, raster_region_count(backward));
            fail("order independence: count", seed);
        } else {
            for (size_t i = 0; i < n; ++i) {
                raster_rect f = raster_region_rect(forward, i);
                raster_rect b = raster_region_rect(backward, i);
                if (f.x0 != b.x0 || f.y0 != b.y0 || f.x1 != b.x1 || f.y1 != b.y1) {
                    fprintf(stderr, "  rect %zu: (%d,%d,%d,%d) vs (%d,%d,%d,%d)\n",
                            i, f.x0, f.y0, f.x1, f.y1, b.x0, b.y0, b.x1, b.y1);
                    fail("order independence: rects", seed);
                    break;
                }
            }
        }
        check_all(backward, &m, "backward build", seed);
        raster_region_destroy(forward);
        raster_region_destroy(backward);
    }
}

// The even-odd fill rule: an overlap of two added rectangles is *out*. This is what
// LayerRenderer's brush-preview clip produces, and it is the reason `xor` exists.
static void test_even_odd(void) {
    for (unsigned seed = 1; seed <= 400; ++seed) {
        rngState = seed * 2246822519u + 11;
        enum { N = 5 };
        bitmap m;
        bitmap_clear(&m);
        raster_region *region = raster_region_create();
        for (int i = 0; i < N; ++i) {
            raster_rect r = random_rect();
            bitmap_xor_rect(&m, r);
            raster_region *piece = raster_region_create_rect(r);
            raster_region *merged = raster_region_xor(region, piece);
            raster_region_destroy(piece);
            raster_region_destroy(region);
            region = merged;
        }
        check_all(region, &m, "even-odd accumulation", seed);
        raster_region_destroy(region);
    }
}

// Nothing in the engine may assume non-negative coordinates: a clip is device-space, and
// TiledLayerRenderer happily asks for tiles left of and above the origin.
static void test_negative_coordinates(void) {
    const int32_t shift = -17;
    for (unsigned seed = 1; seed <= 300; ++seed) {
        rngState = seed * 3266489917u + 23;
        enum { N = 5 };
        raster_rect rects[N];
        bitmap m;
        raster_region *positive = random_region(N, &m, rects);

        raster_region *shifted = raster_region_create();
        for (int i = 0; i < N; ++i) {
            raster_rect r = rects[i];
            r.x0 += shift; r.x1 += shift; r.y0 += shift; r.y1 += shift;
            raster_region *piece = raster_region_create_rect(r);
            raster_region *merged = raster_region_union(shifted, piece);
            raster_region_destroy(piece);
            raster_region_destroy(shifted);
            shifted = merged;
        }

        // Translating every input by a constant must translate the output by the same
        // constant and change nothing else -- same rect count, same shape.
        ++checks;
        size_t n = raster_region_count(positive);
        if (raster_region_count(shifted) != n) {
            fail("negative coords: rect count changed", seed);
        } else {
            for (size_t i = 0; i < n; ++i) {
                raster_rect p = raster_region_rect(positive, i);
                raster_rect s = raster_region_rect(shifted, i);
                if (s.x0 != p.x0 + shift || s.x1 != p.x1 + shift ||
                    s.y0 != p.y0 + shift || s.y1 != p.y1 + shift) {
                    fprintf(stderr, "  rect %zu: (%d,%d,%d,%d) shifted to (%d,%d,%d,%d)\n",
                            i, p.x0, p.y0, p.x1, p.y1, s.x0, s.y0, s.x1, s.y1);
                    fail("negative coords: wrong offset", seed);
                    break;
                }
            }
        }
        check_canonical(shifted, "negative coords canonical", seed);

        raster_rect bounds = raster_region_bounds(shifted);
        raster_rect want = raster_region_bounds(positive);
        if (!raster_region_is_empty(positive)) {
            want.x0 += shift; want.x1 += shift; want.y0 += shift; want.y1 += shift;
            if (memcmp(&bounds, &want, sizeof(raster_rect)) != 0)
                fail("negative coords: bounds", seed);
        }
        if (!raster_region_contains(shifted, want.x0, want.y0) &&
            !raster_region_is_empty(shifted)) {
            // The bounds corner need not be covered; just prove contains works down there
            // by probing every pixel of the shifted bounding box against `positive`.
            for (int32_t y = want.y0; y < want.y1; ++y)
                for (int32_t x = want.x0; x < want.x1; ++x)
                    if (raster_region_contains(shifted, x, y) !=
                        raster_region_contains(positive, x - shift, y - shift)) {
                        fail("negative coords: contains", seed);
                        y = want.y1;
                        break;
                    }
        }

        raster_region_destroy(positive);
        raster_region_destroy(shifted);
    }
}

// Bands that the merge step has to collapse, written out by hand so a regression in
// merge_with_previous_band shows up as a named failure rather than a rare seed.
static void test_band_merging(void) {
    // Two rectangles stacked exactly: one rectangle out.
    raster_region *top = raster_region_create_rect((raster_rect){ 4, 0, 10, 5 });
    raster_region *bottom = raster_region_create_rect((raster_rect){ 4, 5, 10, 9 });
    raster_region *stacked = raster_region_union(top, bottom);
    if (raster_region_count(stacked) != 1) {
        fprintf(stderr, "  stacked union has %zu rects\n", raster_region_count(stacked));
        fail("stacked union should merge to one rect", 0);
    }
    raster_rect r = raster_region_rect(stacked, 0);
    if (r.x0 != 4 || r.y0 != 0 || r.x1 != 10 || r.y1 != 9) fail("stacked union geometry", 0);
    raster_region_destroy(top);
    raster_region_destroy(bottom);
    raster_region_destroy(stacked);

    // Side by side and touching: one rectangle out.
    raster_region *left = raster_region_create_rect((raster_rect){ 0, 0, 5, 5 });
    raster_region *right = raster_region_create_rect((raster_rect){ 5, 0, 11, 5 });
    raster_region *side = raster_region_union(left, right);
    if (raster_region_count(side) != 1) {
        fprintf(stderr, "  side-by-side union has %zu rects\n", raster_region_count(side));
        fail("touching union should merge to one rect", 0);
    }
    raster_region_destroy(left);
    raster_region_destroy(right);
    raster_region_destroy(side);

    // Three identical bands in a row, fed in as three separate stacked rectangles.
    raster_region *acc = raster_region_create();
    for (int i = 0; i < 3; ++i) {
        raster_region *piece = raster_region_create_rect((raster_rect){ 2, i * 4, 7, i * 4 + 4 });
        raster_region *merged = raster_region_union(acc, piece);
        raster_region_destroy(piece);
        raster_region_destroy(acc);
        acc = merged;
    }
    if (raster_region_count(acc) != 1) {
        fprintf(stderr, "  three stacked rects gave %zu rects\n", raster_region_count(acc));
        fail("three stacked rects should merge", 0);
    }
    raster_region_destroy(acc);

    // A plus sign: the classic three-band shape. Must be exactly three rectangles.
    raster_region *vertical = raster_region_create_rect((raster_rect){ 4, 0, 7, 12 });
    raster_region *horizontal = raster_region_create_rect((raster_rect){ 0, 4, 12, 7 });
    raster_region *plus = raster_region_union(vertical, horizontal);
    if (raster_region_count(plus) != 3) {
        fprintf(stderr, "  plus has %zu rects\n", raster_region_count(plus));
        fail("plus should be three bands", 0);
    }
    raster_rect bounds = raster_region_bounds(plus);
    if (bounds.x0 != 0 || bounds.y0 != 0 || bounds.x1 != 12 || bounds.y1 != 12)
        fail("plus bounds", 0);
    // The corners are outside the plus -- this is the case a conservative bounds would
    // get right and a wrong `contains` would not.
    if (raster_region_contains(plus, 0, 0)) fail("plus contains corner", 0);
    if (!raster_region_contains(plus, 5, 0)) fail("plus missing arm", 0);
    if (!raster_region_contains(plus, 0, 5)) fail("plus missing arm", 0);

    // Punching the centre out leaves four arms.
    raster_region *centre = raster_region_create_rect((raster_rect){ 4, 4, 7, 7 });
    raster_region *arms = raster_region_subtract(plus, centre);
    if (raster_region_count(arms) != 4) {
        fprintf(stderr, "  arms has %zu rects\n", raster_region_count(arms));
        fail("plus minus centre should be four rects", 0);
    }
    if (raster_region_contains(arms, 5, 5)) fail("arms still contain centre", 0);
    bitmap m;
    bitmap_clear(&m);
    bitmap_add_rect(&m, (raster_rect){ 4, 0, 7, 12 });
    bitmap_add_rect(&m, (raster_rect){ 0, 4, 12, 7 });
    for (int32_t y = 4; y < 7; ++y)
        for (int32_t x = 4; x < 7; ++x) m.on[y][x] = false;
    check_all(arms, &m, "plus minus centre", 0);

    raster_region_destroy(vertical);
    raster_region_destroy(horizontal);
    raster_region_destroy(plus);
    raster_region_destroy(centre);
    raster_region_destroy(arms);

    // A hole: subtracting an interior rectangle gives the four-band frame.
    raster_region *outer = raster_region_create_rect((raster_rect){ 0, 0, 20, 20 });
    raster_region *inner = raster_region_create_rect((raster_rect){ 5, 5, 15, 15 });
    raster_region *frame = raster_region_subtract(outer, inner);
    if (raster_region_count(frame) != 4) {
        fprintf(stderr, "  frame has %zu rects\n", raster_region_count(frame));
        fail("frame should be four rects", 0);
    }
    raster_rect frameBounds = raster_region_bounds(frame);
    if (frameBounds.x0 != 0 || frameBounds.y0 != 0 ||
        frameBounds.x1 != 20 || frameBounds.y1 != 20)
        fail("frame bounds should stay exact", 0);
    // Filling the hole back in must return the original single rectangle.
    raster_region *refilled = raster_region_union(frame, inner);
    if (raster_region_count(refilled) != 1) {
        fprintf(stderr, "  refilled has %zu rects\n", raster_region_count(refilled));
        fail("frame plus hole should collapse to one rect", 0);
    }
    raster_region_destroy(outer);
    raster_region_destroy(inner);
    raster_region_destroy(frame);
    raster_region_destroy(refilled);
}

// The shape the plan actually cares about: a painted layer's patch list under the 256-pixel
// tile grid, clipped by a rectangle. Runs at real scale rather than 32x32.
static void test_patch_list_scale(void) {
    raster_region *patches = raster_region_create();
    rngState = 99;
    // 16x16 tiles of 256 px, about a third of them painted -- what a half-covered 4000px
    // canvas looks like to the renderer.
    for (int ty = 0; ty < 16; ++ty)
        for (int tx = 0; tx < 16; ++tx) {
            if (next_random() % 3 != 0) continue;
            raster_rect tile = { tx * 256, ty * 256, tx * 256 + 256, ty * 256 + 256 };
            raster_region *piece = raster_region_create_rect(tile);
            raster_region *merged = raster_region_union(patches, piece);
            raster_region_destroy(piece);
            raster_region_destroy(patches);
            patches = merged;
        }
    check_canonical(patches, "patch list canonical", 0);

    raster_rect view = { 300, 700, 2500, 3100 };
    raster_region *clipped = raster_region_intersect_rect(patches, view);
    check_canonical(clipped, "clipped patch list canonical", 0);
    raster_rect bounds = raster_region_bounds(clipped);
    if (bounds.x0 < view.x0 || bounds.y0 < view.y0 ||
        bounds.x1 > view.x1 || bounds.y1 > view.y1)
        fail("clipped bounds escaped the clip rect", 0);

    // Clipping twice by the same rectangle is idempotent.
    raster_region *again = raster_region_intersect_rect(clipped, view);
    ++checks;
    if (raster_region_count(again) != raster_region_count(clipped)) {
        fail("intersect_rect is not idempotent", 0);
    } else {
        size_t n = raster_region_count(again);
        for (size_t i = 0; i < n; ++i) {
            raster_rect x = raster_region_rect(again, i);
            raster_rect y = raster_region_rect(clipped, i);
            if (memcmp(&x, &y, sizeof(raster_rect)) != 0) {
                fail("intersect_rect is not idempotent", 0);
                break;
            }
        }
    }
    raster_region_destroy(again);
    raster_region_destroy(clipped);
    raster_region_destroy(patches);
    fprintf(stderr, "  (patch list scale case ran)\n");
}

int main(void) {
    test_empty_and_degenerate();
    test_single_rect();
    test_band_merging();
    test_operations(1, 300);
    test_operations(2, 300);
    test_operations(3, 400);
    test_operations(5, 400);
    test_operations(9, 200);
    test_operations(16, 80);
    test_order_independence();
    test_even_odd();
    test_negative_coordinates();
    test_patch_list_scale();

    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
