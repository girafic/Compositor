// Property test for coverage.c and context.c against brute-force oracles.
//
// Same shape as region-oracle.c: an independent implementation of what the engine claims,
// compared over a large random input space, with the degenerate cases written out by hand.
// This exists for what CI cannot do — valgrind, sanitizers, and a sweep across optimisation
// levels — because the Swift side is only checked by a remote runner.
//
// Stage 1 (this file so far) covers the device-pixel arithmetic, which is the most
// consequential code in the drawing path: get the edge rule wrong and every clip in the app
// is off by a pixel, in a way that only shows up as a hairline on translucent layers.

#include "raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "FAIL: %s\n      %s\n", what, detail ? detail : "");
    ++failures;
}

#define CHECK(cond, what, detail) do { ++checks; if (!(cond)) fail((what), (detail)); } while (0)

// MARK: - Random input

static unsigned rngState = 1;
static unsigned next_random(void) {
    rngState = rngState * 1664525u + 1013904223u;
    return rngState >> 8;
}

static double random_double(double lo, double hi) {
    return lo + (hi - lo) * ((double)(next_random() % 100000) / 100000.0);
}

// Coordinates are drawn from a set that deliberately over-represents the cases where an edge
// rule can go wrong: exact integers, exact half-integers, and values one ulp either side of
// a half-integer. Uniform random doubles would essentially never hit those.
static double random_edge(void) {
    double whole = (double)((int)(next_random() % 80) - 40);
    switch (next_random() % 8) {
    case 0: return whole;
    case 1: return whole + 0.5;
    case 2: return nextafter(whole + 0.5, -1e300);
    case 3: return nextafter(whole + 0.5, 1e300);
    case 4: return whole + 0.25;
    case 5: return whole + 0.75;
    default: return whole + random_double(0.0, 1.0);
    }
}

static raster_frect random_frect(void) {
    double x0 = random_edge(), x1 = random_edge();
    double y0 = random_edge(), y1 = random_edge();
    if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { double t = y0; y0 = y1; y1 = t; }
    raster_frect r = { x0, y0, x1 - x0, y1 - y0 };
    // Half the time hand it over unstandardised, so the standardising path is exercised too.
    if (next_random() % 2 == 0) { r.x = x1; r.width = -(x1 - x0); }
    return r;
}

// Rectilinear matrices only, from both families, with negative scales and offsets.
static raster_matrix random_rectilinear(void) {
    raster_matrix m = { 1, 0, 0, 1, 0, 0 };
    double sx = random_double(0.25, 4.0) * (next_random() % 3 == 0 ? -1.0 : 1.0);
    double sy = random_double(0.25, 4.0) * (next_random() % 3 == 0 ? -1.0 : 1.0);
    double tx = random_double(-50.0, 50.0), ty = random_double(-50.0, 50.0);
    if (next_random() % 4 == 0) {
        // Family B: a quarter turn. Written the way a real rotation produces it, with
        // cos(pi/2) left in place, so the canonicalisation is under test.
        m.a = cos(M_PI / 2) * sx; m.d = cos(M_PI / 2) * sy;
        m.b = sy; m.c = -sx;
    } else {
        m.a = sx; m.d = sy; m.b = 0; m.c = 0;
    }
    m.tx = tx; m.ty = ty;
    return m;
}

// MARK: - The oracle

// Maps the rectangle's corners independently of coverage.c and takes the extremes.
static bool oracle_box(raster_matrix m, raster_frect r, double out[4]) {
    double ux0 = r.width >= 0 ? r.x : r.x + r.width;
    double ux1 = r.width >= 0 ? r.x + r.width : r.x;
    double uy0 = r.height >= 0 ? r.y : r.y + r.height;
    double uy1 = r.height >= 0 ? r.y + r.height : r.y;
    if (ux1 <= ux0 || uy1 <= uy0) return false;

    double xs[4], ys[4];
    double cx[4] = { ux0, ux1, ux0, ux1 }, cy[4] = { uy0, uy0, uy1, uy1 };
    for (int i = 0; i < 4; ++i) {
        xs[i] = m.a * cx[i] + m.c * cy[i] + m.tx;
        ys[i] = m.b * cx[i] + m.d * cy[i] + m.ty;
    }
    out[0] = out[2] = xs[0];
    out[1] = out[3] = ys[0];
    for (int i = 1; i < 4; ++i) {
        if (xs[i] < out[0]) out[0] = xs[i];
        if (xs[i] > out[2]) out[2] = xs[i];
        if (ys[i] < out[1]) out[1] = ys[i];
        if (ys[i] > out[3]) out[3] = ys[i];
    }
    return out[2] > out[0] && out[3] > out[1];
}

// A pixel belongs when its centre is inside [lo, hi).
static bool centre_in(double lo, double hi, int32_t i) {
    double centre = (double)i + 0.5;
    return lo <= centre && centre < hi;
}

// A pixel belongs when the overlap has positive area.
static bool area_in(double lo, double hi, int32_t i) {
    double left = (double)i, right = (double)i + 1.0;
    double overlap = fmin(hi, right) - fmax(lo, left);
    return overlap > 0.0;
}

#define SCAN_LO (-400)
#define SCAN_HI (400)

// Collects the pixel range a predicate accepts, and checks it is contiguous — a non
// contiguous answer would mean the rule is not an interval rule at all.
static bool oracle_range(double lo, double hi, bool (*pred)(double, double, int32_t),
                         int32_t *outLo, int32_t *outHi, const char *what) {
    int32_t first = 0, last = 0;
    bool any = false, broken = false;
    for (int32_t i = SCAN_LO; i < SCAN_HI; ++i) {
        if (pred(lo, hi, i)) {
            if (!any) { first = i; any = true; }
            else if (i != last) broken = true;
            last = i + 1;
        }
    }
    if (broken) fail(what, "the oracle's accepted pixels are not contiguous");
    *outLo = first; *outHi = last;
    return any;
}

// MARK: - Cases

