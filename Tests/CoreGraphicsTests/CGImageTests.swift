import Testing
import CoreGraphics

// The sampler's arithmetic is proved in C — Spikes/context-oracle.c runs a million checks
// against brute-force references under valgrind and the sanitizers. These cover the binding:
// the raw-bytes initialiser, cropping's identity and intersection rules, and that a draw
// through the public API behaves the way the app's call sites assume.

/// A context with CoreGraphics' own user space: origin at the bottom left, y up. That is
/// what a bitmap context has before anything flips it, and drawing an image into
/// `(0, 0, w, h)` there lands source row 0 on device row 0 — the upright 1:1 case.
///
/// `makeContext` flips, as `BrushRaster.context` does, so an image drawn into it arrives
/// upside down; the app cancels that with a second flip inside `BrushRaster.draw`. Source
/// images here are authored through the flipped helper, where filling at y = 0 lands on
/// buffer row 0, and then drawn into an unflipped one.
func makeRawContext(_ width: Int, _ height: Int, mask: Bool = false) throws -> CGContext {
    let space = mask ? CGColorSpaceCreateDeviceGray()
                     : try #require(CGColorSpace(name: CGColorSpace.sRGB))
    let info = mask ? CGImageAlphaInfo.none.rawValue
                    : (CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)
    return try #require(CGContext(data: nil, width: width, height: height,
                                  bitsPerComponent: 8,
                                  bytesPerRow: width * (mask ? 1 : 4),
                                  space: space, bitmapInfo: info))
}

@Suite("CGImage: construction")
struct CGImageCreationTests {
    @Test("a one-pixel grey mask can be built from a provider")
    func onePixelMask() throws {
        // LayerMask.solid(revealing:) builds exactly this and then uses it as a clip mask
        // stretched over a whole layer.
        let provider = try #require(CGDataProvider(data: Data([255])))
        let image = try #require(CGImage(width: 1, height: 1, bitsPerComponent: 8,
                                         bitsPerPixel: 8, bytesPerRow: 1,
                                         space: CGColorSpaceCreateDeviceGray(),
                                         bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                                         provider: provider, decode: nil,
                                         shouldInterpolate: false, intent: .defaultIntent))
        #expect(image.width == 1 && image.height == 1)
        #expect(image.bitsPerPixel == 8)
        #expect(image.alphaInfo == .none)
        #expect(image.colorSpace?.model == .monochrome)
        #expect(!image.isMask, "LayerMask.isValid checks this")
        #expect(try #require(image.pixels)[0] == 255)
    }

    @Test("a provider too small for the stated size is refused")
    func shortProvider() throws {
        let provider = try #require(CGDataProvider(data: Data([1, 2, 3])))
        #expect(CGImage(width: 4, height: 4, bitsPerComponent: 8, bitsPerPixel: 8,
                        bytesPerRow: 4, space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                        provider: provider, decode: nil, shouldInterpolate: false,
                        intent: .defaultIntent) == nil)
        #expect(CGDataProvider(data: Data()) == nil, "an empty provider is not a provider")
    }

    @Test("a layout the engine has no format for is refused rather than guessed")
    func refusedLayouts() throws {
        let provider = try #require(CGDataProvider(data: Data(repeating: 0, count: 256)))
        let sRGB = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        #expect(CGImage(width: 4, height: 4, bitsPerComponent: 8, bitsPerPixel: 32,
                        bytesPerRow: 16, space: sRGB,
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue
                                                         | CGBitmapInfo.byteOrder32Little.rawValue),
                        provider: provider, decode: nil, shouldInterpolate: true,
                        intent: .defaultIntent) == nil, "little-endian would be BGRA")
        #expect(CGImage(width: 4, height: 4, bitsPerComponent: 8, bitsPerPixel: 8,
                        bytesPerRow: 4, space: sRGB,
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                        provider: provider, decode: nil, shouldInterpolate: true,
                        intent: .defaultIntent) == nil, "grey alpha on an RGB space")
    }
}

