import Testing
import CCompositorKernels

// MARK: - BrushPixels.c

@Suite("brush_alpha_bounds")
struct AlphaBoundsTests {
    @Test("an empty buffer reports all zeros")
    func empty() {
        let raster = Raster(clear: 8, height: 6)
        let bounds = raster.bytes.withUnsafeBufferPointer { buffer in
            fourOut { brush_alpha_bounds(buffer.baseAddress, 8, 6, raster.stride, $0) }
        }
        #expect(bounds == [0, 0, 0, 0])
    }

    @Test("a single opaque pixel reports its half-open bounds")
    func singlePixel() {
        let raster = Raster(width: 8, height: 6) { x, y in
            (x == 2 && y == 3) ? [10, 20, 30, 255] : [0, 0, 0, 0]
        }
        let bounds = raster.bytes.withUnsafeBufferPointer { buffer in
            fourOut { brush_alpha_bounds(buffer.baseAddress, 8, 6, raster.stride, $0) }
        }
        #expect(bounds == [2, 3, 3, 4])
    }

    @Test("row padding is not mistaken for content")
    func honoursStride() {
        // A kernel assuming stride == width * 4 would read one row's padding as the start of the
        // next and widen the bounds.
        let raster = Raster(width: 4, height: 4, padding: 12) { x, y in
            (x == 1 && y == 1) ? [255, 255, 255, 255] : [0, 0, 0, 0]
        }
        let bounds = raster.bytes.withUnsafeBufferPointer { buffer in
            fourOut { brush_alpha_bounds(buffer.baseAddress, 4, 4, raster.stride, $0) }
        }
        #expect(bounds == [1, 1, 2, 2])
    }

    @Test("a fully covered buffer spans the whole rect")
    func full() {
        let raster = Raster(width: 5, height: 3, solid: [1, 2, 3])
        let bounds = raster.bytes.withUnsafeBufferPointer { buffer in
            fourOut { brush_alpha_bounds(buffer.baseAddress, 5, 3, raster.stride, $0) }
        }
        #expect(bounds == [0, 0, 5, 3])
    }
}

@Suite("layer alpha split and restore")
struct LayerAlphaTests {
    @Test("layer_extract_alpha copies the alpha plane")
    func extract() {
        let raster = Raster(width: 4, height: 2) { x, y in [0, 0, 0, UInt8(x * 10 + y)] }
        var gray = [UInt8](repeating: 0, count: 8)
        raster.bytes.withUnsafeBufferPointer { source in
            gray.withUnsafeMutableBufferPointer { destination in
                layer_extract_alpha(source.baseAddress, raster.stride, destination.baseAddress, 4, 4, 2)
            }
        }
        #expect(gray == [0, 10, 20, 30, 1, 11, 21, 31])
    }

