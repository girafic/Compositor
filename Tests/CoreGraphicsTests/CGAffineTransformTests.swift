import Testing
import CoreGraphics

@Suite("CGAffineTransform construction")
struct TransformConstructionTests {
    @Test("identity leaves points alone")
    func identity() {
        #expect(CGAffineTransform.identity.isIdentity)
        #expect(CGAffineTransform().isIdentity)
        let point = CGPoint(x: 3, y: -7)
        #expect(point.applying(.identity) == point)
    }

    @Test("translation moves, scale multiplies")
    func translationAndScale() {
        let moved = CGPoint(x: 1, y: 2).applying(CGAffineTransform(translationX: 10, y: 20))
        #expect(moved == CGPoint(x: 11, y: 22))

        let scaled = CGPoint(x: 3, y: 4).applying(CGAffineTransform(scaleX: 2, y: -1))
        #expect(scaled == CGPoint(x: 6, y: -4))
    }

    @Test("a quarter turn sends the x-axis to the y-axis")
    func rotation() {
        // Positive angles run from +x towards +y, which in the app's y-down document space
        // reads as clockwise on screen — the convention LayerTransform documents.
        let turned = CGPoint(x: 1, y: 0).applying(CGAffineTransform(rotationAngle: .pi / 2))
        #expect(abs(turned.x) < 1e-12)
        #expect(abs(turned.y - 1) < 1e-12)
    }

    @Test("the explicit matrix initialiser maps as a·x + c·y + tx")
    func explicitMatrix() {
        // LayerFlip builds its mirrors this way.
        let mirror = CGAffineTransform(a: -1, b: 0, c: 0, d: 1, tx: 500, ty: 0)
        #expect(CGPoint(x: 0, y: 10).applying(mirror) == CGPoint(x: 500, y: 10))
        #expect(CGPoint(x: 500, y: 10).applying(mirror) == CGPoint(x: 0, y: 10))
    }
}

@Suite("CGAffineTransform composition order")
struct TransformCompositionTests {
    // Getting these backwards compiles cleanly and breaks every layer placement, so each
    // direction is pinned separately.

    @Test("concatenating applies the receiver first, then the argument")
    func concatenatingIsPostOrder() {
        let scale = CGAffineTransform(scaleX: 2, y: 2)
        let move = CGAffineTransform(translationX: 10, y: 0)

        // Scale first: (1,0) -> (2,0) -> (12,0)
        #expect(CGPoint(x: 1, y: 0).applying(scale.concatenating(move)) == CGPoint(x: 12, y: 0))
        // Move first: (1,0) -> (11,0) -> (22,0)
        #expect(CGPoint(x: 1, y: 0).applying(move.concatenating(scale)) == CGPoint(x: 22, y: 0))
    }

    @Test("translatedBy, scaledBy and rotated all pre-concatenate")
    func modifiersArePreOrder() {
        let base = CGAffineTransform(translationX: 10, y: 0)

        // The translation is applied to the point *before* the receiver, so this is
        // move-by-5 then move-by-10.
        #expect(CGPoint(x: 0, y: 0).applying(base.translatedBy(x: 5, y: 0)) == CGPoint(x: 15, y: 0))

        // Scale before translate: (1,0) -> (2,0) -> (12,0).
        #expect(CGPoint(x: 1, y: 0).applying(base.scaledBy(x: 2, y: 2)) == CGPoint(x: 12, y: 0))

        // Each modifier is exactly its own transform concatenated onto the receiver.
        #expect(base.scaledBy(x: 2, y: 3) == CGAffineTransform(scaleX: 2, y: 3).concatenating(base))
        #expect(base.rotated(by: 0.4) == CGAffineTransform(rotationAngle: 0.4).concatenating(base))
    }

    @Test("BrushRaster.pixelToDocument places a layer's grid on the document")
    func pixelToDocument() {
        // The canonical chain from BrushStroke.swift:77-83. It maps layer pixel space
        // (y-down, origin at the grid's top-left) onto document space (also y-down), so
        // pixel (0,0) must land exactly on the layer's origin and (width,height) on its
        // opposite corner. The whole renderer's placement rests on this.
        let origin = CGPoint(x: 100, y: 50)
        let size = CGSize(width: 200, height: 100)
        let width = 200.0, height = 100.0
        let center = CGPoint(x: origin.x + size.width / 2, y: origin.y + size.height / 2)

        let pixelToDocument = CGAffineTransform(translationX: center.x, y: center.y)
            .rotated(by: 0)
            .scaledBy(x: size.width / width, y: size.height / height)
            .translatedBy(x: -width / 2, y: -height / 2)

        #expect(CGPoint(x: 0, y: 0).applying(pixelToDocument) == origin)
        #expect(CGPoint(x: width, y: height).applying(pixelToDocument)
                == CGPoint(x: origin.x + size.width, y: origin.y + size.height))
    }

    @Test("a flipped layer mirrors within its own bounds")
    func pixelToDocumentFlipped() {
        let size = CGSize(width: 200, height: 100)
        let center = CGPoint(x: 100, y: 50)
        let flipped = CGAffineTransform(translationX: center.x, y: center.y)
            .rotated(by: 0)
            .scaledBy(x: -size.width / 200, y: size.height / 100)
            .translatedBy(x: -100, y: -50)

        // With flipX the grid's left edge lands on the layer's right edge.
        #expect(CGPoint(x: 0, y: 0).applying(flipped) == CGPoint(x: 200, y: 0))
        #expect(CGPoint(x: 200, y: 100).applying(flipped) == CGPoint(x: 0, y: 100))
    }
}

