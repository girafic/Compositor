import Foundation
import Testing
import CCompositorKernels

// MARK: - WandPixels.c

@Suite("wand_mask")
struct WandMaskTests {
    private static let red: [UInt8] = [255, 0, 0, 255]
    private static let blue: [UInt8] = [0, 0, 255, 255]

    /// Runs the kernel and hands back the selected count together with the mask plane.
    private func select(_ raster: Raster, seed: (Int, Int), radius: Int = 0,
                        tolerance: Int32 = 0, contiguous: Int32) -> (count: Int, mask: [UInt8]) {
        var mask = [UInt8](repeating: 0, count: raster.width * raster.height)
        let count = raster.bytes.withUnsafeBufferPointer { pixels in
            mask.withUnsafeMutableBufferPointer { out in
                wand_mask(pixels.baseAddress, raster.width, raster.height, raster.stride,
                          seed.0, seed.1, radius, tolerance, contiguous, out.baseAddress)
            }
        }
        return (Int(count), mask)
    }

    @Test("a uniform image selects entirely")
    func uniform() {
        let raster = Raster(width: 4, height: 4, solid: [30, 60, 90])
        let (count, mask) = select(raster, seed: (0, 0), contiguous: 1)
        #expect(count == 16)
        #expect(mask.allSatisfy { $0 == 255 })
    }

    @Test("zero tolerance splits two flat colours")
    func twoTone() {
        // Left two columns red, right two blue.
        let raster = Raster(width: 4, height: 4) { x, _ in x < 2 ? Self.red : Self.blue }
        let (count, mask) = select(raster, seed: (0, 0), contiguous: 1)
        #expect(count == 8)
        for y in 0..<4 {
            #expect(mask[y * 4 + 0] == 255)
            #expect(mask[y * 4 + 1] == 255)
            #expect(mask[y * 4 + 2] == 0)
            #expect(mask[y * 4 + 3] == 0)
        }
    }

    @Test("contiguous stops at the gap, global does not")
    func contiguity() {
        // red, blue, blue, blue, red — the two reds match each other but are not connected.
        let raster = Raster(width: 5, height: 1) { x, _ in (x == 0 || x == 4) ? Self.red : Self.blue }

        let connected = select(raster, seed: (0, 0), contiguous: 1)
        #expect(connected.count == 1)
        #expect(connected.mask == [255, 0, 0, 0, 0])

        let global = select(raster, seed: (0, 0), contiguous: 0)
        #expect(global.count == 2)
        #expect(global.mask == [255, 0, 0, 0, 255])
    }

    @Test("a wide tolerance takes both colours")
    func tolerance() {
        let raster = Raster(width: 5, height: 1) { x, _ in (x == 0 || x == 4) ? Self.red : Self.blue }
        let (count, mask) = select(raster, seed: (0, 0), tolerance: 255, contiguous: 1)
        #expect(count == 5)
        #expect(mask.allSatisfy { $0 == 255 })
    }

    @Test("a seed outside the image selects nothing")
    func seedOutOfRange() {
        let raster = Raster(width: 4, height: 4, solid: [10, 10, 10])
        let (count, mask) = select(raster, seed: (4, 0), contiguous: 1)
        #expect(count == 0)
        #expect(mask.allSatisfy { $0 == 0 })
    }
}

@Suite("wand_trace")
struct WandTraceTests {
    /// Traces a mask and returns its loops as arrays of (x, y) corner points.
    private func trace(_ mask: [UInt8], width: Int, height: Int) -> (result: Int32, loops: [[[Int32]]]) {
        var points: UnsafeMutablePointer<Int32>?
        var loops: UnsafeMutablePointer<Int32>?
        var pointCount = 0
        var loopCount = 0
        let result = mask.withUnsafeBufferPointer { plane in
            wand_trace(plane.baseAddress, width, height, &points, &pointCount, &loops, &loopCount)
        }
        defer { free(points); free(loops) }
        guard result == 0, let points, let loops else { return (result, []) }
        var shapes: [[[Int32]]] = []
        var offset = 0
        for loop in 0..<loopCount {
            let corners = Int(loops[loop])
            shapes.append((0..<corners).map { [points[(offset + $0) * 2], points[(offset + $0) * 2 + 1]] })
            offset += corners
        }
        #expect(offset == pointCount)
        return (result, shapes)
    }

    @Test("an empty mask traces nothing")
    func empty() {
        let (result, loops) = trace([UInt8](repeating: 0, count: 9), width: 3, height: 3)
        #expect(result == 0)
        #expect(loops.isEmpty)
    }

    @Test("a single pixel traces one clockwise square")
    func singlePixel() {
        // Pixel (1,1) of a 3x3 mask. Its outline runs along pixel edges, so the corners are the
        // grid points (1,1) (2,1) (2,2) (1,2) — clockwise in top-left coordinates, which is what
        // the non-zero winding rule needs to fill exactly that pixel.
        var mask = [UInt8](repeating: 0, count: 9)
        mask[1 * 3 + 1] = 255
        let (result, loops) = trace(mask, width: 3, height: 3)
        #expect(result == 0)
        #expect(loops.count == 1)
        #expect(loops.first == [[1, 1], [2, 1], [2, 2], [1, 2]])
    }

    @Test("a filled mask traces the whole rect")
    func full() {
        let (result, loops) = trace([UInt8](repeating: 255, count: 12), width: 4, height: 3)
        #expect(result == 0)
        #expect(loops.count == 1)
        #expect(loops.first == [[0, 0], [4, 0], [4, 3], [0, 3]])
    }

