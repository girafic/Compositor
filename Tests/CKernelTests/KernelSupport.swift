import CCompositorKernels

/// A premultiplied-RGBA test buffer: 4 bytes per pixel, `stride` bytes per row, rows top-down.
///
/// That is the layout the macOS app got from `CGBitmapContext` with
/// `premultipliedLast | byteOrder32Big`, and it is what all eight kernels assume. Nothing here
/// depends on CoreGraphics — these tests pin the kernels' behaviour on Linux before any of the
/// renderer is ported onto them.
struct Raster {
    var bytes: [UInt8]
    let width: Int
    let height: Int
    let stride: Int

    /// `padding` adds unused bytes after each row, so tests catch kernels that assume
    /// `stride == width * 4`.
    init(width: Int, height: Int, padding: Int = 0, pixel: (Int, Int) -> [UInt8]) {
        self.width = width
        self.height = height
        self.stride = width * 4 + padding
        self.bytes = [UInt8](repeating: 0, count: stride * height)
        for y in 0..<height {
            for x in 0..<width {
                let value = pixel(x, y)
                let i = y * stride + x * 4
                bytes[i] = value[0]; bytes[i + 1] = value[1]
                bytes[i + 2] = value[2]; bytes[i + 3] = value[3]
            }
        }
    }

    /// Solid colour, fully opaque.
    init(width: Int, height: Int, solid rgb: [UInt8], padding: Int = 0) {
        self.init(width: width, height: height, padding: padding) { _, _ in
            [rgb[0], rgb[1], rgb[2], 255]
        }
    }

    /// Fully transparent.
    init(clear width: Int, height: Int, padding: Int = 0) {
        self.init(width: width, height: height, padding: padding) { _, _ in [0, 0, 0, 0] }
    }

    subscript(x: Int, y: Int) -> [UInt8] {
        let i = y * stride + x * 4
        return Array(bytes[i..<(i + 4)])
    }
}

/// Calls a kernel that writes four integers through an out-parameter and returns them as `Int`s.
/// The element type is inferred from the kernel, so this serves both the `size_t bounds[4]` and
/// `long bounds[4]` signatures without a test having to name either.
func fourOut<T: FixedWidthInteger>(_ call: (UnsafeMutablePointer<T>) -> Void) -> [Int] {
    let out = UnsafeMutablePointer<T>.allocate(capacity: 4)
    out.initialize(repeating: 0, count: 4)
    defer { out.deallocate() }
    call(out)
    return (0..<4).map { Int(out[$0]) }
}
