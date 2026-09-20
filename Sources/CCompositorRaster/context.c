#include "raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The drawing destination and its graphics state stack — what CGContext is.
//
// Three shapes here are load-bearing and were arrived at by working backwards from what the
// app does, rather than from what looks tidy:
//
//   The clip is refcounted and immutable, so `save` is a pointer bump. Both renderers save
//   and restore inside per-tile loops, and deep-copying a patch list of a few hundred
//   rectangles on every save would be pure waste.
//
//   Copy-on-write runs the opposite way from the obvious one. The context's own surface is
//   never detached, because CGContext.data has to stay pointer-stable across draws; the
//   snapshots detach instead, and only the ones somebody still holds.
//
//   The current path belongs to the context, not to the graphics state. CoreGraphics does
//   not save or restore it, and LayerRenderer relies on that: it calls addRect again
//   immediately after a clip.

// MARK: - Clips

typedef struct raster_clip {
    size_t refcount;
    raster_region *region;     // never NULL
    // NULL means 255 everywhere inside `region`, which is the common case and costs nothing.
    // Otherwise GRAY8, positioned at exactly raster_region_bounds(region) and the same size.
    // Tying it to the bounds rather than giving it its own origin removes a whole class of
    // off-by-one: there is no second coordinate system to keep in step.
    raster_surface *coverage;
} raster_clip;

// Takes ownership of `region`, and of one reference to `coverage` (which may be NULL).
static raster_clip *clip_create(raster_region *region, raster_surface *coverage) {
    if (!region) {
        raster_surface_release(coverage);
        return NULL;
    }
    raster_clip *clip = malloc(sizeof(raster_clip));
    if (!clip) {
        raster_region_destroy(region);
        raster_surface_release(coverage);
        return NULL;
    }
    clip->refcount = 1;
    clip->region = region;
    clip->coverage = coverage;
    return clip;
}

static raster_clip *clip_retain(raster_clip *clip) {
    if (clip) ++clip->refcount;
    return clip;
}

static void clip_release(raster_clip *clip) {
    if (!clip || --clip->refcount > 0) return;
    raster_region_destroy(clip->region);
    raster_surface_release(clip->coverage);
    free(clip);
}

// MARK: - State

typedef struct {
    raster_matrix ctm;  // user space -> device pixels, base flip already folded in
    raster_clip *clip;
    double alpha;
    double fill[4];  // unpremultiplied RGBA in the destination's own space
    raster_blend blend;
    raster_interpolation interpolation;
    bool antialias;
} raster_gstate;

struct raster_context {
    raster_surface *target;

    raster_gstate state;
    raster_gstate *saved;
    size_t depth, savedCapacity;

    raster_rect *path;  // device space, as CoreGraphics resolves points when they are added
    size_t pathCount, pathCapacity;

    raster_surface **snapshots;  // retained
    size_t snapshotCount, snapshotCapacity;

    uint8_t *scratch;  // one row of coverage bytes, grown as needed
    size_t scratchCapacity;
};

// MARK: - Lifetime

raster_context *raster_context_create(raster_surface *target) {
    if (!target) return NULL;
    size_t width = raster_surface_width(target), height = raster_surface_height(target);
    if (!width || !height) return NULL;

    raster_context *ctx = calloc(1, sizeof(raster_context));
    if (!ctx) return NULL;

    raster_rect bounds = { 0, 0, (int32_t)width, (int32_t)height };
    raster_clip *clip = clip_create(raster_region_create_rect(bounds), NULL);
    if (!clip) {
        free(ctx);
        return NULL;
    }

    ctx->target = raster_surface_retain(target);
    ctx->state.clip = clip;
    // CoreGraphics hands a bitmap context a user space whose origin is the bottom-left with
    // y running up, so the base transform flips. BrushRaster then undoes it, which is why
    // its user space is device pixels exactly.
    ctx->state.ctm = (raster_matrix){ 1, 0, 0, -1, 0, (double)height };
    ctx->state.alpha = 1.0;
    ctx->state.blend = RASTER_BLEND_NORMAL;
    ctx->state.interpolation = RASTER_INTERPOLATION_DEFAULT;
    ctx->state.antialias = true;
    ctx->state.fill[0] = ctx->state.fill[1] = ctx->state.fill[2] = 0.0;
    ctx->state.fill[3] = 1.0;
    return ctx;
}

