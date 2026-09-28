#include "raster.h"

#include <math.h>

// The geometry of a user-space rectangle under a transform that is not rectilinear. Like
// coverage.c this file holds no context and allocates nothing, because it decides which
// pixels get painted and that has to be checkable against a brute-force oracle on its own.
//
// An affine map sends a rectangle to a parallelogram, never a general quadrilateral —
// opposite sides stay parallel. Two things follow, and together they are most of the file:
//
//   Containment is a test against the rectangle's own user-space edges, so the map back to
//   user space answers it with two comparisons per axis and no edge equations.
//
//   Along one device row, both user coordinates are affine in x. So the covered span of a
//   row is the intersection of at most four half-lines — solved in closed form, never by
//   walking pixels.
//
// The one thing here that was arrived at by being wrong first: the shape is *not* normalised
// to a unit square. Doing that divides by each rectangle's own width, so two abutting
// rectangles reach their shared edge through different arithmetic and disagree by an ulp.
// That is not theoretical — it doubled or dropped every pixel whose centre landed exactly on
// a tile boundary, which an oracle seeded with exactly-representable values hits constantly.
// Comparing against the user-space edges means both neighbours test the same literal.

// The same limit coverage.c uses, for the same reason: every integer conversion below has to
// be unreachable by a NaN or an infinity, because casting those is undefined and an optimised
// build turns it into an arbitrary answer rather than a crash.
#define RASTER_QUAD_COORD_LIMIT 1.0e9

static bool finite_coord(double v) {
    return isfinite(v) && v >= -RASTER_QUAD_COORD_LIMIT && v <= RASTER_QUAD_COORD_LIMIT;
}

bool raster_quad_make(raster_matrix m, raster_frect rect, raster_quad *out) {
    if (!out) return false;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.width) || !isfinite(rect.height))
        return false;
    if (!isfinite(m.a) || !isfinite(m.b) || !isfinite(m.c) || !isfinite(m.d)) return false;
    if (!isfinite(m.tx) || !isfinite(m.ty)) return false;

    // Standardise, as raster_device_box does: CGRect may carry a negative extent and
    // CoreGraphics normalises before using it, and TiledLayerRenderer produces such rects.
    double ux0 = rect.width >= 0 ? rect.x : rect.x + rect.width;
    double ux1 = rect.width >= 0 ? rect.x + rect.width : rect.x;
    double uy0 = rect.height >= 0 ? rect.y : rect.y + rect.height;
    double uy1 = rect.height >= 0 ? rect.y + rect.height : rect.y;
    if (!(ux1 > ux0) || !(uy1 > uy0)) return false;

    // Corners in order around the shape, so the polygon clip below sees a simple polygon
    // whichever way a mirrored transform winds it.
    const double cx[4] = { ux0, ux1, ux1, ux0 };
    const double cy[4] = { uy0, uy0, uy1, uy1 };
    for (int i = 0; i < 4; ++i) {
        out->x[i] = m.a * cx[i] + m.c * cy[i] + m.tx;
        out->y[i] = m.b * cx[i] + m.d * cy[i] + m.ty;
        if (!finite_coord(out->x[i]) || !finite_coord(out->y[i])) return false;
    }

    double determinant = m.a * m.d - m.b * m.c;
    if (determinant == 0.0 || !isfinite(determinant)) return false;

    raster_matrix toUser = {
        m.d / determinant, -m.b / determinant,
        -m.c / determinant, m.a / determinant,
        (m.c * m.ty - m.d * m.tx) / determinant,
        (m.b * m.tx - m.a * m.ty) / determinant,
    };
    if (!isfinite(toUser.a) || !isfinite(toUser.b) || !isfinite(toUser.c) ||
        !isfinite(toUser.d) || !isfinite(toUser.tx) || !isfinite(toUser.ty))
        return false;

    out->toUser = toUser;
    out->ux0 = ux0; out->ux1 = ux1;
    out->uy0 = uy0; out->uy1 = uy1;
    return true;
}

