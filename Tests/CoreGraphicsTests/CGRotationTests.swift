import Testing
import CoreGraphics

// The parallelogram arithmetic is proved in C — Spikes/context-oracle.c runs ten million
// checks, including a partition counter built the way TiledLayerRenderer builds its tiles, and
// every claim here about blur or aliasing was measured rather than reasoned out. These cover
// what C cannot see: that the app's own transform sequences work, under angles the app actually
// produces, and that a singular matrix answers instead of taking the process down.
//
// 37 and 25 degrees throughout, never 90. A quarter turn is *rectilinear* — cos(pi/2) is
// 6.1e-17, which the tolerance forgives — so it takes the exact integer path and exercises none
// of this. The app's own rotated cases are 25 degrees in TiledLayerTests and 37 in BrushTests.

/// The transform `LayerRenderer.draw` installs: translate to the centre, rotate, then flip.
/// Reproduced rather than paraphrased, because the sequence is what has to work.
func installLayerTransform(_ context: CGContext, degrees: CGFloat, centre: CGPoint,
                           flipX: Bool = false, flipY: Bool = false) {
    context.translateBy(x: centre.x, y: centre.y)
    context.rotate(by: degrees * .pi / 180)
    context.scaleBy(x: flipX ? -1 : 1, y: flipY ? -1 : 1)
}

/// Every painted pixel's coordinates.
func paintedPixels(_ context: CGContext) -> [(x: Int, y: Int)] {
    var found: [(x: Int, y: Int)] = []
    for y in 0..<context.height {
        for x in 0..<context.width where pixel(context, x, y).contains(where: { $0 != 0 }) {
            found.append((x, y))
        }
    }
    return found
}

@Suite("CGContext: rotation and shear")
struct CGRotationTests {
    @Test("a rotated flipped layer fills its own area and stays centred")
    func rotatedLayerFill() throws {
        // LayerRenderer.draw's preamble, then a fill of the centred bounds it builds.
        let context = try makeContext(24, 24, mask: true)
        installLayerTransform(context, degrees: 37, centre: CGPoint(x: 12, y: 12), flipX: true)
        context.setShouldAntialias(false)
        context.setFillColor(gray: 1, alpha: 1)
        let bounds = CGRect(x: -7, y: -5, width: 14, height: 10)
        context.fill(bounds)

        let painted = paintedPixels(context)
        // A hard centre rule tiles the plane, so the count of pixel centres inside a shape is
        // its area. Measured at exactly 140 for this fixture, which is 14 x 10 — the tolerance
        // is for a last-bit difference in cos(37 degrees) across libm versions, not for slack
        // in the rule. A bounding-box fill would paint 256 and a dropped region far fewer.
        #expect(abs(painted.count - 140) <= 2,
                "a rotated fill covers the rectangle's area, not its bounding box")

