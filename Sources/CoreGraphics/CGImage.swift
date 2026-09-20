import CCompositorRaster

/// A block of pixel bytes an image can be built from.
///
/// The app uses exactly one shape of this — `LayerMask.solid(revealing:)` wraps a single
/// byte to make the 1×1 mask that gets stretched over a whole layer. The direct-callback
/// variant CoreGraphics also offers has one user, `RasterSnapshot`, whose lazy
/// materialisation is rewritten as a Swift `lazy var` when it ports, so a C callback
/// mechanism for a caller that is about to disappear would be pure ballast.
public final class CGDataProvider: @unchecked Sendable {
    let bytes: [UInt8]

    public init?(data: Data) {
        guard !data.isEmpty else { return nil }
        self.bytes = [UInt8](data)
    }
}

/// A bitmap image.
///
/// Reference identity is part of the contract, not an implementation detail: `===` appears
/// 25 times across the app and `==` never. Three `Equatable` implementations — `ImageLayer`,
/// `LayerMask`, `LayerShape` — are defined in terms of it, and `DownsampleCache` and
/// `DocumentHistory` key caches on `ObjectIdentifier`. So this is a class, and
/// `cropping(to:)` returns a *distinct* object even when the crop covers everything, or
/// `Distort`'s `warped.image !== image` no-op check would invert.
public final class CGImage: @unchecked Sendable {
    /// The pixels. Shared with whatever produced them until someone writes.
    let surface: OpaquePointer

    /// The context this image was snapshotted from, if any. Held so that a crop of a
    /// snapshot can join the same detach list — without that the crop keeps the context's
    /// store shared and the context can no longer be drawn into. Strong on purpose: the
    /// context must outlive any image that may still need to register against it, and there
    /// is no cycle because the context's own list holds C surfaces, not Swift images.
    private let origin: CGContext?

    public let width: Int
    public let height: Int
    public let bitsPerComponent: Int
    public let bitsPerPixel: Int
    public let bytesPerRow: Int
    public let colorSpace: CGColorSpace?
    public let alphaInfo: CGImageAlphaInfo
    public let bitmapInfo: CGBitmapInfo

    /// False for everything this port can produce. CoreGraphics reserves `true` for images
    /// built by `CGImage(maskWidth:...)`, which nothing here calls.
    public let isMask: Bool = false

    /// Kept alive for an image built from a provider, whose bytes this surface borrows.
    private let provider: CGDataProvider?

    /// Takes ownership of one reference to `surface`.
    init(surface: OpaquePointer, colorSpace: CGColorSpace?, alphaInfo: CGImageAlphaInfo,
         bitmapInfo: CGBitmapInfo, origin: CGContext? = nil, provider: CGDataProvider? = nil) {
        self.surface = surface
        self.origin = origin
        self.provider = provider
        self.width = raster_surface_width(surface)
        self.height = raster_surface_height(surface)
        self.bytesPerRow = raster_surface_stride(surface)
        self.bitsPerComponent = 8
        self.bitsPerPixel = raster_surface_format(surface) == RasterFormat.gray8 ? 8 : 32
        self.colorSpace = colorSpace
        self.alphaInfo = alphaInfo
        self.bitmapInfo = bitmapInfo
    }

    /// Builds an image over bytes the caller supplies.
    ///
    /// The label order is CoreGraphics', matched so the app's two call sites port unchanged.
    /// Unlike `CGContext.init` this one takes the flags as a struct — that asymmetry is
    /// CoreGraphics' and is reproduced rather than tidied.
    public convenience init?(width: Int, height: Int, bitsPerComponent: Int, bitsPerPixel: Int,
                             bytesPerRow: Int, space: CGColorSpace, bitmapInfo: CGBitmapInfo,
                             provider: CGDataProvider, decode: UnsafePointer<CGFloat>?,
                             shouldInterpolate: Bool, intent: CGColorRenderingIntent) {
        guard width > 0, height > 0, bitsPerComponent == 8 else { return nil }
        guard let alpha = bitmapInfo.alphaInfo else { return nil }
        let order = bitmapInfo.intersection(.byteOrderMask)

        // The same table as CGContext's, and refusing for the same reason: a layout the
        // engine has no format for cannot be guessed at.
        let format: raster_format
        switch (alpha, space.model) {
        case (.premultipliedLast, .rgb), (.noneSkipLast, .rgb):
            guard order == [] || order == .byteOrder32Big, bitsPerPixel == 32 else { return nil }
            format = RasterFormat.rgba8
        case (.none, .monochrome):
            guard order == [], bitsPerPixel == 8 else { return nil }
            format = RasterFormat.gray8
        default:
            return nil
        }
        guard bytesPerRow >= width * (format == RasterFormat.gray8 ? 1 : 4) else { return nil }
        guard provider.bytes.count >= bytesPerRow * height else { return nil }

        // Copied rather than borrowed. The provider's array is a Swift value whose storage
        // could be moved or freed independently of this image, and the app's one provider
        // holds a single byte, so there is nothing to gain by sharing it.
        guard let surface = raster_surface_create(width, height, format) else { return nil }
        if let destination = raster_surface_mutable_bytes(surface) {
            let stride = raster_surface_stride(surface)
            let row = width * (format == RasterFormat.gray8 ? 1 : 4)
            provider.bytes.withUnsafeBufferPointer { source in
                guard let base = source.baseAddress else { return }
                for y in 0..<height {
                    destination.advanced(by: y * stride)
                        .update(from: base.advanced(by: y * bytesPerRow), count: row)
                }
            }
        }
        self.init(surface: surface, colorSpace: space, alphaInfo: alpha,
                  bitmapInfo: bitmapInfo, provider: provider)
    }

    deinit { raster_surface_release(surface) }

    /// A view onto part of this image, sharing its pixels.
    ///
    /// The rectangle is integralised and intersected with the image, matching
    /// `CGImageCreateWithImageInRect`; nil means the intersection is empty. Thirteen call
    /// sites branch on that, and three of them treat nil as "drop this piece", so refusing an
    /// overhang instead of trimming it would silently lose painted pixels.
    ///
    /// The result shares pixels rather than copying, which the sparse raster depends on — a
    /// paint commit re-splits its whole patch list, and copying each time would copy the
    /// painted area on every stroke.
    public func cropping(to rect: CGRect) -> CGImage? {
        guard !rect.isNull, !rect.isInfinite else { return nil }
        let integral = rect.integral
        guard integral.width > 0, integral.height > 0 else { return nil }

        let x = Int(integral.origin.x), y = Int(integral.origin.y)
        guard x < width, y < height,
              Int(integral.maxX) > 0, Int(integral.maxY) > 0 else { return nil }
        let left = max(0, x), top = max(0, y)
        let right = min(width, Int(integral.maxX)), bottom = min(height, Int(integral.maxY))
        guard right > left, bottom > top else { return nil }

        guard let cropped = raster_surface_crop(surface, left, top, right - left, bottom - top)
        else { return nil }

        // A crop of a snapshot shares the context's store, so it has to join the same detach
        // list. Otherwise the store stays shared and the context cannot be written to at
        // all — which is what DownsampleCache would hit, snapshotting, cropping one row and
        // drawing that row straight back in. Registering retains it; the reference
        // `raster_surface_crop` returned passes to the image below.
        if let origin { origin.register(snapshot: cropped) }

        return CGImage(surface: cropped, colorSpace: colorSpace, alphaInfo: alphaInfo,
                       bitmapInfo: bitmapInfo, origin: origin, provider: provider)
    }

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