void raster_context_destroy(raster_context *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->snapshotCount; ++i) raster_surface_release(ctx->snapshots[i]);
    free(ctx->snapshots);
    clip_release(ctx->state.clip);
    for (size_t i = 0; i < ctx->depth; ++i) clip_release(ctx->saved[i].clip);
    free(ctx->saved);
    free(ctx->path);
    free(ctx->scratch);
    raster_surface_release(ctx->target);
    free(ctx);
}

raster_surface *raster_context_target(raster_context *ctx) { return ctx ? ctx->target : NULL; }

// MARK: - Graphics state

raster_status raster_context_save(raster_context *ctx) {
    if (!ctx) return RASTER_OK;
    if (ctx->depth == ctx->savedCapacity) {
        size_t capacity = ctx->savedCapacity ? ctx->savedCapacity * 2 : 8;
        raster_gstate *grown = realloc(ctx->saved, capacity * sizeof(raster_gstate));
        if (!grown) return RASTER_OUT_OF_MEMORY;
        ctx->saved = grown;
        ctx->savedCapacity = capacity;
    }
    ctx->saved[ctx->depth] = ctx->state;
    clip_retain(ctx->state.clip);
    ++ctx->depth;
    return RASTER_OK;
}

void raster_context_restore(raster_context *ctx) {
    // Unbalanced restores are a no-op, not an error. Nothing in the app depends on that —
    // its one apparent imbalance is a single save with two exit paths — but an underflow
    // that trapped would take the whole test process down with it, and CoreGraphics itself
    // logs and continues.
    if (!ctx || ctx->depth == 0) return;
    --ctx->depth;
    clip_release(ctx->state.clip);
    ctx->state = ctx->saved[ctx->depth];
}

size_t raster_context_depth(const raster_context *ctx) { return ctx ? ctx->depth : 0; }

raster_matrix raster_context_matrix(const raster_context *ctx) {
    if (!ctx) return (raster_matrix){ 1, 0, 0, 1, 0, 0 };
    return ctx->state.ctm;
}

void raster_context_set_matrix(raster_context *ctx, raster_matrix m) {
    if (ctx) ctx->state.ctm = m;
}

void raster_context_set_alpha(raster_context *ctx, double alpha) {
    if (!ctx) return;
    ctx->state.alpha = alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha);
}

void raster_context_set_blend(raster_context *ctx, raster_blend mode) {
    if (ctx) ctx->state.blend = mode;
}

void raster_context_set_antialias(raster_context *ctx, bool on) {
    if (ctx) ctx->state.antialias = on;
}

void raster_context_set_interpolation(raster_context *ctx, raster_interpolation quality) {
    if (ctx) ctx->state.interpolation = quality;
}

raster_interpolation raster_context_interpolation(const raster_context *ctx) {
    return ctx ? ctx->state.interpolation : RASTER_INTERPOLATION_DEFAULT;
}

void raster_context_set_fill_color(raster_context *ctx, const double rgba[4]) {
    if (!ctx || !rgba) return;
    for (int i = 0; i < 4; ++i) {
        double v = rgba[i];
        if (!isfinite(v)) v = 0;
        ctx->state.fill[i] = v < 0 ? 0 : (v > 1 ? 1 : v);
    }
}

// MARK: - Clipping

