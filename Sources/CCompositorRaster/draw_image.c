#include "raster.h"

#include <math.h>
#include <string.h>

// The image sampler: given a device pixel, which source pixels does it read and with what
// weights. Kept apart from the context for the same reason coverage.c is — it is pure
// arithmetic over buffers, so it can be checked against a reference implementation without
// constructing anything.
//
// The property everything else rests on: at unit scale with an integer translation, every
// kernel here gives weight 1 to the sample it lands on and 0 to its neighbours, so all five
// interpolation qualities produce the identical exact selection. That is a consequence of
// the kernels rather than a special case, which means it cannot be broken by a change that
// forgets the special case existed.

#define MAX_TAPS 64

// MARK: - Kernels

// Tent. At stretch 1 this is ordinary bilinear: k(0) = 1, k(+-1) = 0.
static double tent(double t) {
    t = fabs(t);
    return t < 1.0 ? 1.0 - t : 0.0;
}

// Catmull-Rom, the Mitchell family at B = 0. Chosen over a Gaussian because it satisfies
// k(0) = 1 and k(+-1) = k(+-2) = 0, which is what makes a unit-scale draw exact. It has
// negative lobes, so results need clamping back under their alpha.
static double catmull_rom(double t) {
    t = fabs(t);
    if (t < 1.0) return ((1.5 * t - 2.5) * t) * t + 1.0;
    if (t < 2.0) return (((-0.5 * t) + 2.5) * t - 4.0) * t + 2.0;
    return 0.0;
}

static double kernel_radius(raster_interpolation quality) {
    switch (quality) {
    case RASTER_INTERPOLATION_HIGH:
    case RASTER_INTERPOLATION_MEDIUM: return 2.0;
    default: return 1.0;
    }
}

static double kernel_weight(raster_interpolation quality, double t) {
    switch (quality) {
    case RASTER_INTERPOLATION_HIGH:
    case RASTER_INTERPOLATION_MEDIUM: return catmull_rom(t);
    default: return tent(t);
    }
}

// The weights for one axis at sample position `p`, in a source-pixel space where pixel i
// spans [i, i+1). `stretch` widens the kernel for a reduction; at 1 it is the natural
// kernel. Indices are clamped into [0, extent), which replicates the edge.
//
// Returns the tap count, writes indices into `index` and weights into `weight`.
static size_t axis_taps(raster_interpolation quality, double p, double stretch, int32_t extent,
                        int32_t *index, double *weight) {
    if (quality == RASTER_INTERPOLATION_NONE) {
        int32_t i = (int32_t)floor(p);
        if (i < 0) i = 0;
        if (i >= extent) i = extent - 1;
        index[0] = i;
        weight[0] = 1.0;
        return 1;
    }

    double radius = kernel_radius(quality) * stretch;
    int32_t first = (int32_t)ceil(p - 0.5 - radius);
    int32_t last = (int32_t)floor(p - 0.5 + radius);
    if (last < first) last = first;
    if ((size_t)(last - first + 1) > MAX_TAPS) last = first + MAX_TAPS - 1;

    size_t count = 0;
    double sum = 0.0;
    for (int32_t i = first; i <= last; ++i) {
        double w = kernel_weight(quality, ((double)i + 0.5 - p) / stretch);
        if (w == 0.0 && count == 0 && i < last) continue;  // skip a leading zero tap
        int32_t clamped = i < 0 ? 0 : (i >= extent ? extent - 1 : i);
        index[count] = clamped;
        weight[count] = w;
        sum += w;
        ++count;
    }
    if (count == 0) {
        int32_t i = (int32_t)floor(p);
        index[0] = i < 0 ? 0 : (i >= extent ? extent - 1 : i);
        weight[0] = 1.0;
        return 1;
    }
    // Normalise so a flat field stays flat. At unit stretch on an integer sample the weights
    // are already exactly (.., 1, ..) summing to 1, so this divides by one and changes
    // nothing — the exactness survives.
    if (sum != 0.0 && sum != 1.0)
        for (size_t k = 0; k < count; ++k) weight[k] /= sum;
    return count;
}

// MARK: - The mapping

