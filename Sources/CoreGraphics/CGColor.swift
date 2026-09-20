import CCompositorRaster

/// A colour space.
///
/// Only the two the app actually uses exist as instances, and they are interned, because
/// several call sites compare them: `LayerMask` and `DownsampleCache` branch on
/// `colorSpace?.model == .monochrome` to tell a mask apart from an image, and the export
/// tests compare `colorSpace?.name == CGColorSpace.sRGB`.
public final class CGColorSpace: @unchecked Sendable {
    /// CoreGraphics identifies named spaces by `CFString` constants. A wrapper type keeps
    /// `CGColorSpace(name: .sRGB)` reading the same here as it does on Darwin while staying
    /// comparable and hashable.
    public struct Name: Hashable, Sendable {
        let raw: String
        init(_ raw: String) { self.raw = raw }
    }

    public static let sRGB = Name("kCGColorSpaceSRGB")
    public static let genericGray = Name("kCGColorSpaceGenericGray")
    public static let displayP3 = Name("kCGColorSpaceDisplayP3")

    public let name: Name?
    public let model: CGColorSpaceModel
    public let numberOfComponents: Int

    private init(name: Name?, model: CGColorSpaceModel, numberOfComponents: Int) {
        self.name = name
        self.model = model
        self.numberOfComponents = numberOfComponents
    }

    static let deviceGray = CGColorSpace(name: genericGray, model: .monochrome, numberOfComponents: 1)
    static let deviceRGB = CGColorSpace(name: sRGB, model: .rgb, numberOfComponents: 3)

    /// Returns nil for a space the engine cannot actually honour.
    ///
    /// Display P3 is the one that matters: it appears once in the app, and quietly handing
    /// back sRGB instead would be a colour bug that nothing downstream could detect. Failing
    /// is the same choice `raster_blend_supported` makes for an unreachable blend mode.
    public init?(name: Name) {
        switch name {
        case CGColorSpace.sRGB:
            self.name = name; self.model = .rgb; self.numberOfComponents = 3
        case CGColorSpace.genericGray:
            self.name = name; self.model = .monochrome; self.numberOfComponents = 1
        default:
            return nil
        }
    }
}

public func CGColorSpaceCreateDeviceGray() -> CGColorSpace { .deviceGray }
public func CGColorSpaceCreateDeviceRGB() -> CGColorSpace { .deviceRGB }

/// A colour, as components in a space plus alpha.
///
/// The app builds these nine times and only ever reads them back through `setFillColor`, so
/// this is deliberately a value holder rather than a colour-management type. Conversion
/// between models happens where the colour meets a surface, in `CGContext`.
public final class CGColor: @unchecked Sendable {
    public let colorSpace: CGColorSpace
    /// The space's own components followed by alpha, so `numberOfComponents + 1` of them.
    public let components: [CGFloat]

    public var numberOfComponents: Int { components.count }
    public var alpha: CGFloat { components.last ?? 1 }

    /// Fails when the component count does not match the space, which is the check that
    /// makes the two-component grey in `Gradient` a real case rather than a silent misread.
    public init?(colorSpace: CGColorSpace, components: [CGFloat]) {
        guard components.count == colorSpace.numberOfComponents + 1 else { return nil }
        self.colorSpace = colorSpace
        self.components = components
    }

    public init(srgbRed red: CGFloat, green: CGFloat, blue: CGFloat, alpha: CGFloat) {
        self.colorSpace = .deviceRGB
        self.components = [red, green, blue, alpha]
    }

    public init(gray: CGFloat, alpha: CGFloat) {
        self.colorSpace = .deviceGray
        self.components = [gray, alpha]
    }

    /// Unpremultiplied RGBA, which is what the engine's fill colour wants. A grey resolves
    /// to equal channels; an RGB colour reaching a grey surface is collapsed by `CGContext`,
    /// not here, because only the destination knows which way to go.
    var rgba: (Double, Double, Double, Double) {
        let values = components.map(Double.init)
        switch colorSpace.model {
        case .monochrome:
            let grey = values.first ?? 0
            return (grey, grey, grey, values.count > 1 ? values[1] : 1)
        default:
            return (values.count > 0 ? values[0] : 0,
                    values.count > 1 ? values[1] : 0,
                    values.count > 2 ? values[2] : 0,
                    values.count > 3 ? values[3] : 1)
        }
    }
}