static bool path_append(raster_context *ctx, raster_rect rect) {
    if (raster_rect_is_empty(rect)) return true;
    if (ctx->pathCount == ctx->pathCapacity) {
        size_t capacity = ctx->pathCapacity ? ctx->pathCapacity * 2 : 8;
        raster_rect *grown = realloc(ctx->path, capacity * sizeof(raster_rect));
        if (!grown) return false;
        ctx->path = grown;
        ctx->pathCapacity = capacity;
    }
    ctx->path[ctx->pathCount++] = rect;
    return true;
}

void raster_context_reset_path(raster_context *ctx) {
    if (ctx) ctx->pathCount = 0;
}

// Rectangles are resolved to device pixels when they are added, not when the path is used,
// because that is when CoreGraphics applies the CTM.
raster_status raster_context_add_rect(raster_context *ctx, raster_frect rect) {
    if (!ctx) return RASTER_OK;
    raster_matrix canonical;
    if (!raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical))
        return RASTER_UNSUPPORTED_TRANSFORM;

    raster_rect device;
    // An empty rectangle contributes nothing, which is not a failure: CGPath accepts one.
    if (!raster_device_rect_covered(canonical, rect, &device)) return RASTER_OK;
    return path_append(ctx, device) ? RASTER_OK : RASTER_OUT_OF_MEMORY;
}

// The existing plane, re-based onto `bounds`. Because the plane is always positioned at its
// own region's bounds and `bounds` can only have shrunk, this is a crop view — no pixels move
// and nothing is copied, which is what keeps saveGState/restoreGState around a tile loop cheap.
//
// Returns false only on allocation failure; a NULL plane in, or an empty region, gives a NULL
// plane out, which is the "opaque inside the region" case and not an error.
static bool coverage_rebase(const raster_clip *from, raster_rect bounds, raster_surface **out) {
    *out = NULL;
    if (!from->coverage) return true;
    if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0) return true;

    raster_rect was = raster_region_bounds(from->region);
    raster_surface *view = raster_surface_crop(from->coverage,
                                               (size_t)(bounds.x0 - was.x0),
                                               (size_t)(bounds.y0 - was.y0),
                                               (size_t)(bounds.x1 - bounds.x0),
                                               (size_t)(bounds.y1 - bounds.y0));
    if (!view) return false;
    *out = view;
    return true;
}

// Replaces the clip with its intersection with `region`, which this takes ownership of.
static raster_status clip_intersect(raster_context *ctx, raster_region *region) {
    if (!region) return RASTER_OUT_OF_MEMORY;
    raster_region *narrowed = raster_region_intersect(ctx->state.clip->region, region);
    raster_region_destroy(region);
    if (!narrowed) return RASTER_OUT_OF_MEMORY;

    raster_surface *coverage;
    if (!coverage_rebase(ctx->state.clip, raster_region_bounds(narrowed), &coverage)) {
        raster_region_destroy(narrowed);
        return RASTER_OUT_OF_MEMORY;
    }

    raster_clip *clip = clip_create(narrowed, coverage);
    if (!clip) return RASTER_OUT_OF_MEMORY;
    clip_release(ctx->state.clip);
    ctx->state.clip = clip;
    return RASTER_OK;
}

raster_status raster_context_clip_path(raster_context *ctx, bool even_odd) {
    if (!ctx) return RASTER_OK;

    // An empty path clips everything away. CoreGraphics does not specify this and the app
    // never reaches it — TiledLayerRenderer guards with `cut.isEmpty` — so the choice is
    // ours; it is written down here so it cannot be made twice, differently.
    raster_region *accumulated = raster_region_create();
    if (!accumulated) return RASTER_OUT_OF_MEMORY;

    for (size_t i = 0; i < ctx->pathCount; ++i) {
        raster_region *piece = raster_region_create_rect(ctx->path[i]);
        raster_region *merged = piece
            ? (even_odd ? raster_region_xor(accumulated, piece)
                        : raster_region_union(accumulated, piece))
            : NULL;
        raster_region_destroy(piece);
        raster_region_destroy(accumulated);
        if (!merged) { ctx->pathCount = 0; return RASTER_OUT_OF_MEMORY; }
        accumulated = merged;
    }
    ctx->pathCount = 0;  // clipping consumes the path
    return clip_intersect(ctx, accumulated);
}