bool raster_image_mapping(raster_matrix ctm, raster_frect rect,
                          size_t imageWidth, size_t imageHeight, raster_matrix *out) {
    if (!out || !imageWidth || !imageHeight) return false;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.width) || !isfinite(rect.height))
        return false;
    if (rect.width == 0.0 || rect.height == 0.0) return false;

    raster_matrix canonical;
    if (!raster_matrix_is_rectilinear(ctm, rect, &canonical)) return false;

    // Source pixel space -> user space. Row 0 lands at the rect's maximum y, which is how
    // CoreGraphics places an image and what the app's own flip then cancels.
    double sx = rect.width / (double)imageWidth;
    double sy = rect.height / (double)imageHeight;
    raster_matrix imageToUser = { sx, 0, 0, -sy, rect.x, rect.y + rect.height };

    // Compose with the CTM: (a b; c d; tx ty) applied as a row vector, so image -> user ->
    // device is imageToUser followed by the CTM.
    raster_matrix m = {
        imageToUser.a * canonical.a + imageToUser.b * canonical.c,
        imageToUser.a * canonical.b + imageToUser.b * canonical.d,
        imageToUser.c * canonical.a + imageToUser.d * canonical.c,
        imageToUser.c * canonical.b + imageToUser.d * canonical.d,
        imageToUser.tx * canonical.a + imageToUser.ty * canonical.c + canonical.tx,
        imageToUser.tx * canonical.b + imageToUser.ty * canonical.d + canonical.ty,
    };

    double determinant = m.a * m.d - m.b * m.c;
    if (determinant == 0.0 || !isfinite(determinant)) return false;

    raster_matrix inverse = {
        m.d / determinant, -m.b / determinant,
        -m.c / determinant, m.a / determinant,
        (m.c * m.ty - m.d * m.tx) / determinant,
        (m.b * m.tx - m.a * m.ty) / determinant,
    };
    if (!isfinite(inverse.a) || !isfinite(inverse.b) || !isfinite(inverse.c) ||
        !isfinite(inverse.d) || !isfinite(inverse.tx) || !isfinite(inverse.ty))
        return false;

    *out = inverse;
    return true;
}

// MARK: - Resampling

// How far apart two device pixels land in source pixels, per axis. A value above 1 is a
// reduction and widens the kernel by exactly that much.
static void stretches(raster_matrix deviceToImage, double *outU, double *outV) {
    // A rectilinear inverse has either (b, c) or (a, d) zero, so one device axis feeds each
    // source axis and the step is the magnitude of whichever term is live.
    double u = fabs(deviceToImage.a) + fabs(deviceToImage.c);
    double v = fabs(deviceToImage.b) + fabs(deviceToImage.d);
    *outU = u > 1.0 ? u : 1.0;
    *outV = v > 1.0 ? v : 1.0;
}

void raster_sample_row(const raster_surface *image, raster_matrix deviceToImage,
                       int32_t x0, int32_t y, size_t count,
                       raster_interpolation quality, uint8_t *out) {
    if (!image || !out || !count) return;

    const uint8_t *pixels = raster_surface_bytes(image);
    if (!pixels) return;
    size_t stride = raster_surface_stride(image);
    raster_format format = raster_surface_format(image);
    size_t bpp = raster_bytes_per_pixel(format);
    int32_t width = (int32_t)raster_surface_width(image);
    int32_t height = (int32_t)raster_surface_height(image);

    double stretchU, stretchV;
    stretches(deviceToImage, &stretchU, &stretchV);

    int32_t indexU[MAX_TAPS], indexV[MAX_TAPS];
    double weightU[MAX_TAPS], weightV[MAX_TAPS];

    for (size_t k = 0; k < count; ++k) {
        // Closed form per pixel, never accumulated: a running sum drifts, and the drift is
        // exactly what would break the guarantee that a piece of an image matches the same
        // region of the whole.
        double dx = (double)(x0 + (int32_t)k) + 0.5;
        double dy = (double)y + 0.5;
        double u = deviceToImage.a * dx + deviceToImage.c * dy + deviceToImage.tx;
        double v = deviceToImage.b * dx + deviceToImage.d * dy + deviceToImage.ty;

        uint8_t *dst = out + k * bpp;
        if (!isfinite(u) || !isfinite(v)) {
            memset(dst, 0, bpp);
            continue;
        }

        size_t taps = axis_taps(quality, u, stretchU, width, indexU, weightU);
        size_t rows = axis_taps(quality, v, stretchV, height, indexV, weightV);

        double accumulated[4] = { 0, 0, 0, 0 };
        for (size_t j = 0; j < rows; ++j) {
            const uint8_t *row = pixels + (size_t)indexV[j] * stride;
            for (size_t i = 0; i < taps; ++i) {
                double w = weightV[j] * weightU[i];
                if (w == 0.0) continue;
                const uint8_t *sample = row + (size_t)indexU[i] * bpp;
                for (size_t c = 0; c < bpp; ++c) accumulated[c] += w * (double)sample[c];
            }
        }

        for (size_t c = 0; c < bpp; ++c) {
            double value = accumulated[c];
            if (!(value > 0.0)) value = 0.0;  // also catches NaN
            if (value > 255.0) value = 255.0;
            dst[c] = (uint8_t)(value + 0.5);
        }
        // Catmull-Rom undershoots and overshoots, and in premultiplied space that produces a
        // colour brighter than its own alpha, which shows as a glowing edge. Pull it back.
        if (format == RASTER_RGBA8)
            for (size_t c = 0; c < 3; ++c) if (dst[c] > dst[3]) dst[c] = dst[3];
    }
}
