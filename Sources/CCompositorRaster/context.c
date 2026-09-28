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

// How many parallelogram clips ride along before they are folded into a plane. Four covers
// what the app nests — a layer's own mask inside a folder mask inside a tile clip — and the
// overflow path is correct rather than merely unlikely.
#define RASTER_CLIP_MAX_QUADS 4

typedef struct raster_clip {
    size_t refcount;
    raster_region *region;     // never NULL
    // NULL means 255 everywhere inside `region`, which is the common case and costs nothing.
    // Otherwise GRAY8, positioned at exactly raster_region_bounds(region) and the same size.
    // Tying it to the bounds rather than giving it its own origin removes a whole class of
    // off-by-one: there is no second coordinate system to keep in step.
    raster_surface *coverage;

    // Clips taken under a transform that is not rectilinear. Carried as shapes and evaluated
    // per row, not rasterised, because BrushStroke clips inside its per-dab loop — roughly 14
    // dabs per mouse move — and a plane there would be a multi-megabyte calloc per dab on the
    // one path with a performance document to its name. `soft` records whether each was taken
    // with antialiasing on, which decides area coverage versus a hard span.
    raster_quad quads[RASTER_CLIP_MAX_QUADS];
    bool soft[RASTER_CLIP_MAX_QUADS];
    size_t quadCount;
} raster_clip;

// Takes ownership of `region`, and of one reference to `coverage` (which may be NULL).
static raster_clip *clip_create(raster_region *region, raster_surface *coverage) {
    if (!region) {
        raster_surface_release(coverage);
        return NULL;
    }
    raster_clip *clip = calloc(1, sizeof(raster_clip));
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

// Copies `from`'s parallelograms into `clip`. They are plain values, so a clip that inherits
// them shares nothing and a restoreGState needs no undoing.
static void clip_inherit_quads(raster_clip *clip, const raster_clip *from) {
    for (size_t i = 0; i < from->quadCount; ++i) {
        clip->quads[i] = from->quads[i];
        clip->soft[i] = from->soft[i];
    }
    clip->quadCount = from->quadCount;
}

// One row of a parallelogram's contribution: area coverage when it was taken with antialiasing
// on, a hard span otherwise. `out` receives `count` bytes.
static void quad_row(const raster_quad *quad, bool soft, int32_t x0, int32_t y, size_t count,
                     uint8_t *out) {
    if (soft) {
        raster_quad_coverage_row(quad, x0, y, count, out);
        return;
    }
    int32_t a, b;
    if (!raster_quad_row_span(quad, y, &a, &b)) {
        memset(out, 0, count);
        return;
    }
    for (size_t x = 0; x < count; ++x) {
        int32_t column = x0 + (int32_t)x;
        out[x] = (column >= a && column < b) ? 255 : 0;
    }
}

// Rasterises the clip's parallelograms into its plane and empties the list.
//
// Only reached when nesting runs past the inline list. Everything else evaluates the shapes per
// row, which is the point of carrying them; this exists so that depth is a cost rather than a
// limit.
static bool clip_fold_quads(raster_clip *clip) {
    if (!clip->quadCount) return true;

    raster_rect bounds = raster_region_bounds(clip->region);
    if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0) {
        clip->quadCount = 0;
        return true;
    }
    size_t width = (size_t)(bounds.x1 - bounds.x0), height = (size_t)(bounds.y1 - bounds.y0);

    // A fresh plane, never a write through the existing one: that may be a crop view sharing
    // an outer clip's store, and writing through it would change what a restoreGState returns
    // to.
    raster_surface *plane = raster_surface_create(width, height, RASTER_GRAY8);
    uint8_t *bytes = plane ? raster_surface_mutable_bytes(plane) : NULL;
    uint8_t *temp = bytes ? malloc(width) : NULL;
    if (!bytes || !temp) {
        free(temp);
        raster_surface_release(plane);
        return false;
    }
    size_t stride = raster_surface_stride(plane);

    const raster_surface *existing = clip->coverage;
    const uint8_t *old = existing ? raster_surface_bytes(existing) : NULL;
    size_t oldStride = existing ? raster_surface_stride(existing) : 0;

    for (size_t row = 0; row < height; ++row) {
        int32_t y = bounds.y0 + (int32_t)row;
        uint8_t *dst = bytes + row * stride;
        if (old) memcpy(dst, old + row * oldStride, width);
        else memset(dst, 255, width);
        for (size_t i = 0; i < clip->quadCount; ++i) {
            quad_row(&clip->quads[i], clip->soft[i], bounds.x0, y, width, temp);
            for (size_t x = 0; x < width; ++x)
                dst[x] = (uint8_t)((dst[x] * temp[x] + 127) / 255);
        }
    }

    free(temp);
    raster_surface_release(clip->coverage);
    clip->coverage = plane;
    clip->quadCount = 0;
    return true;
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
    clip_inherit_quads(clip, ctx->state.clip);
    clip_release(ctx->state.clip);
    ctx->state.clip = clip;
    return RASTER_OK;
}

