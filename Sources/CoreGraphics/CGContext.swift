import CCompositorRaster

/// A bitmap drawing destination.
///
/// This slice covers everything the app does to a context that does not involve an image or
/// a path: construction, the graphics state stack, the CTM, rectangle clipping, filling and
/// clearing, and reading the result back out. Images, curves, gradients, transparency layers
/// and stroking arrive later.
///
/// Composition of the CTM lives here rather than in C, so there is exactly one graphics
/// state stack (in C, where save and restore live) and exactly one implementation of pre-
/// versus post-concatenation (`CGAffineTransform`, already tested). The C side only reads
/// and writes the whole matrix.
public final class CGContext: @unchecked Sendable {
    private let raster: OpaquePointer
    private let surface: OpaquePointer
    private let format: raster_format

    public let width: Int
    public let height: Int
    public let bitsPerComponent: Int
    public let bitsPerPixel: Int
    public let bytesPerRow: Int
    /// Non-optional in practice for every context this port can build, which is what lets
    /// `ImageExporter` force-unwrap it.
    public let colorSpace: CGColorSpace?
    public let alphaInfo: CGImageAlphaInfo
    public let bitmapInfo: CGBitmapInfo

    /// Builds a bitmap context, or returns nil for a layout the engine does not implement.
    ///
    /// The label order and the bare `UInt32` for `bitmapInfo` are CoreGraphics', matched so
    /// that the app's seventeen construction sites port unchanged. `CGImage.init` takes the
    /// same flags as a struct instead; that asymmetry is theirs and is reproduced rather
    /// than tidied away.
    public init?(data: UnsafeMutableRawPointer?, width: Int, height: Int,
                 bitsPerComponent: Int, bytesPerRow: Int,
                 space: CGColorSpace, bitmapInfo: UInt32) {
        guard width > 0, height > 0, bitsPerComponent == 8 else { return nil }

        let info = CGBitmapInfo(rawValue: bitmapInfo)
        let order = info.intersection(.byteOrderMask)
        guard let alpha = info.alphaInfo else { return nil }

        // Reject rather than guess. The four spellings below are the only ones the app
        // produces; everything else would have to be invented, and inventing a pixel layout
        // is how a port acquires a colour bug that no test can see.
        let resolved: raster_format
        switch (alpha, space.model) {
        case (.premultipliedLast, .rgb), (.noneSkipLast, .rgb):
            // Big-endian 32-bit and the default are the same bytes at 8 bits per component:
            // R, G, B, A. Little-endian would be BGRA, which the engine has no layout for.
            guard order == [] || order == .byteOrder32Big else { return nil }
            guard bytesPerRow >= width * 4 else { return nil }
            resolved = RasterFormat.rgba8
        case (.none, .monochrome):
            guard order == [] else { return nil }
            guard bytesPerRow >= width else { return nil }
            resolved = RasterFormat.gray8
        default:
            return nil
        }

        let created: OpaquePointer? = data.map {
            raster_surface_create_borrowed($0, width, height, bytesPerRow, resolved)
        } ?? raster_surface_create(width, height, resolved)
        guard let created, let context = raster_context_create(created) else {
            if let created { raster_surface_release(created) }
            return nil
        }
        raster_surface_release(created)  // the context retains it

        self.raster = context
        self.surface = raster_context_target(context)!
        self.format = resolved
        self.width = width
        self.height = height
        self.bitsPerComponent = 8
        self.bitsPerPixel = resolved == RasterFormat.gray8 ? 8 : 32
        self.bytesPerRow = raster_surface_stride(self.surface)
        self.colorSpace = space
        self.alphaInfo = alpha
        self.bitmapInfo = info
    }

    deinit { raster_context_destroy(raster) }

    // MARK: - Graphics state

    public func saveGState() { _ = raster_context_save(raster) }

    /// Unbalanced restores do nothing, matching CoreGraphics, which logs and continues.
    public func restoreGState() { raster_context_restore(raster) }

    public func setAlpha(_ alpha: CGFloat) { raster_context_set_alpha(raster, Double(alpha)) }

    public func setBlendMode(_ mode: CGBlendMode) {
        raster_context_set_blend(raster, mode.raster)
    }

    public func setShouldAntialias(_ shouldAntialias: Bool) {
        raster_context_set_antialias(raster, shouldAntialias)
    }

    public var interpolationQuality: CGInterpolationQuality {
        get {
            CGInterpolationQuality(rawValue: Int32(bitPattern: raster_context_interpolation(raster)))
                ?? .default
        }
        set {
            raster_context_set_interpolation(raster, raster_interpolation(bitPattern: newValue.rawValue))
        }
    }

