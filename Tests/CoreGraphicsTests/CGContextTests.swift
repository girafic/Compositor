import Testing
import CoreGraphics

// C proves the arithmetic; Spikes/context-oracle.c runs a million checks against brute-force
// oracles under valgrind and the sanitizers. These tests cover what C cannot see: the
// binding decisions. Which bitmapInfo spellings are accepted and which are refused, that an
// empty clip surfaces as CGRect.null rather than CGRect.zero, that the data pointer survives
// a redraw, and that the observable behaviour at the public API is the one the app expects.

// MARK: - Support

/// The context the app actually draws into. `BrushRaster.context` flips at construction, so
/// user space ends up being device pixels with a top-left origin and y running down — which
/// is why nearly every call site in the app can treat the two as the same thing.
func makeContext(_ width: Int, _ height: Int, mask: Bool = false) throws -> CGContext {
    let space = mask ? CGColorSpaceCreateDeviceGray()
                     : try #require(CGColorSpace(name: CGColorSpace.sRGB))
    let info = mask ? CGImageAlphaInfo.none.rawValue
                    : (CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)
    let context = try #require(CGContext(data: nil, width: width, height: height,
                                         bitsPerComponent: 8,
                                         bytesPerRow: width * (mask ? 1 : 4),
                                         space: space, bitmapInfo: info))
    context.translateBy(x: 0, y: CGFloat(height))
    context.scaleBy(x: 1, y: -1)
    return context
}

/// One pixel's bytes, in device coordinates.
func pixel(_ context: CGContext, _ x: Int, _ y: Int) -> [UInt8] {
    let bytesPerPixel = context.bitsPerPixel / 8
    guard let data = context.data else { return [] }
    let base = data.assumingMemoryBound(to: UInt8.self)
    let offset = y * context.bytesPerRow + x * bytesPerPixel
    return (0..<bytesPerPixel).map { base[offset + $0] }
}

/// Every byte of the surface, for comparing two contexts.
func bytes(_ context: CGContext) -> [UInt8] {
    guard let data = context.data else { return [] }
    let base = data.assumingMemoryBound(to: UInt8.self)
    return (0..<(context.bytesPerRow * context.height)).map { base[$0] }
}

/// Which device columns of row `y` received any paint.
func paintedColumns(_ context: CGContext, row y: Int) -> [Int] {
    let bytesPerPixel = context.bitsPerPixel / 8
    return (0..<context.width).filter { x in
        pixel(context, x, y).prefix(bytesPerPixel).contains { $0 != 0 }
    }
}

// MARK: - Construction

