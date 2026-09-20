import CCompositorRaster

/// A bitmap image.
///
/// In this slice a `CGImage` is metadata over a pixel buffer and nothing more: it is what
/// `CGContext.makeImage()` returns, and it exists now rather than later because without a
/// return type the copy-on-write behaviour — the subtlest thing in the context — could not
/// be tested from Swift at all, and CI is the only Swift check this port has.
///
/// The expensive half comes next: decoding, `cropping(to:)`, data providers, and being
/// drawable. All of it is additive, which is why a placeholder type to be renamed later
/// would have been strictly worse than one type that grows.
///
/// Reference identity is part of the contract. Several call sites compare images with `===`
/// to decide whether a layer's pixels actually changed, so this must stay a class.
public final class CGImage: @unchecked Sendable {
    /// The pixels. Shared with whatever produced them until someone writes, at which point
    /// the writer detaches this surface rather than moving its own.
    let surface: OpaquePointer

    public let width: Int
    public let height: Int
    public let bitsPerComponent: Int
    public let bitsPerPixel: Int
    public let bytesPerRow: Int
    public let colorSpace: CGColorSpace?
    public let alphaInfo: CGImageAlphaInfo
    public let bitmapInfo: CGBitmapInfo

    /// False for everything a context can produce. CoreGraphics reserves `true` for images
    /// built by `CGImage(maskWidth:...)`, which this port has no source of.
    public let isMask: Bool = false

    /// Takes ownership of one reference to `surface`.
    init(surface: OpaquePointer, colorSpace: CGColorSpace?, alphaInfo: CGImageAlphaInfo,
         bitmapInfo: CGBitmapInfo) {
        self.surface = surface
        self.width = raster_surface_width(surface)
        self.height = raster_surface_height(surface)
        self.bytesPerRow = raster_surface_stride(surface)
        self.bitsPerComponent = 8
        self.bitsPerPixel = raster_surface_format(surface) == RasterFormat.gray8 ? 8 : 32
        self.colorSpace = colorSpace
        self.alphaInfo = alphaInfo
        self.bitmapInfo = bitmapInfo
    }

    deinit { raster_surface_release(surface) }

    /// The bytes, for tests and for the exporters that will read them in a later slice.
    public var pixels: UnsafePointer<UInt8>? { raster_surface_bytes(surface) }
}

/// Pixel-format numbering, mirroring `raster.h`. Declared rather than imported for the same
/// reason the blend modes are: Swift renames C enum cases by stripping common prefixes, and
/// the exact spelling it lands on is not something to discover from a CI failure.
enum RasterFormat {
    static let rgba8: raster_format = 0
    static let gray8: raster_format = 1
}