    @Test("layer_unpremultiply_opaque divides through and forces alpha to 255")
    func unpremultiply() {
        // a = 128, rounding is (v * 255 + a / 2) / a, clamped to 255:
        //    64 -> (16320 + 64) / 128 = 128
        //    32 -> ( 8160 + 64) / 128 =  64
        //   128 -> (32640 + 64) / 128 = 255 (255.5 before the clamp)
        var raster = Raster(width: 1, height: 1) { _, _ in [64, 32, 128, 128] }
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            layer_unpremultiply_opaque(buffer.baseAddress, raster.stride, 1, 1)
        }
        #expect(raster[0, 0] == [128, 64, 255, 255])
    }

    @Test("layer_unpremultiply_opaque zeroes colour where alpha is zero")
    func unpremultiplyTransparent() {
        var raster = Raster(width: 1, height: 1) { _, _ in [99, 99, 99, 0] }
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            layer_unpremultiply_opaque(buffer.baseAddress, raster.stride, 1, 1)
        }
        #expect(raster[0, 0] == [0, 0, 0, 255])
    }

    @Test("layer_restore_alpha multiplies colour back down")
    func restore() {
        // a = 128, rounding is (v * a + 127) / 255:
        //   255 -> (32640 + 127) / 255 = 128
        //   128 -> (16384 + 127) / 255 =  64
        //     0 -> (    0 + 127) / 255 =   0
        var raster = Raster(width: 1, height: 1) { _, _ in [255, 128, 0, 255] }
        let alpha: [UInt8] = [128]
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            alpha.withUnsafeBufferPointer { plane in
                layer_restore_alpha(buffer.baseAddress, raster.stride, plane.baseAddress, 1, 1, 1)
            }
        }
        #expect(raster[0, 0] == [128, 64, 0, 128])
    }

    @Test("unpremultiply then restore round-trips within one step")
    func roundTrip() {
        // This pair is how LiveMaskRenderer keeps a group's alpha out of an adjustment's way, so
        // the round-trip error is what would show up as banding on a half-transparent folder.
        var raster = Raster(width: 4, height: 1) { x, _ in
            [UInt8(x * 30), UInt8(x * 20), UInt8(x * 10), 200]
        }
        let original = raster
        var alpha = [UInt8](repeating: 0, count: 4)
        raster.bytes.withUnsafeBufferPointer { source in
            alpha.withUnsafeMutableBufferPointer { plane in
                layer_extract_alpha(source.baseAddress, raster.stride, plane.baseAddress, 4, 4, 1)
            }
        }
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            layer_unpremultiply_opaque(buffer.baseAddress, raster.stride, 4, 1)
        }
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            alpha.withUnsafeBufferPointer { plane in
                layer_restore_alpha(buffer.baseAddress, raster.stride, plane.baseAddress, 4, 4, 1)
            }
        }
        for x in 0..<4 {
            let before = original[x, 0], after = raster[x, 0]
            #expect(after[3] == before[3])
            for c in 0..<3 {
                #expect(abs(Int(after[c]) - Int(before[c])) <= 1,
                        "channel \(c) of pixel \(x): \(before[c]) -> \(after[c])")
            }
        }
    }
}

// MARK: - AdjustPixels.c

@Suite("adjust kernels")
struct AdjustTests {
    @Test("rgba_clamp_premultiplied pulls channels back to their alpha")
    func clamp() {
        // Lanczos rings, so a resampled premultiplied pixel can carry more colour than alpha.
        let start: [[UInt8]] = [[200, 100, 50, 128], [10, 20, 30, 255], [255, 0, 255, 0]]
        var raster = Raster(width: 3, height: 1) { x, _ in start[x] }
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            rgba_clamp_premultiplied(buffer.baseAddress, 3)
        }
        #expect(raster[0, 0] == [128, 100, 50, 128])  // 200 clamped down to alpha
        #expect(raster[1, 0] == [10, 20, 30, 255])    // already legal, untouched
        #expect(raster[2, 0] == [0, 0, 0, 0])         // alpha 0 clamps everything
    }

    @Test("adjust_gradient_map replaces colour from the table")
    func gradientMap() {
        // Every level maps to the same colour, so the expectation does not depend on the luminance
        // weights — only on the table lookup and the premultiply on the way out.
        var table = [UInt8](repeating: 0, count: 256 * 3)
        for level in 0..<256 {
            table[level * 3] = 10; table[level * 3 + 1] = 20; table[level * 3 + 2] = 30
        }
        var raster = Raster(width: 2, height: 2, solid: [90, 120, 200])
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            table.withUnsafeBufferPointer { ramp in
                adjust_gradient_map(buffer.baseAddress, 2, 2, raster.stride, ramp.baseAddress)
            }
        }
        for y in 0..<2 {
            for x in 0..<2 { #expect(raster[x, y] == [10, 20, 30, 255], "pixel \(x),\(y)") }
        }
    }

    @Test("adjust_gradient_map leaves fully transparent pixels alone")
    func gradientMapTransparent() {
        let table = [UInt8](repeating: 255, count: 256 * 3)
        var raster = Raster(clear: 1, height: 1)
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            table.withUnsafeBufferPointer { ramp in
                adjust_gradient_map(buffer.baseAddress, 1, 1, raster.stride, ramp.baseAddress)
            }
        }
        #expect(raster[0, 0] == [0, 0, 0, 0])
    }

    @Test("adjust_grain with no amount is a no-op")
    func grainZero() {
        var raster = Raster(width: 8, height: 8, solid: [128, 128, 128])
        let original = raster.bytes
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            adjust_grain(buffer.baseAddress, 8, 8, raster.stride, 0, 1, 50, 7, 0, 0, 1)
        }
        #expect(raster.bytes == original)
    }

    @Test("adjust_grain depends only on position and seed")
    func grainDeterministic() {
        // The header promises a piece of an image gets the same grain as that part of the whole,
        // which is what lets Grain run per tile. Same seed and origin must mean the same result.
        func grained(seed: UInt32) -> [UInt8] {
            var raster = Raster(width: 16, height: 16, solid: [128, 128, 128])
            raster.bytes.withUnsafeMutableBufferPointer { buffer in
                adjust_grain(buffer.baseAddress, 16, 16, raster.stride, 60, 2, 50, seed, 0, 0, 1)
            }
            return raster.bytes
        }
        #expect(grained(seed: 7) == grained(seed: 7))
        #expect(grained(seed: 7) != grained(seed: 8))
    }
}