@Suite("CGContext: construction")
struct CGContextCreationTests {
    @Test("the four layouts the app builds are all accepted")
    func acceptedLayouts() throws {
        let sRGB = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        let gray = CGColorSpaceCreateDeviceGray()

        // premultipliedLast on its own and with byteOrder32Big are the same bytes at 8 bits
        // per component, and the app writes both spellings.
        let plain = try #require(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8,
                                           bytesPerRow: 16, space: sRGB,
                                           bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        #expect(plain.bitsPerPixel == 32)
        #expect(plain.bytesPerRow == 16)

        let bigEndian = try #require(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8,
                                               bytesPerRow: 16, space: sRGB,
                                               bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
                                                         | CGBitmapInfo.byteOrder32Big.rawValue))
        #expect(bigEndian.alphaInfo == .premultipliedLast)

        // The JPEG flatten context. Opaque RGBX, which maps onto RGBA8 — the fill makes the
        // area opaque and the encoder ignores the last byte.
        let skip = try #require(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8,
                                          bytesPerRow: 16, space: sRGB,
                                          bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        #expect(skip.alphaInfo == .noneSkipLast)

        let mask = try #require(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8,
                                          bytesPerRow: 4, space: gray,
                                          bitmapInfo: CGImageAlphaInfo.none.rawValue))
        #expect(mask.bitsPerPixel == 8)
        #expect(mask.alphaInfo == .none)

        // ImageExporter force-unwraps this, so nil would be a crash rather than a failure.
        for context in [plain, bigEndian, skip, mask] {
            #expect(context.colorSpace != nil)
        }
    }

    @Test("layouts the engine has no pixel format for are refused rather than guessed")
    func refusedLayouts() throws {
        let sRGB = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        let gray = CGColorSpaceCreateDeviceGray()

        // Little-endian 32-bit would be BGRA, which the engine does not implement. Guessing
        // here is how a port acquires a channel-swap bug that no test can see.
        #expect(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8, bytesPerRow: 16,
                          space: sRGB,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
                                    | CGBitmapInfo.byteOrder32Little.rawValue) == nil)
        #expect(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8, bytesPerRow: 16,
                          space: sRGB,
                          bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue) == nil,
                "ARGB is a different layout, not a spelling of RGBA")
        #expect(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 16, bytesPerRow: 32,
                          space: sRGB,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) == nil,
                "16 bits per component is not implemented")
        #expect(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8, bytesPerRow: 8,
                          space: sRGB,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) == nil,
                "a row too short for the pixels cannot be honoured")
        #expect(CGContext(data: nil, width: 0, height: 3, bitsPerComponent: 8, bytesPerRow: 0,
                          space: sRGB,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) == nil)
        #expect(CGContext(data: nil, width: 4, height: 3, bitsPerComponent: 8, bytesPerRow: 16,
                          space: gray,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) == nil,
                "an alpha channel on a grey space has nowhere to live")
    }

    @Test("a context over caller memory draws into that memory")
    func borrowedData() throws {
        // The eyedropper: a 1x1 context over the caller's own array, with the whole document
        // drawn through a translated CTM so that one pixel lands in it.
        var buffer = [UInt8](repeating: 0, count: 4)
        let sRGB = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        buffer.withUnsafeMutableBufferPointer { raw in
            let context = CGContext(data: raw.baseAddress, width: 1, height: 1,
                                    bitsPerComponent: 8, bytesPerRow: 4, space: sRGB,
                                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            guard let context else { return }
            context.setShouldAntialias(false)
            context.setFillColor(red: 1, green: 0, blue: 0, alpha: 1)
            context.fill(CGRect(x: 0, y: 0, width: 1, height: 1))
        }
        #expect(buffer == [255, 0, 0, 255], "the fill must land in the caller's array")
    }

    @Test("a colour space the engine cannot honour is nil rather than a silent substitution")
    func unsupportedColorSpace() {
        #expect(CGColorSpace(name: CGColorSpace.displayP3) == nil,
                "handing back sRGB instead would be an undetectable colour bug")
        #expect(CGColorSpace(name: CGColorSpace.sRGB)?.model == .rgb)
        #expect(CGColorSpaceCreateDeviceGray().model == .monochrome)
        #expect(CGColorSpaceCreateDeviceGray().numberOfComponents == 1)
    }
}

// MARK: - Graphics state

@Suite("CGContext: the graphics state stack")
struct CGContextStateTests {
    @Test("save and restore round-trip the whole state, clip included")
    func roundTrip() throws {
        let context = try makeContext(8, 8)
        let original = context.userSpaceToDeviceSpaceTransform

        context.saveGState()
        context.setAlpha(0.25)
        context.setBlendMode(.multiply)
        context.setShouldAntialias(false)
        context.interpolationQuality = .high
        context.translateBy(x: 3, y: 4)
        context.clip(to: CGRect(x: 1, y: 1, width: 2, height: 2))
        #expect(context.interpolationQuality == .high)
        #expect(context.userSpaceToDeviceSpaceTransform != original)
        #expect(context.boundingBoxOfClipPath.width == 2)

        context.restoreGState()
        #expect(context.interpolationQuality == .default)
        #expect(context.userSpaceToDeviceSpaceTransform == original)
        #expect(context.boundingBoxOfClipPath == CGRect(x: 0, y: 0, width: 8, height: 8))
    }

    @Test("restoring more than was saved does nothing")
    func unbalancedRestore() throws {
        // CoreGraphics logs and carries on. A stack underflow that trapped would take the
        // whole test process down with it.
        let context = try makeContext(4, 4)
        context.restoreGState()
        context.restoreGState()
        context.saveGState()
        context.restoreGState()
        context.restoreGState()
        #expect(context.boundingBoxOfClipPath == CGRect(x: 0, y: 0, width: 4, height: 4))
    }

    @Test("the flip at construction makes user space device space")
    func flippedUserSpace() throws {
        let context = try makeContext(4, 4)
        #expect(context.userSpaceToDeviceSpaceTransform == .identity,
                "BrushRaster's flip exactly cancels the base transform")

        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 1))
        #expect(pixel(context, 0, 0)[3] == 255, "the first row of user space is the top row")
        #expect(pixel(context, 0, 3)[3] == 0)
    }

    @Test("an unflipped context has CoreGraphics' bottom-left origin")
    func unflippedUserSpace() throws {
        let sRGB = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        let context = try #require(CGContext(data: nil, width: 4, height: 4, bitsPerComponent: 8,
                                             bytesPerRow: 16, space: sRGB,
                                             bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 1))
        #expect(pixel(context, 0, 3)[3] == 255, "y = 0 is the bottom row without a flip")
        #expect(pixel(context, 0, 0)[3] == 0)
    }
}