    public func setFillColor(_ color: CGColor) {
        let (r, g, b, a) = color.rgba
        var rgba = [r, g, b, a]
        raster_context_set_fill_color(raster, &rgba)
    }

    public func setFillColor(gray: CGFloat, alpha: CGFloat) {
        var rgba = [Double(gray), Double(gray), Double(gray), Double(alpha)]
        raster_context_set_fill_color(raster, &rgba)
    }

    public func setFillColor(red: CGFloat, green: CGFloat, blue: CGFloat, alpha: CGFloat) {
        var rgba = [Double(red), Double(green), Double(blue), Double(alpha)]
        raster_context_set_fill_color(raster, &rgba)
    }

    // MARK: - The CTM

    /// User space to device space, with the bitmap's base flip already folded in. This is the
    /// matrix stored by the engine, so it is returned without further work.
    public var userSpaceToDeviceSpaceTransform: CGAffineTransform {
        let m = raster_context_matrix(raster)
        return CGAffineTransform(a: CGFloat(m.a), b: CGFloat(m.b), c: CGFloat(m.c),
                                 d: CGFloat(m.d), tx: CGFloat(m.tx), ty: CGFloat(m.ty))
    }

    /// The CTM as CoreGraphics reports it: relative to the base transform rather than to
    /// device space. The base flip is its own inverse, so composing with it again recovers
    /// the user-visible matrix.
    public var ctm: CGAffineTransform {
        userSpaceToDeviceSpaceTransform.concatenating(Self.base(height: height).inverted())
    }

