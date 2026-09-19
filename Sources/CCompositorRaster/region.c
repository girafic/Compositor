#include "raster.h"

#include <stdlib.h>
#include <string.h>

// Regions are combined band by band. Every operation collects the distinct y coordinates of
// both inputs, walks the resulting horizontal strips, runs a one-dimensional set operation
// on the x-intervals each input contributes to that strip, and finally merges vertically
// adjacent strips that came out identical. That last step is what makes the representation
// canonical, which is what lets `raster_region_bounds` be exact.
//
// The cost is O(n^2) in the rectangle count, and that is deliberate. The largest region here
// is a painted layer's patch list, bounded by the 256-pixel tile grid over the canvas —
// about 256 rectangles for a 4000x4000 document, so roughly 33 bands of 17 intervals. There
// is nothing to gain from a smarter structure and plenty to lose in correctness.

struct raster_region {
    raster_rect *rects;
    size_t count;
    size_t capacity;
};

// MARK: - Storage

static bool reserve(raster_region *region, size_t needed) {
    if (needed <= region->capacity) return true;
    size_t capacity = region->capacity ? region->capacity * 2 : 8;
    while (capacity < needed) capacity *= 2;
    raster_rect *grown = realloc(region->rects, capacity * sizeof(raster_rect));
    if (!grown) return false;
    region->rects = grown;
    region->capacity = capacity;
    return true;
}

static bool append(raster_region *region, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (x1 <= x0 || y1 <= y0) return true;
    // Extend the previous rectangle instead of adding one when they touch in x and share a
    // band. `combine_band` cannot actually produce two adjacent runs — it closes a run and
    // opens the next one at different sweep coordinates, and the coordinates are distinct —
    // so this branch is unreachable today and the tests cannot cover it. It stays because it
    // is what makes `append` correct on its own terms rather than only in combination with
    // the one caller that happens to respect the invariant.
    if (region->count > 0) {
        raster_rect *last = &region->rects[region->count - 1];
        if (last->y0 == y0 && last->y1 == y1 && last->x1 == x0) {
            last->x1 = x1;
            return true;
        }
    }
    if (!reserve(region, region->count + 1)) return false;
    region->rects[region->count++] = (raster_rect){ x0, y0, x1, y1 };
    return true;
}

raster_region *raster_region_create(void) {
    raster_region *region = malloc(sizeof(raster_region));
    if (!region) return NULL;
    region->rects = NULL;
    region->count = 0;
    region->capacity = 0;
    return region;
}

raster_region *raster_region_create_rect(raster_rect rect) {
    raster_region *region = raster_region_create();
    if (!region) return NULL;
    if (!raster_rect_is_empty(rect) && !append(region, rect.x0, rect.y0, rect.x1, rect.y1)) {
        raster_region_destroy(region);
        return NULL;
    }
    return region;
}

raster_region *raster_region_copy(const raster_region *region) {
    if (!region) return NULL;
    raster_region *copy = raster_region_create();
    if (!copy) return NULL;
    if (region->count) {
        if (!reserve(copy, region->count)) {
            raster_region_destroy(copy);
            return NULL;
        }
        memcpy(copy->rects, region->rects, region->count * sizeof(raster_rect));
        copy->count = region->count;
    }
    return copy;
}

void raster_region_destroy(raster_region *region) {
    if (!region) return;
    free(region->rects);
    free(region);
}

// MARK: - Queries

bool raster_region_is_empty(const raster_region *region) {
    return !region || region->count == 0;
}

size_t raster_region_count(const raster_region *region) {
    return region ? region->count : 0;
}

raster_rect raster_region_rect(const raster_region *region, size_t index) {
    if (!region || index >= region->count) return (raster_rect){ 0, 0, 0, 0 };
    return region->rects[index];
}

raster_rect raster_region_bounds(const raster_region *region) {
    if (raster_region_is_empty(region)) return (raster_rect){ 0, 0, 0, 0 };
    raster_rect bounds = region->rects[0];
    for (size_t i = 1; i < region->count; ++i) {
        raster_rect r = region->rects[i];
        if (r.x0 < bounds.x0) bounds.x0 = r.x0;
        if (r.y0 < bounds.y0) bounds.y0 = r.y0;
        if (r.x1 > bounds.x1) bounds.x1 = r.x1;
        if (r.y1 > bounds.y1) bounds.y1 = r.y1;
    }
    return bounds;
}