raster_status raster_context_clip_rect(raster_context *ctx, raster_frect rect) {
    if (!ctx) return RASTER_OK;
    raster_matrix canonical;
    if (!raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical))
        return RASTER_UNSUPPORTED_TRANSFORM;

    raster_rect device;
    if (!raster_device_rect_covered(canonical, rect, &device)) {
        // An empty rectangle is a legitimate way to clip everything away, and Selection
        // uses it for an empty marquee.
        raster_region *nothing = raster_region_create();
        return clip_intersect(ctx, nothing);
    }
    return clip_intersect(ctx, raster_region_create_rect(device));
}

raster_status raster_context_clip_mask(raster_context *ctx, const raster_surface *mask,
                                       raster_frect rect) {
    if (!ctx) return RASTER_OK;
    if (!mask || raster_surface_format(mask) != RASTER_GRAY8) return RASTER_UNSUPPORTED_TRANSFORM;

    // Redundant on its own: raster_image_mapping below repeats this test and would refuse a
    // rotation anyway, so removing this survives every test. It stays because it makes the
    // refusal this function's own decision rather than a side effect of what a callee happens
    // to check, and because the canonical matrix is wanted regardless.
    raster_matrix canonical;
    if (!raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical))
        return RASTER_UNSUPPORTED_TRANSFORM;

    raster_matrix deviceToMask;
    if (!raster_image_mapping(canonical, rect, raster_surface_width(mask),
                              raster_surface_height(mask), &deviceToMask))
        return RASTER_UNSUPPORTED_TRANSFORM;

    // The rectangle clips hard, by the same centre rule as every other clip. Softness comes
    // from the mask's own values and from nowhere else.
    raster_rect device;
    if (!raster_device_rect_covered(canonical, rect, &device)) {
        raster_region *nothing = raster_region_create();
        return clip_intersect(ctx, nothing);
    }

    raster_region *piece = raster_region_create_rect(device);
    if (!piece) return RASTER_OUT_OF_MEMORY;
    raster_region *narrowed = raster_region_intersect(ctx->state.clip->region, piece);
    raster_region_destroy(piece);
    if (!narrowed) return RASTER_OUT_OF_MEMORY;

    raster_rect bounds = raster_region_bounds(narrowed);
    if (raster_region_is_empty(narrowed)) {
        raster_clip *empty = clip_create(narrowed, NULL);
        if (!empty) return RASTER_OUT_OF_MEMORY;
        clip_release(ctx->state.clip);
        ctx->state.clip = empty;
        return RASTER_OK;
    }

    size_t width = (size_t)(bounds.x1 - bounds.x0), height = (size_t)(bounds.y1 - bounds.y0);
    raster_surface *plane = raster_surface_create(width, height, RASTER_GRAY8);
    uint8_t *planeBytes = plane ? raster_surface_mutable_bytes(plane) : NULL;
    if (!planeBytes) {
        raster_surface_release(plane);
        raster_region_destroy(narrowed);
        return RASTER_OUT_OF_MEMORY;
    }
    size_t planeStride = raster_surface_stride(plane);

    // The parent's plane is read, never written. That is the whole of the copy-on-write
    // story: a nested clip builds its own plane, so a restoreGState finds the outer one
    // exactly as it left it, with no versioning and no copy on the way in.
    const raster_surface *parent = ctx->state.clip->coverage;
    const uint8_t *parentBytes = parent ? raster_surface_bytes(parent) : NULL;
    size_t parentStride = parent ? raster_surface_stride(parent) : 0;
    raster_rect parentBounds = raster_region_bounds(ctx->state.clip->region);

    raster_interpolation quality = ctx->state.interpolation;
    for (size_t row = 0; row < height; ++row) {
        int32_t y = bounds.y0 + (int32_t)row;
        uint8_t *dst = planeBytes + row * planeStride;
        raster_sample_row(mask, deviceToMask, bounds.x0, y, width, quality, dst);
        if (!parentBytes) continue;
        const uint8_t *src = parentBytes + (size_t)(y - parentBounds.y0) * parentStride
                           + (size_t)(bounds.x0 - parentBounds.x0);
        for (size_t x = 0; x < width; ++x)
            dst[x] = (uint8_t)((dst[x] * src[x] + 127) / 255);
    }

    raster_clip *clip = clip_create(narrowed, plane);
    if (!clip) return RASTER_OUT_OF_MEMORY;
    clip_release(ctx->state.clip);
    ctx->state.clip = clip;
    return RASTER_OK;
}

