#include "raster.h"

#include <stdlib.h>
#include <string.h>

// The allocation a surface and all its crop views share. Separating it from the surface is
// what lets `raster_surface_crop` be free: the view gets its own width/height/origin and
// just retains this.
typedef struct {
    uint8_t *bytes;
    size_t length;
    size_t refcount;
    bool owned;  // false when the caller supplied the memory and keeps ownership of it
} raster_store;

struct raster_surface {
    raster_store *store;
    uint8_t *data;  // first byte of this surface's top-left pixel, inside store->bytes
    size_t width;
    size_t height;
    size_t stride;
    raster_format format;
    size_t refcount;
};

static raster_store *store_create(size_t length) {
    raster_store *store = malloc(sizeof(raster_store));
    if (!store) return NULL;
    store->bytes = calloc(length ? length : 1, 1);
    if (!store->bytes) {
        free(store);
        return NULL;
    }
    store->length = length;
    store->refcount = 1;
    store->owned = true;
    return store;
}

static raster_store *store_borrow(void *bytes, size_t length) {
    raster_store *store = malloc(sizeof(raster_store));
    if (!store) return NULL;
    store->bytes = bytes;
    store->length = length;
    store->refcount = 1;
    store->owned = false;
    return store;
}

static void store_release(raster_store *store) {
    if (!store || --store->refcount > 0) return;
    if (store->owned) free(store->bytes);
    free(store);
}

static raster_surface *surface_alloc(raster_store *store, uint8_t *data, size_t width,
                                     size_t height, size_t stride, raster_format format) {
    raster_surface *surface = malloc(sizeof(raster_surface));
    if (!surface) {
        store_release(store);
        return NULL;
    }
    surface->store = store;
    surface->data = data;
    surface->width = width;
    surface->height = height;
    surface->stride = stride;
    surface->format = format;
    surface->refcount = 1;
    return surface;
}

raster_surface *raster_surface_create(size_t width, size_t height, raster_format format) {
    if (!width || !height) return NULL;
    size_t bpp = raster_bytes_per_pixel(format);
    // Guard the multiplications rather than trusting them: a corrupt manifest can ask for a
    // 30,000-per-side image, and width * height * bpp is where that would wrap.
    if (width > SIZE_MAX / bpp) return NULL;
    size_t stride = width * bpp;
    if (height > SIZE_MAX / stride) return NULL;

    raster_store *store = store_create(stride * height);
    if (!store) return NULL;
    return surface_alloc(store, store->bytes, width, height, stride, format);
}

raster_surface *raster_surface_create_borrowed(void *bytes, size_t width, size_t height,
                                               size_t stride, raster_format format) {
    if (!bytes || !width || !height) return NULL;
    if (stride < width * raster_bytes_per_pixel(format)) return NULL;
    if (height > SIZE_MAX / stride) return NULL;

    raster_store *store = store_borrow(bytes, stride * height);
    if (!store) return NULL;
    return surface_alloc(store, bytes, width, height, stride, format);
}

raster_surface *raster_surface_retain(raster_surface *surface) {
    if (surface) ++surface->refcount;
    return surface;
}

void raster_surface_release(raster_surface *surface) {
    if (!surface || --surface->refcount > 0) return;
    store_release(surface->store);
    free(surface);
}

raster_surface *raster_surface_crop(raster_surface *surface, size_t x, size_t y,
                                    size_t width, size_t height) {
    // Empty or out of bounds returns NULL, because CGImage.cropping(to:) returns nil for
    // exactly those and five call sites branch on it.
    if (!surface || !width || !height) return NULL;
    if (x > surface->width || y > surface->height) return NULL;
    if (width > surface->width - x || height > surface->height - y) return NULL;

    uint8_t *data = surface->data + y * surface->stride + x * raster_bytes_per_pixel(surface->format);
    ++surface->store->refcount;
    return surface_alloc(surface->store, data, width, height, surface->stride, surface->format);
}

raster_surface *raster_surface_copy(const raster_surface *surface) {
    if (!surface) return NULL;
    raster_surface *copy = raster_surface_create(surface->width, surface->height, surface->format);
    if (!copy) return NULL;
    size_t row = surface->width * raster_bytes_per_pixel(surface->format);
    for (size_t y = 0; y < surface->height; ++y)
        memcpy(copy->data + y * copy->stride, surface->data + y * surface->stride, row);
    return copy;
}

size_t raster_surface_width(const raster_surface *surface) { return surface ? surface->width : 0; }
size_t raster_surface_height(const raster_surface *surface) { return surface ? surface->height : 0; }
size_t raster_surface_stride(const raster_surface *surface) { return surface ? surface->stride : 0; }

raster_format raster_surface_format(const raster_surface *surface) {
    return surface ? surface->format : RASTER_RGBA8;
}

const uint8_t *raster_surface_bytes(const raster_surface *surface) {
    return surface ? surface->data : NULL;
}

bool raster_surface_is_unique(const raster_surface *surface) {
    return surface && surface->store->refcount == 1;
}

size_t raster_surface_refcount(const raster_surface *surface) {
    return surface ? surface->refcount : 0;
}

bool raster_surface_is_owned(const raster_surface *surface) {
    return surface && surface->store->owned;
}

uint8_t *raster_surface_mutable_bytes(raster_surface *surface) {
    if (!surface || !raster_surface_is_unique(surface)) return NULL;
    return surface->data;
}

bool raster_surface_make_unique(raster_surface *surface) {
    if (!surface) return false;
    if (raster_surface_is_unique(surface)) return true;

    // A borrowed store must never be detached. The caller's buffer *is* the destination —
    // quietly copying away from it would strand every subsequent write somewhere the caller
    // never looks. The one borrowed surface in the app is the eyedropper's 1x1 context
    // (ColorPalette.sampleCompositeColor), which would silently start returning nothing.
    // Callers that need a private copy of a borrowed surface must ask for one explicitly.
    if (!surface->store->owned) return false;

    // Copy only this surface's own window, not the whole parent allocation. A crop view
    // that separates from its parent should not drag the parent's memory along — that is
    // the retention leak BrushStroke.paintSnapshot works around by hand today.
    size_t bpp = raster_bytes_per_pixel(surface->format);
    size_t stride = surface->width * bpp;
    raster_store *store = store_create(stride * surface->height);
    if (!store) return false;
    for (size_t y = 0; y < surface->height; ++y)
        memcpy(store->bytes + y * stride, surface->data + y * surface->stride, stride);

    store_release(surface->store);
    surface->store = store;
    surface->data = store->bytes;
    surface->stride = stride;
    return true;
}