// MARK: - LevelsPixels.c

@Suite("levels kernels")
struct LevelsKernelTests {
    /// The identity transfer function, laid out as the kernel wants it: three 256-entry ramps.
    static var identityTables: [Float] {
        var tables = [Float](repeating: 0, count: 3 * 256)
        for channel in 0..<3 {
            for value in 0..<256 { tables[channel * 256 + value] = Float(value) / 255 }
        }
        return tables
    }

    @Test("levels_apply with an identity ramp leaves opaque pixels untouched")
    func identity() {
        let start: [[UInt8]] = [[0, 1, 2, 255], [10, 200, 255, 255],
                                [128, 128, 128, 255], [255, 255, 255, 255]]
        var raster = Raster(width: 4, height: 1) { x, _ in start[x] }
        let original = raster.bytes
        let tables = Self.identityTables
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            tables.withUnsafeBufferPointer { ramp in
                levels_apply(buffer.baseAddress, 4, ramp.baseAddress)
            }
        }
        #expect(raster.bytes == original)
    }

    @Test("levels_apply skips fully transparent pixels")
    func skipsTransparent() {
        var raster = Raster(width: 1, height: 1) { _, _ in [77, 88, 99, 0] }
        let toWhite = [Float](repeating: 1, count: 3 * 256)
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            toWhite.withUnsafeBufferPointer { ramp in
                levels_apply(buffer.baseAddress, 1, ramp.baseAddress)
            }
        }
        #expect(raster[0, 0] == [77, 88, 99, 0])
    }

    @Test("levels_histogram weights each opaque pixel once per channel")
    func histogram() {
        let raster = Raster(width: 2, height: 2, solid: [128, 128, 128])
        var bins = [Double](repeating: 0, count: 4 * 256)
        raster.bytes.withUnsafeBufferPointer { buffer in
            bins.withUnsafeMutableBufferPointer { out in
                levels_histogram(buffer.baseAddress, nil, 4, out.baseAddress)
            }
        }
        // Four pixels: each adds 1 to its own channel's bin, and 1/3 to the combined bin three
        // times over.
        for channel in 1...3 { #expect(abs(bins[channel * 256 + 128] - 4) < 1e-9) }
        #expect(abs(bins[128] - 4) < 1e-9)
        #expect(abs(bins.reduce(0, +) - 16) < 1e-9)
    }

    @Test("levels_histogram scales by coverage")
    func histogramCoverage() {
        let raster = Raster(width: 2, height: 1, solid: [200, 200, 200])
        let coverage: [UInt8] = [255, 0]
        var bins = [Double](repeating: 0, count: 4 * 256)
        raster.bytes.withUnsafeBufferPointer { buffer in
            coverage.withUnsafeBufferPointer { mask in
                bins.withUnsafeMutableBufferPointer { out in
                    levels_histogram(buffer.baseAddress, mask.baseAddress, 2, out.baseAddress)
                }
            }
        }
        #expect(abs(bins[1 * 256 + 200] - 1) < 1e-9)  // only the covered pixel counts
    }
}

