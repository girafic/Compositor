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

// A view onto a sub-rectangle, sharing the parent's pixels. Returns NULL when the rectangle
// is empty or reaches outside the surface — callers rely on that, because
// CGImage.cropping(to:) returns nil for exactly those cases.
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

// Copy-on-write. If the allocation is shared, replaces it with a private copy so the
// surface can be drawn into without disturbing anyone else's view of the old pixels.
// Returns false only if the copy could not be allocated.
//
// This is what makes CGContext.makeImage() behave like CoreGraphics: the image shares the
// context's pixels until the context is drawn into again, and the draw is what separates
// them. DownsampleCache.halve relies on it — it snapshots a context, crops a row out of the
// snapshot, and draws that row back into the same context.
bool raster_surface_make_unique(raster_surface *surface);

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

#endif
