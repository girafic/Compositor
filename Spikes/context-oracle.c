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

// MARK: - Stage 3: the image sampler

// A context created with raster_context_create has CoreGraphics' own user space: origin at
// the bottom left, y up. Drawing an image into (0, 0, w, h) there lands row 0 on device row
// 0, so that is the natural 1:1 case and what these tests use. The app's own contexts flip
// once at construction and BrushRaster.draw flips again, which comes to the same thing.

static raster_surface *ramp_image(int w, int h, raster_format format, unsigned seed) {
    raster_surface *image = raster_surface_create((size_t)w, (size_t)h, format);
    uint8_t *p = raster_surface_mutable_bytes(image);
    size_t stride = raster_surface_stride(image);
    size_t bpp = raster_bytes_per_pixel(format);
    rngState = seed;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *q = p + (size_t)y * stride + (size_t)x * bpp;
            if (format == RASTER_GRAY8) {
                q[0] = (uint8_t)((x * 7 + y * 13 + (int)(next_random() % 5)) & 0xFF);
            } else {
                uint8_t a = (uint8_t)(128 + (next_random() % 128));
                q[3] = a;
                for (int c = 0; c < 3; ++c)
                    q[c] = (uint8_t)(((x * (c + 3) + y * (c + 5)) % 256) * a / 255);
            }
        }
    return image;
}

// The device -> source mapping, written independently of draw_image.c for the default CTM.
static void reference_source(int X, int Y, double contextHeight, raster_frect rect,
                             int W, int H, double *u, double *v) {
    double xUser = (double)X + 0.5;
    double yUser = contextHeight - ((double)Y + 0.5);
    *u = (xUser - rect.x) * (double)W / rect.width;
    *v = (rect.y + rect.height - yUser) * (double)H / rect.height;
}

static void test_one_to_one_is_exact(void) {
    static const raster_interpolation qualities[] = {
        RASTER_INTERPOLATION_NONE, RASTER_INTERPOLATION_DEFAULT, RASTER_INTERPOLATION_LOW,
        RASTER_INTERPOLATION_MEDIUM, RASTER_INTERPOLATION_HIGH,
    };
    const raster_format formats[] = { RASTER_RGBA8, RASTER_GRAY8 };

    for (int f = 0; f < 2; ++f) {
        raster_format format = formats[f];
        size_t bpp = raster_bytes_per_pixel(format);
        for (int shifted = 0; shifted < 2; ++shifted) {
            int ox = shifted ? 3 : 0, oy = shifted ? 2 : 0;
            raster_surface *image = ramp_image(11, 7, format, 91 + (unsigned)f);

            for (size_t q = 0; q < 5; ++q) {
                for (int aa = 0; aa < 2; ++aa) {
                    raster_surface *target = raster_surface_create(11 + 6, 7 + 6, format);
                    raster_context *ctx = raster_context_create(target);
                    raster_context_set_antialias(ctx, aa != 0);
                    raster_context_set_interpolation(ctx, qualities[q]);
                    raster_context_set_blend(ctx, RASTER_BLEND_COPY);

                    // Bottom-left origin: to land the image at device row `oy` the rect's
                    // top edge has to sit that far below the context's top.
                    double top = (double)(7 + 6) - (double)oy;
                    raster_frect rect = { (double)ox, top - 7.0, 11.0, 7.0 };
                    raster_status status = raster_context_draw_image(ctx, image, rect);
                    ++checks;
                    if (status != RASTER_OK) {
                        fail("draw_image failed at 1:1", NULL);
                        raster_context_destroy(ctx); raster_surface_release(target);
                        raster_surface_release(image);
                        return;
                    }

                    const uint8_t *src = raster_surface_bytes(image);
                    const uint8_t *dst = raster_surface_bytes(target);
                    size_t sstride = raster_surface_stride(image);
                    size_t dstride = raster_surface_stride(target);
                    ++checks;
                    for (int y = 0; y < 7; ++y)
                        if (memcmp(dst + (size_t)(y + oy) * dstride + (size_t)ox * bpp,
                                   src + (size_t)y * sstride, 11 * bpp) != 0) {
                            char buf[200];
                            snprintf(buf, sizeof buf,
                                     "%s quality %zu aa %d shifted %d: row %d differs",
                                     format == RASTER_GRAY8 ? "GRAY8" : "RGBA8", q, aa, shifted, y);
                            fail("a 1:1 draw must be an exact selection", buf);
                            raster_context_destroy(ctx); raster_surface_release(target);
                            raster_surface_release(image);
                            return;
                        }
                    raster_context_destroy(ctx);
                    raster_surface_release(target);
                }
            }
            raster_surface_release(image);
        }
    }
}

// Which source column each destination column reads, recovered by drawing a horizontal ramp
// whose every column is a distinct value.
static void columns_read(raster_surface *target, raster_context *ctx, raster_surface *image,
                         raster_frect rect, int outWidth, int *out) {
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
    raster_context_set_blend(ctx, RASTER_BLEND_COPY);
    raster_context_set_antialias(ctx, false);
    raster_context_draw_image(ctx, image, rect);
    const uint8_t *p = raster_surface_bytes(target);
    for (int x = 0; x < outWidth; ++x) out[x] = p[x];  // GRAY8, row 0
}

static void test_nearest_tie_break(void) {
    // A GRAY8 ramp where column i holds i, so the value read *is* the column index.
    const int W = 6;
    raster_surface *image = raster_surface_create((size_t)W, 1, RASTER_GRAY8);
    uint8_t *ip = raster_surface_mutable_bytes(image);
    for (int i = 0; i < W; ++i) ip[i] = (uint8_t)i;

    struct { const char *name; int outWidth; double rectWidth; double rectX; int want[6]; } cases[] = {
        // 1:1 — the case where round() applied to the same expression shifts everything.
        { "1:1", 6, 6.0, 0.0, { 0, 1, 2, 3, 4, 5 } },
        { "1:1 at x=3", 6, 6.0, 3.0, { 0, 1, 2, 3, 4, 5 } },
        // 2x magnification — corner-to-corner agrees here, which is why it needs 0.5x too.
        { "2x", 6, 12.0, 0.0, { 0, 0, 1, 1, 2, 2 } },
        // 0.5x reduction — this is where corner-to-corner diverges: it would read 0,2,4.
        { "0.5x", 3, 3.0, 0.0, { 1, 3, 5, 0, 0, 0 } },
        // A non-integer origin, where corner-to-corner also diverges.
        { "1:1 at x=2.5", 6, 6.0, 2.5, { 0, 1, 2, 3, 4, 5 } },
    };

    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
        int outWidth = cases[k].outWidth;
        raster_surface *target = raster_surface_create((size_t)(outWidth + 4), 1, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        raster_frect rect = { cases[k].rectX, 0.0, cases[k].rectWidth, 1.0 };
        int got[6] = { 0, 0, 0, 0, 0, 0 };
        int raw[10];
        columns_read(target, ctx, image, rect, outWidth + 4, raw);
        for (int x = 0; x < outWidth; ++x) got[x] = raw[x + (int)cases[k].rectX];

        ++checks;
        for (int x = 0; x < outWidth; ++x)
            if (got[x] != cases[k].want[x]) {
                char buf[200];
                int n = snprintf(buf, sizeof buf, "%s: read", cases[k].name);
                for (int j = 0; j < outWidth; ++j)
                    n += snprintf(buf + n, sizeof buf - (size_t)n, " %d", got[j]);
                n += snprintf(buf + n, sizeof buf - (size_t)n, ", want");
                for (int j = 0; j < outWidth; ++j)
                    n += snprintf(buf + n, sizeof buf - (size_t)n, " %d", cases[k].want[j]);
                fail("nearest tie-break", buf);
                break;
            }
        raster_context_destroy(ctx);
        raster_surface_release(target);
    }
    raster_surface_release(image);
}