// Replaces the clip with its intersection with `quad`. The shape rides along in the clip
// rather than being rasterised; only an overflow of the inline list forces a plane.
static raster_status clip_intersect_quad(raster_context *ctx, const raster_quad *quad, bool soft) {
    raster_rect covered;
    // The exact bounds of the covered pixel set, not the geometry rounded out. A sliver too
    // thin to contain a pixel centre reports nothing here, and the clip has to collapse to
    // empty for it — otherwise boundingBoxOfClipPath answers with a rectangle where Swift owes
    // CGRect.null, and TiledLayerRenderer's -64 inset turns that into nonsense.
    //
    // For an antialiased clip the *touched* set is what gets partial coverage, so the bounds
    // have to be the wider ones or the boundary pixels are cut away before the coverage can
    // soften them.
    bool any = soft ? raster_quad_touched_bounds(quad, &covered)
                    : raster_quad_covered_bounds(quad, &covered);
    if (!any) {
        raster_region *nothing = raster_region_create();
        if (!nothing) return RASTER_OUT_OF_MEMORY;
        raster_clip *empty = clip_create(nothing, NULL);
        if (!empty) return RASTER_OUT_OF_MEMORY;
        clip_release(ctx->state.clip);
        ctx->state.clip = empty;
        return RASTER_OK;
    }

    raster_region *piece = raster_region_create_rect(covered);
    if (!piece) return RASTER_OUT_OF_MEMORY;
    raster_region *narrowed = raster_region_intersect(ctx->state.clip->region, piece);
    raster_region_destroy(piece);
    if (!narrowed) return RASTER_OUT_OF_MEMORY;

    raster_surface *coverage;
    if (!coverage_rebase(ctx->state.clip, raster_region_bounds(narrowed), &coverage)) {
        raster_region_destroy(narrowed);
        return RASTER_OUT_OF_MEMORY;
    }

    raster_clip *clip = clip_create(narrowed, coverage);
    if (!clip) return RASTER_OUT_OF_MEMORY;
    clip_inherit_quads(clip, ctx->state.clip);

    if (clip->quadCount == RASTER_CLIP_MAX_QUADS) {
        // Deeper nesting than the inline list holds. Fold what is there into a plane once, so
        // the list has room again; correctness does not depend on this never happening.
        if (!clip_fold_quads(clip)) {
            clip_release(clip);
            return RASTER_OUT_OF_MEMORY;
        }
    }
    clip->quads[clip->quadCount] = *quad;
    clip->soft[clip->quadCount] = soft;
    ++clip->quadCount;

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
    if (!raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical)) {
        raster_quad quad;
        if (!raster_quad_make(ctx->state.ctm, rect, &quad)) {
            // Degenerate or singular: nothing is covered, so the clip becomes empty. Not a
            // refusal — CGAffineTransform.inverted() hands back a singular matrix unchanged and
            // BrushStroke concatenates the result, so this is reachable and has to answer.
            raster_region *nothing = raster_region_create();
            return clip_intersect(ctx, nothing);
        }
        return clip_intersect_quad(ctx, &quad, ctx->state.antialias);
    }

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
    if (!mask || raster_surface_format(mask) != RASTER_GRAY8) return RASTER_UNSUPPORTED_FORMAT;

    // Not a gate any more, a selector. A rectilinear transform gets the canonical matrix --
    // still worth having, because that canonicalisation is what keeps a quarter-turned mask a
    // bit-exact permutation rather than a resample 6.1e-17 off the grid -- and anything else
    // passes through as it is, because the sampler is general affine.
    raster_matrix canonical;
    bool rectilinear = raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical);
    if (!rectilinear) canonical = ctx->state.ctm;

    // The mapping is computed before anything is narrowed, so its one failure mode cannot
    // leave the clip half-applied. It fails on a degenerate rectangle or a singular transform,
    // both of which mean nothing is covered -- fail closed, never open.
    raster_matrix deviceToMask;
    if (!raster_image_mapping(canonical, rect, raster_surface_width(mask),
                              raster_surface_height(mask), &deviceToMask))
        return clip_intersect(ctx, raster_region_create());

    // Narrow to the rectangle first, then sample the mask over what is left. Splitting it this
    // way keeps the shape's edge rule in one place and means the plane is only ever built over
    // a region already cut down to the shape.
    raster_status status;
    if (rectilinear) {
        // The rectangle clips hard, by the same centre rule as every other clip. Softness comes
        // from the mask's own values and from nowhere else.
        raster_rect device;
        if (!raster_device_rect_covered(canonical, rect, &device))
            return clip_intersect(ctx, raster_region_create());
        status = clip_intersect(ctx, raster_region_create_rect(device));
    } else {
        raster_quad quad;
        if (!raster_quad_make(canonical, rect, &quad))
            return clip_intersect(ctx, raster_region_create());
        status = clip_intersect_quad(ctx, &quad, ctx->state.antialias);
    }
    if (status != RASTER_OK) return status;

    // Whichever branch ran, it built a fresh clip and handed it straight to the state, so this
    // one holds the only reference and can be finished in place. Anything a saveGState is
    // holding points at the clip that was just released.
    raster_clip *clip = ctx->state.clip;
    if (raster_region_is_empty(clip->region)) return RASTER_OK;

    raster_rect bounds = raster_region_bounds(clip->region);
    size_t width = (size_t)(bounds.x1 - bounds.x0), height = (size_t)(bounds.y1 - bounds.y0);
    raster_surface *plane = raster_surface_create(width, height, RASTER_GRAY8);
    uint8_t *planeBytes = plane ? raster_surface_mutable_bytes(plane) : NULL;
    if (!planeBytes) {
        raster_surface_release(plane);
        return RASTER_OUT_OF_MEMORY;
    }
    size_t planeStride = raster_surface_stride(plane);

    // The existing plane is read, never written. That is the whole of the copy-on-write story:
    // a nested clip builds its own, so a restoreGState finds the outer one exactly as it left
    // it, with no versioning and no copy on the way in. It may be a crop view sharing an outer
    // clip's store, which is precisely why writing through it would be wrong.
    const raster_surface *existing = clip->coverage;
    const uint8_t *existingBytes = existing ? raster_surface_bytes(existing) : NULL;
    size_t existingStride = existing ? raster_surface_stride(existing) : 0;

    raster_interpolation quality = ctx->state.interpolation;
    for (size_t row = 0; row < height; ++row) {
        int32_t y = bounds.y0 + (int32_t)row;
        uint8_t *dst = planeBytes + row * planeStride;
        raster_sample_row(mask, deviceToMask, bounds.x0, y, width, quality, dst);
        if (!existingBytes) continue;
        const uint8_t *src = existingBytes + row * existingStride;
        for (size_t x = 0; x < width; ++x)
            dst[x] = (uint8_t)((dst[x] * src[x] + 127) / 255);
    }

    raster_surface_release(clip->coverage);
    clip->coverage = plane;
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