bool raster_context_clip_bounds(const raster_context *ctx, raster_rect *out) {
    if (!ctx || !out) return false;
    if (raster_region_is_empty(ctx->state.clip->region)) return false;
    *out = raster_region_bounds(ctx->state.clip->region);
    return true;
}

const raster_region *raster_context_clip_region(const raster_context *ctx) {
    return ctx ? ctx->state.clip->region : NULL;
}

// MARK: - Snapshots

static bool snapshot_register(raster_context *ctx, raster_surface *snapshot) {
    if (ctx->snapshotCount == ctx->snapshotCapacity) {
        size_t capacity = ctx->snapshotCapacity ? ctx->snapshotCapacity * 2 : 4;
        raster_surface **grown = realloc(ctx->snapshots, capacity * sizeof(raster_surface *));
        if (!grown) return false;
        ctx->snapshots = grown;
        ctx->snapshotCapacity = capacity;
    }
    ctx->snapshots[ctx->snapshotCount++] = raster_surface_retain(snapshot);
    return true;
}

raster_surface *raster_context_make_snapshot(raster_context *ctx) {
    if (!ctx) return NULL;

    // A borrowed target can never be detached from the caller's memory, so a snapshot of one
    // has to be an eager copy. Branching here rather than at detach time is the difference
    // between a copy and a silent failure to write.
    if (!raster_surface_is_owned(ctx->target)) return raster_surface_copy(ctx->target);

    raster_surface *snapshot = raster_surface_crop(ctx->target, 0, 0,
                                                   raster_surface_width(ctx->target),
                                                   raster_surface_height(ctx->target));
    if (!snapshot) return NULL;
    if (!snapshot_register(ctx, snapshot)) {
        raster_surface_release(snapshot);
        return NULL;
    }
    return snapshot;
}

raster_status raster_context_register_snapshot(raster_context *ctx, raster_surface *snapshot) {
    if (!ctx || !snapshot) return RASTER_OK;
    return snapshot_register(ctx, snapshot) ? RASTER_OK : RASTER_OUT_OF_MEMORY;
}

raster_status raster_context_detach_snapshots(raster_context *ctx) {
    if (!ctx) return RASTER_OK;
    raster_status status = RASTER_OK;
    for (size_t i = 0; i < ctx->snapshotCount; ++i) {
        raster_surface *snapshot = ctx->snapshots[i];
        // Only one reference left means this registry holds it and nobody else can observe
        // it, so there is nothing to preserve — drop it instead of copying a whole canvas.
        if (raster_surface_refcount(snapshot) > 1 && !raster_surface_make_unique(snapshot))
            status = RASTER_OUT_OF_MEMORY;
        raster_surface_release(snapshot);
    }
    ctx->snapshotCount = 0;
    return status;
}

// MARK: - Painting

static uint8_t quantise(double v) {
    if (!(v > 0)) return 0;  // also catches NaN
    if (v >= 1) return 255;
    return (uint8_t)(v * 255.0 + 0.5);
}