static void test_pixel_edge_definition(void) {
    // raster_pixel_edge must be exactly "the first column whose centre is >= t".
    for (unsigned seed = 1; seed <= 20000; ++seed) {
        rngState = seed * 2654435761u + 5;
        double t = random_edge();
        int32_t e = raster_pixel_edge(t);
        ++checks;
        if (!(((double)e + 0.5) >= t)) {
            char buf[160];
            snprintf(buf, sizeof buf, "edge(%.17g) = %d, but centre %.17g < t", t, e, (double)e + 0.5);
            fail("pixel_edge: column is before t", buf);
            return;
        }
        if (!(((double)(e - 1) + 0.5) < t)) {
            char buf[160];
            snprintf(buf, sizeof buf, "edge(%.17g) = %d, but centre %.17g also >= t", t, e, (double)(e - 1) + 0.5);
            fail("pixel_edge: not the first such column", buf);
            return;
        }
    }

    // The tie-break, pinned. ceil(t - 0.5) is round-half-DOWN: it is neither round(t) nor
    // floor(t + 0.5), and a later "simplification" to either must fail here.
    struct { double t; int32_t want; } ties[] = {
        { 0.5, 0 }, { 1.5, 1 }, { 2.5, 2 }, { -0.5, -1 }, { -1.5, -2 }, { -2.5, -3 },
        { 0.0, 0 }, { 1.0, 1 }, { -1.0, -1 }, { 0.49, 0 }, { 0.51, 1 }, { -0.51, -1 },
    };
    for (size_t i = 0; i < sizeof ties / sizeof ties[0]; ++i) {
        ++checks;
        int32_t got = raster_pixel_edge(ties[i].t);
        if (got != ties[i].want) {
            char buf[120];
            snprintf(buf, sizeof buf, "edge(%g) = %d, want %d", ties[i].t, got, ties[i].want);
            fail("pixel_edge tie-break", buf);
        }
    }

    // Non-finite input must not reach the integer conversion.
    ++checks;
    if (raster_pixel_edge(NAN) != 0) fail("pixel_edge(NaN)", "should be 0, not a cast of NaN");
    ++checks;
    if (raster_pixel_edge(INFINITY) <= 0) fail("pixel_edge(inf)", "should clamp, not wrap");
    ++checks;
    if (raster_pixel_edge(-INFINITY) >= 0) fail("pixel_edge(-inf)", "should clamp, not wrap");
}

// Splitting a range at any point must give two ranges that tile it exactly: no pixel in
// both, none in neither. This is "piecewise == whole" reduced to integers, and because both
// sides evaluate the same edge function at the same split point it holds for every split,
// including exact half-integers.
static void test_partition(void) {
    for (unsigned seed = 1; seed <= 40000; ++seed) {
        rngState = seed * 40503u + 11;
        double a = random_edge(), b = random_edge();
        if (b < a) { double t = a; a = b; b = t; }
        if (b <= a) continue;
        double m = next_random() % 4 == 0
            ? floor(a) + 0.5 * (double)(next_random() % 4)   // land on integers and halves
            : random_double(a, b);
        if (m <= a || m >= b) continue;

        int32_t whole0 = raster_pixel_edge(a), whole1 = raster_pixel_edge(b);
        int32_t mid = raster_pixel_edge(m);
        ++checks;
        if (!(whole0 <= mid && mid <= whole1)) {
            char buf[200];
            snprintf(buf, sizeof buf, "a=%.17g m=%.17g b=%.17g -> [%d,%d) split at %d",
                     a, m, b, whole0, whole1, mid);
            fail("partition: split point outside the whole range", buf);
            return;
        }
        // [whole0, mid) and [mid, whole1) are contiguous and disjoint by construction; the
        // only way this breaks is if the two calls disagreed about m, which is the bug.
    }
}

static void test_device_rect(void) {
    for (unsigned seed = 1; seed <= 30000; ++seed) {
        rngState = seed * 2246822519u + 17;
        raster_matrix m = random_rectilinear();
        raster_frect r = random_frect();

        raster_matrix canonical;
        ++checks;
        if (!raster_matrix_is_rectilinear(m, r, &canonical)) {
            fail("is_rectilinear rejected a rectilinear matrix", "generator only makes rectilinear ones");
            return;
        }

        double box[4];
        bool oracleHas = oracle_box(m, r, box);

        raster_rect got;
        bool has = raster_device_rect_covered(canonical, r, &got);

        int32_t wantX0 = 0, wantX1 = 0, wantY0 = 0, wantY1 = 0;
        bool wantX = false, wantY = false;
        if (oracleHas) {
            wantX = oracle_range(box[0], box[2], centre_in, &wantX0, &wantX1, "covered x");
            wantY = oracle_range(box[1], box[3], centre_in, &wantY0, &wantY1, "covered y");
        }
        bool want = oracleHas && wantX && wantY;

        ++checks;
        if (has != want) {
            char buf[240];
            snprintf(buf, sizeof buf, "engine %s, oracle %s (box %.17g,%.17g..%.17g,%.17g)",
                     has ? "non-empty" : "empty", want ? "non-empty" : "empty",
                     box[0], box[1], box[2], box[3]);
            fail("device_rect_covered: emptiness", buf);
            return;
        }
        if (!want) continue;

        ++checks;
        if (got.x0 != wantX0 || got.x1 != wantX1 || got.y0 != wantY0 || got.y1 != wantY1) {
            char buf[280];
            snprintf(buf, sizeof buf,
                     "got (%d,%d,%d,%d) want (%d,%d,%d,%d) from box %.17g,%.17g..%.17g,%.17g",
                     got.x0, got.y0, got.x1, got.y1, wantX0, wantY0, wantX1, wantY1,
                     box[0], box[1], box[2], box[3]);
            fail("device_rect_covered: geometry", buf);
            return;
        }

        // The touched range is the other rule, and must contain the covered one.
        raster_rect touched;
        if (raster_device_rect_touched(canonical, r, &touched)) {
            ++checks;
            if (touched.x0 > got.x0 || touched.x1 < got.x1 ||
                touched.y0 > got.y0 || touched.y1 < got.y1) {
                fail("touched must contain covered", NULL);
                return;
            }
            int32_t tx0, tx1, ty0, ty1;
            oracle_range(box[0], box[2], area_in, &tx0, &tx1, "touched x");
            oracle_range(box[1], box[3], area_in, &ty0, &ty1, "touched y");
            ++checks;
            if (touched.x0 != tx0 || touched.x1 != tx1 || touched.y0 != ty0 || touched.y1 != ty1) {
                char buf[240];
                snprintf(buf, sizeof buf, "got (%d,%d,%d,%d) want (%d,%d,%d,%d)",
                         touched.x0, touched.y0, touched.x1, touched.y1, tx0, ty0, tx1, ty1);
                fail("device_rect_touched: geometry", buf);
                return;
            }
        }
    }
}

