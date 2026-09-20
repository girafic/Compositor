#include "raster.h"

#include <math.h>

// The arithmetic that decides which device pixels a user-space rectangle covers. It lives in
// its own file with no context and no allocation, because it is the most consequential code
// in the drawing path and it has to be testable against a brute-force oracle on its own.
//
// Everything here is written to survive three things that pointer-free arithmetic still gets
// wrong: negative coordinates, mirrored transforms, and non-finite inputs.

// The largest device coordinate worth carrying. Well inside int32_t, so the conversions
// below cannot overflow, and far outside any real canvas — a 4000-pixel document at a 64x
// zoom is 256000.
#define RASTER_COORD_LIMIT 1.0e9

static bool finite_box(const double box[4]) {
    for (int i = 0; i < 4; ++i)
        if (!isfinite(box[i]) || box[i] < -RASTER_COORD_LIMIT || box[i] > RASTER_COORD_LIMIT)
            return false;
    return true;
}

int32_t raster_pixel_edge(double t) {
    // ceil, not a cast. Every truncating shortcut — (int)(t + 0.5), (int)(t - 0.5) + 1 —
    // breaks for negative t, and device coordinates go negative whenever the canvas is
    // scrolled past its origin.
    // NaN has no sensible column; the infinities clamp, because their limit is meaningful
    // even though no caller should be passing one. raster_device_box rejects both before
    // they reach here, so this is the last line of defence rather than the first.
    if (isnan(t)) return 0;
    if (t < -RASTER_COORD_LIMIT) return (int32_t)-RASTER_COORD_LIMIT;
    if (t > RASTER_COORD_LIMIT) return (int32_t)RASTER_COORD_LIMIT;

    int32_t i = (int32_t)ceil(t - 0.5);

    // `t - 0.5` can lose its last bit when t is within an ulp of a half-integer, and then
    // ceil lands one column out. Adding 0.5 back to an integer is exact for any i a canvas
    // can reach, so the definition itself — the first column whose centre is at or after t —
    // can be checked directly.
    //
    // These two lines are the specification, and the ceil above is only a seed for them.
    // Any estimate within one of the truth converges here, so replacing the seed with
    // round(t) or floor(t + 0.5) changes nothing: the tie-break cannot drift, whatever a
    // later reader does to the line above.
    if ((double)i + 0.5 < t) ++i;
    else if ((double)(i - 1) + 0.5 >= t) --i;
    return i;
}

// MARK: - Transforms

bool raster_matrix_is_rectilinear(raster_matrix m, raster_frect rect, raster_matrix *out) {
    double width = fabs(rect.width), height = fabs(rect.height);
    if (!isfinite(width) || !isfinite(height)) return false;
    if (!isfinite(m.a) || !isfinite(m.b) || !isfinite(m.c) || !isfinite(m.d)) return false;
    if (!isfinite(m.tx) || !isfinite(m.ty)) return false;

    // The question is not how small the off-diagonal terms are, but how far they move a
    // corner of *this* rectangle. A degenerate rectangle has no corners to move, so fall
    // back to the terms themselves rather than accepting everything.
    double spanX = width > 1.0 ? width : 1.0;
    double spanY = height > 1.0 ? height : 1.0;
    const double tolerance = 1.0e-6;

    // Family A: x depends only on x, y only on y. The ordinary case.
    if (fabs(m.b) * spanX <= tolerance && fabs(m.c) * spanY <= tolerance) {
        if (out) { *out = m; out->b = 0.0; out->c = 0.0; }
        return true;
    }
    // Family B: the axes swap. Every quarter turn lands here, where a and d are
    // cos(pi/2) = 6.1e-17 rather than zero, so a test that looked only at b and c would
    // reject a 90-degree layer — which is a one-click user action, not an edge case.
    if (fabs(m.a) * spanX <= tolerance && fabs(m.d) * spanY <= tolerance) {
        if (out) { *out = m; out->a = 0.0; out->d = 0.0; }
        return true;
    }
    return false;
}