static void test_sampler_against_reference(void) {
    for (unsigned seed = 1; seed <= 400; ++seed) {
        rngState = seed * 2654435761u + 71;
        raster_format format = next_random() % 2 ? RASTER_RGBA8 : RASTER_GRAY8;
        size_t bpp = raster_bytes_per_pixel(format);
        int iw = 3 + (int)(next_random() % 9), ih = 3 + (int)(next_random() % 7);
        raster_surface *image = ramp_image(iw, ih, format, seed);

        int cw = 18, ch = 14;
        raster_surface *target = raster_surface_create((size_t)cw, (size_t)ch, format);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_set_blend(ctx, RASTER_BLEND_COPY);
        raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);

        raster_frect rect = { random_double(-2, 10), random_double(-2, 8),
                              random_double(1, 14), random_double(1, 11) };
        raster_context_draw_image(ctx, image, rect);

        // The reference: for every device pixel the draw could have touched, compute the
        // source coordinate independently and take the nearest sample.
        raster_matrix identity = { 1, 0, 0, 1, 0, 0 };
        raster_rect area;
        if (raster_device_rect_covered(identity, (raster_frect){ rect.x, (double)ch - rect.y - rect.height,
                                                                  rect.width, rect.height }, &area)) {
            const uint8_t *sp = raster_surface_bytes(image);
            const uint8_t *dp = raster_surface_bytes(target);
            size_t sstride = raster_surface_stride(image);
            size_t dstride = raster_surface_stride(target);
            for (int32_t y = area.y0; y < area.y1; ++y) {
                if (y < 0 || y >= ch) continue;
                for (int32_t x = area.x0; x < area.x1; ++x) {
                    if (x < 0 || x >= cw) continue;
                    double u, v;
                    reference_source(x, y, (double)ch, rect, iw, ih, &u, &v);
                    int si = (int)floor(u), sj = (int)floor(v);
                    if (si < 0) si = 0;
                    if (si >= iw) si = iw - 1;
                    if (sj < 0) sj = 0;
                    if (sj >= ih) sj = ih - 1;
                    ++checks;
                    if (memcmp(dp + (size_t)y * dstride + (size_t)x * bpp,
                               sp + (size_t)sj * sstride + (size_t)si * bpp, bpp) != 0) {
                        char buf[240];
                        snprintf(buf, sizeof buf,
                                 "seed %u %s: device (%d,%d) -> source (%d,%d) u=%.4f v=%.4f",
                                 seed, format == RASTER_GRAY8 ? "GRAY8" : "RGBA8",
                                 x, y, si, sj, u, v);
                        fail("nearest sampling disagrees with the reference", buf);
                        raster_context_destroy(ctx); raster_surface_release(target);
                        raster_surface_release(image);
                        return;
                    }
                }
            }
        }
        raster_context_destroy(ctx);
        raster_surface_release(target);
        raster_surface_release(image);
    }
}

// A piece of an image drawn on its own must match the same region of the whole. This is what
// the tiled renderer rests on, and it is why the source coordinate is computed in closed
// form per pixel rather than accumulated.
static void test_crop_invariance(void) {
    static const raster_interpolation qualities[] = {
        RASTER_INTERPOLATION_LOW, RASTER_INTERPOLATION_HIGH, RASTER_INTERPOLATION_NONE,
    };
    for (unsigned seed = 1; seed <= 120; ++seed) {
        for (size_t q = 0; q < 3; ++q) {
            rngState = seed * 40503u + 97;
            raster_surface *image = ramp_image(23, 5, RASTER_RGBA8, seed);
            raster_frect rect = { 1.0, 1.0, random_double(6, 30), 3.0 };
            int cw = 36, ch = 8;

            raster_surface *whole = raster_surface_create((size_t)cw, (size_t)ch, RASTER_RGBA8);
            raster_context *wc = raster_context_create(whole);
            raster_context_set_antialias(wc, false);
            raster_context_set_blend(wc, RASTER_BLEND_COPY);
            raster_context_set_interpolation(wc, qualities[q]);
            raster_context_draw_image(wc, image, rect);

            raster_surface *pieces = raster_surface_create((size_t)cw, (size_t)ch, RASTER_RGBA8);
            raster_context *pc = raster_context_create(pieces);
            raster_context_set_antialias(pc, false);
            raster_context_set_blend(pc, RASTER_BLEND_COPY);
            raster_context_set_interpolation(pc, qualities[q]);
            // Same draw, but each strip clipped to a slice. The clip changes; the geometry
            // handed to the sampler does not.
            for (int s = 0; s < 4; ++s) {
                raster_context_save(pc);
                raster_context_clip_rect(pc, (raster_frect){ (double)(s * 9), -10, 9, 30 });
                raster_context_draw_image(pc, image, rect);
                raster_context_restore(pc);
            }

            ++checks;
            const uint8_t *a = raster_surface_bytes(whole), *b = raster_surface_bytes(pieces);
            size_t astride = raster_surface_stride(whole), bstride = raster_surface_stride(pieces);
            for (int y = 0; y < ch; ++y)
                if (memcmp(a + (size_t)y * astride, b + (size_t)y * bstride, (size_t)cw * 4) != 0) {
                    char buf[160];
                    snprintf(buf, sizeof buf, "seed %u quality %zu: row %d differs", seed, q, y);
                    fail("a clipped piece differs from the whole draw", buf);
                    raster_context_destroy(wc); raster_context_destroy(pc);
                    raster_surface_release(whole); raster_surface_release(pieces);
                    raster_surface_release(image);
                    return;
                }
            raster_context_destroy(wc);
            raster_context_destroy(pc);
            raster_surface_release(whole);
            raster_surface_release(pieces);
            raster_surface_release(image);
        }
    }
}