@Suite("CGImage: cropping")
struct CGImageCroppingTests {
    private func ramp(_ w: Int, _ h: Int) throws -> (CGContext, CGImage) {
        let context = try makeContext(w, h, mask: true)
        context.setShouldAntialias(false)
        for x in 0..<w {
            context.setFillColor(gray: CGFloat(x) / CGFloat(max(1, w - 1)), alpha: 1)
            context.fill(CGRect(x: CGFloat(x), y: 0, width: 1, height: CGFloat(h)))
        }
        return (context, try #require(context.makeImage()))
    }

    @Test("a crop shares the pixels rather than copying them")
    func zeroCopy() throws {
        // RasterSnapshot re-splits its whole patch list on every paint commit; copying on
        // each crop would copy the painted area every stroke.
        let (_, image) = try ramp(8, 4)
        let crop = try #require(image.cropping(to: CGRect(x: 2, y: 1, width: 4, height: 2)))
        #expect(crop.width == 4 && crop.height == 2)
        #expect(crop.bytesPerRow == image.bytesPerRow, "a view keeps the parent's row stride")
        let base = try #require(image.pixels)
        let inner = try #require(crop.pixels)
        #expect(inner == base.advanced(by: 1 * image.bytesPerRow + 2))
    }

    @Test("a crop is a distinct object even when it covers everything")
    func identity() throws {
        // Distort checks `warped.image !== image` to decide a warp was a no-op. If a
        // full-extent crop returned the same object that check would invert.
        let (_, image) = try ramp(4, 4)
        let whole = try #require(image.cropping(to: CGRect(x: 0, y: 0, width: 4, height: 4)))
        #expect(whole !== image)
        #expect(whole.width == 4 && whole.height == 4)
    }

    @Test("an overhanging crop is trimmed, not refused")
    func overhangIntersects() throws {
        // Three of the thirteen call sites treat nil as "drop this piece", so refusing an
        // overhang instead of trimming it would silently lose painted pixels.
        let (_, image) = try ramp(8, 4)
        let over = try #require(image.cropping(to: CGRect(x: 6, y: 2, width: 10, height: 10)))
        #expect(over.width == 2 && over.height == 2)
    }

    @Test("a crop that misses the image entirely is nil")
    func missIsNil() throws {
        let (_, image) = try ramp(8, 4)
        #expect(image.cropping(to: CGRect(x: 8, y: 0, width: 4, height: 4)) == nil)
        #expect(image.cropping(to: CGRect(x: -4, y: 0, width: 4, height: 4)) == nil)
        #expect(image.cropping(to: CGRect(x: 0, y: 0, width: 0, height: 4)) == nil)
        #expect(image.cropping(to: CGRect.null) == nil)
    }

    @Test("a crop of a snapshot survives its context being drawn into")
    func cropOfASnapshotIsRegistered() throws {
        // DownsampleCache does exactly this: snapshot the context, crop one row out of the
        // snapshot, and draw that row straight back into the same context. Without the crop
        // joining the context's detach list, the store stays shared and the context cannot
        // be written to at all.
        let context = try makeContext(8, 4, mask: true)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 8, height: 4))

        let snapshot = try #require(context.makeImage())
        let row = try #require(snapshot.cropping(to: CGRect(x: 0, y: 3, width: 8, height: 1)))
        #expect(try #require(row.pixels)[0] == 255)

        // The snapshot itself goes away; only the crop is still held.
        context.setFillColor(gray: 0, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 8, height: 4))
        #expect(pixel(context, 0, 0) == [0], "the context was still writable")
        #expect(try #require(row.pixels)[0] == 255, "and the crop kept its own pixels")

        context.draw(row, in: CGRect(x: 0, y: 0, width: 8, height: 1))
        #expect(pixel(context, 0, 0) == [255], "the row drew back in")
        #expect(pixel(context, 0, 1) == [0])
    }
}

@Suite("CGContext: drawing an image")
struct CGContextDrawTests {
    /// An image whose pixel values identify their own position.
    private func marker(_ w: Int, _ h: Int) throws -> CGImage {
        let context = try makeContext(w, h, mask: true)
        context.setShouldAntialias(false)
        for y in 0..<h {
            for x in 0..<w {
                context.setFillColor(gray: CGFloat(x * 16 + y * 4) / 255.0, alpha: 1)
                context.fill(CGRect(x: CGFloat(x), y: CGFloat(y), width: 1, height: 1))
            }
        }
        return try #require(context.makeImage())
    }