// Solves `low <= slope * t + offset < high` for the integer coordinates whose *centre* lies in
// the solution, as the half-open device interval `[min, max)`.
//
// Sorting the two solutions before applying the pixel rule is what makes this agree with
// raster_device_box, which does the same with fmin/fmax — and agreeing is not optional. The
// rectilinear selector is per-(matrix, rectangle), so one CTM can send a narrow rectangle down
// the integer path and a wide one down this path; if the two placed the half-open end
// differently they would disagree by a whole row wherever a mirrored transform is in force,
// and two adjacent pieces would double-draw or gap.
//
// Applying it in user space instead — `value >= low && value < high` — reads more naturally
// and is wrong for exactly that reason: a flip reverses which end is which. Both conventions
// tile exactly on their own, which is why this only shows up as a disagreement between paths.
static bool axis_range(double slope, double offset, double low, double high,
                       int32_t *lo, int32_t *hi) {
    double atLow = (low - offset) / slope;
    double atHigh = (high - offset) / slope;
    if (!finite_coord(atLow) || !finite_coord(atHigh)) return false;

    double dlo = atLow < atHigh ? atLow : atHigh;
    double dhi = atLow < atHigh ? atHigh : atLow;
    *lo = raster_pixel_edge(dlo);
    *hi = raster_pixel_edge(dhi);
    return *hi > *lo;
}

// One of the two user axes, as the constraint `low <= p*x + q*y + r < high` over device space.
//
// For a fixed row the constraint is affine in x, so when `p` is non-zero it gives a column
// range directly. When `p` is zero the axis does not vary along the row at all: the constraint
// is on y alone, and the row is either wholly in or wholly out. Both cannot be zero — that
// would make the transform singular, which raster_quad_make has already rejected.
static bool axis_columns(double p, double q, double r, double low, double high,
                         int32_t y, double yc, int32_t *lo, int32_t *hi) {
    if (p != 0.0) return axis_range(p, q * yc + r, low, high, lo, hi);

    int32_t rowLo, rowHi;
    if (!axis_range(q, r, low, high, &rowLo, &rowHi)) return false;
    if (y < rowLo || y >= rowHi) return false;
    *lo = (int32_t)-RASTER_QUAD_COORD_LIMIT;
    *hi = (int32_t)RASTER_QUAD_COORD_LIMIT;
    return true;
}

bool raster_quad_row_span(const raster_quad *quad, int32_t y, int32_t *x0, int32_t *x1) {
    if (!quad || !x0 || !x1) return false;

    double yc = (double)y + 0.5;
    const raster_matrix t = quad->toUser;

    // The one place the covered set is decided. Both axes resolve to integer column ranges
    // through raster_pixel_edge, and that collapse is what absorbs the last-bit disagreement
    // between two abutting rectangles about their shared edge.
    int32_t xLo, xHi, yLo, yHi;
    if (!axis_columns(t.a, t.c, t.tx, quad->ux0, quad->ux1, y, yc, &xLo, &xHi)) return false;
    if (!axis_columns(t.b, t.d, t.ty, quad->uy0, quad->uy1, y, yc, &yLo, &yHi)) return false;

    *x0 = xLo > yLo ? xLo : yLo;
    *x1 = xHi < yHi ? xHi : yHi;
    return *x1 > *x0;
}

// A row range guaranteed to contain every row raster_quad_row_span can accept.
//
// Deliberately a *candidate* range, not the answer: row_span is the single definition of the
// covered set, and anything else that decides a row — here, the shape's corners — is a second
// arithmetic for the same question and will disagree at a tie. It did: a corner-derived range
// stopped one row short of a row whose span came back non-empty, because one route divides to
// find the crossing and the other evaluates at the centre.
//
// One row of slack each side is provably enough rather than hopeful. A row's span is non-empty
// only if the row's own line meets both slabs and the two column ranges then overlap after
// rounding; a column is one unit wide, so that cannot happen more than one row beyond the
// shape's y extent.
static bool row_candidates(const raster_quad *quad, int32_t *first, int32_t *last) {
    double lo = quad->y[0], hi = quad->y[0];
    for (int i = 1; i < 4; ++i) {
        if (quad->y[i] < lo) lo = quad->y[i];
        if (quad->y[i] > hi) hi = quad->y[i];
    }
    if (!finite_coord(lo) || !finite_coord(hi)) return false;
    *first = raster_pixel_edge(lo) - 1;
    *last = raster_pixel_edge(hi) + 1;
    return *last > *first;
}

