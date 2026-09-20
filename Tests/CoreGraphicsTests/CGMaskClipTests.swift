import Testing
import CoreGraphics

// The coverage plane's arithmetic is proved in C — Spikes/context-oracle.c checks the
// multiply, the plane's placement, the hard rectangle edge and the per-pixel compositing
// against brute-force references, under valgrind and the sanitizers. These cover what C
// cannot see: that the app's own idioms, written the way the app writes them, come out right.

/// A grayscale image with the given rows of bytes, built the way the app builds masks.
func makeMask(_ rows: [[UInt8]]) throws -> CGImage {
    let height = rows.count
    let width = try #require(rows.first?.count)
    #expect(rows.allSatisfy { $0.count == width }, "a mask must be rectangular")
    let provider = try #require(CGDataProvider(data: Data(rows.flatMap { $0 })))
    return try #require(CGImage(width: width, height: height, bitsPerComponent: 8,
                                bitsPerPixel: 8, bytesPerRow: width,
                                space: CGColorSpaceCreateDeviceGray(),
                                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                                provider: provider, decode: nil,
                                shouldInterpolate: false, intent: .defaultIntent))
}

@Suite("CGContext: mask clips")
struct CGMaskClipTests {
    /// `BrushRaster.draw(_:in:mask: true, context:)` verbatim, including its transform
    /// preamble. Reproducing the call sequence rather than paraphrasing it is the point: the
    /// sequence is what the app relies on, and any of it could be the thing that breaks.
    func brushRasterDrawMask(_ image: CGImage, in rect: CGRect, context: CGContext) {
        context.saveGState()
        context.interpolationQuality = .none
        context.translateBy(x: rect.minX, y: rect.maxY)
        context.scaleBy(x: 1, y: -1)
        let bounds = CGRect(origin: .zero, size: rect.size)
        context.setFillColor(gray: 0, alpha: 1)
        context.fill(bounds)
        context.clip(to: bounds, mask: image)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(bounds)
        context.restoreGState()
    }

