import Testing
import CCompositorRaster

/// Blend-mode numbering, mirroring the constants in `raster.h`.
///
/// Spelled out here rather than imported: Swift renames C enum cases by stripping common
/// prefixes, and the exact spelling it lands on is not worth discovering from a CI failure.
/// These values are also CGBlendMode's, which is the contract that matters — the Swift shell
/// hands `CGBlendMode.rawValue` straight across.
enum Blend {
    static let normal: raster_blend = 0
    static let multiply: raster_blend = 1
    static let screen: raster_blend = 2
    static let overlay: raster_blend = 3
    static let darken: raster_blend = 4
    static let lighten: raster_blend = 5
    static let colorDodge: raster_blend = 6
    static let colorBurn: raster_blend = 7
    static let softLight: raster_blend = 8
    static let hardLight: raster_blend = 9
    static let difference: raster_blend = 10
    static let exclusion: raster_blend = 11
    static let hue: raster_blend = 12
    static let saturation: raster_blend = 13
    static let color: raster_blend = 14
    static let luminosity: raster_blend = 15
    static let clear: raster_blend = 16
    static let copy: raster_blend = 17
    static let destinationOut: raster_blend = 23
}

/// Composites one premultiplied RGBA source pixel over one backdrop pixel.
func blended(_ backdrop: [UInt8], over source: [UInt8], _ mode: raster_blend,
             alpha: UInt8 = 255, coverage: UInt8 = 255) -> [UInt8] {
    var dst = backdrop
    source.withUnsafeBufferPointer { src in
        dst.withUnsafeMutableBufferPointer { out in
            raster_blend_rgba(out.baseAddress, src.baseAddress, mode, alpha, coverage)
        }
    }
    return dst
}

/// An opaque grey backdrop under an opaque grey source. With both alphas at 1 the composite
/// formula collapses to the blend function itself, so these are the blend functions.
func opaque(_ mode: raster_blend, backdrop: UInt8, source: UInt8) -> Int {
    Int(blended([backdrop, backdrop, backdrop, 255], over: [source, source, source, 255], mode)[0])
}

func blendedGray(_ backdrop: UInt8, over source: UInt8, _ mode: raster_blend,
                 alpha: UInt8 = 255, coverage: UInt8 = 255) -> Int {
    var dst = backdrop
    raster_blend_gray(&dst, source, mode, alpha, coverage)
    return Int(dst)
}

@Suite("separable blend modes")
struct SeparableBlendTests {
    // b = 128/255 = 0.501961, s = 64/255 = 0.250980, per PDF 32000-1 section 11.3.5.
    @Test("each mode matches the specification")
    func specValues() {
        #expect(opaque(Blend.normal, backdrop: 128, source: 64) == 64)
        #expect(opaque(Blend.multiply, backdrop: 128, source: 64) == 32)      // b*s
        #expect(opaque(Blend.screen, backdrop: 128, source: 64) == 160)       // b+s-bs
        #expect(opaque(Blend.darken, backdrop: 128, source: 64) == 64)        // min
        #expect(opaque(Blend.lighten, backdrop: 128, source: 64) == 128)      // max
        #expect(opaque(Blend.difference, backdrop: 128, source: 64) == 64)    // |b-s|
        #expect(opaque(Blend.exclusion, backdrop: 128, source: 64) == 128)    // b+s-2bs
        #expect(opaque(Blend.hardLight, backdrop: 128, source: 64) == 64)     // s<=.5 -> multiply(b, 2s)
        #expect(opaque(Blend.overlay, backdrop: 128, source: 64) == 65)       // hardLight with swapped arguments
        #expect(opaque(Blend.colorDodge, backdrop: 128, source: 64) == 171)   // b/(1-s)
        #expect(opaque(Blend.colorBurn, backdrop: 128, source: 64) == 0)      // 1-min(1,(1-b)/s)
        #expect(opaque(Blend.softLight, backdrop: 128, source: 64) == 96)     // b-(1-2s)b(1-b)
    }