// The invariant that licenses adding antialiased fills in this slice at all: on integer
// geometry the two rules agree and every coverage byte is 255, so the antialias flag is
// invisible on the geometry the app actually uses.
static void test_integer_geometry_is_unambiguous(void) {
    for (unsigned seed = 1; seed <= 4000; ++seed) {
        rngState = seed * 3266489917u + 23;
        raster_matrix m = { 1, 0, 0, 1, 0, 0 };
        m.a = (double)(int)(1 + next_random() % 4) * (next_random() % 3 == 0 ? -1.0 : 1.0);
        m.d = (double)(int)(1 + next_random() % 4) * (next_random() % 3 == 0 ? -1.0 : 1.0);
        m.tx = (double)((int)(next_random() % 40) - 20);
        m.ty = (double)((int)(next_random() % 40) - 20);

        raster_frect r;
        r.x = (double)((int)(next_random() % 40) - 20);
        r.y = (double)((int)(next_random() % 40) - 20);
        r.width = (double)(1 + next_random() % 20);
        r.height = (double)(1 + next_random() % 20);

        raster_rect covered, touched;
        bool a = raster_device_rect_covered(m, r, &covered);
        bool b = raster_device_rect_touched(m, r, &touched);
        ++checks;
        if (a != b || (a && memcmp(&covered, &touched, sizeof(raster_rect)) != 0)) {
            char buf[200];
            snprintf(buf, sizeof buf, "covered (%d,%d,%d,%d) touched (%d,%d,%d,%d)",
                     covered.x0, covered.y0, covered.x1, covered.y1,
                     touched.x0, touched.y0, touched.x1, touched.y1);
            fail("integer geometry: the two rules must agree", buf);
            return;
        }
        if (!a) continue;

        double box[4];
        oracle_box(m, r, box);
        int32_t n = covered.x1 - covered.x0;
        uint8_t *cov = malloc((size_t)n);
        raster_axis_coverage(box[0], box[2], covered.x0, covered.x1, cov);
        for (int32_t i = 0; i < n; ++i) {
            ++checks;
            if (cov[i] != 255) {
                char buf[120];
                snprintf(buf, sizeof buf, "column %d of %d reads %u, want 255", i, n, cov[i]);
                fail("integer geometry: coverage must be solid", buf);
                free(cov);
                return;
            }
        }
        free(cov);
    }
}

static void test_axis_coverage(void) {
    // Exact values first. A sum-based check alone cannot tell rounding from truncation:
    // they differ by at most one part in 255 per partial pixel, which hides under any
    // tolerance loose enough to allow for the rounding itself.
    struct { double lo, hi; int32_t first, last; uint8_t want[4]; } exact[] = {
        { 10.5,  11.0, 10, 11, { 128 } },            // half a pixel
        { 10.0,  10.5, 10, 11, { 128 } },            // the other half
        { 10.25, 10.75, 10, 11, { 128 } },           // half, in the middle
        { 10.0,  11.0, 10, 11, { 255 } },            // exactly one pixel
        { 10.75, 12.25, 10, 13, { 64, 255, 64 } },   // a quarter, whole, a quarter
        // Eighths, not tenths: 0.1 is not representable, so an "exact" expectation written
        // with it would be testing the author's arithmetic rather than the code's.
        { 10.875, 11.125, 10, 12, { 32, 32 } },      // a sliver either side of the edge
    };
    for (size_t k = 0; k < sizeof exact / sizeof exact[0]; ++k) {
        uint8_t got[4] = { 0, 0, 0, 0 };
        raster_axis_coverage(exact[k].lo, exact[k].hi, exact[k].first, exact[k].last, got);
        for (int32_t i = 0; i < exact[k].last - exact[k].first; ++i) {
            ++checks;
            if (got[i] != exact[k].want[i]) {
                char buf[180];
                snprintf(buf, sizeof buf, "[%g,%g) pixel %d reads %u, want %u",
                         exact[k].lo, exact[k].hi, exact[k].first + i, got[i], exact[k].want[i]);
                fail("axis_coverage: exact value", buf);
            }
        }
    }

    for (unsigned seed = 1; seed <= 20000; ++seed) {
        rngState = seed * 1664525u + 29;
        double lo = random_edge(), hi = random_edge();
        if (hi < lo) { double t = lo; lo = hi; hi = t; }
        if (hi <= lo) continue;

        int32_t first = (int32_t)floor(lo), last = (int32_t)ceil(hi);
        int32_t n = last - first;
        if (n <= 0 || n > 200) continue;
        uint8_t *cov = malloc((size_t)n);
        raster_axis_coverage(lo, hi, first, last, cov);

        // The sum of the coverages is the interval's length, to within the rounding of one
        // byte per partial end pixel.
        double sum = 0;
        for (int32_t i = 0; i < n; ++i) sum += (double)cov[i] / 255.0;
        ++checks;
        if (fabs(sum - (hi - lo)) > 0.01) {
            char buf[160];
            snprintf(buf, sizeof buf, "sum %.6g over [%.17g,%.17g) whose length is %.6g",
                     sum, lo, hi, hi - lo);
            fail("axis_coverage: does not sum to the length", buf);
            free(cov);
            return;
        }
        // Only the ends may be partial.
        for (int32_t i = 1; i < n - 1; ++i) {
            ++checks;
            if (cov[i] != 255) {
                fail("axis_coverage: an interior pixel is partial", NULL);
                free(cov);
                return;
            }
        }
        free(cov);
    }
}