        // Symmetric about the centre: the shape is centred on the origin, so its image must be
        // centred on (12, 12). This is what catches a transform composed in the wrong order.
        let xs = painted.map(\.x), ys = painted.map(\.y)
        let minX = try #require(xs.min()), maxX = try #require(xs.max())
        let minY = try #require(ys.min()), maxY = try #require(ys.max())
        #expect(abs((12 - minX) - (maxX - 12)) <= 1, "centred horizontally")
        #expect(abs((12 - minY) - (maxY - 12)) <= 1, "centred vertically")
        // The half-diagonal of a 14x10 rectangle is 8.6, so nothing can reach the border.
        #expect(minX >= 3 && maxX <= 20 && minY >= 3 && maxY <= 20)
    }

    @Test("a hard rotated clip covers exactly the pixels the same fill would")
    func clipEqualsFill() throws {
        // The invariance the tiled renderer rests on, stated at the public API. No constants:
        // the two renders are compared against each other.
        let rect = CGRect(x: 3.5, y: 5.25, width: 13, height: 10.5)
        var renders: [[UInt8]] = []
        for clipping in [false, true] {
            let context = try makeContext(24, 24, mask: true)
            installLayerTransform(context, degrees: 25, centre: CGPoint(x: 0, y: 0))
            context.setShouldAntialias(false)
            context.setFillColor(gray: 1, alpha: 1)
            if clipping {
                context.clip(to: rect)
                context.fill(CGRect(x: -200, y: -200, width: 600, height: 600))
            } else {
                context.fill(rect)
            }
            renders.append(bytes(context))
        }
        #expect(renders[0] == renders[1],
                "clipping to a rotated rectangle and filling everything equals filling it")
    }

    @Test("a rotated clip honours antialiasing where a rectilinear one ignores it")
    func clipHonoursAntialiasing() throws {
        // Under rotation the flag is never invisible, and a hard clip around an antialiased
        // draw throws the smoothing away again — worst at 45 degrees, on every masked rotated
        // layer. TiledLayerRenderer turns antialiasing off exactly where it needs neighbouring
        // pieces to meet, so honouring the flag serves both.
        var partialCounts: [Int] = []
        for soft in [false, true] {
            let context = try makeContext(20, 20, mask: true)
            installLayerTransform(context, degrees: 30, centre: CGPoint(x: 10, y: 10))
            context.setShouldAntialias(soft)
            context.clip(to: CGRect(x: -6, y: -6, width: 12, height: 12))
            context.setShouldAntialias(false)   // keep the fill's own edge out of it
            context.setFillColor(gray: 1, alpha: 1)
            context.fill(CGRect(x: -200, y: -200, width: 600, height: 600))
            partialCounts.append((0..<20).flatMap { y in (0..<20).map { pixel(context, $0, y)[0] } }
                                          .filter { $0 != 0 && $0 != 255 }.count)
        }
        #expect(partialCounts[0] == 0, "antialiasing off gives hard edges")
        #expect(partialCounts[1] > 0, "antialiasing on gives soft ones")
    }

    @Test("rotated clips nest, and compose with a mask clip beneath them")
    func nestedRotatedClips() throws {
        // Five *different* angles, because identical shapes cannot test this: intersecting a
        // shape with itself is idempotent, which is how three separate defects survived a
        // six-deep nest of one shape in C. Five is also deep enough to force the fold that runs
        // when the clip's inline list of shapes overflows.
        let mask = try makeMask([[200, 200], [200, 200]])
        let context = try makeContext(28, 28, mask: true)
        context.clip(to: CGRect(x: 0, y: 0, width: 28, height: 28), mask: mask)

        for degrees in [11.0, 29.0, 47.0, 66.0, 83.0] as [CGFloat] {
            // Undo the transform by multiplying it back out, not with restoreGState — that
            // would discard the clip along with the rotation. This is `FolderMaskClip.apply`'s
            // own idiom, which concatenates a placement and then its inverse for exactly this
            // reason.
            let placement = CGAffineTransform(translationX: 14, y: 14)
                .rotated(by: degrees * .pi / 180)
            context.concatenate(placement)
            context.setShouldAntialias(false)
            context.clip(to: CGRect(x: -9, y: -8, width: 18, height: 16))
            context.concatenate(placement.inverted())
        }

        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: 28, height: 28))

        // Every painted pixel carries the mask's 200, and the region has shrunk to the
        // intersection of five rotated rectangles — smaller than any one of them.
        let painted = paintedPixels(context)
        #expect(!painted.isEmpty, "five rotated clips still leave something")
        #expect(painted.allSatisfy { pixel(context, $0.x, $0.y) == [200] },
                "the mask beneath the nest survives the fold")
        #expect(painted.count < 18 * 16, "and the intersection is smaller than one rectangle")
    }

    @Test("a singular transform paints nothing instead of trapping")
    func singularTransform() throws {
        // Reachable, not theoretical: CGAffineTransform.inverted() returns a singular matrix
        // unchanged and BrushStroke concatenates the result. A trap here would take the whole
        // test process down, so this test passing at all is most of the point.
        let mask = try makeMask([[255]])
        let source = try makeContext(4, 4, mask: true)
        source.setFillColor(gray: 1, alpha: 1)
        source.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        let image = try #require(source.makeImage())

        let context = try makeContext(12, 12, mask: true)
        // Determinant 1*1 - 0.5*2 = 0, and not rectilinear either.
        context.concatenate(CGAffineTransform(a: 1, b: 0.5, c: 2, d: 1, tx: 0, ty: 0))
        context.setFillColor(gray: 1, alpha: 1)
        let all = CGRect(x: 0, y: 0, width: 12, height: 12)
        context.fill(all)
        context.draw(image, in: all)
        context.clip(to: all, mask: mask)
        context.clip(to: all)
        context.fill(all)

        #expect(paintedPixels(context).isEmpty, "a collapsed transform covers nothing")
        #expect(context.boundingBoxOfClipPath.isNull, "and leaves the clip empty")
    }

    @Test("a sheared draw follows the shear rather than its bounding box")
    func shearedDraw() throws {
        // Shear is reachable without any rotation at all: ImageResizer applies a non-uniform
        // scale outside LayerRenderer's rotation, and says so in its own comment.
        let source = try makeContext(8, 8, mask: true)
        source.setFillColor(gray: 1, alpha: 1)
        source.fill(CGRect(x: 0, y: 0, width: 8, height: 8))
        let image = try #require(source.makeImage())

        let context = try makeRawContext(24, 24, mask: true)
        context.concatenate(CGAffineTransform(a: 1, b: 0, c: 0.5, d: 1, tx: 0, ty: 0))
        context.interpolationQuality = .none
        context.setShouldAntialias(false)
        context.setBlendMode(.copy)
        context.draw(image, in: CGRect(x: 2, y: 2, width: 8, height: 8))

        // Each row's painted run is offset from the one above by the shear. A bounding-box draw
        // would start every row at the same column. The rows are found rather than assumed —
        // the shear moves the shape well away from the rectangle's own coordinates, and a test
        // that guessed them would pass vacuously on an empty canvas.
        let painted = paintedPixels(context)
        #expect(!painted.isEmpty, "the sheared image lands somewhere")
        let rows = Set(painted.map(\.y)).sorted()
        let firstColumns = rows.map { row in paintedColumns(context, row: row).first ?? -1 }
        #expect(rows.count >= 6, "and covers several rows")
        #expect(try #require(firstColumns.first) != try #require(firstColumns.last),
                "and each row starts at a different column than the last")
        // Monotone, not merely different: a shear slides every row the same way. Decreasing
        // here because a bitmap context's base flip reverses the direction.
        #expect(zip(firstColumns, firstColumns.dropFirst()).allSatisfy { $0 >= $1 },
                "and the offset moves one way down the shape")
    }

    @Test("a quarter turn is still an exact permutation")
    func quarterTurnStaysExact() throws {
        // The guard on the fast-path selector. A quarter turn is rectilinear by the tolerance,
        // so it must keep taking the integer path: LayerMaskTests requires a 90-degree nearest
        // render to yield exactly the source's values, with no interpolated intermediates.
        let rows: [[UInt8]] = (0..<4).map { y in (0..<4).map { x in UInt8(17 + x * 40 + y * 7) } }
        let mask = try makeMask(rows)

        let context = try makeRawContext(4, 4, mask: true)
        context.translateBy(x: 2, y: 2)
        context.rotate(by: .pi / 2)
        context.translateBy(x: -2, y: -2)
        context.interpolationQuality = .none
        context.setShouldAntialias(false)
        context.setBlendMode(.copy)
        context.draw(mask, in: CGRect(x: 0, y: 0, width: 4, height: 4))

        let got = (0..<4).flatMap { y in (0..<4).map { pixel(context, $0, y)[0] } }.sorted()
        #expect(got == rows.flatMap { $0 }.sorted(),
                "every source value appears once, with nothing invented or lost")
    }

    @Test("the clip's bounds under rotation contain the shape and stay finite")
    func boundsUnderRotation() throws {
        let context = try makeContext(32, 32, mask: true)
        installLayerTransform(context, degrees: 37, centre: CGPoint(x: 16, y: 16))
        let rect = CGRect(x: -8, y: -6, width: 16, height: 12)
        context.clip(to: rect)

        let box = context.boundingBoxOfClipPath
        #expect(!box.isNull && !box.isInfinite, "a rotated clip is not an empty one")
        #expect(box.width.isFinite && box.height.isFinite)
        // A bound, not a tight fit: mapping device bounds back through a rotation gives the box
        // of a box, measured here as −14…14 for a rectangle spanning −8…8. It must still
        // contain what was clipped to, and every consumer either sizes a buffer from it, insets
        // it, or culls with it — so larger costs work and never loses pixels.
        #expect(box.contains(rect), "and it contains the rectangle it was built from")
        #expect(box.width > rect.width, "and is looser than it, because rotation makes it so")
    }
}