    @Test("the brush raster mask idiom reproduces its mask byte for byte")
    func maskIdiomIsExact() throws {
        // This single sequence pins four things at once: that a mask resamples exactly at
        // 1:1, that GRAY8 source-over is a lerp with no alpha channel involved, that
        // setFillColor(gray:) and fill agree with it, and that the rectangle clips hard.
        // Over a hundred exact pixel assertions across the app's test suite read their
        // results back through this idiom.
        let rows: [[UInt8]] = (0..<6).map { y in (0..<7).map { x in UInt8((x * 37 + y * 91) % 256) } }
        let mask = try makeMask(rows)

        // BrushRaster.context flips, and BrushRaster.draw flips again, so the two cancel and
        // mask row 0 lands on buffer row 0.
        let context = try makeContext(7, 6, mask: true)
        brushRasterDrawMask(mask, in: CGRect(x: 0, y: 0, width: 7, height: 6), context: context)

        for y in 0..<6 {
            for x in 0..<7 {
                #expect(pixel(context, x, y) == [rows[y][x]],
                        "mask value at (\(x), \(y)) must survive unchanged")
            }
        }
    }

    @Test("a one-pixel mask stretches over a whole rectangle")
    func solidMaskStretches() throws {
        // LayerMask.solid(revealing:) is a 1x1 image used as a clip over an entire layer.
        for value in [UInt8(0), 64, 128, 255] {
            let mask = try makeMask([[value]])
            let context = try makeContext(5, 4, mask: true)
            context.interpolationQuality = .high
            context.clip(to: CGRect(x: 0, y: 0, width: 5, height: 4), mask: mask)
            context.setFillColor(gray: 1, alpha: 1)
            context.fill(CGRect(x: 0, y: 0, width: 5, height: 4))
            for y in 0..<4 {
                for x in 0..<5 {
                    #expect(pixel(context, x, y) == [value],
                            "a 1x1 mask of \(value) must be flat at (\(x), \(y))")
                }
            }
        }
    }

    @Test("nested mask clips multiply, and restoring undoes only the inner one")
    func nestingMultiplies() throws {
        // FolderMaskClip's own comment says Core Graphics multiplies nested mask clips; that
        // is what lets a folder mask compose with the masks of the layers inside it.
        let outer = try makeMask([[200, 200], [200, 200]])
        let inner = try makeMask([[100, 100], [100, 100]])
        let bounds = CGRect(x: 0, y: 0, width: 2, height: 2)
        let context = try makeContext(2, 2, mask: true)
        context.setFillColor(gray: 1, alpha: 1)

        context.clip(to: bounds, mask: outer)
        context.saveGState()
        context.clip(to: bounds, mask: inner)
        context.fill(bounds)
        #expect(pixel(context, 0, 0) == [UInt8((200 * 100 + 127) / 255)],
                "the nested clip is the product of both masks")
        context.restoreGState()

        // Erase by hand: clear honours the clip, so under the outer mask it would only
        // partly erase and the next fill would measure the leftovers.
        let bytes = try #require(context.data).assumingMemoryBound(to: UInt8.self)
        for row in 0..<2 { for column in 0..<2 { bytes[row * context.bytesPerRow + column] = 0 } }

        context.fill(bounds)
        #expect(pixel(context, 0, 0) == [200],
                "restoring leaves the outer mask exactly as it was")
    }

    @Test("a mask clip narrowed by a rectangle still lines up")
    func narrowingKeepsTheAlignment() throws {
        // The plane sits at the clip region's bounds, so narrowing the region has to re-base
        // it. A flat mask could not tell a correct re-base from a missing one.
        let rows: [[UInt8]] = (0..<8).map { y in (0..<8).map { x in UInt8(16 + x * 20 + y * 3) } }
        let mask = try makeMask(rows)
        let context = try makeContext(8, 8, mask: true)
        context.interpolationQuality = .none
        context.setFillColor(gray: 1, alpha: 1)

        // Both under the flipped user space the app uses, so y counts down the buffer.
        context.clip(to: CGRect(x: 0, y: 0, width: 8, height: 8), mask: mask)
        context.clip(to: CGRect(x: 2, y: 1, width: 4, height: 5))
        context.fill(CGRect(x: 0, y: 0, width: 8, height: 8))

        for y in 0..<8 {
            for x in 0..<8 {
                let inside = (2..<6).contains(x) && (1..<6).contains(y)
                // Bottom-up, and deliberately so. A mask is placed like a drawn image, first
                // row at the rectangle's *maximum* y, and there is no second flip here to
                // cancel it — that is exactly what `BrushRaster.draw` adds and what the idiom
                // test above exercises. Writing `rows[y][x]` here passes only against an
                // implementation that has lost the placement rule.
                let expected: UInt8 = inside ? rows[7 - y][x] : 0
                #expect(pixel(context, x, y) == [expected],
                        "at (\(x), \(y)) after narrowing the clip")
            }
        }
    }

    @Test("a mask clip attenuates an image draw, not only a fill")
    func maskAppliesToImageDraws() throws {
        // LayerRenderer clips to a layer's mask and then draws the layer's image through it.
        let mask = try makeMask([[64, 64], [64, 64]])
        let source = try makeContext(2, 2, mask: true)
        source.setFillColor(gray: 1, alpha: 1)
        source.fill(CGRect(x: 0, y: 0, width: 2, height: 2))
        let image = try #require(source.makeImage())

        let context = try makeRawContext(2, 2, mask: true)
        context.interpolationQuality = .none
        context.clip(to: CGRect(x: 0, y: 0, width: 2, height: 2), mask: mask)
        context.draw(image, in: CGRect(x: 0, y: 0, width: 2, height: 2))

        #expect(pixel(context, 0, 0) == [64], "white through a 64 mask is 64")
    }

    @Test("the clip's bounds are the mask's rectangle, never the mask's content")
    func boundsIgnoreMaskContent() throws {
        // boundingBoxOfClipPath is read as geometry: AdjustmentSurface sizes an offscreen
        // from it and Grain anchors its pattern to the resulting origin, so a mask that
        // happens to be black along an edge must not move it.
        let mask = try makeMask([[0, 0, 0, 0], [0, 255, 255, 0], [0, 255, 255, 0], [0, 0, 0, 0]])
        let context = try makeContext(8, 8, mask: true)
        context.clip(to: CGRect(x: 1, y: 2, width: 4, height: 4), mask: mask)
        #expect(context.boundingBoxOfClipPath == CGRect(x: 1, y: 2, width: 4, height: 4))
    }

    @Test("an empty mask rectangle clips everything away")
    func emptyRectangleClipsAway() throws {
        let mask = try makeMask([[255]])
        let context = try makeContext(4, 4, mask: true)
        context.clip(to: CGRect(x: 0, y: 0, width: 0, height: 4), mask: mask)
        #expect(context.boundingBoxOfClipPath.isNull, "an empty clip reports null, not zero")

        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0) == [0], "and nothing is painted through it")
    }

    @Test("a quarter turn is still rectilinear and still clips")
    func quarterTurnIsAccepted() throws {
        // LayerRenderer clips inside a rotate(by:), and a 90 degree layer gives a matrix
        // whose b and c are 1 while a and d are cos(pi/2) — about 6e-17, not 0. Rejecting
        // that would take every quarter-turned layer down with it.
        let mask = try makeMask([[128, 128], [128, 128]])
        let context = try makeContext(4, 4, mask: true)
        context.translateBy(x: 2, y: 2)
        context.rotate(by: .pi / 2)
        context.clip(to: CGRect(x: -2, y: -2, width: 4, height: 4), mask: mask)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: -2, y: -2, width: 4, height: 4))
        #expect(pixel(context, 0, 0) == [128])
    }
}