static void test_rectilinear_families(void) {
    raster_frect unit = { 0, 0, 100, 100 };
    raster_matrix out;

    // Family A.
    raster_matrix identity = { 1, 0, 0, 1, 0, 0 };
    CHECK(raster_matrix_is_rectilinear(identity, unit, &out), "identity is rectilinear", NULL);

    raster_matrix flipped = { 1, 0, 0, -1, 0, 100 };
    CHECK(raster_matrix_is_rectilinear(flipped, unit, &out), "the base flip is rectilinear", NULL);

    // Family B: every quarter turn, written the way a real rotation produces it.
    for (int k = 1; k <= 3; k += 2) {
        double angle = M_PI / 2 * k;
        raster_matrix quarter = { cos(angle), sin(angle), -sin(angle), cos(angle), 0, 0 };
        ++checks;
        if (!raster_matrix_is_rectilinear(quarter, unit, &out)) {
            char buf[120];
            snprintf(buf, sizeof buf, "%d degrees: a=%.3g b=%.3g c=%.3g d=%.3g",
                     90 * k, quarter.a, quarter.b, quarter.c, quarter.d);
            fail("a quarter turn must be rectilinear", buf);
        } else {
            ++checks;
            if (out.a != 0.0 || out.d != 0.0)
                fail("a quarter turn must canonicalise a and d to exact zero", NULL);
        }
    }
    raster_matrix half = { cos(M_PI), sin(M_PI), -sin(M_PI), cos(M_PI), 0, 0 };
    CHECK(raster_matrix_is_rectilinear(half, unit, &out), "180 degrees is rectilinear", NULL);

    // A real rotation is not.
    raster_matrix tilted = { cos(0.4), sin(0.4), -sin(0.4), cos(0.4), 0, 0 };
    CHECK(!raster_matrix_is_rectilinear(tilted, unit, &out), "0.4 rad is not rectilinear", NULL);

    // The tolerance is rect-relative: the same tiny shear is acceptable over a small
    // rectangle and not over a huge one, because what matters is how far a corner moves.
    raster_matrix shear = { 1, 1e-7, 0, 1, 0, 0 };
    raster_frect small = { 0, 0, 1, 1 };
    raster_frect huge = { 0, 0, 100000, 100000 };
    CHECK(raster_matrix_is_rectilinear(shear, small, &out),
          "1e-7 shear over 1px moves a corner by 1e-7 and is acceptable", NULL);
    CHECK(!raster_matrix_is_rectilinear(shear, huge, &out),
          "the same shear over 100000px moves a corner by 0.01px and is not", NULL);

    // Non-finite matrices are refused rather than propagated.
    raster_matrix broken = { NAN, 0, 0, 1, 0, 0 };
    CHECK(!raster_matrix_is_rectilinear(broken, unit, &out), "NaN is not rectilinear", NULL);
    raster_matrix infinite = { 1, 0, 0, 1, INFINITY, 0 };
    CHECK(!raster_matrix_is_rectilinear(infinite, unit, &out), "an infinite offset is refused", NULL);
}

static void test_degenerate(void) {
    raster_matrix m = { 1, 0, 0, 1, 0, 0 };
    raster_rect out;
    double box[4];

    raster_frect empty = { 5, 5, 0, 10 };
    CHECK(!raster_device_box(m, empty, box), "a zero-width rect has no device box", NULL);
    CHECK(!raster_device_rect_covered(m, empty, &out), "and covers nothing", NULL);

    raster_frect negative = { 10, 10, -5, -5 };
    CHECK(raster_device_box(m, negative, box), "a negative extent standardises", NULL);
    CHECK(box[0] == 5 && box[1] == 5 && box[2] == 10 && box[3] == 10,
          "and standardises to the right box", NULL);

    raster_frect nan = { NAN, 0, 10, 10 };
    CHECK(!raster_device_box(m, nan, box), "a NaN rect has no device box", NULL);
    raster_frect infinite = { 0, 0, INFINITY, 10 };
    CHECK(!raster_device_box(m, infinite, box), "an infinite rect has no device box", NULL);

    // A rect so large it would overflow the integer conversion is refused, not wrapped.
    raster_frect vast = { -1e18, -1e18, 2e18, 2e18 };
    CHECK(!raster_device_box(m, vast, box), "a vast rect is refused rather than wrapped", NULL);

    // Sub-pixel rects that contain no pixel centre cover nothing under the centre rule, but
    // are still touched. This is exactly where the two rules differ.
    raster_frect sliver = { 10.6, 10.6, 0.3, 0.3 };
    CHECK(!raster_device_rect_covered(m, sliver, &out), "a sliver missing every centre covers nothing", NULL);
    CHECK(raster_device_rect_touched(m, sliver, &out), "but it does touch a pixel", NULL);

    CHECK(!raster_device_box(m, (raster_frect){ 0, 0, 10, 10 }, NULL), "a NULL out is refused", NULL);
    CHECK(!raster_device_rect_covered(m, (raster_frect){ 0, 0, 10, 10 }, NULL), "likewise", NULL);
}

// MARK: - Stage 2: the context

// The reference model. A clip is a plain bitmap stack; a fill is a loop over every pixel
// that composites through raster_blend_* one at a time. Nothing here knows about bands,
// strides or row pointers, which is exactly where the engine's bugs would be.

#define CW 20
#define CH 14

typedef struct {
    bool clip[CH][CW];
} model_clip;

typedef struct {
    model_clip state;
    model_clip saved[16];
    size_t depth;
} model;

static void model_init(model *m) {
    memset(m, 0, sizeof(*m));
    for (int y = 0; y < CH; ++y)
        for (int x = 0; x < CW; ++x) m->state.clip[y][x] = true;
}

static void model_save(model *m) {
    if (m->depth < 16) m->saved[m->depth] = m->state;
    ++m->depth;
}

static void model_restore(model *m) {
    if (m->depth == 0) return;  // matches the engine: an unbalanced restore does nothing
    --m->depth;
    if (m->depth < 16) m->state = m->saved[m->depth];
}

// The coverage of device column i by [lo, hi), written independently of coverage.c.
static uint8_t model_axis(double lo, double hi, int32_t i) {
    double left = (double)i, right = (double)i + 1.0;
    double overlap = fmin(hi, right) - fmax(lo, left);
    if (overlap <= 0.0) return 0;
    if (overlap >= 1.0) return 255;
    return (uint8_t)(overlap * 255.0 + 0.5);
}

static void model_fill(model *m, uint8_t *pixels, size_t stride, raster_format format,
                       const double box[4], bool antialias, raster_rect area,
                       const uint8_t rgba[4], uint8_t gray, raster_blend blend, uint8_t alpha8) {
    for (int32_t y = area.y0; y < area.y1; ++y) {
        if (y < 0 || y >= CH) continue;
        for (int32_t x = area.x0; x < area.x1; ++x) {
            if (x < 0 || x >= CW) continue;
            if (!m->state.clip[y][x]) continue;
            uint8_t coverage = 255;
            if (antialias) {
                uint8_t cx = model_axis(box[0], box[2], x);
                uint8_t cy = model_axis(box[1], box[3], y);
                coverage = (uint8_t)((cx * cy + 127) / 255);
            }
            if (coverage == 0) continue;
            size_t bpp = raster_bytes_per_pixel(format);
            uint8_t *dst = pixels + (size_t)y * stride + (size_t)x * bpp;
            if (format == RASTER_GRAY8) raster_blend_gray(dst, gray, blend, alpha8, coverage);
            else raster_blend_rgba(dst, rgba, blend, alpha8, coverage);
        }
    }
}

static bool clips_agree(const model *m, const raster_context *ctx, char *detail, size_t size) {
    const raster_region *region = raster_context_clip_region(ctx);
    for (int y = 0; y < CH; ++y)
        for (int x = 0; x < CW; ++x) {
            bool engine = raster_region_contains(region, x, y);
            if (engine != m->state.clip[y][x]) {
                snprintf(detail, size, "pixel (%d,%d): engine %s, model %s",
                         x, y, engine ? "in" : "out", m->state.clip[y][x] ? "in" : "out");
                return false;
            }
        }
    return true;
}