// The reduction a single draw receives is not bounded by 2: the halving chain saturates at
// level 6, so a very large image landing small arrives here several times over, and the
// brush's stamp fallback can be far worse. A fixed two-tap filter collapses one-pixel
// stripes to a single value there, which is what this measures.
//
// The outermost pixels are excluded, because edge replication legitimately pulls them toward
// whichever border value they sit against — DownsampleTests.swift:58 excludes its own edges
// for the same reason ("The outermost pixels fade into the transparent edge; judge the
// inside").
static void test_reduction_does_not_alias(void) {
    struct { int source, target; const char *name; bool integerRatio; } sizes[] = {
        { 64, 8, "8x", true }, { 64, 16, "4x", true }, { 60, 7, "8.6x, non-integer", false },
    };
    struct { raster_interpolation quality; const char *name; } averaging[] = {
        { RASTER_INTERPOLATION_LOW, "low" },
        { RASTER_INTERPOLATION_DEFAULT, "default" },
        { RASTER_INTERPOLATION_HIGH, "high" },
        { RASTER_INTERPOLATION_MEDIUM, "medium" },
    };

    for (size_t s = 0; s < 3; ++s) {
        int W = sizes[s].source, outW = sizes[s].target;
        raster_surface *image = raster_surface_create((size_t)W, 1, RASTER_GRAY8);
        uint8_t *ip = raster_surface_mutable_bytes(image);
        for (int i = 0; i < W; ++i) ip[i] = (uint8_t)(i % 2 ? 255 : 0);

        for (size_t k = 0; k < 4; ++k) {
            raster_surface *target = raster_surface_create((size_t)outW, 1, RASTER_GRAY8);
            raster_context *ctx = raster_context_create(target);
            raster_context_set_antialias(ctx, false);
            raster_context_set_blend(ctx, RASTER_BLEND_COPY);
            raster_context_set_interpolation(ctx, averaging[k].quality);
            raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, (double)outW, 1 });

            const uint8_t *p = raster_surface_bytes(target);
            int lo = 255, hi = 0;
            double mean = 0;
            int n = 0;
            for (int x = 2; x < outW - 2; ++x) {
                if (p[x] < lo) lo = p[x];
                if (p[x] > hi) hi = p[x];
                mean += p[x];
                ++n;
            }
            ++checks;
            if (n > 0 && (hi - lo > 2 || fabs(mean / n - 127.5) > 2.0)) {
                char buf[200];
                snprintf(buf, sizeof buf, "%s at %s: interior %d..%d, mean %.1f",
                         averaging[k].name, sizes[s].name, lo, hi, mean / n);
                fail("a reduction must average one-pixel stripes to flat grey", buf);
            }
            // Exactly 128, not 127: an even split of 0 and 255 is 127.5, and the samples are
            // rounded rather than truncated on the way back to a byte.
            if (sizes[s].integerRatio && n > 0) {
                ++checks;
                if (lo != 128 || hi != 128) {
                    char buf[200];
                    snprintf(buf, sizeof buf, "%s at %s: interior %d..%d, want exactly 128",
                             averaging[k].name, sizes[s].name, lo, hi);
                    fail("the sample must be rounded, not truncated", buf);
                }
            }
            raster_context_destroy(ctx);
            raster_surface_release(target);
        }

        // The control, which proves the measurement is sensitive: at an integer ratio
        // nearest neighbour lands on the same parity every time and loses the stripes
        // entirely. At a non-integer ratio the sample point drifts across parities, so
        // nearest averages by accident and makes no control at all.
        if (!sizes[s].integerRatio) {
            raster_surface_release(image);
            continue;
        }
        raster_surface *target = raster_surface_create((size_t)outW, 1, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_set_blend(ctx, RASTER_BLEND_COPY);
        raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
        raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, (double)outW, 1 });
        const uint8_t *p = raster_surface_bytes(target);
        double mean = 0;
        for (int x = 0; x < outW; ++x) mean += p[x];
        ++checks;
        if (fabs(mean / outW - 127.5) < 40.0) {
            char buf[160];
            snprintf(buf, sizeof buf, "nearest at %s averaged to %.1f; it should not",
                     sizes[s].name, mean / outW);
            fail("the aliasing measurement is not sensitive", buf);
        }
        raster_context_destroy(ctx);
        raster_surface_release(target);
        raster_surface_release(image);
    }
}

// Samples that fall outside the image replicate its edge. Getting this wrong is invisible in
// the interior and only shows at the border, so it needs its own test: a two-pixel ramp
// magnified must reach the *near* edge's value at each end, not the far one's.
static void test_edge_replication(void) {
    raster_surface *image = raster_surface_create(2, 2, RASTER_GRAY8);
    uint8_t *ip = raster_surface_mutable_bytes(image);
    size_t istride = raster_surface_stride(image);
    ip[0] = 0;   ip[1] = 255;             // top row: dark left, bright right
    ip[istride] = 0; ip[istride + 1] = 255;

    raster_surface *target = raster_surface_create(8, 8, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    raster_context_set_antialias(ctx, false);
    raster_context_set_blend(ctx, RASTER_BLEND_COPY);
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_LOW);
    raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 8, 8 });

    const uint8_t *p = raster_surface_bytes(target);
    size_t stride = raster_surface_stride(target);
    const uint8_t *row = p + 3 * stride;
    CHECK(row[0] <= 4, "the left border replicates the dark edge", NULL);
    CHECK(row[7] >= 251, "the right border replicates the bright edge, not the left one", NULL);
    for (int x = 1; x < 8; ++x) {
        ++checks;
        if (row[x] < row[x - 1]) {
            fail("a magnified ramp must stay monotonic across the border", NULL);
            break;
        }
    }

    // The same vertically, with a ramp down the rows.
    ip[0] = 0; ip[1] = 0;
    ip[istride] = 255; ip[istride + 1] = 255;
    raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 8, 8 });
    // Row 0 of the source lands at device row 0, so device row 0 is dark and row 7 bright.
    CHECK(p[0 * stride] <= 4, "the top border replicates the top edge", NULL);
    CHECK(p[7 * stride] >= 251, "the bottom border replicates the bottom edge", NULL);

    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(image);
}

// `.high` must actually be Catmull-Rom and not a second name for bilinear. They agree
// everywhere the other tests look — both are exact at 1:1 and both average a reduction — so
// the difference only shows as ringing when a step edge is magnified.
static void test_high_is_not_bilinear(void) {
    raster_surface *image = raster_surface_create(4, 1, RASTER_GRAY8);
    uint8_t *ip = raster_surface_mutable_bytes(image);
    ip[0] = 64; ip[1] = 64; ip[2] = 192; ip[3] = 192;

    uint8_t low[16], high[16];
    struct { raster_interpolation quality; uint8_t *out; } runs[] = {
        { RASTER_INTERPOLATION_LOW, low }, { RASTER_INTERPOLATION_HIGH, high },
    };
    for (size_t k = 0; k < 2; ++k) {
        raster_surface *target = raster_surface_create(16, 1, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_set_blend(ctx, RASTER_BLEND_COPY);
        raster_context_set_interpolation(ctx, runs[k].quality);
        raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 16, 1 });
        memcpy(runs[k].out, raster_surface_bytes(target), 16);
        raster_context_destroy(ctx);
        raster_surface_release(target);
    }

    CHECK(memcmp(low, high, 16) != 0, "high and low must not be the same filter", NULL);

    // Bilinear interpolates strictly between its two taps, so it can never leave the source
    // range. Catmull-Rom's negative lobes must, which is what makes it sharper.
    int lowOutside = 0, highOutside = 0;
    for (int x = 0; x < 16; ++x) {
        if (low[x] < 64 || low[x] > 192) ++lowOutside;
        if (high[x] < 64 || high[x] > 192) ++highOutside;
    }
    CHECK(lowOutside == 0, "bilinear stays inside the source range", NULL);
    CHECK(highOutside > 0, "Catmull-Rom overshoots a step edge; a tent would not", NULL);

    raster_surface_release(image);
}