// MARK: - Clipping

@Suite("CGContext: clipping")
struct CGContextClipTests {
    @Test("a fresh context's clip is the whole surface")
    func freshClip() throws {
        let context = try makeContext(6, 5)
        #expect(context.boundingBoxOfClipPath == CGRect(x: 0, y: 0, width: 6, height: 5))
    }

    @Test("an empty clip reports null, not zero")
    func emptyClipIsNull() throws {
        // Selection builds one of these for an empty marquee, and TiledLayerRenderer insets
        // the result by -64. From .zero that inset is a nonsense 128x128 rect at the origin;
        // from .null it stays null, which is what the caller has to see.
        let context = try makeContext(6, 5)
        context.clip(to: CGRect.zero)
        let bounds = context.boundingBoxOfClipPath
        #expect(bounds.isNull)
        #expect(bounds != CGRect.zero)
        #expect(bounds.insetBy(dx: -64, dy: -64).isNull)

        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 6, height: 5))
        #expect(paintedColumns(context, row: 0).isEmpty, "nothing draws through an empty clip")
    }

    @Test("nested clips intersect and unwind")
    func nesting() throws {
        let context = try makeContext(10, 10)
        context.clip(to: CGRect(x: 0, y: 0, width: 6, height: 6))
        #expect(context.boundingBoxOfClipPath == CGRect(x: 0, y: 0, width: 6, height: 6))

        context.saveGState()
        context.clip(to: CGRect(x: 4, y: 4, width: 6, height: 6))
        #expect(context.boundingBoxOfClipPath == CGRect(x: 4, y: 4, width: 2, height: 2))
        context.restoreGState()
        #expect(context.boundingBoxOfClipPath == CGRect(x: 0, y: 0, width: 6, height: 6))
    }

    @Test("addRect twice with evenOdd leaves the overlap out")
    func evenOddLeavesAHole() throws {
        // This is how LayerRenderer makes its tiled clips disjoint, which is what keeps
        // opacity and blend modes applied exactly once per pixel.
        let context = try makeContext(12, 4)
        context.setShouldAntialias(false)
        context.addRect(CGRect(x: 0, y: 0, width: 8, height: 4))
        context.addRect(CGRect(x: 4, y: 0, width: 8, height: 4))
        context.clip(using: .evenOdd)

        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 12, height: 4))
        #expect(paintedColumns(context, row: 0) == [0, 1, 2, 3, 8, 9, 10, 11],
                "the overlap 4..<8 is a hole under even-odd")
    }

    @Test("addRect twice with winding fills the union")
    func windingFillsTheUnion() throws {
        let context = try makeContext(12, 4)
        context.setShouldAntialias(false)
        context.addRect(CGRect(x: 0, y: 0, width: 8, height: 4))
        context.addRect(CGRect(x: 4, y: 0, width: 8, height: 4))
        context.clip(using: .winding)

        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 12, height: 4))
        #expect(paintedColumns(context, row: 0) == Array(0..<12))
    }

    @Test("clipping consumes the current path")
    func clipConsumesThePath() throws {
        // LayerRenderer calls addRect again immediately after a clip, so the path must be
        // empty by then. And the path is not part of the graphics state: CoreGraphics does
        // not save or restore it.
        let context = try makeContext(8, 8)
        context.addRect(CGRect(x: 2, y: 2, width: 4, height: 4))
        context.saveGState()
        context.restoreGState()
        context.clip(using: .winding)
        #expect(context.boundingBoxOfClipPath == CGRect(x: 2, y: 2, width: 4, height: 4),
                "a save/restore pair must not discard the pending path")

        context.clip(using: .winding)
        #expect(context.boundingBoxOfClipPath.isNull,
                "the path was consumed, and an empty path clips everything away")
    }
}

// MARK: - The coverage rule