    private static func base(height: Int) -> CGAffineTransform {
        CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: 0, ty: CGFloat(height))
    }

    private func compose(_ step: CGAffineTransform) {
        // Pre-concatenation: the new step applies in the current user space, then the
        // existing matrix carries the result on to the device.
        let combined = step.concatenating(userSpaceToDeviceSpaceTransform)
        raster_context_set_matrix(raster, raster_matrix(a: Double(combined.a), b: Double(combined.b),
                                                        c: Double(combined.c), d: Double(combined.d),
                                                        tx: Double(combined.tx), ty: Double(combined.ty)))
    }

    public func translateBy(x: CGFloat, y: CGFloat) {
        compose(CGAffineTransform(translationX: x, y: y))
    }

    public func scaleBy(x: CGFloat, y: CGFloat) {
        compose(CGAffineTransform(scaleX: x, y: y))
    }

    public func rotate(by angle: CGFloat) {
        compose(CGAffineTransform(rotationAngle: angle))
    }

    public func concatenate(_ transform: CGAffineTransform) {
        compose(transform)
    }

    // MARK: - Clipping

    public func clip(to rect: CGRect) {
        check(raster_context_clip_rect(raster, Self.frect(rect)), "clip(to:)")
    }

    public func clip(to rects: [CGRect]) {
        // CoreGraphics intersects the clip with the *union* of the rectangles.
        for rect in rects { addRect(rect) }
        check(raster_context_clip_path(raster, false), "clip(to:)")
    }

    /// Adds a rectangle to the current path. The path belongs to the context, not to the
    /// graphics state: CoreGraphics does not save or restore it, and `LayerRenderer` relies
    /// on that by calling this again immediately after a clip.
    public func addRect(_ rect: CGRect) {
        check(raster_context_add_rect(raster, Self.frect(rect)), "addRect")
    }

    public func clip() { clip(using: .winding) }

    /// Intersects the clip with the current path and empties it. An empty path clips
    /// everything away — CoreGraphics leaves that unspecified and the app never reaches it,
    /// so the choice is written down here rather than made twice, differently.
    public func clip(using rule: CGPathFillRule) {
        check(raster_context_clip_path(raster, rule == .evenOdd), "clip(using:)")
    }

    public func resetClip() {
        raster_context_reset_path(raster)
    }

    /// The clip's bounds in the current user space.
    ///
    /// Empty reports `CGRect.null`, not `.zero`. `Selection` builds an empty clip on purpose
    /// for an empty marquee, and `TiledLayerRenderer` insets this value by −64 — from `.zero`
    /// that would be a nonsense 128×128 rectangle at the origin instead of nothing.
    ///
    /// The value is the rasterised bounds mapped back, so it is exactly tight rather than the
    /// strict superset CoreGraphics can return. Every consumer either takes `.integral` and
    /// sizes a buffer from it, insets it before use, or uses it only to cull.
    public var boundingBoxOfClipPath: CGRect {
        var bounds = raster_rect()
        guard raster_context_clip_bounds(raster, &bounds) else { return .null }
        let device = CGRect(x: CGFloat(bounds.x0), y: CGFloat(bounds.y0),
                            width: CGFloat(bounds.x1 - bounds.x0),
                            height: CGFloat(bounds.y1 - bounds.y0))
        return device.applying(userSpaceToDeviceSpaceTransform.inverted())
    }

    // MARK: - Painting

    public func fill(_ rect: CGRect) {
        check(raster_context_fill_rect(raster, Self.frect(rect)), "fill")
    }

    public func fill(_ rects: [CGRect]) {
        for rect in rects { fill(rect) }
    }

    /// Honours the CTM and the clip, and ignores the graphics state's alpha and blend mode.
    /// That is not the same operation as filling with `.clear`, which is alpha-weighted, and
    /// `TiledLayerRenderer` uses both — so they must not collapse into one.
    public func clear(_ rect: CGRect) {
        check(raster_context_clear_rect(raster, Self.frect(rect)), "clear")
    }

    /// Draws `image` to fill `rect`, honouring the CTM, the clip, the alpha, the blend mode
    /// and the interpolation quality.
    ///
    /// At unit scale with an integer translation every quality selects the source pixel
    /// exactly, so the same draw at `.low` and at `.high`, with antialiasing on or off, is
    /// byte for byte the same. That is what `RasterSnapshotTests` compares with `memcmp`
    /// over a whole 4000×4000 buffer, and what a hundred-odd other assertions rest on when
    /// they read a result back through a 1:1 draw.
    public func draw(_ image: CGImage, in rect: CGRect) {
        check(raster_context_draw_image(raster, image.surface, Self.frect(rect)), "draw")
    }

    /// Adds a surface derived from one of this context's snapshots — a crop — to the set
    /// detached before the next write.
    func register(snapshot: OpaquePointer) {
        check(raster_context_register_snapshot(raster, snapshot), "cropping")
    }

    // MARK: - Reading back

    /// The pixels, writable, and **stable across draws**.
    ///
    /// `RasterSnapshotTests` reads this once, freezes the bytes, clears and redraws, and then
    /// reads through the same pointer. So copy-on-write runs the opposite way from the
    /// obvious one: the context never moves, the snapshots detach. Handing out a writable
    /// pointer is itself a write in waiting, which is why it detaches them too.
    public var data: UnsafeMutableRawPointer? {
        _ = raster_context_detach_snapshots(raster)
        return raster_surface_mutable_bytes(surface).map(UnsafeMutableRawPointer.init)
    }

    /// A snapshot of the pixels as they are now. Shares them until the next draw.
    public func makeImage() -> CGImage? {
        guard let snapshot = raster_context_make_snapshot(raster) else { return nil }
        // The image remembers where it came from, so that a crop of it can join the same
        // detach list rather than quietly keeping this context's store shared.
        return CGImage(surface: snapshot, colorSpace: colorSpace, alphaInfo: alphaInfo,
                       bitmapInfo: bitmapInfo, origin: self)
    }

    // MARK: - Boundary

    private static func frect(_ rect: CGRect) -> raster_frect {
        raster_frect(x: Double(rect.origin.x), y: Double(rect.origin.y),
                     width: Double(rect.size.width), height: Double(rect.size.height))
    }

    /// Turns the engine's refusals into a trap.
    ///
    /// A non-rectilinear transform means the request is a rotated quadrilateral, which a
    /// rectangle region cannot hold. Approximating it by a bounding box would fail *open* —
    /// drawing pixels the caller asked to mask away, with nothing downstream to catch it —
    /// so it fails loudly instead. Unreachable today: the app is not in the build yet, and
    /// the coverage plane lands before it is.
    ///
    /// When the first real call site is wired up this becomes fail-*closed* (an empty clip,
    /// which renders nothing and is just as unmissable) rather than a trap, because a
    /// `preconditionFailure` inside swift-testing takes the whole process with it.
    private func check(_ status: raster_status, _ what: String) {
        switch status {
        case RasterStatus.ok:
            return
        case RasterStatus.unsupportedTransform:
            preconditionFailure("""
                CGContext.\(what) under a transform that is neither axis-aligned nor a \
                quarter turn. Rectangle regions cannot express a rotated quadrilateral; \
                the coverage plane that will is not implemented yet.
                """)
        default:
            preconditionFailure("CGContext.\(what) ran out of memory")
        }
    }
}

/// Status numbering, mirroring `raster.h`, for the same reason the blend modes are declared
/// rather than imported.
enum RasterStatus {
    static let ok: raster_status = 0
    static let outOfMemory: raster_status = 1
    static let unsupportedTransform: raster_status = 2
}