// No colour channel may come out above its own alpha. Catmull-Rom's negative lobes make that
// reachable: where alpha dips and a colour channel does not, the weighted sums cross, and in
// premultiplied space the result is an edge that glows. DownsampleTests.swift:72 asserts the
// same invariant from the app's side, and LiveMaskTests.swift:19-20 the converse.
//
// The shapes matter. Random premultiplied noise never triggers it, because colour and alpha
// ring together; it needs alpha to undershoot where the colour does not.
static void test_premultiplied_stays_valid(void) {
    raster_surface *image = raster_surface_create(4, 1, RASTER_RGBA8);
    uint8_t *ip = raster_surface_mutable_bytes(image);
    const uint8_t alpha[4] = { 255, 255, 0, 255 };

    // Every channel gets its own spike, at its own distance from the hole at index 2. A single
    // red spike would let a clamp that only ever looks at channel 0 pass: mutation testing
    // found exactly that. Different patterns per channel also catch a clamp that decides once
    // and applies the same answer to all three. Each row is valid premultiplied input --
    // colour never exceeds alpha -- so any violation downstream is the resampler's doing.
    const uint8_t colour[3][4] = {
        { 0, 255, 0, 0 },    // red:   adjacent to the hole from the left
        { 0, 0, 0, 255 },    // green: adjacent from the right
        { 255, 0, 0, 0 },    // blue:  two pixels away, so its lobe reaches further
    };
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) ip[i * 4 + c] = colour[c][i];
        ip[i * 4 + 3] = alpha[i];
    }

    raster_surface *target = raster_surface_create(16, 1, RASTER_RGBA8);
    raster_context *ctx = raster_context_create(target);
    raster_context_set_antialias(ctx, false);
    raster_context_set_blend(ctx, RASTER_BLEND_COPY);
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_HIGH);
    raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 16, 1 });

    const uint8_t *p = raster_surface_bytes(target);
    static const char *const names[3] = { "red", "green", "blue" };
    for (int x = 0; x < 16; ++x) {
        for (int c = 0; c < 3; ++c) {
            ++checks;
            if (p[x * 4 + c] > p[x * 4 + 3]) {
                char buf[160];
                snprintf(buf, sizeof buf, "pixel %d has %s %u above alpha %u",
                         x, names[c], p[x * 4 + c], p[x * 4 + 3]);
                fail("a resampled colour must not exceed its own alpha", buf);
                break;
            }
        }
    }
    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(image);
}

static void test_draw_respects_state(void) {
    raster_surface *image = raster_surface_create(4, 4, RASTER_RGBA8);
    uint8_t *ip = raster_surface_mutable_bytes(image);
    size_t istride = raster_surface_stride(image);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t *q = ip + (size_t)y * istride + (size_t)x * 4;
            q[0] = 255; q[1] = 0; q[2] = 0; q[3] = 255;
        }

    // Alpha scales the source.
    {
        raster_surface *target = raster_surface_create(4, 4, RASTER_RGBA8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_set_alpha(ctx, 0.5);
        raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 });
        CHECK(raster_surface_bytes(target)[3] == 128, "setAlpha scales an image draw", NULL);
        raster_context_destroy(ctx); raster_surface_release(target);
    }
    // The clip applies.
    {
        raster_surface *target = raster_surface_create(4, 4, RASTER_RGBA8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_clip_rect(ctx, (raster_frect){ 2, 0, 2, 4 });
        raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 });
        const uint8_t *p = raster_surface_bytes(target);
        CHECK(p[0 * 4 + 3] == 0, "outside the clip nothing is drawn", NULL);
        CHECK(p[2 * 4 + 3] == 255, "inside it the image is", NULL);
        raster_context_destroy(ctx); raster_surface_release(target);
    }
    // A rotated CTM is refused, a quarter turn is not.
    {
        raster_surface *target = raster_surface_create(8, 8, RASTER_RGBA8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_matrix(ctx, (raster_matrix){ cos(0.4), sin(0.4), -sin(0.4), cos(0.4), 0, 0 });
        CHECK(raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 })
                  == RASTER_UNSUPPORTED_TRANSFORM, "a rotated image draw is refused", NULL);
        raster_context_set_matrix(ctx, (raster_matrix){ cos(M_PI / 2), sin(M_PI / 2),
                                                        -sin(M_PI / 2), cos(M_PI / 2), 8, 0 });
        CHECK(raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 }) == RASTER_OK,
              "a quarter turn is accepted", NULL);
        raster_context_destroy(ctx); raster_surface_release(target);
    }
    // Mixing formats is refused rather than invented.
    {
        raster_surface *target = raster_surface_create(4, 4, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        CHECK(raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 })
                  == RASTER_UNSUPPORTED_FORMAT, "RGBA8 into GRAY8 is refused", NULL);
        raster_context_destroy(ctx); raster_surface_release(target);
    }
    // A snapshot of the context, drawn back into it, must see its own frozen pixels.
    // Drawing has to be the thing that detaches it: there is no fill in between, and
    // DownsampleCache's mask path is exactly this shape — snapshot, crop a row, draw the
    // row straight back into the context it came from.
    {
        raster_surface *target = raster_surface_create(4, 4, RASTER_RGBA8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_set_blend(ctx, RASTER_BLEND_COPY);
        raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 });

        raster_surface *snapshot = raster_context_make_snapshot(ctx);
        // Straight back in, with nothing in between to have detached it.
        raster_status status = raster_context_draw_image(ctx, snapshot, (raster_frect){ 0, 0, 4, 4 });
        CHECK(status == RASTER_OK, "drawing a live snapshot back in must succeed", NULL);
        CHECK(raster_surface_bytes(target)[0] == 255, "and reproduce the red it held", NULL);
        raster_surface_release(snapshot);
        raster_context_destroy(ctx); raster_surface_release(target);
    }
    // The same with a write in between, which is the sequence the fill path already covers.
    {
        raster_surface *target = raster_surface_create(4, 4, RASTER_RGBA8);
        raster_context *ctx = raster_context_create(target);
        raster_context_set_antialias(ctx, false);
        raster_context_set_blend(ctx, RASTER_BLEND_COPY);
        raster_context_draw_image(ctx, image, (raster_frect){ 0, 0, 4, 4 });
        raster_surface *snapshot = raster_context_make_snapshot(ctx);
        double black[4] = { 0, 0, 0, 1 };
        raster_context_set_fill_color(ctx, black);
        raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 4, 4 });
        raster_context_draw_image(ctx, snapshot, (raster_frect){ 0, 0, 4, 4 });
        CHECK(raster_surface_bytes(target)[0] == 255,
              "the snapshot still held the red it was taken from", NULL);
        raster_surface_release(snapshot);
        raster_context_destroy(ctx); raster_surface_release(target);
    }
    raster_surface_release(image);
}