@Suite("CGContext: which pixels a fill covers")
struct CGContextCoverageTests {
    @Test("a hard-edged fill covers exactly the pixels whose centres it contains")
    func centreRule() throws {
        // The tie-break is pinned here on purpose. A pixel spans [i, i+1) with centre i+0.5,
        // so an edge at exactly i+0.5 includes column i, and switching the rule to
        // floor(t + 0.5) would flip these and must fail.
        let cases: [(CGFloat, CGFloat, [Int])] = [
            (0.0, 4.0, [0, 1, 2, 3]),
            (0.4, 4.0, [0, 1, 2, 3]),
            (0.5, 4.0, [0, 1, 2, 3]),
            (0.6, 4.0, [1, 2, 3]),
            (1.5, 4.0, [1, 2, 3]),
            (1.6, 4.0, [2, 3]),
            (0.0, 3.5, [0, 1, 2]),
            (0.0, 3.6, [0, 1, 2, 3]),
            (2.4, 2.6, [2]),   // narrower than a pixel, but it does contain 2.5
            (2.6, 2.9, []),    // narrower than a pixel and contains no centre at all
        ]
        for (start, end, want) in cases {
            let context = try makeContext(6, 2)
            context.setShouldAntialias(false)
            context.setFillColor(gray: 1, alpha: 1)
            context.fill(CGRect(x: start, y: 0, width: end - start, height: 2))
            #expect(paintedColumns(context, row: 0) == want,
                    "x from \(start) to \(end) should paint \(want)")
        }
    }

    @Test("an antialiased fill feathers a fractional edge instead of snapping it")
    func antialiasedEdges() throws {
        let context = try makeContext(6, 2)
        context.setShouldAntialias(true)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 1.5, y: 0, width: 3, height: 2))
        // Column 1 is half covered, 2 and 3 fully, 4 half.
        #expect(pixel(context, 0, 0)[3] == 0)
        #expect(pixel(context, 1, 0)[3] == 128)
        #expect(pixel(context, 2, 0)[3] == 255)
        #expect(pixel(context, 3, 0)[3] == 255)
        #expect(pixel(context, 4, 0)[3] == 128)
        #expect(pixel(context, 5, 0)[3] == 0)
    }

    @Test("on integer geometry antialiasing changes nothing at all")
    func antialiasingIsInvisibleOnIntegers() throws {
        // This is what licenses implementing antialiased fills in the same slice as hard
        // ones: on the geometry the app actually uses the flag has no effect, so the two
        // paths cannot disagree where it matters.
        let rect = CGRect(x: 1, y: 1, width: 4, height: 3)
        let hard = try makeContext(8, 6)
        hard.setShouldAntialias(false)
        hard.setFillColor(red: 0.3, green: 0.6, blue: 0.9, alpha: 0.7)
        hard.fill(rect)

        let soft = try makeContext(8, 6)
        soft.setShouldAntialias(true)
        soft.setFillColor(red: 0.3, green: 0.6, blue: 0.9, alpha: 0.7)
        soft.fill(rect)

        #expect(bytes(hard) == bytes(soft))
    }
}

// MARK: - Piecewise equals whole

@Suite("CGContext: a clip cut into pieces paints the same as one draw")
struct CGContextPiecewiseTests {
    /// The decisive property, and the reason the edge rule has to be exact rather than
    /// approximate: neighbouring hard clips must cover their shared edge exactly once. Under
    /// CoreGraphics' own rule they cover it twice, which is the hairline that
    /// `TiledLayerRenderer` spends a whole file working around.
    ///
    /// Only the *clip* is cut, never the fill rectangle. Two partial fills of one pixel
    /// compose as 1 − (1−a₁)(1−a₂), which is not a₁ + a₂ — true of CoreGraphics too — so
    /// splitting the fill would be testing the wrong thing.
    private func check(mask: Bool, blend: CGBlendMode, cuts: [CGFloat]) throws {
        let rect = CGRect(x: 1, y: 1, width: 14, height: 8)

        let whole = try makeContext(16, 10, mask: mask)
        let pieces = try makeContext(16, 10, mask: mask)
        for context in [whole, pieces] {
            context.setShouldAntialias(false)
            context.setFillColor(gray: 0.6, alpha: 1)
            context.fill(CGRect(x: 0, y: 0, width: 16, height: 10))  // a backdrop to blend into
            context.setAlpha(0.5)
            context.setBlendMode(blend)
            context.setFillColor(red: 0.8, green: 0.4, blue: 0.2, alpha: 1)
        }

        whole.fill(rect)

        let edges = [-100 as CGFloat] + cuts + [100]
        for i in 0..<(edges.count - 1) {
            pieces.saveGState()
            pieces.clip(to: CGRect(x: edges[i], y: -100,
                                   width: edges[i + 1] - edges[i], height: 300))
            pieces.fill(rect)
            pieces.restoreGState()
        }

        #expect(bytes(whole) == bytes(pieces))
    }