static raster_frect random_small_frect(void) {
    double x0 = random_double(-4, CW + 4), y0 = random_double(-4, CH + 4);
    double w = random_double(0.4, 12), h = random_double(0.4, 10);
    if (next_random() % 3 == 0) { x0 = floor(x0); y0 = floor(y0); w = floor(w) + 1; h = floor(h) + 1; }
    return (raster_frect){ x0, y0, w, h };
}

// The identity user space: BrushRaster's flip cancels the base transform exactly, so this is
// the CTM almost every drawing site in the app actually runs under.
static void install_identity(raster_context *ctx) {
    raster_context_set_matrix(ctx, (raster_matrix){ 1, 0, 0, 1, 0, 0 });
}

static void test_clip_stack(void) {
    for (unsigned seed = 1; seed <= 3000; ++seed) {
        rngState = seed * 2654435761u + 41;
        raster_surface *surface = raster_surface_create(CW, CH, RASTER_RGBA8);
        raster_context *ctx = raster_context_create(surface);
        install_identity(ctx);
        model m;
        model_init(&m);

        for (int step = 0; step < 12; ++step) {
            switch (next_random() % 5) {
            case 0:
                raster_context_save(ctx);
                model_save(&m);
                break;
            case 1:
                raster_context_restore(ctx);
                model_restore(&m);
                break;
            case 2: {
                raster_frect r = random_small_frect();
                raster_rect device;
                raster_matrix identity = { 1, 0, 0, 1, 0, 0 };
                bool any = raster_device_rect_covered(identity, r, &device);
                raster_context_clip_rect(ctx, r);
                for (int y = 0; y < CH; ++y)
                    for (int x = 0; x < CW; ++x) {
                        bool in = any && x >= device.x0 && x < device.x1 &&
                                  y >= device.y0 && y < device.y1;
                        if (!in) m.state.clip[y][x] = false;
                    }
                break;
            }
            case 3: {
                // addRect x N then clip, winding or even-odd.
                int n = 1 + (int)(next_random() % 3);
                bool evenOdd = next_random() % 2 == 0;
                bool accumulated[CH][CW];
                memset(accumulated, 0, sizeof accumulated);
                for (int k = 0; k < n; ++k) {
                    raster_frect r = random_small_frect();
                    raster_rect device;
                    raster_matrix identity = { 1, 0, 0, 1, 0, 0 };
                    bool any = raster_device_rect_covered(identity, r, &device);
                    raster_context_add_rect(ctx, r);
                    if (!any) continue;
                    for (int y = 0; y < CH; ++y)
                        for (int x = 0; x < CW; ++x)
                            if (x >= device.x0 && x < device.x1 &&
                                y >= device.y0 && y < device.y1) {
                                accumulated[y][x] = evenOdd ? !accumulated[y][x] : true;
                            }
                }
                raster_context_clip_path(ctx, evenOdd);
                for (int y = 0; y < CH; ++y)
                    for (int x = 0; x < CW; ++x)
                        if (!accumulated[y][x]) m.state.clip[y][x] = false;
                break;
            }
            default: {
                // An empty clip, the way Selection builds one.
                if (next_random() % 4 == 0) {
                    raster_context_clip_rect(ctx, (raster_frect){ 0, 0, 0, 0 });
                    memset(m.state.clip, 0, sizeof m.state.clip);
                }
                break;
            }
            }

            char detail[160];
            ++checks;
            if (!clips_agree(&m, ctx, detail, sizeof detail)) {
                char buf[240];
                snprintf(buf, sizeof buf, "seed %u step %d: %s", seed, step, detail);
                fail("clip stack disagrees with the bitmap model", buf);
                raster_context_destroy(ctx);
                raster_surface_release(surface);
                return;
            }

            // The reported bounds must be exactly the model's, and empty must be reported as
            // empty rather than as a degenerate rectangle.
            raster_rect bounds;
            bool has = raster_context_clip_bounds(ctx, &bounds);
            bool anyPixel = false;
            raster_rect want = { 0, 0, 0, 0 };
            for (int y = 0; y < CH; ++y)
                for (int x = 0; x < CW; ++x)
                    if (m.state.clip[y][x]) {
                        if (!anyPixel) { want = (raster_rect){ x, y, x + 1, y + 1 }; anyPixel = true; }
                        else {
                            if (x < want.x0) want.x0 = x;
                            if (y < want.y0) want.y0 = y;
                            if (x + 1 > want.x1) want.x1 = x + 1;
                            if (y + 1 > want.y1) want.y1 = y + 1;
                        }
                    }
            ++checks;
            if (has != anyPixel) {
                fail("clip_bounds: emptiness disagrees with the model", NULL);
                raster_context_destroy(ctx);
                raster_surface_release(surface);
                return;
            }
            if (has && memcmp(&bounds, &want, sizeof(raster_rect)) != 0) {
                char buf[200];
                snprintf(buf, sizeof buf, "got (%d,%d,%d,%d) want (%d,%d,%d,%d)",
                         bounds.x0, bounds.y0, bounds.x1, bounds.y1,
                         want.x0, want.y0, want.x1, want.y1);
                fail("clip_bounds: geometry", buf);
                raster_context_destroy(ctx);
                raster_surface_release(surface);
                return;
            }
        }
        raster_context_destroy(ctx);
        raster_surface_release(surface);
    }
}