static void test_crop_intersects(void) {
    raster_surface *surface = raster_surface_create(10, 10, RASTER_RGBA8);

    raster_surface *inside = raster_surface_crop(surface, 2, 2, 4, 4);
    CHECK(inside != NULL && raster_surface_width(inside) == 4, "a contained crop is exact", NULL);
    raster_surface_release(inside);

    // Overhanging is intersected, not refused: RasterSnapshot drops a whole patch when a
    // crop comes back empty, so refusing here would silently lose painted pixels.
    raster_surface *over = raster_surface_crop(surface, 8, 8, 5, 5);
    CHECK(over != NULL, "an overhanging crop is intersected, not refused", NULL);
    CHECK(over && raster_surface_width(over) == 2 && raster_surface_height(over) == 2,
          "and comes back clipped to what exists", NULL);
    raster_surface_release(over);

    CHECK(raster_surface_crop(surface, 10, 0, 1, 1) == NULL, "starting past the edge is empty", NULL);
    CHECK(raster_surface_crop(surface, 0, 10, 1, 1) == NULL, "likewise vertically", NULL);
    CHECK(raster_surface_crop(surface, 0, 0, 0, 4) == NULL, "a zero extent is empty", NULL);
    CHECK(raster_surface_crop(NULL, 0, 0, 1, 1) == NULL, "NULL has nothing to crop", NULL);

    raster_surface_release(surface);
}

// MARK: - Stage 4: mask clips and the coverage plane

// BrushRaster.draw(image, in:, mask: true, context:) verbatim. Its shape is the point: fill
// black, clip to the mask, fill white. On GRAY8 that is a lerp from 0 to 255 by the mask's
// own value, so at 1:1 the result must come back equal to the mask, byte for byte.
static void brush_raster_draw_mask(raster_context *ctx, const raster_surface *mask,
                                   raster_frect rect) {
    raster_context_save(ctx);
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
    // A bitmap context starts with CoreGraphics' flip already in its CTM and
    // BrushRaster.context's translate(0,h) + scale(1,-1) cancels it exactly, leaving user
    // space equal to device pixels. What remains is BrushRaster.draw's own flip.
    raster_context_set_matrix(ctx, (raster_matrix){ 1, 0, 0, -1, rect.x, rect.y + rect.height });

    raster_frect bounds = { 0, 0, rect.width, rect.height };
    static const double black[4] = { 0, 0, 0, 1 };
    static const double white[4] = { 1, 1, 1, 1 };
    raster_context_set_fill_color(ctx, black);
    raster_context_fill_rect(ctx, bounds);
    CHECK(raster_context_clip_mask(ctx, mask, bounds) == RASTER_OK,
          "the mask idiom's clip is accepted", NULL);
    raster_context_set_fill_color(ctx, white);
    raster_context_fill_rect(ctx, bounds);
    raster_context_restore(ctx);
}

static void test_mask_idiom_reproduces_the_mask(void) {
    static const struct { size_t w, h; } sizes[] = { { 8, 8 }, { 37, 23 }, { 1, 1 }, { 1, 9 }, { 9, 1 } };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        for (int antialias = 0; antialias <= 1; ++antialias) {
            size_t w = sizes[s].w, h = sizes[s].h;
            raster_surface *mask = raster_surface_create(w, h, RASTER_GRAY8);
            uint8_t *mp = raster_surface_mutable_bytes(mask);
            size_t mstride = raster_surface_stride(mask);
            for (size_t y = 0; y < h; ++y)
                for (size_t x = 0; x < w; ++x)
                    mp[y * mstride + x] = (uint8_t)((x * 37 + y * 91) % 256);

            raster_surface *target = raster_surface_create(w, h, RASTER_GRAY8);
            raster_context *ctx = raster_context_create(target);
            raster_context_set_antialias(ctx, antialias != 0);
            brush_raster_draw_mask(ctx, mask, (raster_frect){ 0, 0, (double)w, (double)h });

            const uint8_t *tp = raster_surface_bytes(target);
            size_t tstride = raster_surface_stride(target);
            bool same = true;
            for (size_t y = 0; y < h && same; ++y)
                for (size_t x = 0; x < w; ++x)
                    if (tp[y * tstride + x] != mp[y * mstride + x]) {
                        char buf[160];
                        snprintf(buf, sizeof buf, "%zux%zu aa=%d at (%zu,%zu): got %u want %u",
                                 w, h, antialias, x, y, tp[y * tstride + x], mp[y * mstride + x]);
                        fail("the mask idiom must reproduce its mask exactly", buf);
                        same = false;
                        break;
                    }
            ++checks;
            raster_context_destroy(ctx);
            raster_surface_release(target);
            raster_surface_release(mask);
        }
    }
}

// LayerMask.solid(revealing:) builds a 1x1 GRAY8 image and uses it as a clip over a whole
// layer, so a mask has to stretch to any size and stay flat while doing it.
static void test_one_by_one_mask_stretches(void) {
    for (unsigned value = 0; value <= 255; value += 17) {
        raster_surface *mask = raster_surface_create(1, 1, RASTER_GRAY8);
        raster_surface_mutable_bytes(mask)[0] = (uint8_t)value;

        raster_surface *target = raster_surface_create(11, 7, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        install_identity(ctx);
        raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_HIGH);
        static const double white[4] = { 1, 1, 1, 1 };
        CHECK(raster_context_clip_mask(ctx, mask, (raster_frect){ 0, 0, 11, 7 }) == RASTER_OK,
              "a 1x1 mask clips", NULL);
        raster_context_set_fill_color(ctx, white);
        raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 11, 7 });

        const uint8_t *tp = raster_surface_bytes(target);
        size_t tstride = raster_surface_stride(target);
        bool flat = true;
        for (int y = 0; y < 7 && flat; ++y)
            for (int x = 0; x < 11; ++x)
                if (tp[(size_t)y * tstride + (size_t)x] != (uint8_t)value) { flat = false; break; }
        ++checks;
        if (!flat) {
            char buf[120];
            snprintf(buf, sizeof buf, "value %u gave %u at (0,0)", value, tp[0]);
            fail("a 1x1 mask must stretch flat over the whole rect", buf);
        }
        raster_context_destroy(ctx);
        raster_surface_release(target);
        raster_surface_release(mask);
    }
}

// A uniform mask of value v, so the expected coverage is exactly v and the arithmetic can be
// checked in closed form rather than against another implementation of the same rounding.
static raster_surface *uniform_mask(size_t w, size_t h, uint8_t value) {
    raster_surface *mask = raster_surface_create(w, h, RASTER_GRAY8);
    uint8_t *p = raster_surface_mutable_bytes(mask);
    size_t stride = raster_surface_stride(mask);
    for (size_t y = 0; y < h; ++y) memset(p + y * stride, value, w);
    return mask;
}

static void test_nested_masks_multiply(void) {
    // CoreGraphics multiplies nested mask clips, which is what makes a folder mask compose
    // with a layer's own mask. LayerMask's own comment says so; this pins the arithmetic.
    for (unsigned a = 0; a <= 255; a += 15)
        for (unsigned b = 0; b <= 255; b += 15) {
            raster_surface *outer = uniform_mask(8, 8, (uint8_t)a);
            raster_surface *inner = uniform_mask(8, 8, (uint8_t)b);
            raster_surface *target = raster_surface_create(8, 8, RASTER_GRAY8);
            raster_context *ctx = raster_context_create(target);
            install_identity(ctx);
            static const double white[4] = { 1, 1, 1, 1 };
            raster_frect all = { 0, 0, 8, 8 };
            raster_context_clip_mask(ctx, outer, all);
            raster_context_clip_mask(ctx, inner, all);
            raster_context_set_fill_color(ctx, white);
            raster_context_fill_rect(ctx, all);

            uint8_t expected = (uint8_t)((a * b + 127) / 255);
            uint8_t got = raster_surface_bytes(target)[0];
            char buf[120];
            snprintf(buf, sizeof buf, "%u then %u gave %u, expected %u", a, b, got, expected);
            CHECK(got == expected, "nested mask clips multiply", buf);

            raster_context_destroy(ctx);
            raster_surface_release(target);
            raster_surface_release(inner);
            raster_surface_release(outer);
        }
}