    @Test("a 1:1 draw is exact at every interpolation quality, with antialiasing either way")
    func oneToOneIsExact() throws {
        // The requirement RasterSnapshotTests enforces with a 64 MB memcmp between a `.high`,
        // antialiasing-off, per-patch-clipped render and a `.low`, antialiasing-on, single
        // draw. Over a hundred other assertions read their result back through a 1:1 draw.
        let source = try marker(6, 5)
        var renders: [[UInt8]] = []
        for quality in [CGInterpolationQuality.none, .default, .low, .medium, .high] {
            for antialias in [false, true] {
                let context = try makeRawContext(6, 5, mask: true)
                context.setShouldAntialias(antialias)
                context.interpolationQuality = quality
                context.setBlendMode(.copy)
                context.draw(source, in: CGRect(x: 0, y: 0, width: 6, height: 5))
                renders.append(bytes(context))
            }
        }
        for render in renders.dropFirst() {
            #expect(render == renders[0], "every quality and antialias setting must agree")
        }

        // And it is the source, not merely self-consistent.
        let first = renders[0]
        let expected = try #require(source.pixels)
        for y in 0..<5 {
            for x in 0..<6 {
                #expect(first[y * 6 + x] == expected[y * source.bytesPerRow + x])
            }
        }
    }

    @Test("an image is drawn upright, with its first row at the top")
    func orientation() throws {
        // BrushRaster flips at construction, so in the app's user space row 0 is the top.
        let context = try makeRawContext(2, 2, mask: true)
        let source = try marker(2, 2)
        context.setShouldAntialias(false)
        context.setBlendMode(.copy)
        context.draw(source, in: CGRect(x: 0, y: 0, width: 2, height: 2))
        let expected = try #require(source.pixels)
        #expect(pixel(context, 0, 0) == [expected[0]], "source row 0 lands on device row 0")
        #expect(pixel(context, 1, 1) == [expected[source.bytesPerRow + 1]])
    }

    @Test("nearest neighbour picks the pixel whose span contains the sample")
    func nearestTieBreak() throws {
        // A six-column ramp where column i holds i, so the value read is the index. At 1:1
        // every sample lands exactly on a tie, which is where rounding the same expression
        // instead of flooring it would shift the whole image by one.
        let source = try makeContext(6, 1, mask: true)
        source.setShouldAntialias(false)
        for x in 0..<6 {
            source.setFillColor(gray: CGFloat(x) / 255.0, alpha: 1)
            source.fill(CGRect(x: CGFloat(x), y: 0, width: 1, height: 1))
        }
        let image = try #require(source.makeImage())

        let oneToOne = try makeRawContext(6, 1, mask: true)
        oneToOne.setShouldAntialias(false)
        oneToOne.interpolationQuality = .none
        oneToOne.setBlendMode(.copy)
        oneToOne.draw(image, in: CGRect(x: 0, y: 0, width: 6, height: 1))
        #expect(bytes(oneToOne) == [0, 1, 2, 3, 4, 5])

        let magnified = try makeRawContext(6, 1, mask: true)
        magnified.setShouldAntialias(false)
        magnified.interpolationQuality = .none
        magnified.setBlendMode(.copy)
        magnified.draw(image, in: CGRect(x: 0, y: 0, width: 12, height: 1))
        #expect(bytes(magnified) == [0, 0, 1, 1, 2, 2], "2x magnification doubles each column")

        // Halving is the case that separates the centre rule from a corner-to-corner
        // mapping: corner-to-corner would read 0, 2, 4 here.
        let reduced = try makeRawContext(3, 1, mask: true)
        reduced.setShouldAntialias(false)
        reduced.interpolationQuality = .none
        reduced.setBlendMode(.copy)
        reduced.draw(image, in: CGRect(x: 0, y: 0, width: 3, height: 1))
        #expect(bytes(reduced) == [1, 3, 5])
    }