bool raster_region_contains(const raster_region *region, int32_t x, int32_t y) {
    if (!region) return false;
    for (size_t i = 0; i < region->count; ++i) {
        raster_rect r = region->rects[i];
        if (y < r.y0) break;  // bands are sorted, so nothing further down can match
        if (y < r.y1 && x >= r.x0 && x < r.x1) return true;
    }
    return false;
}

// MARK: - Combining

typedef enum { OP_UNION, OP_INTERSECT, OP_SUBTRACT, OP_XOR } region_op;

// Collects every distinct y coordinate of both inputs, sorted. These are the strip edges:
// within one strip neither input changes, so the problem reduces to one dimension.
// Returns false only on allocation failure; both inputs empty is a success with no edges.
// Keeping those two apart matters: an empty result and "could not compute" have to reach the
// caller as different things, or an allocation failure silently becomes an empty region.
static bool band_edges(const raster_region *a, const raster_region *b,
                       int32_t **outEdges, size_t *outCount) {
    *outEdges = NULL;
    *outCount = 0;

    size_t capacity = (a->count + b->count) * 2;
    if (capacity == 0) return true;
    int32_t *edges = malloc(capacity * sizeof(int32_t));
    if (!edges) return false;

    size_t count = 0;
    for (size_t i = 0; i < a->count; ++i) {
        edges[count++] = a->rects[i].y0;
        edges[count++] = a->rects[i].y1;
    }
    for (size_t i = 0; i < b->count; ++i) {
        edges[count++] = b->rects[i].y0;
        edges[count++] = b->rects[i].y1;
    }

    // Insertion sort: `count` is small and this keeps the file free of qsort comparators.
    for (size_t i = 1; i < count; ++i) {
        int32_t value = edges[i];
        size_t j = i;
        while (j > 0 && edges[j - 1] > value) { edges[j] = edges[j - 1]; --j; }
        edges[j] = value;
    }
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i)
        if (unique == 0 || edges[unique - 1] != edges[i]) edges[unique++] = edges[i];

    *outEdges = edges;
    *outCount = unique;
    return true;
}

// The x-intervals one region contributes to the strip [y0, y1). Because bands never
// straddle a strip edge, a rectangle either covers the strip completely or not at all.
static size_t intervals_in_band(const raster_region *region, int32_t y0, int32_t y1,
                                int32_t *out, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < region->count && count + 2 <= capacity; ++i) {
        raster_rect r = region->rects[i];
        if (r.y0 <= y0 && r.y1 >= y1) {
            out[count++] = r.x0;
            out[count++] = r.x1;
        }
    }
    return count;
}

static bool op_keeps(region_op op, bool inA, bool inB) {
    switch (op) {
    case OP_UNION: return inA || inB;
    case OP_INTERSECT: return inA && inB;
    case OP_SUBTRACT: return inA && !inB;
    case OP_XOR: return inA != inB;
    }
    return false;
}

// Sweeps two sorted lists of disjoint intervals and emits the combined runs for one strip.
static bool combine_band(raster_region *result, region_op op, int32_t y0, int32_t y1,
                         const int32_t *a, size_t aCount, const int32_t *b, size_t bCount) {
    size_t i = 0, j = 0;
    bool inA = false, inB = false;
    int32_t runStart = 0;
    bool inRun = false;

    while (i < aCount || j < bCount) {
        int32_t x;
        if (j >= bCount) x = a[i];
        else if (i >= aCount) x = b[j];
        else x = a[i] < b[j] ? a[i] : b[j];

        // Both lists are advanced past this coordinate before the decision. An edge shared
        // between a and b is the common case and the decision must see both toggles, or
        // every operation is wrong by one interval at each shared boundary.
        //
        // The loops are loops rather than ifs for a case that canonical input cannot
        // produce: two intervals of the *same* list touching at x. Unreachable today, kept
        // so the sweep does not silently depend on its caller's invariant.
        while (i < aCount && a[i] == x) { inA = !inA; ++i; }
        while (j < bCount && b[j] == x) { inB = !inB; ++j; }

        bool keep = op_keeps(op, inA, inB);
        if (keep && !inRun) { runStart = x; inRun = true; }
        else if (!keep && inRun) {
            if (!append(result, runStart, y0, x, y1)) return false;
            inRun = false;
        }
    }
    return true;
}