// The clip's whole contribution to one row: its plane, then each parallelogram it carries.
// `coverage` is what the shape being painted produced, or NULL for "fully covered", and the
// answer is the same thing with the clip multiplied in.
//
// `scratch` and `temp` are separate rows of at least `count` bytes. `scratch` may alias
// `coverage`, which stays safe because every output depends only on the input at the same
// index; `temp` may not, because it is written whole before it is read.
static const uint8_t *clip_apply(const raster_clip *clip, const coverage_plane *plane,
                                 uint8_t *scratch, uint8_t *temp, const uint8_t *coverage,
                                 int32_t x0, int32_t y, size_t count) {
    coverage = plane_apply(plane, scratch, coverage, x0, y, count);
    for (size_t i = 0; i < clip->quadCount; ++i) {
        quad_row(&clip->quads[i], clip->soft[i], x0, y, count, temp);
        if (!coverage) {
            memcpy(scratch, temp, count);
        } else {
            for (size_t x = 0; x < count; ++x)
                scratch[x] = (uint8_t)((coverage[x] * temp[x] + 127) / 255);
        }
        coverage = scratch;
    }
    return coverage;
}

// One fill, shared by fill_rect and clear_rect. `blend` and `alpha` are passed in rather
// than read from the state because clear ignores both of the state's.
static raster_status paint(raster_context *ctx, raster_frect rect, raster_blend blend,
                           double alpha, const double fill[4]) {
    // A selector, not a gate. A rectilinear transform keeps the exact integer path unchanged,
    // which is what leaves the existing million-check oracle untouched; anything else -- a
    // rotation, a shear, or both -- goes through the parallelogram.
    raster_matrix canonical;
    bool rectilinear = raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical);
    bool antialias = ctx->state.antialias;

    double box[4];
    raster_quad quad;
    raster_rect area;
    if (rectilinear) {
        if (!raster_device_box(canonical, rect, box)) return RASTER_OK;  // empty, not an error
        bool any = antialias ? raster_device_rect_touched(canonical, rect, &area)
                             : raster_device_rect_covered(canonical, rect, &area);
        if (!any) return RASTER_OK;
    } else {
        // A degenerate rectangle or a singular transform covers nothing. That is an answer,
        // not a failure: CGAffineTransform.inverted() returns a singular matrix unchanged and
        // BrushStroke concatenates the result.
        if (!raster_quad_make(ctx->state.ctm, rect, &quad)) return RASTER_OK;
        bool any = antialias ? raster_quad_touched_bounds(&quad, &area)
                             : raster_quad_covered_bounds(&quad, &area);
        if (!any) return RASTER_OK;
    }

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

    // Three rows: the column profile (rectilinear only), the row being combined, and a
    // temporary for one shape's contribution. Reserved unconditionally, because the quad path
    // needs the last two whether or not there is a plane, and getting this wrong is a heap
    // overflow the oracle cannot see.
    int32_t width = area.x1 - area.x0;
    if (!scratch_reserve(ctx, (size_t)width * 3)) return RASTER_OUT_OF_MEMORY;
    uint8_t *combined = ctx->scratch + width;
    uint8_t *temp = ctx->scratch + (size_t)width * 2;

    const uint8_t *columnCoverage = NULL;
    if (rectilinear && antialias) {
        raster_axis_coverage(box[0], box[2], area.x0, area.x1, ctx->scratch);
        columnCoverage = ctx->scratch;
    }

    const raster_clip *state = ctx->state.clip;
    const raster_region *clip = state->region;
    coverage_plane plane = clip_plane(state);

    size_t clipCount = raster_region_count(clip);
    for (size_t i = 0; i < clipCount; ++i) {
        raster_rect band = raster_region_rect(clip, i);
        raster_rect hit = {
            band.x0 > area.x0 ? band.x0 : area.x0, band.y0 > area.y0 ? band.y0 : area.y0,
            band.x1 < area.x1 ? band.x1 : area.x1, band.y1 < area.y1 ? band.y1 : area.y1,
        };
        if (raster_rect_is_empty(hit)) continue;

        for (int32_t y = hit.y0; y < hit.y1; ++y) {
            int32_t rowX0 = hit.x0;
            size_t count = (size_t)(hit.x1 - hit.x0);
            const uint8_t *coverage = NULL;

            if (!rectilinear) {
                if (antialias) {
                    raster_quad_coverage_row(&quad, rowX0, y, count, combined);
                    coverage = combined;
                } else {
                    // Hard edges: narrow the row to the span instead of carrying zeroes
                    // through the blend. The narrowing happens *after* the intersection with
                    // the clip band, which is what keeps plane_apply inside its buffer.
                    int32_t spanLo, spanHi;
                    if (!raster_quad_row_span(&quad, y, &spanLo, &spanHi)) continue;
                    if (spanLo < rowX0) spanLo = rowX0;
                    if (spanHi > hit.x1) spanHi = hit.x1;
                    if (spanHi <= spanLo) continue;
                    rowX0 = spanLo;
                    count = (size_t)(spanHi - spanLo);
                }
            } else if (antialias) {
                uint8_t row;
                raster_axis_coverage(box[1], box[3], y, y + 1, &row);
                if (row == 0) continue;
                const uint8_t *columns = columnCoverage + (hit.x0 - area.x0);
                if (row == 255) {
                    coverage = columns;
                } else {
                    // Only the top and bottom rows of an antialiased fill are partial, so
                    // this scratch pass runs at most twice per fill.
                    for (size_t x = 0; x < count; ++x)
                        combined[x] = (uint8_t)((columns[x] * row + 127) / 255);
                    coverage = combined;
                }
            }

            coverage = clip_apply(state, &plane, combined, temp, coverage, rowX0, y, count);
            uint8_t *dst = pixels + (size_t)y * stride + (size_t)rowX0 * bpp;
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
        return RASTER_UNSUPPORTED_FORMAT;

    // A selector, not a gate. The sampler itself has always been general affine -- it
    // evaluates the source coordinate from the full inverse per pixel, and applies the kernel
    // in source space -- so only the destination extent ever needed a second path.
    raster_matrix canonical;
    bool rectilinear = raster_matrix_is_rectilinear(ctx->state.ctm, rect, &canonical);
    if (!rectilinear) canonical = ctx->state.ctm;

    // The canonical matrix, not the raw one. These disagreed before -- this call took
    // ctx->state.ctm while raster_device_box two lines down took the canonical form -- and it
    // was invisible only because raster_image_mapping canonicalises again internally. It still
    // does, so passing the raw matrix here survives every test; the change is defensive, and
    // the thing it defends against is someone later deciding that the callers canonicalise so
    // the callee need not. A quarter turn's 6.1e-17 reaching the composition costs the
    // bit-exact permutation that a memcmp over a whole 4000x4000 buffer depends on.
    raster_matrix deviceToImage;
    if (!raster_image_mapping(canonical, rect, raster_surface_width(image),
                              raster_surface_height(image), &deviceToImage))
        return RASTER_OK;  // degenerate, not a failure

    bool antialias = ctx->state.antialias;
    double box[4];
    raster_quad quad;
    raster_rect area;
    if (rectilinear) {
        if (!raster_device_box(canonical, rect, box)) return RASTER_OK;
        bool any = antialias ? raster_device_rect_touched(canonical, rect, &area)
                             : raster_device_rect_covered(canonical, rect, &area);
        if (!any) return RASTER_OK;
    } else {
        if (!raster_quad_make(canonical, rect, &quad)) return RASTER_OK;
        bool any = antialias ? raster_quad_touched_bounds(&quad, &area)
                             : raster_quad_covered_bounds(&quad, &area);
        if (!any) return RASTER_OK;
    }

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
    // A row of resampled source, a row of column coverage, the row being combined, and a
    // temporary for one shape's contribution.
    if (!scratch_reserve(ctx, width * (bpp + 3))) return RASTER_OUT_OF_MEMORY;
    uint8_t *samples = ctx->scratch;
    uint8_t *columnCoverage = ctx->scratch + width * bpp;
    uint8_t *combined = columnCoverage + width;
    uint8_t *temp = combined + width;
    if (rectilinear && antialias)
        raster_axis_coverage(box[0], box[2], area.x0, area.x1, columnCoverage);

    const raster_clip *state = ctx->state.clip;
    const raster_region *clip = state->region;
    coverage_plane plane = clip_plane(state);
    size_t clipCount = raster_region_count(clip);
    for (size_t i = 0; i < clipCount; ++i) {
        raster_rect band = raster_region_rect(clip, i);
        raster_rect hit = {
            band.x0 > area.x0 ? band.x0 : area.x0, band.y0 > area.y0 ? band.y0 : area.y0,
            band.x1 < area.x1 ? band.x1 : area.x1, band.y1 < area.y1 ? band.y1 : area.y1,
        };
        if (raster_rect_is_empty(hit)) continue;

        for (int32_t y = hit.y0; y < hit.y1; ++y) {
            int32_t rowX0 = hit.x0;
            size_t count = (size_t)(hit.x1 - hit.x0);
            const uint8_t *coverage = NULL;

            if (!rectilinear) {
                if (antialias) {
                    raster_quad_coverage_row(&quad, rowX0, y, count, combined);
                    coverage = combined;
                } else {
                    // Narrow to the span rather than sampling the whole bounding row. Not just
                    // waste: raster_sample_row replicates the source's edge outside it, so a
                    // bounding-box row with non-zero coverage would smear the image's border
                    // across the box. The narrowing happens after the intersection with the
                    // clip band, which keeps plane_apply inside its buffer.
                    int32_t spanLo, spanHi;
                    if (!raster_quad_row_span(&quad, y, &spanLo, &spanHi)) continue;
                    if (spanLo < rowX0) spanLo = rowX0;
                    if (spanHi > hit.x1) spanHi = hit.x1;
                    if (spanHi <= spanLo) continue;
                    rowX0 = spanLo;
                    count = (size_t)(spanHi - spanLo);
                }
            } else if (antialias) {
                uint8_t row;
                raster_axis_coverage(box[1], box[3], y, y + 1, &row);
                if (row == 0) continue;
                const uint8_t *columns = columnCoverage + (hit.x0 - area.x0);
                if (row == 255) {
                    coverage = columns;
                } else {
                    for (size_t x = 0; x < count; ++x)
                        combined[x] = (uint8_t)((columns[x] * row + 127) / 255);
                    coverage = combined;
                }
            }

            coverage = clip_apply(state, &plane, combined, temp, coverage, rowX0, y, count);
            raster_sample_row(image, deviceToImage, rowX0, y, count,
                              ctx->state.interpolation, samples);
            uint8_t *dst = pixels + (size_t)y * stride + (size_t)rowX0 * bpp;
            if (format == RASTER_GRAY8)
                raster_blend_row_gray(dst, samples, count, blend, alpha8, coverage);
            else
                raster_blend_row_rgba(dst, samples, count, blend, alpha8, coverage);
        }
    }
    return RASTER_OK;
}
