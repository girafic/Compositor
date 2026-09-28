#ifndef raster_h
#define raster_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The raster engine behind Compositor's CoreGraphics replacement.
//
// The whole app uses exactly two pixel layouts, and the engine implements only those:
//
//   RASTER_RGBA8  8 bits per component, premultiplied, byte order R,G,B,A. This is what
//                 CGBitmapContext gave us as `premultipliedLast | byteOrder32Big`.
//   RASTER_GRAY8  8 bits, ONE component, and no alpha channel at all — the layout the app
//                 gets from CGColorSpaceCreateDeviceGray() with CGImageAlphaInfo.none.
//
// The second one is easy to get wrong. A GRAY8 surface is an *opaque* grayscale image, not
// an alpha plane: masks, brush coverage and selections are all stored as gray samples where
// white means "paint fully". Source-over onto it is therefore a lerp, not alpha
// compositing, and clearing it writes black rather than transparency.

// Formats and blend modes cross into Swift as plain integers rather than C enums on
// purpose: Swift's importer renames C enum cases by stripping common prefixes, and the
// exact spelling it lands on is not something to discover from a CI failure. The named
// constants below stay the vocabulary on the C side.
typedef uint32_t raster_format;
typedef uint32_t raster_blend;

enum {
    RASTER_RGBA8 = 0,
    RASTER_GRAY8 = 1,
};

static inline size_t raster_bytes_per_pixel(raster_format format) {
    return format == RASTER_GRAY8 ? 1 : 4;
}

// Blend modes. The values match CGBlendMode's so a Swift `CGBlendMode.rawValue` can be
// handed straight across; the gaps are the Porter-Duff modes the app never uses.
enum {
    RASTER_BLEND_NORMAL = 0,
    RASTER_BLEND_MULTIPLY = 1,
    RASTER_BLEND_SCREEN = 2,
    RASTER_BLEND_OVERLAY = 3,
    RASTER_BLEND_DARKEN = 4,
    RASTER_BLEND_LIGHTEN = 5,
    RASTER_BLEND_COLOR_DODGE = 6,
    RASTER_BLEND_COLOR_BURN = 7,
    RASTER_BLEND_SOFT_LIGHT = 8,
    RASTER_BLEND_HARD_LIGHT = 9,
    RASTER_BLEND_DIFFERENCE = 10,
    RASTER_BLEND_EXCLUSION = 11,
    RASTER_BLEND_HUE = 12,
    RASTER_BLEND_SATURATION = 13,
    RASTER_BLEND_COLOR = 14,
    RASTER_BLEND_LUMINOSITY = 15,
    RASTER_BLEND_CLEAR = 16,
    RASTER_BLEND_COPY = 17,
    RASTER_BLEND_DESTINATION_OUT = 23,
};

// True for the modes the app can actually reach. Everything else is rejected at the
// boundary rather than silently drawn as Normal.
bool raster_blend_supported(raster_blend mode);

// MARK: - Surfaces

// An immutable-by-convention pixel buffer with a reference count.
//
// `data` points at the first byte of this surface's own top-left pixel, which for a crop
// view is an offset into the parent's allocation — cropping never copies, exactly as
// CGImage.cropping(to:) never copies. `stride` is the parent's row stride in that case, so
// it is not necessarily `width * bytes_per_pixel`.
//
// Lifetime is by reference count on the underlying allocation, shared between a surface and
// every crop view of it. `raster_surface_retain`/`release` are the only way to manage it.
typedef struct raster_surface raster_surface;

// Allocates a zero-filled surface. Returns NULL if the dimensions are non-positive or the
// allocation fails. The caller owns one reference.
raster_surface *raster_surface_create(size_t width, size_t height, raster_format format);

// Wraps caller-owned memory without taking ownership of it. The caller guarantees the bytes
// outlive every reference to the surface. Used for the one place the app hands CGContext a
// `data:` pointer it allocated itself.
raster_surface *raster_surface_create_borrowed(void *bytes, size_t width, size_t height,
                                               size_t stride, raster_format format);

raster_surface *raster_surface_retain(raster_surface *surface);
void raster_surface_release(raster_surface *surface);