static void test_mask_clip_restores(void) {
    // The parent's plane must survive a child's. Nothing here copies it -- a nested clip
    // builds its own -- so what this really checks is that nothing writes through it.
    raster_surface *outer = uniform_mask(8, 8, 200);
    raster_surface *inner = uniform_mask(8, 8, 100);
    raster_surface *target = raster_surface_create(8, 8, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    install_identity(ctx);
    static const double white[4] = { 1, 1, 1, 1 };
    raster_frect all = { 0, 0, 8, 8 };
    raster_context_set_fill_color(ctx, white);

    raster_context_clip_mask(ctx, outer, all);
    raster_context_save(ctx);
    raster_context_clip_mask(ctx, inner, all);
    raster_context_fill_rect(ctx, all);
    uint8_t nested = raster_surface_bytes(target)[0];
    raster_context_restore(ctx);

    // Reset the canvas by hand rather than with clear_rect: clear honours the clip, so under
    // the outer mask it would only partly erase and the second fill would measure the
    // leftovers instead of the plane.
    uint8_t *tp = raster_surface_mutable_bytes(target);
    for (size_t row = 0; row < 8; ++row)
        memset(tp + row * raster_surface_stride(target), 0, 8);
    raster_context_fill_rect(ctx, all);
    uint8_t restored = raster_surface_bytes(target)[0];

    CHECK(nested == (uint8_t)((200 * 100 + 127) / 255), "the nested clip multiplied", NULL);
    CHECK(restored == 200, "and restoreGState left the outer plane untouched", NULL);

    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(inner);
    raster_surface_release(outer);
}

// The subtle one. A mask clip positions its plane at the region's bounds; a later rectangle
// clip shrinks the region, and the plane has to be re-based onto the new bounds or every
// lookup after it is offset. A uniform mask cannot catch that -- the values have to vary.
static void test_plane_rebases_after_a_rect_clip(void) {
    raster_surface *mask = raster_surface_create(16, 16, RASTER_GRAY8);
    uint8_t *mp = raster_surface_mutable_bytes(mask);
    size_t mstride = raster_surface_stride(mask);
    for (size_t y = 0; y < 16; ++y)
        for (size_t x = 0; x < 16; ++x)
            mp[y * mstride + x] = (uint8_t)(x * 16 + y);

    for (int trial = 0; trial < 2; ++trial) {
        raster_surface *target = raster_surface_create(16, 16, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        install_identity(ctx);
        static const double white[4] = { 1, 1, 1, 1 };
        raster_context_set_fill_color(ctx, white);

        raster_frect all = { 0, 0, 16, 16 };
        raster_frect inner = { 5, 3, 7, 9 };
        // Once with the rectangle clip after the mask, once before: the plane is built over
        // the already-narrowed region in the second case and cropped in the first, and both
        // have to land on the same pixels.
        if (trial == 0) {
            raster_context_clip_mask(ctx, mask, all);
            raster_context_clip_rect(ctx, inner);
        } else {
            raster_context_clip_rect(ctx, inner);
            raster_context_clip_mask(ctx, mask, all);
        }
        raster_context_fill_rect(ctx, all);

        const uint8_t *tp = raster_surface_bytes(target);
        size_t tstride = raster_surface_stride(target);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                bool in = x >= 5 && x < 12 && y >= 3 && y < 12;
                // A mask is placed exactly as a drawn image is, source row 0 at the rect's
                // *maximum* y, so under an identity CTM it reads bottom-up. BrushRaster.draw
                // uses one preamble for its mask branch and its image branch, which is what
                // says the two placements are the same thing.
                uint8_t want = in ? mp[(size_t)(15 - y) * mstride + (size_t)x] : 0;
                uint8_t got = tp[(size_t)y * tstride + (size_t)x];
                ++checks;
                if (got != want) {
                    char buf[160];
                    snprintf(buf, sizeof buf, "trial %d at (%d,%d): got %u want %u",
                             trial, x, y, got, want);
                    fail("a mask plane must survive a later rectangle clip", buf);
                    y = 16;
                    break;
                }
            }
        raster_context_destroy(ctx);
        raster_surface_release(target);
    }
    raster_surface_release(mask);
}

static void test_mask_clip_applies_to_image_draws(void) {
    // LayerRenderer clips to a mask and then draws an image through it, so the plane has to
    // reach the image path and not only the fill path.
    raster_surface *image = raster_surface_create(8, 8, RASTER_GRAY8);
    uint8_t *ip = raster_surface_mutable_bytes(image);
    size_t istride = raster_surface_stride(image);
    for (size_t y = 0; y < 8; ++y) memset(ip + y * istride, 255, 8);

    raster_surface *mask = uniform_mask(8, 8, 64);
    raster_surface *target = raster_surface_create(8, 8, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    install_identity(ctx);
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
    raster_frect all = { 0, 0, 8, 8 };
    raster_context_clip_mask(ctx, mask, all);
    // The image is drawn under the context's own flip, which puts source row 0 at the
    // rect's maximum y; a uniform image makes that irrelevant and keeps this about coverage.
    raster_context_draw_image(ctx, image, all);

    CHECK(raster_surface_bytes(target)[0] == 64,
          "a mask clip attenuates an image draw", NULL);

    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(mask);
    raster_surface_release(image);
}

static void test_mask_clip_refusals(void) {
    raster_surface *gray = uniform_mask(4, 4, 255);
    raster_surface *colour = raster_surface_create(4, 4, RASTER_RGBA8);
    raster_surface *target = raster_surface_create(8, 8, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    install_identity(ctx);
    raster_frect all = { 0, 0, 8, 8 };

    // A format refusal and a transform refusal are separate statuses on purpose: they are
    // told apart only by the message they produce, and a wrong diagnosis costs an hour.
    CHECK(raster_context_clip_mask(ctx, colour, all) == RASTER_UNSUPPORTED_FORMAT,
          "a colour mask is refused rather than guessed at", NULL);
    CHECK(raster_context_clip_mask(ctx, NULL, all) == RASTER_UNSUPPORTED_FORMAT,
          "so is a missing mask", NULL);

    // A rotation: the app reaches this through LayerRenderer and FolderMaskClip, and it has
    // to refuse rather than silently clip to an axis-aligned approximation.
    raster_context_set_matrix(ctx, (raster_matrix){ 0.8, 0.6, -0.6, 0.8, 0, 0 });
    CHECK(raster_context_clip_mask(ctx, gray, all) == RASTER_UNSUPPORTED_TRANSFORM,
          "a rotated mask clip is refused", NULL);

    // A quarter turn is rectilinear and must still be accepted.
    raster_context_set_matrix(ctx, (raster_matrix){ 6.1e-17, 1, -1, 6.1e-17, 8, 0 });
    CHECK(raster_context_clip_mask(ctx, gray, all) == RASTER_OK,
          "a quarter turn is rectilinear and is accepted", NULL);

    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(colour);
    raster_surface_release(gray);
}

static void test_mask_clip_bounds_ignore_the_mask(void) {
    // boundingBoxOfClipPath is read as geometry: AdjustmentSurface sizes an offscreen from it
    // and Grain anchors its pattern to the origin. A mask that is black along one edge must
    // not move it, or the grain visibly shifts.
    raster_surface *mask = raster_surface_create(8, 8, RASTER_GRAY8);
    uint8_t *mp = raster_surface_mutable_bytes(mask);
    size_t mstride = raster_surface_stride(mask);
    for (size_t y = 0; y < 8; ++y)
        for (size_t x = 0; x < 8; ++x)
            mp[y * mstride + x] = (x < 3 || y < 2) ? 0 : 255;

    raster_surface *target = raster_surface_create(8, 8, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    install_identity(ctx);
    raster_context_clip_mask(ctx, mask, (raster_frect){ 1, 1, 6, 6 });

    raster_rect bounds;
    CHECK(raster_context_clip_bounds(ctx, &bounds), "a mask clip is not an empty clip", NULL);
    CHECK(bounds.x0 == 1 && bounds.y0 == 1 && bounds.x1 == 7 && bounds.y1 == 7,
          "the bounds are the mask's rectangle, not its content", NULL);

    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(mask);
}

// The mask clip's *rectangle* clips hard, by the centre rule, exactly like every other clip
// here. Antialiasing does not soften it -- softness comes only from the mask's values. With
// integer edges the two rules agree, so this needs a fractional rectangle to say anything.
static void test_mask_rect_clips_hard(void) {
    raster_surface *mask = uniform_mask(16, 16, 255);
    raster_surface *target = raster_surface_create(16, 16, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    install_identity(ctx);
    raster_context_set_antialias(ctx, true);
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
    static const double white[4] = { 1, 1, 1, 1 };
    raster_context_set_fill_color(ctx, white);

    raster_frect soft = { 2.4, 1.6, 5.2, 4.3 };
    raster_context_clip_mask(ctx, mask, soft);
    raster_context_save(ctx);
    raster_context_set_antialias(ctx, false);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 16, 16 });
    raster_context_restore(ctx);

    const uint8_t *tp = raster_surface_bytes(target);
    size_t tstride = raster_surface_stride(target);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            // Centre in, or out. Nothing in between.
            double cx = x + 0.5, cy = y + 0.5;
            bool in = cx >= soft.x && cx < soft.x + soft.width
                   && cy >= soft.y && cy < soft.y + soft.height;
            uint8_t want = in ? 255 : 0;
            uint8_t got = tp[(size_t)y * tstride + (size_t)x];
            ++checks;
            if (got != want) {
                char buf[140];
                snprintf(buf, sizeof buf, "at (%d,%d): got %u want %u", x, y, got, want);
                fail("a mask clip's rectangle clips hard even with antialiasing on", buf);
                y = 16;
                break;
            }
        }
    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(mask);
}

// A soft fill edge crossing a mask has to multiply the two coverages, with the same rounding
// as everywhere else. BrushRaster's contexts have antialiasing on and clip to masks, so this
// combination is the app's normal case, not a corner.
static void test_antialiased_fill_through_a_mask(void) {
    for (unsigned value = 0; value <= 255; value += 5) {
        raster_surface *mask = uniform_mask(16, 8, (uint8_t)value);
        raster_surface *target = raster_surface_create(16, 8, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        install_identity(ctx);
        raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
        raster_context_set_antialias(ctx, true);
        static const double white[4] = { 1, 1, 1, 1 };
        raster_context_set_fill_color(ctx, white);
        raster_context_clip_mask(ctx, mask, (raster_frect){ 0, 0, 16, 8 });
        // Fractional in x only, so every row is fully covered and the column coverage is the
        // whole story.
        raster_context_fill_rect(ctx, (raster_frect){ 1.3, 0, 4.1, 8 });

        uint8_t columns[16];
        raster_axis_coverage(1.3, 5.4, 0, 16, columns);

        const uint8_t *tp = raster_surface_bytes(target);  // one row, so no stride arithmetic
        for (int x = 0; x < 16; ++x) {
            uint8_t combined = (uint8_t)((columns[x] * value + 127) / 255);
            uint8_t want = 0;
            raster_fill_row_gray(&want, 255, 1, RASTER_BLEND_NORMAL, 255, &combined);
            uint8_t got = tp[x];
            ++checks;
            if (got != want) {
                char buf[170];
                snprintf(buf, sizeof buf,
                         "mask %u at x=%d: got %u want %u (column coverage %u)",
                         value, x, got, want, columns[x]);
                fail("an antialiased edge through a mask multiplies both coverages", buf);
                break;
            }
        }
        raster_context_destroy(ctx);
        raster_surface_release(target);
        raster_surface_release(mask);
    }
}

// Nesting where the inner rectangle sits somewhere else. The parent's plane is positioned at
// the parent region's bounds, so reading it for the narrower child needs both offsets; with
// equal-sized rectangles both are zero and a missing one is invisible.
static void test_nested_masks_at_an_offset(void) {
    raster_surface *outer = raster_surface_create(16, 16, RASTER_GRAY8);
    uint8_t *op = raster_surface_mutable_bytes(outer);
    size_t ostride = raster_surface_stride(outer);
    for (size_t y = 0; y < 16; ++y)
        for (size_t x = 0; x < 16; ++x)
            op[y * ostride + x] = (uint8_t)(17 + x * 13 + y * 3);

    raster_surface *inner = raster_surface_create(8, 9, RASTER_GRAY8);
    uint8_t *inp = raster_surface_mutable_bytes(inner);
    size_t instride = raster_surface_stride(inner);
    for (size_t y = 0; y < 9; ++y)
        for (size_t x = 0; x < 8; ++x)
            inp[y * instride + x] = (uint8_t)(200 - x * 11 - y * 7);

    raster_surface *target = raster_surface_create(16, 16, RASTER_GRAY8);
    raster_context *ctx = raster_context_create(target);
    install_identity(ctx);
    raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
    raster_context_set_antialias(ctx, false);
    static const double white[4] = { 1, 1, 1, 1 };
    raster_context_set_fill_color(ctx, white);

    raster_frect innerRect = { 5, 3, 8, 9 };
    raster_context_clip_mask(ctx, outer, (raster_frect){ 0, 0, 16, 16 });
    raster_context_clip_mask(ctx, inner, innerRect);
    raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 16, 16 });

    const uint8_t *tp = raster_surface_bytes(target);
    size_t tstride = raster_surface_stride(target);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            bool in = x >= 5 && x < 13 && y >= 3 && y < 12;
            uint8_t want = 0;
            if (in) {
                // Both masks are 1:1 over their own rectangles and both are placed bottom-up.
                uint8_t a = op[(size_t)(15 - y) * ostride + (size_t)x];
                uint8_t b = inp[(size_t)(11 - y) * instride + (size_t)(x - 5)];
                want = (uint8_t)((a * b + 127) / 255);
            }
            uint8_t got = tp[(size_t)y * tstride + (size_t)x];
            ++checks;
            if (got != want) {
                char buf[150];
                snprintf(buf, sizeof buf, "at (%d,%d): got %u want %u", x, y, got, want);
                fail("a nested mask clip reads the parent plane at the right offset", buf);
                y = 16;
                break;
            }
        }
    raster_context_destroy(ctx);
    raster_surface_release(target);
    raster_surface_release(inner);
    raster_surface_release(outer);
}