    @Test("with integer cuts")
    func integerCuts() throws {
        try check(mask: false, blend: .normal, cuts: [4, 9, 12])
        try check(mask: true, blend: .normal, cuts: [4, 9, 12])
    }

    @Test("with fractional cuts, including exact half-pixels")
    func fractionalCuts() throws {
        try check(mask: false, blend: .normal, cuts: [3.5, 7.5, 11.25])
        try check(mask: false, blend: .normal, cuts: [2.3, 6.7, 10.5, 13.5])
        try check(mask: true, blend: .normal, cuts: [3.5, 7.5, 11.25])
    }

    @Test("under a blend mode that would show any double coverage")
    func underMultiply() throws {
        // Multiply is order-independent per pixel only if each pixel is touched exactly
        // once, so a doubled column comes out visibly darker.
        try check(mask: false, blend: .multiply, cuts: [4, 7.5, 11])
        try check(mask: true, blend: .multiply, cuts: [4, 7.5, 11])
    }
}

// MARK: - Snapshots

@Suite("CGContext: snapshots and the data pointer")
struct CGContextSnapshotTests {
    @Test("the data pointer survives a clear and a redraw")
    func pointerStability() throws {
        // RasterSnapshotTests reads `data` once, freezes the bytes, clears, redraws, and
        // then reads through the same pointer. So copy-on-write has to run the other way
        // round from the obvious one: the context never moves, the snapshot detaches.
        let context = try makeContext(8, 6)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 8, height: 6))

        let pointer = try #require(context.data)
        let base = pointer.assumingMemoryBound(to: UInt8.self)
        let frozen = (0..<(context.bytesPerRow * context.height)).map { base[$0] }
        #expect(frozen[3] == 255)

        let snapshot = try #require(context.makeImage())
        context.clear(CGRect(x: 0, y: 0, width: 8, height: 6))
        #expect(context.data == pointer, "clear must not move the pixels")
        #expect(base[3] == 0, "and must actually clear them")

        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 8, height: 6))
        #expect(context.data == pointer, "redrawing must not move them either")
        let after = (0..<(context.bytesPerRow * context.height)).map { base[$0] }
        #expect(after == frozen, "and the same drawing gives the same bytes")

        let held = try #require(snapshot.pixels)
        #expect(held[3] == 255, "the snapshot kept the pixels it was taken from")
    }

    @Test("a snapshot freezes while the context moves on")
    func snapshotsAreIndependent() throws {
        let context = try makeContext(4, 4)
        context.setShouldAntialias(false)
        context.setFillColor(red: 1, green: 0, blue: 0, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        let first = try #require(context.makeImage())

        context.setFillColor(red: 0, green: 1, blue: 0, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        let second = try #require(context.makeImage())

        let firstPixels = try #require(first.pixels)
        let secondPixels = try #require(second.pixels)
        #expect(firstPixels[0] == 255, "the first snapshot is still red")
        #expect(firstPixels[1] == 0)
        #expect(secondPixels[1] == 255, "the second is green")
        #expect(first !== second, "and they are distinct objects")
    }

    @Test("a snapshot carries the context's layout")
    func snapshotMetadata() throws {
        let colour = try makeContext(5, 3)
        let image = try #require(colour.makeImage())
        #expect(image.width == 5 && image.height == 3)
        #expect(image.bitsPerComponent == 8 && image.bitsPerPixel == 32)
        #expect(image.alphaInfo == .premultipliedLast)
        #expect(image.colorSpace?.model == .rgb)
        #expect(!image.isMask)

        let mask = try makeContext(5, 3, mask: true)
        let grey = try #require(mask.makeImage())
        #expect(grey.bitsPerPixel == 8)
        #expect(grey.alphaInfo == .none)
        #expect(grey.colorSpace?.model == .monochrome,
                "LayerMask and DownsampleCache both branch on this")
    }
}

// MARK: - Fill and clear