    @Test("the graphics state applies to an image draw")
    func stateApplies() throws {
        let source = try marker(4, 4)

        let faded = try makeRawContext(4, 4, mask: true)
        faded.setShouldAntialias(false)
        faded.setFillColor(gray: 0, alpha: 1)
        faded.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        faded.setAlpha(0.5)
        faded.draw(source, in: CGRect(x: 0, y: 0, width: 4, height: 4))
        let expected = try #require(source.pixels)
        #expect(pixel(faded, 3, 0)[0] < expected[3], "setAlpha lerps it towards the backdrop")

        let clipped = try makeRawContext(4, 4, mask: true)
        clipped.setShouldAntialias(false)
        clipped.setBlendMode(.copy)
        clipped.clip(to: CGRect(x: 2, y: 0, width: 2, height: 4))
        clipped.draw(source, in: CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(clipped, 0, 0) == [0], "outside the clip nothing is drawn")
        #expect(pixel(clipped, 2, 0) == [expected[2]], "inside it the image is")
    }

    @Test("a reduction averages rather than dropping pixels")
    func reductionAverages() throws {
        // The reduction a single draw receives is not bounded by 2 — the halving chain
        // saturates — so the filter has to widen with the scale. One-pixel stripes reduced
        // eightfold must come out flat grey; nearest loses them entirely.
        let source = try makeContext(64, 1, mask: true)
        source.setShouldAntialias(false)
        for x in 0..<64 {
            source.setFillColor(gray: x % 2 == 0 ? 0 : 1, alpha: 1)
            source.fill(CGRect(x: CGFloat(x), y: 0, width: 1, height: 1))
        }
        let image = try #require(source.makeImage())

        let smooth = try makeRawContext(8, 1, mask: true)
        smooth.setShouldAntialias(false)
        smooth.interpolationQuality = .low
        smooth.setBlendMode(.copy)
        smooth.draw(image, in: CGRect(x: 0, y: 0, width: 8, height: 1))
        let averaged = bytes(smooth)
        // The outermost pixels are pulled by edge replication; judge the inside, as
        // DownsampleTests does for the same reason.
        for x in 2..<6 { #expect(averaged[x] == 128) }

        let nearest = try makeRawContext(8, 1, mask: true)
        nearest.setShouldAntialias(false)
        nearest.interpolationQuality = .none
        nearest.setBlendMode(.copy)
        nearest.draw(image, in: CGRect(x: 0, y: 0, width: 8, height: 1))
        #expect(bytes(nearest).allSatisfy { $0 == 0 },
                "nearest lands on one parity and loses the stripes, which is the control")
    }

    @Test("a clipped piece of a draw matches the same region of the whole draw")
    func cropInvariance() throws {
        // What the tiled renderer rests on: a piece composed on its own must equal the same
        // region of the whole. It holds because the source coordinate is computed in closed
        // form per pixel rather than accumulated across the row.
        let source = try marker(11, 3)
        let rect = CGRect(x: 1, y: 0, width: 17.5, height: 3)

        let whole = try makeRawContext(24, 4, mask: true)
        whole.setShouldAntialias(false)
        whole.interpolationQuality = .low
        whole.setBlendMode(.copy)
        whole.draw(source, in: rect)

        let pieces = try makeRawContext(24, 4, mask: true)
        pieces.setShouldAntialias(false)
        pieces.interpolationQuality = .low
        pieces.setBlendMode(.copy)
        for start in stride(from: CGFloat(0), to: 24, by: 7) {
            pieces.saveGState()
            pieces.clip(to: CGRect(x: start, y: -5, width: 7, height: 14))
            pieces.draw(source, in: rect)
            pieces.restoreGState()
        }

        #expect(bytes(whole) == bytes(pieces))
    }

    @Test("drawing a live snapshot back into its own context sees the frozen pixels")
    func snapshotDrawnBackIn() throws {
        let context = try makeContext(4, 4, mask: true)
        context.setShouldAntialias(false)
        context.setBlendMode(.copy)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))

        let snapshot = try #require(context.makeImage())
        context.setFillColor(gray: 0, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0) == [0])

        context.draw(snapshot, in: CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0) == [255], "the snapshot still held white")
    }
}