    @Test("the identities the specification guarantees")
    func identities() {
        #expect(opaque(Blend.multiply, backdrop: 200, source: 255) == 200)
        #expect(opaque(Blend.multiply, backdrop: 200, source: 0) == 0)
        #expect(opaque(Blend.screen, backdrop: 200, source: 0) == 200)
        #expect(opaque(Blend.screen, backdrop: 200, source: 255) == 255)
        #expect(opaque(Blend.difference, backdrop: 200, source: 0) == 200)
        #expect(opaque(Blend.difference, backdrop: 200, source: 200) == 0)
        #expect(opaque(Blend.overlay, backdrop: 255, source: 100) == 255)
        // A half-strength source is the fixed point of both light modes.
        #expect(abs(opaque(Blend.softLight, backdrop: 200, source: 128) - 200) <= 1)
        #expect(abs(opaque(Blend.hardLight, backdrop: 200, source: 128) - 200) <= 1)
    }
}

@Suite("source-over compositing")
struct CompositingTests {
    @Test("a premultiplied source over nothing keeps its values")
    func overEmpty() {
        let result = blended([0, 0, 0, 0], over: [128, 64, 32, 128], Blend.normal)
        #expect(result == [128, 64, 32, 128])
    }

    @Test("half-alpha black over white is mid grey")
    func halfOverWhite() {
        let result = blended([255, 255, 255, 255], over: [0, 0, 0, 128], Blend.normal)
        #expect(abs(Int(result[0]) - 127) <= 1)
        #expect(result[3] == 255)
    }

    @Test("graphics-state alpha and clip coverage both scale the source")
    func alphaAndCoverage() {
        let opaqueBlack: [UInt8] = [0, 0, 0, 255]
        let viaAlpha = blended([255, 255, 255, 255], over: opaqueBlack, Blend.normal, alpha: 128)
        let viaCoverage = blended([255, 255, 255, 255], over: opaqueBlack, Blend.normal, coverage: 128)
        let viaSource = blended([255, 255, 255, 255], over: [0, 0, 0, 128], Blend.normal)
        #expect(viaAlpha == viaCoverage)
        #expect(viaAlpha == viaSource)
    }

    @Test("zero coverage leaves the backdrop untouched")
    func zeroCoverage() {
        let backdrop: [UInt8] = [10, 20, 30, 40]
        #expect(blended(backdrop, over: [255, 255, 255, 255], Blend.normal, coverage: 0) == backdrop)
    }
}

@Suite("Color Dodge and Color Burn respect source alpha")
struct DodgeBurnTests {
    // CoreGraphics gets these two wrong: it ignores how transparent the source is, so a soft
    // brush comes out with a hard edge. The macOS app detours through CoreImage to avoid it
    // (Attic/appkit/SeparableBlend.swift). Implementing them to specification means the
    // detour is deleted rather than reproduced -- so this is the test that licenses that
    // deletion, and it is deliberately the one place Linux output differs from macOS.
    @Test("a half-alpha dodge is partial, not saturated")
    func dodge() {
        // Premultiplied white at alpha 0.5 over an opaque mid grey.
        let result = blended([128, 128, 128, 255], over: [128, 128, 128, 128], Blend.colorDodge)
        #expect(abs(Int(result[0]) - 192) <= 1, "CoreGraphics would saturate this to 255")
        #expect(result[3] == 255)
    }

    @Test("a half-alpha burn is partial too")
    func burn() {
        let result = blended([128, 128, 128, 255], over: [0, 0, 0, 128], Blend.colorBurn)
        #expect(abs(Int(result[0]) - 64) <= 1)
    }
}

@Suite("non-separable blend modes")
struct NonSeparableBlendTests {
    @Test("luminosity takes the source's lightness and the backdrop's colour")
    func luminosity() {
        // Lum(grey) = 0.501961, Lum(red) = 0.30, so SetLum adds 0.201961 to every channel,
        // giving (1.201961, 0.201961, 0.201961). That overshoots, so ClipColor scales the
        // colour towards L by (1-L)/(x-L) = 0.711484 -- which pulls red back to exactly 1.0
        // and *raises* green and blue to 0.288516 rather than leaving them where they were.
        let result = blended([255, 0, 0, 255], over: [128, 128, 128, 255], Blend.luminosity)
        #expect(result[0] == 255)
        #expect(abs(Int(result[1]) - 74) <= 1)
        #expect(abs(Int(result[2]) - 74) <= 1)
    }