bool raster_quad_covered_bounds(const raster_quad *quad, raster_rect *out) {
    if (!quad || !out) return false;
    int32_t first, last;
    if (!row_candidates(quad, &first, &last)) return false;

    // Walking the rows rather than rounding the geometry out is what makes this exact, and
    // exactness is the point: a sliver narrower than a pixel has a non-empty geometric box
    // and covers nothing, and boundingBoxOfClipPath owes CGRect.null for that rather than a
    // rectangle. Allocation-free, so a rotated clip can ask without materialising a plane.
    bool any = false;
    raster_rect bounds = { 0, 0, 0, 0 };
    for (int32_t y = first; y < last; ++y) {
        int32_t x0, x1;
        if (!raster_quad_row_span(quad, y, &x0, &x1)) continue;
        if (!any) {
            bounds = (raster_rect){ x0, y, x1, y + 1 };
            any = true;
        } else {
            if (x0 < bounds.x0) bounds.x0 = x0;
            if (x1 > bounds.x1) bounds.x1 = x1;
            bounds.y1 = y + 1;
        }
    }
    if (!any) return false;
    *out = bounds;
    return true;
}

bool raster_quad_touched_bounds(const raster_quad *quad, raster_rect *out) {
    if (!quad || !out) return false;
    double xlo = quad->x[0], xhi = quad->x[0], ylo = quad->y[0], yhi = quad->y[0];
    for (int i = 1; i < 4; ++i) {
        if (quad->x[i] < xlo) xlo = quad->x[i];
        if (quad->x[i] > xhi) xhi = quad->x[i];
        if (quad->y[i] < ylo) ylo = quad->y[i];
        if (quad->y[i] > yhi) yhi = quad->y[i];
    }
    if (!finite_coord(xlo) || !finite_coord(xhi) || !finite_coord(ylo) || !finite_coord(yhi))
        return false;

    // Positive-area overlap: floor on the near edge, ceil on the far one, matching
    // raster_device_rect_touched. This is the set an antialiased draw writes, with fractional
    // coverage on the boundary — the covered set would cut those pixels away before the
    // coverage could soften them.
    out->x0 = (int32_t)floor(xlo);
    out->y0 = (int32_t)floor(ylo);
    out->x1 = (int32_t)ceil(xhi);
    out->y1 = (int32_t)ceil(yhi);
    return out->x1 > out->x0 && out->y1 > out->y0;
}

// MARK: - Area coverage

#define RASTER_QUAD_MAX_VERTICES 12

// Clips `count` vertices against a half-plane, writing the result to `outX`/`outY` and
// returning its vertex count. Sutherland-Hodgman, one edge at a time; positive is inside.
//
// A convex polygon clipped by a half-plane gains at most one vertex, so four clips over a
// quadrilateral stay well inside the bound above.
static size_t clip_half_plane(const double *inX, const double *inY, size_t count,
                              double nx, double ny, double offset,
                              double *outX, double *outY) {
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t j = (i + 1) % count;
        double di = nx * inX[i] + ny * inY[i] - offset;
        double dj = nx * inX[j] + ny * inY[j] - offset;
        if (di >= 0.0 && written < RASTER_QUAD_MAX_VERTICES) {
            outX[written] = inX[i];
            outY[written] = inY[i];
            ++written;
        }
        // A sign change means the edge crosses, so emit the crossing. Equality at either end
        // is not a crossing: that vertex was already emitted, or will be.
        if (((di > 0.0 && dj < 0.0) || (di < 0.0 && dj > 0.0)) &&
            written < RASTER_QUAD_MAX_VERTICES) {
            double factor = di / (di - dj);
            outX[written] = inX[i] + factor * (inX[j] - inX[i]);
            outY[written] = inY[i] + factor * (inY[j] - inY[i]);
            ++written;
        }
    }
    return written;
}