bool raster_device_box(raster_matrix m, raster_frect rect, double out[4]) {
    if (!out) return false;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.width) || !isfinite(rect.height))
        return false;

    // Standardise: CGRect may carry a negative extent and CoreGraphics normalises before
    // using it. TiledLayerRenderer can produce a zero-or-negative extent of its own.
    double ux0 = rect.width >= 0 ? rect.x : rect.x + rect.width;
    double ux1 = rect.width >= 0 ? rect.x + rect.width : rect.x;
    double uy0 = rect.height >= 0 ? rect.y : rect.y + rect.height;
    double uy1 = rect.height >= 0 ? rect.y + rect.height : rect.y;
    if (ux1 <= ux0 || uy1 <= uy0) return false;

    // Both families reduce to one scalar map per device axis. Taking min and max of the two
    // mapped endpoints absorbs a negative scale, so neither the base flip that every bitmap
    // context installs nor a mirrored layer needs a special case.
    double x0, x1, y0, y1;
    if (m.b == 0.0 && m.c == 0.0) {
        double xa = m.a * ux0 + m.tx, xb = m.a * ux1 + m.tx;
        double ya = m.d * uy0 + m.ty, yb = m.d * uy1 + m.ty;
        x0 = fmin(xa, xb); x1 = fmax(xa, xb);
        y0 = fmin(ya, yb); y1 = fmax(ya, yb);
    } else if (m.a == 0.0 && m.d == 0.0) {
        double xa = m.c * uy0 + m.tx, xb = m.c * uy1 + m.tx;
        double ya = m.b * ux0 + m.ty, yb = m.b * ux1 + m.ty;
        x0 = fmin(xa, xb); x1 = fmax(xa, xb);
        y0 = fmin(ya, yb); y1 = fmax(ya, yb);
    } else {
        return false;  // caller skipped raster_matrix_is_rectilinear
    }

    out[0] = x0; out[1] = y0; out[2] = x1; out[3] = y1;
    if (!finite_box(out)) return false;
    return x1 > x0 && y1 > y0;
}

bool raster_device_rect_covered(raster_matrix m, raster_frect rect, raster_rect *out) {
    double box[4];
    if (!out || !raster_device_box(m, rect, box)) return false;
    out->x0 = raster_pixel_edge(box[0]);
    out->y0 = raster_pixel_edge(box[1]);
    out->x1 = raster_pixel_edge(box[2]);
    out->y1 = raster_pixel_edge(box[3]);
    return !raster_rect_is_empty(*out);
}

bool raster_device_rect_touched(raster_matrix m, raster_frect rect, raster_rect *out) {
    double box[4];
    if (!out || !raster_device_box(m, rect, box)) return false;
    // Positive-area overlap: a pixel is in when the box reaches into it, which for the
    // half-open pixel [i, i+1) means floor on the near edge and ceil on the far one. At an
    // integer edge this agrees with the centre rule exactly, which is why the antialias flag
    // is invisible on the integer geometry the app almost always uses.
    out->x0 = (int32_t)floor(box[0]);
    out->y0 = (int32_t)floor(box[1]);
    out->x1 = (int32_t)ceil(box[2]);
    out->y1 = (int32_t)ceil(box[3]);
    return !raster_rect_is_empty(*out);
}

// MARK: - Antialiased edges

void raster_axis_coverage(double lo, double hi, int32_t first, int32_t last, uint8_t *out) {
    if (!out || last <= first) return;
    for (int32_t i = first; i < last; ++i) {
        double left = (double)i, right = (double)i + 1.0;
        double overlap = fmin(hi, right) - fmax(lo, left);
        if (overlap <= 0.0) { out[i - first] = 0; continue; }
        if (overlap >= 1.0) { out[i - first] = 255; continue; }
        // Round rather than truncate, so a pixel the box covers almost entirely reads 255
        // and one it barely grazes reads 0 — the same convention the blend code uses.
        out[i - first] = (uint8_t)(overlap * 255.0 + 0.5);
    }
}