@Suite("CGAffineTransform inversion")
struct TransformInversionTests {
    @Test("inverting round-trips a point")
    func roundTrip() {
        let transform = CGAffineTransform(translationX: 30, y: -12)
            .rotated(by: 0.7)
            .scaledBy(x: 2, y: 0.5)
        let point = CGPoint(x: 13, y: -4)
        let back = point.applying(transform).applying(transform.inverted())
        #expect(abs(back.x - point.x) < 1e-9)
        #expect(abs(back.y - point.y) < 1e-9)
    }

    @Test("a transform times its inverse is the identity")
    func inverseIsIdentity() {
        let transform = CGAffineTransform(a: 3, b: 1, c: -2, d: 4, tx: 7, ty: -5)
        let product = transform.concatenating(transform.inverted())
        for (got, want) in [(product.a, 1.0), (product.b, 0.0), (product.c, 0.0),
                            (product.d, 1.0), (product.tx, 0.0), (product.ty, 0.0)] {
            #expect(abs(got - want) < 1e-9)
        }
    }

    @Test("a singular transform inverts to itself rather than trapping")
    func singular() {
        // TiledLayerRenderer.snapped calls inverted() on whatever context transform it is
        // handed, so this has to be total.
        let collapsed = CGAffineTransform(scaleX: 0, y: 0)
        #expect(collapsed.inverted() == collapsed)
    }
}

@Suite("applying a transform to a rect")
struct RectApplyingTests {
    @Test("translation and scale move the rect")
    func simple() {
        let rect = CGRect(x: 10, y: 20, width: 30, height: 40)
        #expect(rect.applying(CGAffineTransform(translationX: 5, y: -5))
                == CGRect(x: 15, y: 15, width: 30, height: 40))
        #expect(rect.applying(CGAffineTransform(scaleX: 2, y: 2))
                == CGRect(x: 20, y: 40, width: 60, height: 80))
    }

    @Test("a negative scale keeps the rect standardised")
    func negativeScale() {
        let rect = CGRect(x: 10, y: 0, width: 30, height: 10)
        let mirrored = rect.applying(CGAffineTransform(scaleX: -1, y: 1))
        #expect(mirrored == CGRect(x: -40, y: 0, width: 30, height: 10))
    }

    @Test("rotation yields the bounding box, which is larger")
    func rotation() {
        // A unit square turned 45 degrees has a bounding box of side sqrt(2).
        let square = CGRect(x: -0.5, y: -0.5, width: 1, height: 1)
        let box = square.applying(CGAffineTransform(rotationAngle: .pi / 4))
        #expect(abs(box.width - 2.0.squareRoot()) < 1e-9)
        #expect(abs(box.height - 2.0.squareRoot()) < 1e-9)
        #expect(abs(box.midX) < 1e-9)
    }

    @Test("the null rect is left alone")
    func null() {
        #expect(CGRect.null.applying(CGAffineTransform(translationX: 5, y: 5)).isNull)
    }
}

@Suite("enumeration raw values match CoreGraphics")
struct EnumRawValueTests {
    // These numbers are the contract between CGBlendMode and the raster engine: rawValue is
    // handed straight to raster_blend_*. They are also CoreGraphics' own values, so a
    // project file's stored blend mode keeps its meaning.
    @Test("blend modes are numbered as CoreGraphics numbers them")
    func blendModes() {
        #expect(CGBlendMode.normal.rawValue == 0)
        #expect(CGBlendMode.multiply.rawValue == 1)
        #expect(CGBlendMode.screen.rawValue == 2)
        #expect(CGBlendMode.overlay.rawValue == 3)
        #expect(CGBlendMode.darken.rawValue == 4)
        #expect(CGBlendMode.lighten.rawValue == 5)
        #expect(CGBlendMode.colorDodge.rawValue == 6)
        #expect(CGBlendMode.colorBurn.rawValue == 7)
        #expect(CGBlendMode.difference.rawValue == 10)
        #expect(CGBlendMode.hue.rawValue == 12)
        #expect(CGBlendMode.saturation.rawValue == 13)
        #expect(CGBlendMode.color.rawValue == 14)
        #expect(CGBlendMode.luminosity.rawValue == 15)
        #expect(CGBlendMode.clear.rawValue == 16)
        #expect(CGBlendMode.copy.rawValue == 17)
        #expect(CGBlendMode.destinationOut.rawValue == 23)
    }

    @Test("the bitmap layout the whole app uses packs as CoreGraphics packs it")
    func bitmapInfo() {
        // BrushRaster.context builds exactly this for colour, and plain .none for masks.
        let colour = CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
        #expect(colour == 1 | (4 << 12))
        #expect(CGBitmapInfo(rawValue: colour).alphaInfo == .premultipliedLast)
        #expect(CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue).alphaInfo == CGImageAlphaInfo.none)
    }

    @Test("gradient options combine as an option set")
    func gradientOptions() {
        let both: CGGradientDrawingOptions = [.drawsBeforeStartLocation, .drawsAfterEndLocation]
        #expect(both.contains(.drawsBeforeStartLocation))
        #expect(both.contains(.drawsAfterEndLocation))
        #expect(!CGGradientDrawingOptions([.drawsBeforeStartLocation]).contains(.drawsAfterEndLocation))
    }
}