// A view onto a sub-rectangle, sharing the parent's pixels.
//
// The rectangle is intersected with the surface rather than refused when it overhangs,
// matching CGImageCreateWithImageInRect. NULL means the request was empty or missed the
// surface entirely — the thirteen call sites all branch on that, and three of them treat it
// as "drop this piece", so refusing an overhang would silently lose pixels.
raster_surface *raster_surface_crop(raster_surface *surface, size_t x, size_t y,
                                    size_t width, size_t height);

// An independent, tightly packed copy.
raster_surface *raster_surface_copy(const raster_surface *surface);

size_t raster_surface_width(const raster_surface *surface);
size_t raster_surface_height(const raster_surface *surface);
size_t raster_surface_stride(const raster_surface *surface);
raster_format raster_surface_format(const raster_surface *surface);

// The pixels. `raster_surface_mutable_bytes` returns NULL when the surface shares its
// allocation with another reference — see raster_surface_make_unique.
const uint8_t *raster_surface_bytes(const raster_surface *surface);
uint8_t *raster_surface_mutable_bytes(raster_surface *surface);

// True when this surface is the only owner of its allocation and can therefore be written
// to in place.
bool raster_surface_is_unique(const raster_surface *surface);

// References to this surface *object*, as opposed to the number of surfaces sharing its
// pixels — `raster_surface_is_unique` answers that one. The context's snapshot registry uses
// this to tell a snapshot somebody still holds from one that has already been released, and
// so to skip the copy for the second kind.
size_t raster_surface_refcount(const raster_surface *surface);

// False when the caller supplied the memory. Such a surface can never be detached from it,
// so anything that would need a private copy has to be given one up front instead.
bool raster_surface_is_owned(const raster_surface *surface);

// Copy-on-write. If the allocation is shared, replaces it with a private copy so the
// surface can be drawn into without disturbing anyone else's view of the old pixels.
// Returns false only if the copy could not be allocated.
//
// This is what makes CGContext.makeImage() behave like CoreGraphics: the image shares the
// context's pixels until the context is drawn into again, and the draw is what separates
// them. DownsampleCache.halve relies on it — it snapshots a context, crops a row out of the
// snapshot, and draws that row back into the same context.
bool raster_surface_make_unique(raster_surface *surface);

// MARK: - Regions

// A set of device pixels, held as y-bands of disjoint half-open rectangles.
//
// This is the rectilinear half of the clip. A graphics state's clip is a region plus an
// optional coverage plane; keeping the rectilinear part exact matters because
// `boundingBoxOfClipPath` is read as *geometry*, not as a hint — AdjustmentSurface sizes an
// offscreen from it, and Grain anchors its noise pattern to the resulting origin, so a
// loose bound visibly shifts the grain.
//
// Canonical form, maintained by every operation:
//   - rectangles are sorted by y0, then x0;
//   - rectangles sharing a y0 share a y1 (they form one band) and do not touch or overlap;
//   - two vertically adjacent bands never have identical x-intervals (they get merged).
// Two regions covering the same pixels therefore have identical rectangle lists.
typedef struct { int32_t x0, y0, x1, y1; } raster_rect;  // half-open: x0 <= x < x1