// The mask is resampled at the state's interpolation quality. FolderMaskClip sets it from the
// layer's sampling mode immediately before clipping, so a clip that ignored it would quietly
// give every folder mask nearest-neighbour edges.
static void test_mask_clip_honours_interpolation(void) {
    uint8_t results[2][8];
    static const raster_interpolation qualities[2] = {
        RASTER_INTERPOLATION_NONE, RASTER_INTERPOLATION_HIGH,
    };
    for (int q = 0; q < 2; ++q) {
        raster_surface *mask = raster_surface_create(2, 1, RASTER_GRAY8);
        uint8_t *mp = raster_surface_mutable_bytes(mask);
        mp[0] = 0; mp[1] = 240;

        raster_surface *target = raster_surface_create(8, 1, RASTER_GRAY8);
        raster_context *ctx = raster_context_create(target);
        install_identity(ctx);
        raster_context_set_antialias(ctx, false);
        raster_context_set_interpolation(ctx, qualities[q]);
        static const double white[4] = { 1, 1, 1, 1 };
        raster_context_set_fill_color(ctx, white);
        raster_context_clip_mask(ctx, mask, (raster_frect){ 0, 0, 8, 1 });
        raster_context_fill_rect(ctx, (raster_frect){ 0, 0, 8, 1 });

        const uint8_t *tp = raster_surface_bytes(target);
        for (int x = 0; x < 8; ++x) results[q][x] = tp[x];
        raster_context_destroy(ctx);
        raster_surface_release(target);
        raster_surface_release(mask);
    }

    // Nearest gives two flat blocks and nothing else.
    for (int x = 0; x < 8; ++x) {
        uint8_t want = x < 4 ? 0 : 240;
        char buf[120];
        snprintf(buf, sizeof buf, "at x=%d: got %u want %u", x, results[0][x], want);
        CHECK(results[0][x] == want, "nearest resamples a mask in flat blocks", buf);
    }
    // The smooth kernel must actually interpolate: somewhere across the step there has to be
    // a value that is neither endpoint, which a nearest-neighbour fallback can never produce.
    bool intermediate = false;
    for (int x = 0; x < 8; ++x)
        if (results[1][x] != 0 && results[1][x] != 240) intermediate = true;
    CHECK(intermediate, "a smooth quality resamples a mask smoothly", NULL);
}