static bool scratch_reserve(raster_context *ctx, size_t count) {
    if (count <= ctx->scratchCapacity) return true;
    uint8_t *grown = realloc(ctx->scratch, count);
    if (!grown) return false;
    ctx->scratch = grown;
    ctx->scratchCapacity = count;
    return true;
}

// The clip's coverage plane, read-only, with the origin it is positioned at. `bytes` is NULL
// when the clip is a plain region, which is the ordinary case.
typedef struct {
    const uint8_t *bytes;
    size_t stride;
    int32_t x0, y0;
} coverage_plane;

static coverage_plane clip_plane(const raster_clip *clip) {
    coverage_plane plane = { NULL, 0, 0, 0 };
    if (!clip->coverage) return plane;
    plane.bytes = raster_surface_bytes(clip->coverage);
    plane.stride = raster_surface_stride(clip->coverage);
    raster_rect bounds = raster_region_bounds(clip->region);
    plane.x0 = bounds.x0;
    plane.y0 = bounds.y0;
    return plane;
}

// Folds the plane into one row's coverage. `coverage` is what the edge antialiasing produced,
// or NULL for "fully covered"; the answer is the same thing with the mask multiplied in.
//
// The NULL case is the one worth having: a hard-edged fill through a mask clip needs no
// arithmetic at all, because the plane's row *is* the coverage. Only a soft edge crossing a
// mask has to multiply, and then it does so into `scratch`.
static const uint8_t *plane_apply(const coverage_plane *plane, uint8_t *scratch,
                                  const uint8_t *coverage, int32_t x0, int32_t y, size_t count) {
    if (!plane->bytes) return coverage;
    const uint8_t *row = plane->bytes + (size_t)(y - plane->y0) * plane->stride
                       + (size_t)(x0 - plane->x0);
    if (!coverage) return row;
    // scratch may alias `coverage`; every output depends only on the input at the same index.
    for (size_t x = 0; x < count; ++x)
        scratch[x] = (uint8_t)((coverage[x] * row[x] + 127) / 255);
    return scratch;
}