    @Test("a ring traces an outer loop and a hole")
    func hole() {
        // A 4x4 block with its middle 2x2 removed: one outer boundary and one hole, which must
        // wind opposite ways or the winding fill would paint the hole in.
        var mask = [UInt8](repeating: 255, count: 16)
        for y in 1...2 { for x in 1...2 { mask[y * 4 + x] = 0 } }
        let (result, loops) = trace(mask, width: 4, height: 4)
        #expect(result == 0)
        #expect(loops.count == 2)
        #expect(loops.contains([[0, 0], [4, 0], [4, 4], [0, 4]]))       // outer, clockwise
        #expect(loops.contains([[1, 1], [1, 3], [3, 3], [3, 1]]))       // hole, counterclockwise
    }
}

// MARK: - HealPixels.c

@Suite("heal kernels")
struct HealTests {
    @Test("heal_coverage_bounds reports zeros for an empty coverage plane")
    func emptyBounds() {
        let gray = [UInt8](repeating: 0, count: 20)
        let bounds = gray.withUnsafeBufferPointer { plane in
            fourOut { heal_coverage_bounds(plane.baseAddress, 5, 4, 5, $0) }
        }
        #expect(bounds == [0, 0, 0, 0])
    }

    @Test("heal_coverage_bounds reports half-open bounds around the marked pixels")
    func bounds() {
        var gray = [UInt8](repeating: 0, count: 20)
        gray[1 * 5 + 2] = 255
        gray[2 * 5 + 3] = 1      // any non-zero byte counts
        let bounds = gray.withUnsafeBufferPointer { plane in
            fourOut { heal_coverage_bounds(plane.baseAddress, 5, 4, 5, $0) }
        }
        #expect(bounds == [2, 1, 4, 3])
    }

    @Test("spot_heal with empty coverage is a no-op")
    func nothingToHeal() {
        var raster = Raster(width: 8, height: 8, solid: [120, 130, 140])
        let original = raster.bytes
        let coverage = [UInt8](repeating: 0, count: 64)
        let result = raster.bytes.withUnsafeMutableBufferPointer { buffer in
            coverage.withUnsafeBufferPointer { plane in
                spot_heal(buffer.baseAddress, plane.baseAddress, 8, 8, raster.stride, 1, 0, 1)
            }
        }
        #expect(result == 0)
        #expect(raster.bytes == original)
    }

    @Test("spot_heal repairs a blob and leaves uncovered pixels alone", arguments: [0, 1, 2] as [Int32])
    func heals(mode: Int32) {
        // A textured background with a flat magenta blot in the middle. Whatever each mode does
        // with the blot, pixels outside the coverage must survive untouched — the kernel blends
        // its result in by coverage.
        var raster = Raster(width: 32, height: 32) { x, y in
            if (12..<20).contains(x) && (12..<20).contains(y) { return [255, 0, 255, 255] }
            let value = UInt8((x * 7 + y * 3) % 200 + 20)
            return [value, value, value, 255]
        }
        var coverage = [UInt8](repeating: 0, count: 32 * 32)
        for y in 12..<20 { for x in 12..<20 { coverage[y * 32 + x] = 255 } }

        let before = raster
        let result = raster.bytes.withUnsafeMutableBufferPointer { buffer in
            coverage.withUnsafeBufferPointer { plane in
                spot_heal(buffer.baseAddress, plane.baseAddress, 32, 32, raster.stride, 1, mode, 9)
            }
        }
        #expect(result == 0)
        for y in 0..<32 {
            for x in 0..<32 where coverage[y * 32 + x] == 0 {
                #expect(raster[x, y] == before[x, y], "uncovered pixel \(x),\(y) changed")
            }
        }
    }
}

// MARK: - ContentFill.c

@Suite("content_fill")
struct ContentFillTests {
    @Test("a fully masked image has no source to borrow from")
    func noSource() {
        var raster = Raster(width: 16, height: 16, solid: [80, 90, 100])
        let mask = [UInt8](repeating: 255, count: 16 * 16)
        let result = raster.bytes.withUnsafeMutableBufferPointer { buffer in
            mask.withUnsafeBufferPointer { plane in
                content_fill(buffer.baseAddress, raster.stride, plane.baseAddress, 16, 16, 16)
            }
        }
        #expect(result == 0)
    }

    @Test("a hole in a textured image is filled from its surroundings")
    func fillsHole() {
        var raster = Raster(width: 32, height: 32) { x, y in
            let value = UInt8((x * 5 + y * 11) % 180 + 40)
            return [value, value, value, 255]
        }
        var mask = [UInt8](repeating: 0, count: 32 * 32)
        for y in 13..<19 { for x in 13..<19 { mask[y * 32 + x] = 255 } }

        let before = raster
        let result = raster.bytes.withUnsafeMutableBufferPointer { buffer in
            mask.withUnsafeBufferPointer { plane in
                content_fill(buffer.baseAddress, raster.stride, plane.baseAddress, 32, 32, 32)
            }
        }
        #expect(result == 1)
        for y in 0..<32 {
            for x in 0..<32 where mask[y * 32 + x] == 0 {
                #expect(raster[x, y] == before[x, y], "pixel \(x),\(y) outside the hole changed")
            }
        }
    }
}