// Twice the signed area, by the shoelace formula.
//
// The sign is always positive here, and for a reason worth recording: what gets clipped is the
// *pixel square*, wound counter-clockwise, and clipping a convex polygon by half-planes
// preserves its orientation. So a mirrored transform — flipX, flipY — cannot flip this, because
// the shape's own winding never enters the calculation. `fabs` at the call site is therefore
// redundant, confirmed by mutation testing; it stays as the cheapest possible guard against a
// later change that clips the parallelogram instead.
static double double_area(const double *x, const double *y, size_t count) {
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        size_t j = (i + 1) % count;
        sum += x[i] * y[j] - x[j] * y[i];
    }
    return sum;
}

void raster_quad_coverage_row(const raster_quad *quad, int32_t x0, int32_t y, size_t count,
                              uint8_t *out) {
    if (!quad || !out || !count) return;

    // Each edge as a half-plane over the device point, so the clip below needs no per-vertex
    // mapping: u - ux0 >= 0, ux1 - u >= 0, and the same for v.
    const raster_matrix t = quad->toUser;
    const double planes[4][3] = {
        {  t.a,  t.c, quad->ux0 - t.tx },
        { -t.a, -t.c, t.tx - quad->ux1 },
        {  t.b,  t.d, quad->uy0 - t.ty },
        { -t.b, -t.d, t.ty - quad->uy1 },
    };

    for (size_t k = 0; k < count; ++k) {
        double px = (double)(x0 + (int32_t)k), py = (double)y;
        out[k] = 0;

        // The user coordinates of the pixel's four corners. Both are affine, so the corner
        // extremes bound the whole square: that settles fully-in and fully-out without any
        // clipping, which is every pixel but the boundary ones.
        double uLo = 0, uHi = 0, vLo = 0, vHi = 0;
        for (int corner = 0; corner < 4; ++corner) {
            double cxv = px + (corner == 1 || corner == 2 ? 1.0 : 0.0);
            double cyv = py + (corner >= 2 ? 1.0 : 0.0);
            double u = t.a * cxv + t.c * cyv + t.tx;
            double v = t.b * cxv + t.d * cyv + t.ty;
            if (!isfinite(u) || !isfinite(v)) return;
            if (corner == 0) { uLo = uHi = u; vLo = vHi = v; continue; }
            if (u < uLo) uLo = u;
            if (u > uHi) uHi = u;
            if (v < vLo) vLo = v;
            if (v > vHi) vHi = v;
        }
        if (uHi <= quad->ux0 || uLo >= quad->ux1 || vHi <= quad->uy0 || vLo >= quad->uy1)
            continue;
        if (uLo >= quad->ux0 && uHi <= quad->ux1 && vLo >= quad->uy0 && vHi <= quad->uy1) {
            out[k] = 255;
            continue;
        }

        // Clip the pixel square by the parallelogram's four edges. This way round rather than
        // clipping the parallelogram by the square keeps the input at four vertices whatever
        // the shape's size, and the two give the same intersection.
        double ax[RASTER_QUAD_MAX_VERTICES], ay[RASTER_QUAD_MAX_VERTICES];
        double bx[RASTER_QUAD_MAX_VERTICES], by[RASTER_QUAD_MAX_VERTICES];
        ax[0] = px;       ay[0] = py;
        ax[1] = px + 1.0; ay[1] = py;
        ax[2] = px + 1.0; ay[2] = py + 1.0;
        ax[3] = px;       ay[3] = py + 1.0;
        size_t n = 4;

        for (int plane = 0; plane < 4 && n > 0; ++plane) {
            n = clip_half_plane(ax, ay, n, planes[plane][0], planes[plane][1], planes[plane][2],
                                bx, by);
            for (size_t i = 0; i < n; ++i) { ax[i] = bx[i]; ay[i] = by[i]; }
        }
        if (n < 3) continue;

        double area = fabs(double_area(ax, ay, n)) * 0.5;
        if (!(area > 0.0)) continue;
        if (area >= 1.0) { out[k] = 255; continue; }
        // Round rather than truncate, the same convention raster_axis_coverage and the blend
        // code use, so a pixel the shape almost fills reads 255 and one it grazes reads 0.
        out[k] = (uint8_t)(area * 255.0 + 0.5);
    }
}