static void test_fill_against_reference(raster_format format, const char *label) {
    static const raster_blend modes[] = {
        RASTER_BLEND_NORMAL, RASTER_BLEND_MULTIPLY, RASTER_BLEND_SCREEN,
        RASTER_BLEND_LIGHTEN, RASTER_BLEND_COPY, RASTER_BLEND_DESTINATION_OUT,
    };
    for (unsigned seed = 1; seed <= 4000; ++seed) {
        rngState = seed * 40503u + 53;
        size_t bpp = raster_bytes_per_pixel(format);

        raster_surface *surface = raster_surface_create(CW, CH, format);
        raster_context *ctx = raster_context_create(surface);
        install_identity(ctx);

        // A non-trivial backdrop, so a blend mode that reads the destination has something
        // to read and "did nothing" cannot masquerade as correct.
        uint8_t *pixels = raster_surface_mutable_bytes(surface);
        size_t stride = raster_surface_stride(surface);
        uint8_t reference[CH * CW * 4];
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x)
                for (size_t b = 0; b < bpp; ++b) {
                    uint8_t v = (uint8_t)(next_random() % 256);
                    if (format == RASTER_RGBA8 && b < 3) {
                        // Keep the backdrop premultiplied, or the blend results are undefined.
                        v = (uint8_t)(next_random() % 256);
                    }
                    pixels[(size_t)y * stride + (size_t)x * bpp + b] = v;
                }
        if (format == RASTER_RGBA8)
            for (int y = 0; y < CH; ++y)
                for (int x = 0; x < CW; ++x) {
                    uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4;
                    for (int b = 0; b < 3; ++b) if (p[b] > p[3]) p[b] = p[3];
                }
        for (int y = 0; y < CH; ++y)
            memcpy(reference + (size_t)y * CW * bpp, pixels + (size_t)y * stride, CW * bpp);

        model m;
        model_init(&m);

        // A clip, so the band iteration has more than one rectangle to walk. The clip itself
        // is verified independently by test_clip_stack; here it is only scenery, so the
        // model takes the engine's answer for it and the comparison stays about compositing.
        if (next_random() % 2 == 0) {
            int n = 1 + (int)(next_random() % 3);
            for (int k = 0; k < n; ++k) raster_context_add_rect(ctx, random_small_frect());
            raster_context_clip_path(ctx, false);
            const raster_region *region = raster_context_clip_region(ctx);
            for (int y = 0; y < CH; ++y)
                for (int x = 0; x < CW; ++x)
                    m.state.clip[y][x] = raster_region_contains(region, x, y);
        }

        bool antialias = next_random() % 2 == 0;
        raster_blend blend = modes[next_random() % (sizeof modes / sizeof modes[0])];
        double alpha = (double)(next_random() % 101) / 100.0;
        double fill[4] = {
            (double)(next_random() % 256) / 255.0, (double)(next_random() % 256) / 255.0,
            (double)(next_random() % 256) / 255.0, (double)(next_random() % 256) / 255.0,
        };
        raster_frect rect = random_small_frect();

        raster_context_set_antialias(ctx, antialias);
        raster_context_set_blend(ctx, blend);
        raster_context_set_alpha(ctx, alpha);
        raster_context_set_fill_color(ctx, fill);
        raster_status status = raster_context_fill_rect(ctx, rect);
        ++checks;
        if (status != RASTER_OK) {
            fail("fill_rect failed under an identity CTM", label);
            raster_context_destroy(ctx);
            raster_surface_release(surface);
            return;
        }

        // The same fill, computed pixel by pixel.
        raster_matrix identity = { 1, 0, 0, 1, 0, 0 };
        double box[4];
        if (raster_device_box(identity, rect, box)) {
            raster_rect area;
            bool any = antialias ? raster_device_rect_touched(identity, rect, &area)
                                 : raster_device_rect_covered(identity, rect, &area);
            if (any) {
                uint8_t rgba[4] = { 0, 0, 0, 0 }, gray = 0;
                double weight = alpha;
                if (format == RASTER_GRAY8) {
                    gray = (uint8_t)(fill[0] * 255.0 + 0.5);
                    weight = alpha * fill[3];
                } else {
                    rgba[3] = (uint8_t)(fill[3] * 255.0 + 0.5);
                    for (int i = 0; i < 3; ++i)
                        rgba[i] = (uint8_t)(fill[i] * fill[3] * 255.0 + 0.5);
                }
                uint8_t alpha8 = (uint8_t)(weight * 255.0 + 0.5);
                model_fill(&m, reference, CW * bpp, format, box, antialias, area,
                           rgba, gray, blend, alpha8);
            }
        }

        for (int y = 0; y < CH; ++y) {
            ++checks;
            if (memcmp(pixels + (size_t)y * stride, reference + (size_t)y * CW * bpp, CW * bpp) != 0) {
                char buf[240];
                int x = 0;
                for (; x < CW; ++x)
                    if (memcmp(pixels + (size_t)y * stride + (size_t)x * bpp,
                               reference + ((size_t)y * CW + (size_t)x) * bpp, bpp) != 0) break;
                snprintf(buf, sizeof buf,
                         "%s seed %u row %d first differs at x=%d (aa %d, blend %u, alpha %.2f)",
                         label, seed, y, x, antialias, blend, alpha);
                fail("fill disagrees with the per-pixel reference", buf);
                raster_context_destroy(ctx);
                raster_surface_release(surface);
                return;
            }
        }
        raster_context_destroy(ctx);
        raster_surface_release(surface);
    }
}