static inline bool raster_rect_is_empty(raster_rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

typedef struct raster_region raster_region;

raster_region *raster_region_create(void);                   // empty
raster_region *raster_region_create_rect(raster_rect rect);  // empty rect gives an empty region
raster_region *raster_region_copy(const raster_region *region);
void raster_region_destroy(raster_region *region);

bool raster_region_is_empty(const raster_region *region);
// Exact, not conservative. All zeroes when empty.
raster_rect raster_region_bounds(const raster_region *region);
bool raster_region_contains(const raster_region *region, int32_t x, int32_t y);

size_t raster_region_count(const raster_region *region);
raster_rect raster_region_rect(const raster_region *region, size_t index);

// Each returns a newly allocated region, or NULL if an allocation failed.
//
// `union` is what accumulating rectangles under the winding fill rule produces; `xor` is
// what the even-odd rule produces, where an overlap of two added rectangles is *out*. Both
// are needed: LayerRenderer builds its brush-preview clips with addRect + evenOdd, while
// every other accumulation is winding.
raster_region *raster_region_union(const raster_region *a, const raster_region *b);
raster_region *raster_region_intersect(const raster_region *a, const raster_region *b);
raster_region *raster_region_subtract(const raster_region *a, const raster_region *b);
raster_region *raster_region_xor(const raster_region *a, const raster_region *b);

// The hot path: clip(to: rect) under an axis-aligned transform.
raster_region *raster_region_intersect_rect(const raster_region *region, raster_rect rect);

// MARK: - Device geometry

// Laid out to match CGAffineTransform and CGRect, so Swift can hand them across unchanged.
typedef struct { double a, b, c, d, tx, ty; } raster_matrix;
typedef struct { double x, y, width, height; } raster_frect;

// Outcomes that a caller has to tell apart. In particular an empty result and "could not
// compute" must never arrive as the same thing.
typedef uint32_t raster_status;

enum {
    RASTER_OK = 0,
    RASTER_OUT_OF_MEMORY = 1,
    // The transform is not rectilinear *and* the operation has no parallelogram path yet.
    // Since the general affine sampler landed, that is only `add_rect` and `clip_path`: their
    // current path storage is device-space integer rectangles, which cannot hold a sheared
    // quadrilateral, and TiledLayerRenderer's own use needs CGPath.subtracting besides.
    //
    // Everything else — draw, fill, clear, clip(to:), clip(to:mask:) — accepts any affine
    // transform. A degenerate or singular one draws nothing rather than refusing, which is
    // what CoreGraphics does and what the app reaches: CGAffineTransform.inverted() returns
    // itself for a singular matrix and BrushStroke concatenates the result.
    //
    // It is still refused rather than approximated by a bounding box, for the same reason an
    // unsupported blend mode is rejected rather than silently drawn as Normal: a clip that
    // fails *open* draws pixels the caller asked to have masked away, and nothing downstream
    // would catch it.
    RASTER_UNSUPPORTED_TRANSFORM = 2,
    // A surface whose pixel layout the operation has no answer for: an image drawn into a
    // target of the other format, or a mask that is not GRAY8. Distinct from the above
    // because the two are told apart only by the message they produce, and a port whose only
    // Swift check is CI cannot afford a diagnostic that names the wrong cause.
    RASTER_UNSUPPORTED_FORMAT = 3,
};

typedef uint32_t raster_interpolation;

enum {
    RASTER_INTERPOLATION_DEFAULT = 0,
    RASTER_INTERPOLATION_NONE = 1,
    RASTER_INTERPOLATION_LOW = 2,
    RASTER_INTERPOLATION_HIGH = 3,
    RASTER_INTERPOLATION_MEDIUM = 4,
};

// The device pixel column a device-space edge falls on, under the rule that a pixel belongs
// to a range when its *centre* does: column i spans [i, i+1) with centre i + 0.5, so the
// range [lo, hi) covers exactly the columns [raster_pixel_edge(lo), raster_pixel_edge(hi)).
//
// This is ceil(t - 0.5), which is round-half-*down* — not round(t), and not floor(t + 0.5).
// Which way ties go is a free choice; what is not free is that both edges make the same
// choice, because that is what makes two ranges that share an edge partition it exactly,
// with no pixel doubled and none skipped, at *any* edge position. Tests pin the tie-break so
// a later "simplification" to floor(t + 0.5) fails loudly rather than quietly.
//
// That guarantee is stronger than it looks, and it is worth knowing why: two abutting
// rectangles do *not* hand us the same double for their shared edge. CGRect derives maxX as
// origin + size, so TiledLayerRenderer's grid, which computes each tile's x and width
// separately, produces neighbours whose shared edge differs in the last bits — measured at
// up to 1.1e-13 on a realistic layer, on 8 of 11 tile boundaries. The integer collapse here
// is what absorbs that: both sides land on the same column unless a pixel centre happens to
// fall inside that 1e-13 window. See the parallelogram section for what this costs once the
// edge is no longer vertical.
//
// CoreGraphics itself uses the other rule — a pixel belongs when the overlap has positive
// area — which is why its hard clips hairline on fractional edges and why
// TiledLayerRenderer rounds clip edges to whole device pixels by hand before using them.
// The two rules agree exactly when both edges are integers, which is nearly all of this app.
int32_t raster_pixel_edge(double t);

// True when `m` maps axis-aligned rectangles to axis-aligned rectangles.
//
// This is a fast-path *selector*, not a gate. A rectilinear transform takes the exact integer
// path in coverage.c; anything else takes the parallelogram path below. Keeping the selector
// is what preserves the quarter-turn exactness the rest of the engine rests on — the
// canonicalisation it performs is the reason a 90-degree draw is a bit-exact permutation
// rather than a resample at 6.1e-17 off the grid.
//
// Two families qualify, and missing the second is a real bug rather than a theoretical one:
// a quarter turn gives a = d = cos(pi/2) = 6.1e-17 with b, c = +-1, so testing only |b| and
// |c| rejects every 90-degree layer, which is a one-click user action.
//
// The tolerance is rect-relative on purpose. A bare `|b| < 1e-9` is wrong in both
// directions: over a 4000-pixel rectangle it rejects matrices that are straight to within
// 4e-6 of a pixel, and at a degenerate scale it accepts anything. What decides the answer is
// how far the off-diagonal terms actually move a corner of *this* rectangle.
//
// Being rect-relative also means the answer is per-(matrix, rectangle), not per-matrix: the
// same CTM can take the integer path for a 256-wide rectangle and the parallelogram path for
// a 4000-wide one. The two paths quantise antialiasing differently — per axis then
// multiplied, versus the area once — so they can disagree by 1 at that boundary. Pinned by a
// test rather than left to be discovered.
//
// On success `out` receives `m` with the negligible pair forced to exact zero, so that
// 6.1e-17 never leaks into a device coordinate.
bool raster_matrix_is_rectilinear(raster_matrix m, raster_frect rect, raster_matrix *out);

// The exact device-space box a user rectangle maps to, before any pixel rule is applied:
// x0, y0, x1, y1 with x0 <= x1 and y0 <= y1. `m` must have passed
// raster_matrix_is_rectilinear — on any other matrix this returns false, which every caller
// reads as "empty", so calling it without the selector first is a silent no-op rather than
// an error. False when the rectangle is empty or any coordinate is not
// finite — callers must not reach the integer conversions with a NaN or an infinity, because
// casting those is undefined and in an optimised build yields an arbitrary region rather
// than a crash. That is reachable: CGAffineTransform.inverted() returns itself for a
// singular matrix, and BrushStroke concatenates the result.
bool raster_device_box(raster_matrix m, raster_frect rect, double out[4]);

// The device pixels a user rectangle covers, under each of the two rules above.
// `_touched` is the wider one and is the range an antialiased fill writes, with fractional
// coverage on the boundary pixels; `_covered` is for hard clips and non-antialiased fills.
bool raster_device_rect_covered(raster_matrix m, raster_frect rect, raster_rect *out);
bool raster_device_rect_touched(raster_matrix m, raster_frect rect, raster_rect *out);

// Per-pixel coverage of the device interval [lo, hi) over the columns [first, last), written
// as `last - first` bytes. Only the two end pixels can be partial; everything between is
// 255. Multiply the two axes' answers to get a pixel's coverage.
void raster_axis_coverage(double lo, double hi, int32_t first, int32_t last, uint8_t *out);

// MARK: - Parallelograms

// A user rectangle under a transform that is *not* rectilinear — a rotation, a shear, or
// both. Shear is not hypothetical: ImageResizer applies a non-uniform scale outside
// LayerRenderer's rotation and says so in its own comment, and any composition of two
// layers' matrices shears once they differ in both angle and pixel aspect.
//
// An affine map takes a rectangle to a *parallelogram*, never a general quadrilateral —
// opposite sides stay parallel. That is what keeps this cheap: the shape is fully described
// by its four device corners plus the map back to user space, where containment is two
// comparisons per axis against the rectangle's own edges.
//
// Deliberately *not* normalised to a unit square, though that would make containment
// `0 <= s < 1` and read more neatly. Normalising divides by each rectangle's own width, so
// two abutting rectangles compute their shared edge through different arithmetic and disagree
// by an ulp — measured, and enough to double or drop a pixel whose centre lands on the edge.
// Comparing against the user-space edges instead means both neighbours test the same literal,
// and the partition is exact whenever their rectangles agree as doubles.
typedef struct {
    raster_matrix toUser;              // device point -> user space
    double ux0, ux1, uy0, uy1;         // the rectangle, standardised so ux0 < ux1
    double x[4], y[4];                 // device corners, in order around the shape
} raster_quad;

// Builds the parallelogram for `rect` under `m`. False when the rectangle is degenerate, the
// matrix is singular, or anything is not finite — all of which mean "nothing is covered",
// which is the same answer raster_device_box gives on the rectilinear path.
bool raster_quad_make(raster_matrix m, raster_frect rect, raster_quad *out);

// The half-open column span of device row `y` whose pixel *centres* lie inside the
// parallelogram. False when the row covers nothing.
//
// This is the only place the covered set is decided. The obvious alternative — map each
// pixel centre back and test it against the rectangle — is a different arithmetic for the
// same question, and the two disagree at the last bit, which is exactly one doubled or
// dropped pixel at a row end. So there is one expression, and it ends in raster_pixel_edge
// for the reason given there: the integer collapse absorbs the 1e-13 by which two abutting
// rectangles disagree about their shared edge.
//
// What that buys, stated honestly, because "exact" would be a lie: a vertical seam makes one
// such comparison, a diagonal seam makes one *per row*. The chance of a doubled or dropped
// pixel therefore goes from ~1e-13 per seam to ~N * 1e-13, about 4e-10 down a 4000-pixel
// edge. Small enough to build on, not small enough to call exact. It is also why
// TiledLayerRenderer's rotated tolerance is 12 and its unrotated one is 2.
bool raster_quad_row_span(const raster_quad *quad, int32_t y, int32_t *x0, int32_t *x1);

// The exact bounding box of the covered pixel set — found by walking the rows, so it is
// tighter than the rounded-out box of the geometry and is honest for
// boundingBoxOfClipPath. False when nothing is covered at all, which a sliver narrower than
// a pixel reaches while still having a non-empty geometric box; the clip must collapse to
// empty there, or Swift reports a rectangle where it owes CGRect.null.
//
// Allocates nothing: the cost is one pass over the rows, which is what lets a rotated clip
// avoid materialising a coverage plane it may never read.
bool raster_quad_covered_bounds(const raster_quad *quad, raster_rect *out);

// Exact area coverage of each pixel in `[x0, x0 + count)` of row `y`: the area of the pixel
// square intersected with the parallelogram, in 0...255.
//
// Exact rather than supersampled, because a rotated layer's edge is the one place the app
// has no tolerance to spare and because an exact answer is checkable — the areas must sum to
// |det| times the rectangle's area, which catches any doubled or dropped sliver in one
// assertion. Used when antialiasing is on; raster_quad_row_span is the hard-edged answer.
void raster_quad_coverage_row(const raster_quad *quad, int32_t x0, int32_t y, size_t count,
                              uint8_t *out);

// MARK: - Sampling

// The device-pixel -> source-pixel mapping for drawing an image of `imageWidth` x
// `imageHeight` into `rect` under `ctm`. Source row 0 maps to the rect's maximum y.
//
// Applying the result to a device pixel's *centre* gives a continuous source coordinate in
// which source pixel i spans [i, i+1) and has its centre at i + 0.5.
//
// General affine: rotation and shear are handled, not refused. The tensor-product kernel in
// raster_sample_row is applied in *source* space, which is valid under any affine map, so
// nothing about a rotated draw needs a separate path — only the destination extent does.
// A rectilinear `ctm` is still canonicalised on the way through, because that is what keeps a
// quarter turn a bit-exact permutation.
//
// False when the rect or the image is degenerate, or when the mapping is not invertible.
bool raster_image_mapping(raster_matrix ctm, raster_frect rect,
                          size_t imageWidth, size_t imageHeight, raster_matrix *out);

// Resamples `count` pixels of `image` into `out`, for device row `y` starting at device
// column `x0`, under the mapping from raster_image_mapping.
//
// `out` receives tightly packed pixels in the image's own format. Samples outside the image
// replicate its edge rather than fading to transparency, so a reduction does not pull
// emptiness into the border — which is the artefact TiledLayerRenderer's margins exist to
// avoid at the layer's own edge.
//
// The kernel is chosen by `quality` and stretched by the reduction factor, never by anything
// derived from the image's size or position: that is what keeps a piece of an image
// identical to the same region of the whole.
void raster_sample_row(const raster_surface *image, raster_matrix deviceToImage,
                       int32_t x0, int32_t y, size_t count,
                       raster_interpolation quality, uint8_t *out);

// MARK: - Compositing

// One source-over-with-blend of a single RGBA8 pixel.
//
// `src` and `dst` are premultiplied RGBA. `alpha` is the constant alpha from the graphics
// state and `coverage` the clip's coverage at this pixel, both 0-255; they multiply into the
// source the way CGContext.setAlpha and a clip mask do.
void raster_blend_rgba(uint8_t *dst, const uint8_t src[4], raster_blend mode,
                       uint8_t alpha, uint8_t coverage);

// The same for a GRAY8 surface, which has no alpha channel: the result is a lerp of `dst`
// towards the blended value by alpha x coverage.
void raster_blend_gray(uint8_t *dst, uint8_t src, raster_blend mode,
                       uint8_t alpha, uint8_t coverage);

// A whole run of pixels. `coverage` may be NULL, meaning fully covered.
void raster_blend_row_rgba(uint8_t *dst, const uint8_t *src, size_t count, raster_blend mode,
                           uint8_t alpha, const uint8_t *coverage);
void raster_blend_row_gray(uint8_t *dst, const uint8_t *src, size_t count, raster_blend mode,
                           uint8_t alpha, const uint8_t *coverage);

// A run of one constant colour, which is what every fill() ends up doing. `src` is a
// premultiplied RGBA quadruple for RGBA8, a single gray sample for GRAY8.
void raster_fill_row_rgba(uint8_t *dst, const uint8_t src[4], size_t count, raster_blend mode,
                          uint8_t alpha, const uint8_t *coverage);
void raster_fill_row_gray(uint8_t *dst, uint8_t src, size_t count, raster_blend mode,
                          uint8_t alpha, const uint8_t *coverage);

// MARK: - Contexts

// A drawing destination plus a stack of graphics states. This is what CGContext is.
//
// The CTM is stored as user space -> *device* space, already composed with the bitmap's base
// flip, because that is the matrix every fill needs. Composition itself lives on the Swift
// side: this object only reads and writes the whole matrix, so there is exactly one graphics
// state stack (here) and exactly one implementation of pre- versus post-concatenation
// (CGAffineTransform, already tested). Splitting it the other way needs two stacks kept in
// lockstep.
typedef struct raster_context raster_context;

raster_context *raster_context_create(raster_surface *target);  // retains `target`
void            raster_context_destroy(raster_context *ctx);
raster_surface *raster_context_target(raster_context *ctx);     // borrowed, not retained

// MARK: Graphics state

// `save` copies the top of the stack; `restore` at depth 0 is a no-op rather than an error.
// Nothing in the app relies on that — its one apparent imbalance is a single save with two
// exit paths — but a stack underflow that traps would take the whole test process with it.
raster_status raster_context_save(raster_context *ctx);
void          raster_context_restore(raster_context *ctx);
size_t        raster_context_depth(const raster_context *ctx);

raster_matrix raster_context_matrix(const raster_context *ctx);
void          raster_context_set_matrix(raster_context *ctx, raster_matrix m);

// Alpha stays a double until the moment it reaches raster_blend_*, which takes a byte.
// Quantising into the state instead would make setAlpha(0.5) compute with 0.50196 — harmless
// for a self-comparison, wrong against any hand-computed expectation, and compounding once a
// transparency layer multiplies a second alpha in (0.5 * 0.5 = 0.25, but 128*128/255^2 =
// 0.2522).
void raster_context_set_alpha(raster_context *ctx, double alpha);
void raster_context_set_blend(raster_context *ctx, raster_blend mode);
void raster_context_set_antialias(raster_context *ctx, bool on);
void raster_context_set_interpolation(raster_context *ctx, raster_interpolation quality);
raster_interpolation raster_context_interpolation(const raster_context *ctx);

// Unpremultiplied RGBA in the destination's own space: for GRAY8 only the first component
// and the alpha are read.
void raster_context_set_fill_color(raster_context *ctx, const double rgba[4]);

// MARK: Clipping

// The current path is a list of rectangles and belongs to the *context*, not to the graphics
// state: CoreGraphics does not save or restore it, and LayerRenderer depends on that — it
// calls addRect again immediately after a clip. `clip` consumes the list.
//
// Curves are absent rather than stubbed, so a premature call site fails to compile instead of
// silently drawing nothing.
raster_status raster_context_add_rect(raster_context *ctx, raster_frect rect);
raster_status raster_context_clip_path(raster_context *ctx, bool even_odd);
void          raster_context_reset_path(raster_context *ctx);

// Intersects the clip with `rect` under the current CTM.
//
// Under a rectilinear CTM this is a region intersection and nothing else. Under any other
// affine transform the rectangle becomes a parallelogram, and the clip carries it as a shape
// alongside its region rather than rasterising it: `raster_clip` holds a short list of
// parallelograms, and a row's coverage is the plane times each of them. That is not an
// optimisation for its own sake — BrushStroke clips inside its per-dab loop, roughly 14 dabs
// per mouse move, and materialising a plane there would mean a multi-megabyte calloc per dab
// on the one path the app has a performance document about.
//
// A rotated clip honours the antialias flag, where a rectilinear one is always hard. That
// looks inconsistent and is not: on integer geometry the flag is provably invisible (a test
// pins it), so the rectilinear path loses nothing by ignoring it, whereas a rotated edge is
// never integer and a hard clip around an antialiased draw throws the smoothing away again —
// worst at 45 degrees, on every masked rotated layer. TiledLayerRenderer turns antialiasing
// off exactly where it needs neighbouring pieces to meet, so honouring the flag gives both.
raster_status raster_context_clip_rect(raster_context *ctx, raster_frect rect);

// Intersects the clip with `rect` and attenuates it by `mask`'s grey values, sampled over that
// rect at the current interpolation quality. This is CGContextClipToMask.
//
// Two halves that are easy to conflate: the *rectangle* clips hard, by the same device-pixel
// centre rule as every other clip here, and the mask supplies all of the softness inside it.
// Nested mask clips multiply -- a * b / 255 -- which is what makes a folder mask and a layer's
// own mask compose the way the app expects.
//
// `mask` must be GRAY8. The app's masks are all DeviceGray with alphaInfo .none, and a colour
// image would raise a question (its grey? its alpha?) that is better refused than guessed.
//
// Any affine transform is accepted. Under rotation the sampled mask multiplies the
// parallelogram's coverage, exactly as it multiplies the enclosing rectangle otherwise, and
// the case is thoroughly real: LayerRenderer and FolderMaskClip both clip inside a
// rotate(by:).
raster_status raster_context_clip_mask(raster_context *ctx, const raster_surface *mask,
                                       raster_frect rect);

// The exact device bounds of the clip. False means the clip is empty, which the caller must
// keep distinct from a degenerate rectangle: raster_region_bounds answers all-zeroes for an
// empty region, and Swift has to turn "empty" into CGRect.null rather than CGRect.zero.
// Selection builds an empty clip deliberately, and TiledLayerRenderer insets the result by
// -64 — from .zero that would be a nonsense 128x128 rectangle at the origin.
//
// A mask clip contributes its *rectangle* and never its contents: a mask that happens to be
// black along one edge does not shrink the bounds. This is read as geometry rather than as a
// hint — AdjustmentSurface sizes an offscreen from it and Grain anchors its pattern to the
// resulting origin — so it has to depend only on things the caller can predict.
//
// For a rotated clip these device bounds are still exact — raster_quad_covered_bounds walks
// the rows rather than rounding the geometry out. What is *not* tight is what Swift does with
// them afterwards: mapping a device box back through a rotated CTM yields the box of a box,
// four times the area at 45 degrees. That is inherent in answering with a CGRect, and
// CGContextGetClipBoundingBox has the same property, but it means the Swift-side promise has
// to read "a bound" rather than "tight" under rotation.
bool raster_context_clip_bounds(const raster_context *ctx, raster_rect *out);

// For tests: the clip region itself, borrowed.
const raster_region *raster_context_clip_region(const raster_context *ctx);

// MARK: Painting

raster_status raster_context_fill_rect(raster_context *ctx, raster_frect rect);

// Draws `image` to fill `rect` in user space.
//
// Source row 0 lands at the rect's maximum y, which is what CoreGraphics does and what the
// app's own flip at BrushRaster.context then cancels, leaving its user space equal to device
// pixels.
//
// All five interpolation qualities agree exactly when the mapping is an integer translation
// at unit scale, and that is a consequence of the kernels rather than a special case: every
// one of them gives weight 1 to the sample it lands on and 0 to its neighbours at integer
// offsets. That identity is load-bearing. RasterSnapshotTests compares a `.high`,
// antialiasing-off, N-times-clipped render against a `.low`, antialiasing-on, single draw
// with memcmp over 64 MB, and a hundred-odd other assertions read their result back through
// a 1:1 draw.
//
// `low` and `default` widen their kernel by the reduction factor, because the reduction
// handed to a single draw is not bounded by 2: the halving chain saturates at level 6, so a
// 30000-pixel image landing in 100 pixels arrives here as a 4.7x reduction, and the brush's
// stamp fallback can be far worse. A fixed two-tap filter aliases visibly there. Widening by
// the *scale* keeps the kernel independent of where the image sits and how big it is, which
// is what tiled rendering needs — it is prefiltering chosen from the image's own extent that
// breaks crop invariance, not a wider kernel as such.
raster_status raster_context_draw_image(raster_context *ctx, const raster_surface *image,
                                        raster_frect rect);

// CGContextClearRect honours the CTM and the clip but ignores the graphics state's alpha and
// blend mode. It is therefore *not* "fill with the clear blend" — TiledLayerRenderer does
// that separately, inside a transparency layer, and the two must not collapse into one.
raster_status raster_context_clear_rect(raster_context *ctx, raster_frect rect);

// MARK: Snapshots

// The copy-on-write direction is the opposite of the obvious one: the context never moves,
// the snapshot detaches. CGContext.data has to stay pointer-stable across draws, because
// RasterSnapshotTests reads it once, clears and redraws, and then reads through the same
// pointer — and raster_surface_make_unique *moves* the bytes it is called on.
//
// So the context holds a retained list of live snapshots and detaches them before any write,
// including before handing out a writable pointer, which is itself a write in waiting.
// Detaching is self-cleaning: a snapshot nobody else references is released instead of
// copied, so an image that was taken and dropped costs a refcount decrement rather than a
// full-canvas memcpy.
//
// A borrowed target cannot be detached at all, so raster_context_make_snapshot copies it
// eagerly instead.
//
// Unfinished, and deliberately recorded here rather than discovered later: a crop of a
// snapshot shares the same store but is not in this list, so detaching the snapshot would
// leave the crop aliasing the context's live pixels. DownsampleCache does exactly that —
// snapshot, crop one row, draw the row back into the same context — so whoever adds
// cropping must register the derived view with the same context.
raster_surface *raster_context_make_snapshot(raster_context *ctx);  // caller owns one reference
raster_status   raster_context_detach_snapshots(raster_context *ctx);

// Adds an existing surface to the context's snapshot list, retaining it. This is how a view
// derived from a snapshot — a crop, above all — joins the set that gets detached before the
// next write. Without it the derived view keeps the store shared and the context's own
// pixels become unwritable, which surfaces as an out-of-memory rather than as corruption,
// but breaks all the same.
raster_status raster_context_register_snapshot(raster_context *ctx, raster_surface *snapshot);

#endif