// Merges a strip into the band above it when their x-intervals match exactly. Without this
// the representation is not canonical and `bounds` stays exact but rectangle counts grow
// without bound as clips nest.
static void merge_with_previous_band(raster_region *region, size_t bandStart, size_t previousStart) {
    if (previousStart >= bandStart || bandStart >= region->count) return;
    size_t previousCount = bandStart - previousStart;
    size_t bandCount = region->count - bandStart;
    if (previousCount != bandCount) return;
    if (region->rects[previousStart].y1 != region->rects[bandStart].y0) return;

    for (size_t i = 0; i < bandCount; ++i) {
        if (region->rects[previousStart + i].x0 != region->rects[bandStart + i].x0) return;
        if (region->rects[previousStart + i].x1 != region->rects[bandStart + i].x1) return;
    }
    int32_t y1 = region->rects[bandStart].y1;
    for (size_t i = 0; i < previousCount; ++i) region->rects[previousStart + i].y1 = y1;
    region->count = bandStart;
}

static raster_region *combine(const raster_region *a, const raster_region *b, region_op op) {
    static const raster_region empty = { NULL, 0, 0 };
    if (!a) a = &empty;
    if (!b) b = &empty;

    raster_region *result = raster_region_create();
    if (!result) return NULL;

    size_t edgeCount = 0;
    int32_t *edges = NULL;
    if (!band_edges(a, b, &edges, &edgeCount)) {
        raster_region_destroy(result);
        return NULL;
    }
    if (edgeCount < 2) {  // both inputs empty
        free(edges);
        return result;
    }

    size_t capacity = (a->count + b->count) * 2 + 2;
    int32_t *aIntervals = malloc(capacity * sizeof(int32_t));
    int32_t *bIntervals = malloc(capacity * sizeof(int32_t));
    if (!aIntervals || !bIntervals) {
        free(edges); free(aIntervals); free(bIntervals);
        raster_region_destroy(result);
        return NULL;
    }

    size_t previousBandStart = 0;
    for (size_t e = 0; e + 1 < edgeCount; ++e) {
        int32_t y0 = edges[e], y1 = edges[e + 1];
        if (y1 <= y0) continue;
        size_t aCount = intervals_in_band(a, y0, y1, aIntervals, capacity);
        size_t bCount = intervals_in_band(b, y0, y1, bIntervals, capacity);

        size_t bandStart = result->count;
        if (!combine_band(result, op, y0, y1, aIntervals, aCount, bIntervals, bCount)) {
            free(edges); free(aIntervals); free(bIntervals);
            raster_region_destroy(result);
            return NULL;
        }
        if (result->count > bandStart) {
            merge_with_previous_band(result, bandStart, previousBandStart);
            // If the merge collapsed this band into the previous one, the previous band is
            // still the one to compare against next time.
            if (result->count != bandStart) previousBandStart = bandStart;
        }
    }

    free(edges); free(aIntervals); free(bIntervals);
    return result;
}

raster_region *raster_region_union(const raster_region *a, const raster_region *b) {
    return combine(a, b, OP_UNION);
}
raster_region *raster_region_intersect(const raster_region *a, const raster_region *b) {
    return combine(a, b, OP_INTERSECT);
}
raster_region *raster_region_subtract(const raster_region *a, const raster_region *b) {
    return combine(a, b, OP_SUBTRACT);
}
raster_region *raster_region_xor(const raster_region *a, const raster_region *b) {
    return combine(a, b, OP_XOR);
}

raster_region *raster_region_intersect_rect(const raster_region *region, raster_rect rect) {
    raster_region *other = raster_region_create_rect(rect);
    if (!other) return NULL;
    raster_region *result = combine(region, other, OP_INTERSECT);
    raster_region_destroy(other);
    return result;
}
