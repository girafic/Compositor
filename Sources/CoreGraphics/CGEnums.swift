import CCompositorRaster

/// Blend modes, with CoreGraphics' raw values.
///
/// The numbering is the contract between this type and the raster engine: `rawValue` is
/// handed straight to `raster_blend_*`. Only the cases the app actually reaches are
/// declared — a grep across the whole tree found no use of `sourceIn`, `sourceOut`,
/// `sourceAtop`, `destinationOver`, `destinationIn`, `destinationAtop`, `xor`, `plusDarker`
/// or `plusLighter`, so they would be dead code that the engine would have to answer for.
public enum CGBlendMode: Int32, Sendable, Hashable {
    case normal = 0
    case multiply = 1
    case screen = 2
    case overlay = 3
    case darken = 4
    case lighten = 5
    case colorDodge = 6
    case colorBurn = 7
    case softLight = 8
    case hardLight = 9
    case difference = 10
    case exclusion = 11
    case hue = 12
    case saturation = 13
    case color = 14
    case luminosity = 15
    case clear = 16
    case copy = 17
    case destinationOut = 23

    var raster: raster_blend { raster_blend(bitPattern: rawValue) }
}

public enum CGInterpolationQuality: Int32, Sendable, Hashable {
    case `default` = 0
    case none = 1
    case low = 2
    case medium = 4
    case high = 3
}

/// How a bitmap's alpha channel is arranged. The app only ever produces three of these.
public enum CGImageAlphaInfo: UInt32, Sendable, Hashable {
    case none = 0
    case premultipliedLast = 1
    case premultipliedFirst = 2
    case last = 3
    case first = 4
    case noneSkipLast = 5
    case noneSkipFirst = 6
    case alphaOnly = 7
}

/// Byte order and float flags, packed alongside a `CGImageAlphaInfo` in one word.
///
/// `CGContext.init` takes this as a bare `UInt32` while `CGImage.init` takes the struct.
/// That asymmetry is CoreGraphics' and is reproduced rather than tidied, because all 50
/// context call sites pass `.rawValue` expressions and all 7 image call sites wrap in
/// `CGBitmapInfo(rawValue:)`.
public struct CGBitmapInfo: OptionSet, Sendable, Hashable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let alphaInfoMask = CGBitmapInfo(rawValue: 0x1F)
    public static let byteOrderMask = CGBitmapInfo(rawValue: 0x7000)
    public static let byteOrderDefault = CGBitmapInfo(rawValue: 0)
    public static let byteOrder16Little = CGBitmapInfo(rawValue: 1 << 12)
    public static let byteOrder32Little = CGBitmapInfo(rawValue: 2 << 12)
    public static let byteOrder16Big = CGBitmapInfo(rawValue: 3 << 12)
    public static let byteOrder32Big = CGBitmapInfo(rawValue: 4 << 12)
    public static let floatComponents = CGBitmapInfo(rawValue: 1 << 8)

    /// The alpha arrangement packed into these flags, if it is one we recognise.
    public var alphaInfo: CGImageAlphaInfo? {
        CGImageAlphaInfo(rawValue: rawValue & CGBitmapInfo.alphaInfoMask.rawValue)
    }
}

public enum CGPathFillRule: Int32, Sendable, Hashable {
    case winding = 0
    case evenOdd = 1
}

public enum CGLineCap: Int32, Sendable, Hashable {
    case butt = 0
    case round = 1
    case square = 2
}

public enum CGLineJoin: Int32, Sendable, Hashable {
    case miter = 0
    case round = 1
    case bevel = 2
}

public enum CGColorRenderingIntent: Int32, Sendable, Hashable {
    case defaultIntent = 0
    case absoluteColorimetric = 1
    case relativeColorimetric = 2
    case perceptual = 3
    case saturation = 4
}

public enum CGColorSpaceModel: Int32, Sendable, Hashable {
    case unknown = -1
    case monochrome = 0
    case rgb = 1
    case cmyk = 2
    case lab = 3
    case deviceN = 4
    case indexed = 5
    case pattern = 6
}

public struct CGGradientDrawingOptions: OptionSet, Sendable, Hashable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let drawsBeforeStartLocation = CGGradientDrawingOptions(rawValue: 1 << 0)
    public static let drawsAfterEndLocation = CGGradientDrawingOptions(rawValue: 1 << 1)
}

public enum CGPathElementType: Int32, Sendable, Hashable {
    case moveToPoint = 0
    case addLineToPoint = 1
    case addQuadCurveToPoint = 2
    case addCurveToPoint = 3
    case closeSubpath = 4
}