// Against a per-pixel reference, over random masks, alphas and blend modes. The engine walks
// bands, strides and row pointers; the reference walks pixels and knows none of that.
static void test_mask_clip_against_reference(void) {
    static const raster_blend modes[] = {
        RASTER_BLEND_NORMAL, RASTER_BLEND_MULTIPLY, RASTER_BLEND_SCREEN, RASTER_BLEND_COPY,
    };
    for (unsigned seed = 1; seed <= 600; ++seed) {
        rngState = seed * 2246822519u + 7;

        raster_surface *mask = raster_surface_create(CW, CH, RASTER_GRAY8);
        uint8_t *mp = raster_surface_mutable_bytes(mask);
        size_t mstride = raster_surface_stride(mask);
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x)
                mp[(size_t)y * mstride + (size_t)x] = (uint8_t)(next_random() % 256);

        raster_surface *target = raster_surface_create(CW, CH, RASTER_GRAY8);
        uint8_t *tp = raster_surface_mutable_bytes(target);
        size_t tstride = raster_surface_stride(target);
        uint8_t reference[CH][CW];
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x)
                reference[y][x] = tp[(size_t)y * tstride + (size_t)x] = (uint8_t)(next_random() % 256);

        raster_blend blend = modes[next_random() % (sizeof modes / sizeof modes[0])];
        double alpha = (double)(next_random() % 5) / 4.0;
        double grey = (double)(next_random() % 256) / 255.0;

        raster_context *ctx = raster_context_create(target);
        install_identity(ctx);
        raster_context_set_antialias(ctx, false);
        raster_context_set_interpolation(ctx, RASTER_INTERPOLATION_NONE);
        raster_context_set_blend(ctx, blend);
        raster_context_set_alpha(ctx, alpha);
        double fill[4] = { grey, grey, grey, 1 };
        raster_context_set_fill_color(ctx, fill);
        raster_frect all = { 0, 0, CW, CH };
        raster_context_clip_mask(ctx, mask, all);
        raster_context_fill_rect(ctx, all);

        uint8_t source = (uint8_t)(grey * 255.0 + 0.5);
        uint8_t alpha8 = alpha <= 0 ? 0 : (alpha >= 1 ? 255 : (uint8_t)(alpha * 255.0 + 0.5));
        bool ok = true;
        for (int y = 0; y < CH && ok; ++y)
            for (int x = 0; x < CW; ++x) {
                // Bottom-up, for the placement reason spelled out above.
                uint8_t coverage = mp[(size_t)(CH - 1 - y) * mstride + (size_t)x];
                uint8_t want = reference[y][x];
                // One pixel at a time, exactly as the fill reference in stage 2 does: blend.c
                // is verified on its own, so what this isolates is the band walk and the
                // coverage lookup, not the arithmetic inside a row.
                raster_fill_row_gray(&want, source, 1, blend, alpha8, &coverage);
                uint8_t got = tp[(size_t)y * tstride + (size_t)x];
                ++checks;
                if (got != want) {
                    char buf[190];
                    snprintf(buf, sizeof buf,
                             "seed %u at (%d,%d): got %u want %u (mask %u, alpha %u, blend %u)",
                             seed, x, y, got, want, coverage, alpha8, blend);
                    fail("a mask clip must composite as a per-pixel coverage", buf);
                    ok = false;
                    break;
                }
            }

        raster_context_destroy(ctx);
        raster_surface_release(target);
        raster_surface_release(mask);
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

    test_crop_intersects();
    test_one_to_one_is_exact();
    test_nearest_tie_break();
    test_sampler_against_reference();
    test_crop_invariance();
    test_reduction_does_not_alias();
    test_edge_replication();
    test_high_is_not_bilinear();
    test_premultiplied_stays_valid();
    test_draw_respects_state();

    test_mask_idiom_reproduces_the_mask();
    test_one_by_one_mask_stretches();
    test_nested_masks_multiply();
    test_mask_clip_restores();
    test_plane_rebases_after_a_rect_clip();
    test_mask_clip_applies_to_image_draws();
    test_mask_clip_refusals();
    test_mask_clip_bounds_ignore_the_mask();
    test_mask_rect_clips_hard();
    test_antialiased_fill_through_a_mask();
    test_nested_masks_at_an_offset();
    test_mask_clip_honours_interpolation();
    test_mask_clip_against_reference();

    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