    @Test("every non-separable mode is a no-op against itself",
          arguments: [Blend.hue, Blend.saturation, Blend.color, Blend.luminosity])
    func selfIsIdentity(mode: raster_blend) {
        let colour: [UInt8] = [200, 100, 50, 255]
        let result = blended(colour, over: colour, mode)
        for channel in 0..<3 {
            #expect(abs(Int(result[channel]) - Int(colour[channel])) <= 1, "channel \(channel)")
        }
    }
}

@Suite("the Porter-Duff modes the app reaches")
struct PorterDuffTests {
    @Test("copy replaces alpha, so a transparent source erases")
    func copyErases() {
        #expect(blended([200, 100, 50, 255], over: [0, 0, 0, 0], Blend.copy)[3] == 0)
    }

    @Test("copy is weighted by the clip rather than an unconditional store")
    func copyIsClipWeighted() {
        // Clone Stamp draws in this mode through a soft coverage clip and depends on it.
        let result = blended([200, 100, 50, 255], over: [0, 0, 0, 0], Blend.copy, coverage: 128)
        #expect(abs(Int(result[0]) - 100) <= 1)
        #expect(abs(Int(result[3]) - 128) <= 1)
    }

    @Test("clear zeroes the pixel")
    func clear() {
        #expect(blended([200, 100, 50, 255], over: [0, 0, 0, 0], Blend.clear) == [0, 0, 0, 0])
    }

    @Test("destinationOut erases in proportion to the source's alpha")
    func destinationOut() {
        #expect(blended([200, 100, 50, 255], over: [255, 255, 255, 255], Blend.destinationOut)[3] == 0)
        let half = blended([200, 100, 50, 255], over: [128, 128, 128, 128], Blend.destinationOut)
        #expect(abs(Int(half[3]) - 128) <= 1)
        #expect(abs(Int(half[0]) - 100) <= 1, "premultiplied colour scales with alpha")
    }
}

@Suite("GRAY8 is opaque, so compositing is a lerp")
struct GrayBlendTests {
    @Test("source-over lerps towards the source")
    func lerp() {
        #expect(blendedGray(0, over: 255, Blend.normal) == 255)
        #expect(abs(blendedGray(0, over: 255, Blend.normal, coverage: 128) - 128) <= 1)
    }

    @Test("lighten keeps the brighter sample — the hard-tip brush path")
    func lighten() {
        #expect(blendedGray(200, over: 100, Blend.lighten) == 200)
        #expect(blendedGray(100, over: 200, Blend.lighten) == 200)
    }

    @Test("screen accumulates coverage — the soft-tip brush path")
    func screen() {
        // cov = 1 - (1-cov)(1-dab)
        #expect(abs(blendedGray(128, over: 128, Blend.screen) - 192) <= 1)
    }

    @Test("multiply combines two masks: what either hides stays hidden")
    func multiply() {
        #expect(blendedGray(255, over: 128, Blend.multiply) == 128)
        #expect(blendedGray(0, over: 255, Blend.multiply) == 0)
    }

    @Test("clearing a gray plane writes black, not transparency")
    func clearIsBlack() {
        #expect(blendedGray(200, over: 0, Blend.clear) == 0)
    }
}

@Suite("unsupported modes are rejected")
struct SupportedModeTests {
    @Test("the modes the app uses are supported",
          arguments: [Blend.normal, Blend.multiply, Blend.luminosity, Blend.clear,
                      Blend.copy, Blend.destinationOut])
    func supported(mode: raster_blend) {
        #expect(raster_blend_supported(mode))
    }

    @Test("the Porter-Duff modes the app never reaches are not",
          arguments: [18, 19, 20, 21, 22, 24, 25, 26, 27] as [raster_blend])
    func unsupported(mode: raster_blend) {
        #expect(!raster_blend_supported(mode))
    }
}