// One fill, shared by fill_rect and clear_rect. `blend` and `alpha` are passed in rather
// than read from the state because clear ignores both of the state's.
static raster_status paint(raster_context *ctx, raster_frect rect, raster_blend blend,
                           double alpha, const double fill[4]) {
    raster_matrix canonical;
    if (!raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical))
        return RASTER_UNSUPPORTED_TRANSFORM;

    double box[4];
    if (!raster_device_box(canonical, rect, box)) return RASTER_OK;  // empty, not an error

    bool antialias = ctx->state.antialias;
    raster_rect area;
    bool any = antialias ? raster_device_rect_touched(canonical, rect, &area)
                         : raster_device_rect_covered(canonical, rect, &area);
    if (!any) return RASTER_OK;

    raster_status status = raster_context_detach_snapshots(ctx);
    if (status != RASTER_OK) return status;

    uint8_t *pixels = raster_surface_mutable_bytes(ctx->target);
    if (!pixels) return RASTER_OUT_OF_MEMORY;

    size_t stride = raster_surface_stride(ctx->target);
    raster_format format = raster_surface_format(ctx->target);
    size_t bpp = raster_bytes_per_pixel(format);

    // The source colour. For RGBA8 the colour's own alpha lives in the premultiplied
    // samples and the state's alpha rides alongside; GRAY8 has no alpha channel at all, so
    // the colour's alpha has to fold into the lerp weight instead.
    uint8_t rgba[4];
    uint8_t gray = 0;
    double weight = alpha;
    if (format == RASTER_GRAY8) {
        gray = quantise(fill[0]);
        weight = alpha * fill[3];
    } else {
        rgba[3] = quantise(fill[3]);
        for (int i = 0; i < 3; ++i) rgba[i] = quantise(fill[i] * fill[3]);
    }
    uint8_t alpha8 = quantise(weight);

    int32_t width = area.x1 - area.x0;
    const uint8_t *columnCoverage = NULL;
    if (antialias) {
        if (!scratch_reserve(ctx, (size_t)width * 2)) return RASTER_OUT_OF_MEMORY;
        raster_axis_coverage(box[0], box[2], area.x0, area.x1, ctx->scratch);
        columnCoverage = ctx->scratch;
    }

    const raster_region *clip = ctx->state.clip->region;
    coverage_plane plane = clip_plane(ctx->state.clip);
    if (plane.bytes && !scratch_reserve(ctx, (size_t)width * 2)) return RASTER_OUT_OF_MEMORY;

    size_t clipCount = raster_region_count(clip);
    for (size_t i = 0; i < clipCount; ++i) {
        raster_rect band = raster_region_rect(clip, i);
        raster_rect hit = {
            band.x0 > area.x0 ? band.x0 : area.x0, band.y0 > area.y0 ? band.y0 : area.y0,
            band.x1 < area.x1 ? band.x1 : area.x1, band.y1 < area.y1 ? band.y1 : area.y1,
        };
        if (raster_rect_is_empty(hit)) continue;

        size_t count = (size_t)(hit.x1 - hit.x0);
        for (int32_t y = hit.y0; y < hit.y1; ++y) {
            const uint8_t *coverage = NULL;
            if (antialias) {
                uint8_t row;
                raster_axis_coverage(box[1], box[3], y, y + 1, &row);
                if (row == 0) continue;
                const uint8_t *columns = columnCoverage + (hit.x0 - area.x0);
                if (row == 255) {
                    coverage = columns;
                } else {
                    // Only the top and bottom rows of an antialiased fill are partial, so
                    // this scratch pass runs at most twice per fill.
                    uint8_t *scaled = ctx->scratch + width;
                    for (size_t x = 0; x < count; ++x)
                        scaled[x] = (uint8_t)((columns[x] * row + 127) / 255);
                    coverage = scaled;
                }
            }
            coverage = plane_apply(&plane, ctx->scratch + width, coverage, hit.x0, y, count);
            uint8_t *dst = pixels + (size_t)y * stride + (size_t)hit.x0 * bpp;
            if (format == RASTER_GRAY8)
                raster_fill_row_gray(dst, gray, count, blend, alpha8, coverage);
            else
                raster_fill_row_rgba(dst, rgba, count, blend, alpha8, coverage);
        }
    }
    return RASTER_OK;
}

raster_status raster_context_fill_rect(raster_context *ctx, raster_frect rect) {
    if (!ctx) return RASTER_OK;
    return paint(ctx, rect, ctx->state.blend, ctx->state.alpha, ctx->state.fill);
}

raster_status raster_context_clear_rect(raster_context *ctx, raster_frect rect) {
    if (!ctx) return RASTER_OK;
    // CGContextClearRect honours the CTM and the clip and ignores the state's alpha and
    // blend mode. Implementing it as "fill with the clear blend" would make
    // setAlpha(0.5); clear(r) erase half, which is not what it does.
    static const double opaque[4] = { 0, 0, 0, 1 };
    return paint(ctx, rect, RASTER_BLEND_CLEAR, 1.0, opaque);
}

