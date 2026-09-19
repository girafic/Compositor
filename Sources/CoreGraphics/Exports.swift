// A CoreGraphics replacement for Linux.
//
// The module is named `CoreGraphics` deliberately. The app has roughly 800 `context.*` call
// sites and 288 tests written against `CGContext`, `CGImage` and `CGPath`; taking the name
// turns the port from "touch every file" into "implement these members correctly". There is
// no system CoreGraphics on Linux to collide with — a toolchain probe confirmed that, along
// with the fact that a consumer's single `import CoreGraphics` sees both our declarations
// and everything re-exported below (see Spikes/README.md).
//
// What Foundation already provides on Linux, and we therefore must NOT redeclare:
//
//     CGFloat, CGPoint, CGSize, CGRect
//     the whole rect algebra — intersection, union, insetBy, offsetBy, isNull, isEmpty,
//     integral, standardized, contains, intersects, .zero, .null
//
// What it does not provide, and this module supplies:
//
//     CGAffineTransform, and therefore CGRect.applying / CGPoint.applying
//     CGContext, CGImage, CGPath, CGColor, CGColorSpace, CGGradient, CGDataProvider
//     the enumerations below
//
// One trap worth naming: swift-corelibs-foundation does have `AffineTransform`, from
// `NSAffineTransform`. It is a different type with different conventions and is not a
// substitute for the 26 `inverted()` and 8 `concatenating()` call sites in the app.

@_exported import Foundation