static void test_copy_on_write(void) {
    // The sequence RasterSnapshotTests performs: read the pointer once, freeze the bytes,
    // clear, redraw, read through the same pointer. The context must never move.
    raster_surface *surface = raster_surface_create(8, 6, RASTER_RGBA8);
    raster_context *ctx = raster_context_create(surface);
    install_identity(ctx);

    const uint8_t *before = raster_surface_bytes(surface);
    double white[4] = { 1, 1, 1, 1 };
    raster_context_set_fill_color(ctx, white);
    raster_context_set_antialias(ctx, false);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 8, 6 });

    raster_surface *snapshot = raster_context_make_snapshot(ctx);
    CHECK(snapshot != NULL, "makeImage produced a snapshot", NULL);
    // Zero-copy: a snapshot of an owned target must share its pixels until something is
    // drawn, which is the whole point of deferring the copy. Copying eagerly would still be
    // *correct*, so nothing else here would notice a full-canvas memcpy per makeImage.
    CHECK(raster_surface_bytes(snapshot) == raster_surface_bytes(surface),
          "a fresh snapshot shares the context's pixels", NULL);
    CHECK(!raster_surface_is_unique(surface),
          "so the context knows its store is shared", NULL);
    uint8_t frozen[8 * 6 * 4];
    memcpy(frozen, raster_surface_bytes(snapshot), sizeof frozen);
    CHECK(frozen[0] == 255, "the snapshot holds the painted pixels", NULL);

    raster_context_clear_rect(ctx, (raster_frect){ 0, 0, 8, 6 });
    CHECK(raster_surface_bytes(surface) == before, "clear must not move the context's pixels", NULL);
    CHECK(raster_surface_bytes(surface)[0] == 0, "and it must actually clear", NULL);
    CHECK(memcmp(raster_surface_bytes(snapshot), frozen, sizeof frozen) == 0,
          "the snapshot stays frozen across the clear", NULL);

    double grey[4] = { 0.5, 0.5, 0.5, 1 };
    raster_context_set_fill_color(ctx, grey);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 8, 6 });
    CHECK(raster_surface_bytes(surface) == before, "redrawing must not move them either", NULL);
    CHECK(memcmp(raster_surface_bytes(snapshot), frozen, sizeof frozen) == 0,
          "and the snapshot is still frozen", NULL);

    raster_surface_release(snapshot);

    // A snapshot nobody kept costs a release, not a copy. Observable only as behaviour, but
    // valgrind confirms there is no allocation here.
    raster_surface *dropped = raster_context_make_snapshot(ctx);
    raster_surface_release(dropped);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 8, 6 });
    CHECK(raster_surface_bytes(surface) == before, "the pointer survives a dropped snapshot", NULL);

    // Two outstanding at once.
    raster_surface *a = raster_context_make_snapshot(ctx);
    raster_surface *b = raster_context_make_snapshot(ctx);
    double black[4] = { 0, 0, 0, 1 };
    raster_context_set_fill_color(ctx, black);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 8, 6 });
    CHECK(raster_surface_bytes(a)[0] == 128, "the first snapshot kept its pixels", NULL);
    CHECK(raster_surface_bytes(b)[0] == 128, "so did the second", NULL);
    CHECK(raster_surface_bytes(surface)[0] == 0, "and the context moved on", NULL);
    raster_surface_release(a);
    raster_surface_release(b);

    raster_context_destroy(ctx);
    raster_surface_release(surface);

    // A borrowed target can never detach, so its snapshot has to be an eager copy.
    uint8_t caller[4 * 4 * 4];
    memset(caller, 0, sizeof caller);
    raster_surface *borrowed = raster_surface_create_borrowed(caller, 4, 4, 16, RASTER_RGBA8);
    raster_context *bctx = raster_context_create(borrowed);
    install_identity(bctx);
    raster_context_set_fill_color(bctx, white);
    raster_context_set_antialias(bctx, false);
    raster_context_fill_rect(bctx, (raster_frect){ 0, 0, 4, 4 });
    CHECK(caller[0] == 255, "a borrowed context writes into the caller's buffer", NULL);

    raster_surface *borrowedSnapshot = raster_context_make_snapshot(bctx);
    CHECK(borrowedSnapshot != NULL, "a borrowed context can still be snapshotted", NULL);
    CHECK(raster_surface_bytes(borrowedSnapshot) != caller,
          "and the snapshot is a copy, not a view", NULL);
    raster_context_set_fill_color(bctx, black);
    raster_context_fill_rect(bctx, (raster_frect){ 0, 0, 4, 4 });
    CHECK(caller[0] == 0, "the caller's buffer is still the destination after a snapshot", NULL);
    CHECK(raster_surface_bytes(borrowedSnapshot)[0] == 255, "and the copy is unaffected", NULL);
    raster_surface_release(borrowedSnapshot);
    raster_context_destroy(bctx);
    raster_surface_release(borrowed);
}

static void test_context_semantics(void) {
    raster_surface *surface = raster_surface_create(10, 10, RASTER_RGBA8);
    raster_context *ctx = raster_context_create(surface);

    // A fresh context's clip is the whole surface, and its user space is CoreGraphics's:
    // bottom-left origin, y up. BrushRaster's flip cancels it exactly.
    raster_rect bounds;
    CHECK(raster_context_clip_bounds(ctx, &bounds), "a fresh clip is not empty", NULL);
    CHECK(bounds.x0 == 0 && bounds.y0 == 0 && bounds.x1 == 10 && bounds.y1 == 10,
          "a fresh clip is the whole surface", NULL);
    raster_matrix base = raster_context_matrix(ctx);
    CHECK(base.a == 1 && base.b == 0 && base.c == 0 && base.d == -1 && base.tx == 0 && base.ty == 10,
          "the base CTM flips y", NULL);

    // restore without save does nothing rather than trapping.
    raster_context_restore(ctx);
    raster_context_restore(ctx);
    CHECK(raster_context_depth(ctx) == 0, "restore at depth 0 is a no-op", NULL);
    CHECK(raster_context_clip_bounds(ctx, &bounds), "and leaves the clip alone", NULL);

    // save/restore round-trips the clip.
    install_identity(ctx);
    raster_context_save(ctx);
    raster_context_clip_rect(ctx, (raster_frect){ 2, 2, 3, 3 });
    CHECK(raster_context_clip_bounds(ctx, &bounds) && bounds.x1 == 5, "the clip narrowed", NULL);
    raster_context_restore(ctx);
    CHECK(raster_context_clip_bounds(ctx, &bounds) && bounds.x1 == 10, "and was restored", NULL);

    // An empty clip is reported as empty, not as a degenerate rectangle at the origin.
    raster_context_save(ctx);
    raster_context_clip_rect(ctx, (raster_frect){ 0, 0, 0, 0 });
    CHECK(!raster_context_clip_bounds(ctx, &bounds), "an empty clip has no bounds", NULL);
    double white[4] = { 1, 1, 1, 1 };
    raster_context_set_fill_color(ctx, white);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 10, 10 });
    CHECK(raster_surface_bytes(surface)[0] == 0, "and nothing draws through it", NULL);
    raster_context_restore(ctx);

    // A rotated CTM is refused rather than approximated.
    raster_context_save(ctx);
    raster_context_set_matrix(ctx, (raster_matrix){ cos(0.4), sin(0.4), -sin(0.4), cos(0.4), 0, 0 });
    CHECK(raster_context_clip_rect(ctx, (raster_frect){ 0, 0, 4, 4 }) == RASTER_UNSUPPORTED_TRANSFORM,
          "a rotated clip is refused", NULL);
    CHECK(raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 4, 4 }) == RASTER_UNSUPPORTED_TRANSFORM,
          "so is a rotated fill", NULL);
    CHECK(raster_context_add_rect(ctx, (raster_frect){ 0, 0, 4, 4 }) == RASTER_UNSUPPORTED_TRANSFORM,
          "and a rotated addRect", NULL);
    // A quarter turn is not rotated in this sense and must work.
    raster_context_set_matrix(ctx, (raster_matrix){ cos(M_PI / 2), sin(M_PI / 2),
                                                    -sin(M_PI / 2), cos(M_PI / 2), 10, 0 });
    CHECK(raster_context_clip_rect(ctx, (raster_frect){ 0, 0, 4, 4 }) == RASTER_OK,
          "a quarter turn is accepted", NULL);
    raster_context_restore(ctx);

    // clear ignores the state's alpha and blend mode; a fill with .clear does not.
    install_identity(ctx);
    raster_context_set_antialias(ctx, false);
    raster_context_set_fill_color(ctx, white);
    raster_context_set_alpha(ctx, 1.0);
    raster_context_set_blend(ctx, RASTER_BLEND_NORMAL);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 10, 10 });
    CHECK(raster_surface_bytes(surface)[3] == 255, "painted opaque", NULL);

    raster_context_set_alpha(ctx, 0.5);
    raster_context_clear_rect(ctx, (raster_frect){ 0, 0, 10, 10 });
    CHECK(raster_surface_bytes(surface)[3] == 0, "clear ignores setAlpha and erases fully", NULL);

    raster_context_set_alpha(ctx, 1.0);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 10, 10 });
    raster_context_set_alpha(ctx, 0.5);
    raster_context_set_blend(ctx, RASTER_BLEND_CLEAR);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 10, 10 });
    CHECK(raster_surface_bytes(surface)[3] != 0,
          "but a fill with the clear blend at half alpha erases only half", NULL);

    // The current path is not part of the graphics state.
    raster_context_set_blend(ctx, RASTER_BLEND_NORMAL);
    raster_context_add_rect(ctx, (raster_frect){ 1, 1, 2, 2 });
    raster_context_save(ctx);
    raster_context_restore(ctx);
    raster_context_clip_path(ctx, false);
    CHECK(raster_context_clip_bounds(ctx, &bounds) && bounds.x0 == 1 && bounds.x1 == 3,
          "a save/restore pair does not discard the current path", NULL);

    // Clipping consumes the path: a second clip with nothing added clips everything away.
    raster_context_clip_path(ctx, false);
    CHECK(!raster_context_clip_bounds(ctx, &bounds), "clip consumes the path, and an empty path clips all", NULL);

    raster_context_destroy(ctx);
    raster_surface_release(surface);

    CHECK(raster_context_create(NULL) == NULL, "a NULL target has no context", NULL);
    raster_context_destroy(NULL);
    raster_context_restore(NULL);
}