raster_status raster_context_draw_image(raster_context *ctx, const raster_surface *image,
                                        raster_frect rect) {
    if (!ctx || !image) return RASTER_OK;
    // Converting between the two pixel layouts mid-draw is not something the app ever asks
    // for — every draw is colour into colour or mask into mask — so it is refused rather
    // than invented.
    if (raster_surface_format(image) != raster_surface_format(ctx->target))
        return RASTER_UNSUPPORTED_TRANSFORM;

    raster_matrix canonical;
    if (!raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical))
        return RASTER_UNSUPPORTED_TRANSFORM;

    raster_matrix deviceToImage;
    if (!raster_image_mapping(ctx->state.ctm, rect, raster_surface_width(image),
                              raster_surface_height(image), &deviceToImage))
        return RASTER_OK;  // degenerate, not a failure

    double box[4];
    if (!raster_device_box(canonical, rect, box)) return RASTER_OK;

    bool antialias = ctx->state.antialias;
    raster_rect area;
    bool any = antialias ? raster_device_rect_touched(canonical, rect, &area)
                         : raster_device_rect_covered(canonical, rect, &area);
    if (!any) return RASTER_OK;

    // Before a single source byte is read. If `image` is a live snapshot of this very
    // context — which DownsampleCache does deliberately: snapshot, crop one row, draw it
    // straight back in — this is what gives it a private copy of the pixels it was taken
    // from, after which the source and the destination no longer overlap.
    raster_status status = raster_context_detach_snapshots(ctx);
    if (status != RASTER_OK) return status;

    uint8_t *pixels = raster_surface_mutable_bytes(ctx->target);
    if (!pixels) return RASTER_OUT_OF_MEMORY;

    size_t stride = raster_surface_stride(ctx->target);
    raster_format format = raster_surface_format(ctx->target);
    size_t bpp = raster_bytes_per_pixel(format);
    uint8_t alpha8 = quantise(ctx->state.alpha);
    raster_blend blend = ctx->state.blend;

    size_t width = (size_t)(area.x1 - area.x0);
    // A row of resampled source, a row of column coverage, and a scratch row for the two
    // partial edge rows an antialiased draw has.
    if (!scratch_reserve(ctx, width * (bpp + 2))) return RASTER_OUT_OF_MEMORY;
    uint8_t *samples = ctx->scratch;
    uint8_t *columnCoverage = ctx->scratch + width * bpp;
    uint8_t *scaledCoverage = columnCoverage + width;
    if (antialias) raster_axis_coverage(box[0], box[2], area.x0, area.x1, columnCoverage);

    const raster_region *clip = ctx->state.clip->region;
    coverage_plane plane = clip_plane(ctx->state.clip);
    size_t clipCount = raster_region_count(clip);
    for (size_t i = 0; i < clipCount; ++i) {
        raster_rect band = raster_region_rect(clip, i);
        raster_rect hit = {
            band.x0 > area.x0 ? band.x0 : area.x0, band.y0 > area.y0 ? band.y0 : area.y0,
            band.x1 < area.x1 ? band.x1 : area.x1, band.y1 < area.y1 ? band.y1 : area.y1,
        };
        if (raster_rect_is_empty(hit)) continue;

        size_t count = (size_t)(hit.x1 - hit.x0);
        for (int32_t y = hit.y0; y < hit.y1; ++y) {
            const uint8_t *coverage = NULL;
            if (antialias) {
                uint8_t row;
                raster_axis_coverage(box[1], box[3], y, y + 1, &row);
                if (row == 0) continue;
                const uint8_t *columns = columnCoverage + (hit.x0 - area.x0);
                if (row == 255) {
                    coverage = columns;
                } else {
                    for (size_t x = 0; x < count; ++x)
                        scaledCoverage[x] = (uint8_t)((columns[x] * row + 127) / 255);
                    coverage = scaledCoverage;
                }
            }
            coverage = plane_apply(&plane, scaledCoverage, coverage, hit.x0, y, count);
            raster_sample_row(image, deviceToImage, hit.x0, y, count,
                              ctx->state.interpolation, samples);
            uint8_t *dst = pixels + (size_t)y * stride + (size_t)hit.x0 * bpp;
            if (format == RASTER_GRAY8)
                raster_blend_row_gray(dst, samples, count, blend, alpha8, coverage);
            else
                raster_blend_row_rgba(dst, samples, count, blend, alpha8, coverage);
        }
    }
    return RASTER_OK;
}