@Suite("CGContext: fill and clear")
struct CGContextFillTests {
    @Test("clear ignores the graphics state's alpha and blend mode")
    func clearIgnoresState() throws {
        // CGContextClearRect honours the CTM and the clip and nothing else. Implementing it
        // as "fill with the clear blend" would make this erase only half.
        let context = try makeContext(4, 4)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0)[3] == 255)

        context.setAlpha(0.5)
        context.setBlendMode(.multiply)
        context.clear(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0)[3] == 0, "clear erases fully whatever the state says")
    }

    @Test("filling with the clear blend is a different operation and is alpha-weighted")
    func clearBlendIsNotClear() throws {
        let context = try makeContext(4, 4)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))

        context.setAlpha(0.5)
        context.setBlendMode(.clear)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0)[3] != 0, "half alpha erases half")
        #expect(pixel(context, 0, 0)[3] != 255)
    }

    @Test("clear honours the clip")
    func clearIsClipped() throws {
        let context = try makeContext(8, 2)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 8, height: 2))
        context.clip(to: CGRect(x: 2, y: 0, width: 3, height: 2))
        context.clear(CGRect(x: 0, y: 0, width: 8, height: 2))
        #expect(paintedColumns(context, row: 0) == [0, 1, 5, 6, 7])
    }

    @Test("a grey surface has no alpha channel, so source-over is a lerp")
    func greyIsOpaque() throws {
        // GRAY8 stores an opaque grey sample. Half-alpha white onto black is mid grey, and
        // clearing writes black rather than transparency.
        let context = try makeContext(4, 4, mask: true)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.setAlpha(0.5)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0) == [128])

        context.setAlpha(1)
        context.clear(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0) == [0], "clearing a grey plane writes black")
    }

    @Test("the fill colour's own alpha multiplies with the state's")
    func colourAlphaCompounds() throws {
        let context = try makeContext(4, 4)
        context.setShouldAntialias(false)
        context.setAlpha(0.5)
        context.setFillColor(red: 1, green: 1, blue: 1, alpha: 0.5)
        context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        #expect(pixel(context, 0, 0)[3] == 64, "half of a half")
    }

    @Test("a fill outside the surface is a no-op, not a crash")
    func outOfBounds() throws {
        let context = try makeContext(4, 4)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 100, y: 100, width: 10, height: 10))
        context.fill(CGRect(x: -100, y: -100, width: 10, height: 10))
        context.fill(CGRect(x: 0, y: 0, width: 0, height: 0))
        context.fill(CGRect(x: 2, y: 2, width: -4, height: -4))  // standardises
        #expect(pixel(context, 0, 0)[3] == 255, "the standardised rect did paint")
        #expect(pixel(context, 3, 3)[3] == 0, "and nothing else did")
    }
}

// MARK: - Colour

@Suite("CGColor")
struct CGColorTests {
    @Test("the component count has to match the space")
    func componentCount() throws {
        let sRGB = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        let gray = CGColorSpaceCreateDeviceGray()

        #expect(CGColor(colorSpace: sRGB, components: [1, 0, 0, 1]) != nil)
        #expect(CGColor(colorSpace: sRGB, components: [1, 0, 0]) == nil, "alpha is missing")
        // Gradient builds a two-component grey, so this is a real case rather than a guard
        // against a hypothetical.
        #expect(CGColor(colorSpace: gray, components: [0.5, 1]) != nil)
        #expect(CGColor(colorSpace: gray, components: [0.5, 0.5, 0.5, 1]) == nil)
    }

    @Test("a grey colour paints the same as the grey convenience initialiser")
    func greyResolves() throws {
        let gray = CGColorSpaceCreateDeviceGray()
        let colour = try #require(CGColor(colorSpace: gray, components: [0.25, 1]))
        #expect(colour.alpha == 1)
        #expect(colour.numberOfComponents == 2)

        let viaColor = try makeContext(2, 2)
        viaColor.setShouldAntialias(false)
        viaColor.setFillColor(colour)
        viaColor.fill(CGRect(x: 0, y: 0, width: 2, height: 2))

        let viaGray = try makeContext(2, 2)
        viaGray.setShouldAntialias(false)
        viaGray.setFillColor(gray: 0.25, alpha: 1)
        viaGray.fill(CGRect(x: 0, y: 0, width: 2, height: 2))

        #expect(bytes(viaColor) == bytes(viaGray))
    }

    @Test("srgbRed builds an opaque colour in the RGB space")
    func srgbInitialiser() {
        let colour = CGColor(srgbRed: 1, green: 0.5, blue: 0, alpha: 1)
        #expect(colour.colorSpace.model == .rgb)
        #expect(colour.components == [1, 0.5, 0, 1])
        #expect(CGColor(gray: 0.5, alpha: 0.25).alpha == 0.25)
    }
}