// MARK: - LensPixels.c

@Suite("lens_distort")
struct LensTests {
    @Test("k = 0 copies the source exactly")
    func identity() {
        let source = Raster(width: 6, height: 5) { x, y in
            [UInt8(x * 40), UInt8(y * 50), UInt8(x * y), 255]
        }
        var destination = Raster(clear: 6, height: 5)
        source.bytes.withUnsafeBufferPointer { input in
            destination.bytes.withUnsafeMutableBufferPointer { output in
                lens_distort(input.baseAddress, output.baseAddress, 6, 5, source.stride, 0)
            }
        }
        for y in 0..<5 {
            for x in 0..<6 { #expect(destination[x, y] == source[x, y], "pixel \(x),\(y)") }
        }
    }

    @Test("a non-zero k actually moves samples")
    func distorts() {
        // A checkerboard makes any resampling visible.
        let source = Raster(width: 16, height: 16) { x, y in
            (x + y) % 2 == 0 ? [255, 255, 255, 255] : [0, 0, 0, 255]
        }
        var destination = Raster(clear: 16, height: 16)
        source.bytes.withUnsafeBufferPointer { input in
            destination.bytes.withUnsafeMutableBufferPointer { output in
                lens_distort(input.baseAddress, output.baseAddress, 16, 16, source.stride, 0.3)
            }
        }
        #expect(destination.bytes != source.bytes)
        #expect(destination.bytes.count == source.bytes.count)
    }
}

// MARK: - NoisePixels.c

@Suite("noise_add")
struct NoiseTests {
    @Test("fully transparent pixels are left alone")
    func skipsTransparent() {
        var raster = Raster(clear: 4, height: 4)
        let original = raster.bytes
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            noise_add(buffer.baseAddress, 4, 4, raster.stride, 50, 0, 0, 1)
        }
        #expect(raster.bytes == original)
    }

    @Test("the same seed gives the same grain")
    func deterministic() {
        func noised(seed: UInt32) -> [UInt8] {
            var raster = Raster(width: 16, height: 16, solid: [128, 128, 128])
            raster.bytes.withUnsafeMutableBufferPointer { buffer in
                noise_add(buffer.baseAddress, 16, 16, raster.stride, 40, 0, 0, seed)
            }
            return raster.bytes
        }
        #expect(noised(seed: 3) == noised(seed: 3))
        #expect(noised(seed: 3) != noised(seed: 4))
    }

    @Test("monochromatic noise moves all three channels together")
    func monochromatic() {
        var raster = Raster(width: 8, height: 8, solid: [128, 128, 128])
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            noise_add(buffer.baseAddress, 8, 8, raster.stride, 40, 0, 1, 11)
        }
        for y in 0..<8 {
            for x in 0..<8 {
                let pixel = raster[x, y]
                #expect(pixel[0] == pixel[1] && pixel[1] == pixel[2], "pixel \(x),\(y): \(pixel)")
                #expect(pixel[3] == 255)
            }
        }
    }

    @Test("alpha is never touched")
    func keepsAlpha() {
        var raster = Raster(width: 8, height: 8) { x, _ in [100, 100, 100, UInt8(1 + x * 30)] }
        raster.bytes.withUnsafeMutableBufferPointer { buffer in
            noise_add(buffer.baseAddress, 8, 8, raster.stride, 60, 1, 0, 5)
        }
        for y in 0..<8 {
            for x in 0..<8 { #expect(raster[x, y][3] == UInt8(1 + x * 30), "pixel \(x),\(y)") }
        }
    }
}