// Piecewise == whole: a rectangle filled once against the same rectangle filled N times,
// each under a hard clip that takes one slice of it. This is the property the whole edge
// rule exists to guarantee, observed from the outside.
static void test_piecewise_equals_whole(void) {
    for (unsigned seed = 1; seed <= 2000; ++seed) {
        rngState = seed * 3266489917u + 67;
        raster_format format = next_random() % 2 ? RASTER_RGBA8 : RASTER_GRAY8;
        size_t bpp = raster_bytes_per_pixel(format);
        double alpha = 0.5;
        double fill[4] = { 0.8, 0.4, 0.2, 1.0 };
        raster_frect rect = { 1.0, 1.0, 16.0, 10.0 };
        raster_blend blend = next_random() % 2 ? RASTER_BLEND_NORMAL : RASTER_BLEND_MULTIPLY;

        raster_surface *whole = raster_surface_create(CW, CH, format);
        raster_surface *pieces = raster_surface_create(CW, CH, format);
        // Identical non-trivial backdrops.
        uint8_t *wp = raster_surface_mutable_bytes(whole);
        uint8_t *pp = raster_surface_mutable_bytes(pieces);
        size_t ws = raster_surface_stride(whole), ps = raster_surface_stride(pieces);
        for (int y = 0; y < CH; ++y)
            for (size_t b = 0; b < CW * bpp; ++b) {
                uint8_t v = (uint8_t)(next_random() % 256);
                wp[(size_t)y * ws + b] = v;
                pp[(size_t)y * ps + b] = v;
            }
        if (format == RASTER_RGBA8)
            for (int y = 0; y < CH; ++y)
                for (int x = 0; x < CW; ++x) {
                    uint8_t *w = wp + (size_t)y * ws + (size_t)x * 4;
                    uint8_t *p = pp + (size_t)y * ps + (size_t)x * 4;
                    for (int b = 0; b < 3; ++b) { if (w[b] > w[3]) w[b] = w[3]; p[b] = w[b]; }
                }

        raster_context *wc = raster_context_create(whole);
        raster_context *pc = raster_context_create(pieces);
        install_identity(wc);
        install_identity(pc);
        for (raster_context *c = wc; c; c = c == wc ? pc : NULL) {
            raster_context_set_antialias(c, false);
            raster_context_set_alpha(c, alpha);
            raster_context_set_blend(c, blend);
            raster_context_set_fill_color(c, fill);
        }

        raster_context_fill_rect(wc, rect);

        // Cut the CLIP into slices, never the fill rectangle: two partial fills of one pixel
        // compose as 1-(1-a1)(1-a2), which is not a1+a2, and that is true of CoreGraphics
        // too. The clip is the only thing that may vary.
        int cuts = 1 + (int)(next_random() % 4);
        double edges[6];
        edges[0] = -100;
        for (int i = 1; i <= cuts; ++i) edges[i] = random_double(rect.x, rect.x + rect.width);
        edges[cuts + 1] = 100;
        for (int i = 1; i <= cuts; ++i)  // insertion sort
            for (int j = i; j > 1 && edges[j - 1] > edges[j]; --j) {
                double t = edges[j]; edges[j] = edges[j - 1]; edges[j - 1] = t;
            }
        for (int i = 0; i <= cuts; ++i) {
            raster_context_save(pc);
            raster_context_clip_rect(pc, (raster_frect){ edges[i], -100,
                                                          edges[i + 1] - edges[i], 300 });
            raster_context_fill_rect(pc, rect);
            raster_context_restore(pc);
        }

        for (int y = 0; y < CH; ++y) {
            ++checks;
            if (memcmp(wp + (size_t)y * ws, pp + (size_t)y * ps, CW * bpp) != 0) {
                char buf[200];
                snprintf(buf, sizeof buf, "seed %u, %s, %d cuts, row %d", seed,
                         format == RASTER_GRAY8 ? "GRAY8" : "RGBA8", cuts, y);
                fail("piecewise != whole", buf);
                raster_context_destroy(wc); raster_context_destroy(pc);
                raster_surface_release(whole); raster_surface_release(pieces);
                return;
            }
        }
        raster_context_destroy(wc);
        raster_context_destroy(pc);
        raster_surface_release(whole);
        raster_surface_release(pieces);
    }
}

int main(void) {
    test_pixel_edge_definition();
    test_partition();
    test_rectilinear_families();
    test_device_rect();
    test_integer_geometry_is_unambiguous();
    test_axis_coverage();
    test_degenerate();

    test_context_semantics();
    test_clip_stack();
    test_fill_against_reference(RASTER_RGBA8, "RGBA8");
    test_fill_against_reference(RASTER_GRAY8, "GRAY8");
    test_piecewise_equals_whole();
    test_copy_on_write();

    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
