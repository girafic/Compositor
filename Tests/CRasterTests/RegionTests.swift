import Testing
import CCompositorRaster

// Regions are checked against a brute-force bitmap rather than against hand-written expected
// rectangle lists. The rectangle list is an implementation detail of the canonical form; what
// the clip stack actually promises is a set of pixels, so that is what gets compared. The
// canonical form is then checked separately, as a structural property, because it is what
// makes `raster_region_bounds` exact and what lets two clips covering the same pixels compare
// equal rectangle-for-rectangle.

// MARK: - Support

/// Owns a `raster_region *`. Manual destroy calls and `#expect` do not mix: the first failing
/// expectation in a test would leak every region allocated after it.
final class Region {
    let handle: OpaquePointer

    init(_ handle: OpaquePointer) { self.handle = handle }

    convenience init() { self.init(raster_region_create()!) }

    convenience init(_ rect: raster_rect) { self.init(raster_region_create_rect(rect)!) }

    deinit { raster_region_destroy(handle) }

    var count: Int { raster_region_count(handle) }
    var isEmpty: Bool { raster_region_is_empty(handle) }
    var bounds: raster_rect { raster_region_bounds(handle) }

    subscript(index: Int) -> raster_rect { raster_region_rect(handle, index) }

    var rects: [raster_rect] { (0..<count).map { self[$0] } }

    func contains(_ x: Int32, _ y: Int32) -> Bool { raster_region_contains(handle, x, y) }

    func copy() -> Region { Region(raster_region_copy(handle)!) }

    func union(_ other: Region) -> Region { Region(raster_region_union(handle, other.handle)!) }
    func intersect(_ other: Region) -> Region { Region(raster_region_intersect(handle, other.handle)!) }
    func subtract(_ other: Region) -> Region { Region(raster_region_subtract(handle, other.handle)!) }
    func xor(_ other: Region) -> Region { Region(raster_region_xor(handle, other.handle)!) }

    func intersecting(_ rect: raster_rect) -> Region {
        Region(raster_region_intersect_rect(handle, rect)!)
    }
}

func rect(_ x0: Int32, _ y0: Int32, _ x1: Int32, _ y1: Int32) -> raster_rect {
    raster_rect(x0: x0, y0: y0, x1: x1, y1: y1)
}

extension raster_rect: @retroactive Equatable {
    public static func == (a: raster_rect, b: raster_rect) -> Bool {
        a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1
    }
}

/// The oracle: a plain grid of booleans, with every set operation written out the obvious way.
struct Bitmap {
    static let size: Int32 = 32
    private var on = [Bool](repeating: false, count: Int(size * size))

    subscript(x: Int32, y: Int32) -> Bool {
        get {
            guard x >= 0, x < Self.size, y >= 0, y < Self.size else { return false }
            return on[Int(y * Self.size + x)]
        }
        set {
            guard x >= 0, x < Self.size, y >= 0, y < Self.size else { return }
            on[Int(y * Self.size + x)] = newValue
        }
    }

    mutating func add(_ r: raster_rect) {
        for y in r.y0..<max(r.y0, r.y1) {
            for x in r.x0..<max(r.x0, r.x1) { self[x, y] = true }
        }
    }

    mutating func toggle(_ r: raster_rect) {
        for y in r.y0..<max(r.y0, r.y1) {
            for x in r.x0..<max(r.x0, r.x1) { self[x, y] = !self[x, y] }
        }
    }

    func combined(_ other: Bitmap, _ op: (Bool, Bool) -> Bool) -> Bitmap {
        var result = Bitmap()
        for y in 0..<Self.size {
            for x in 0..<Self.size { result[x, y] = op(self[x, y], other[x, y]) }
        }
        return result
    }

    /// The tight bounding box of the set pixels, all zeroes when there are none.
    var bounds: raster_rect {
        var found = false
        var box = rect(0, 0, 0, 0)
        for y in 0..<Self.size {
            for x in 0..<Self.size where self[x, y] {
                if !found {
                    box = rect(x, y, x + 1, y + 1)
                    found = true
                } else {
                    box.x0 = min(box.x0, x)
                    box.y0 = min(box.y0, y)
                    box.x1 = max(box.x1, x + 1)
                    box.y1 = max(box.y1, y + 1)
                }
            }
        }
        return box
    }

    var isEmpty: Bool {
        !on.contains(true)
    }

    /// Prints the grid, so a failure shows the shapes rather than a rectangle count.
    var drawn: String {
        (0..<Self.size).map { y in
            String((0..<Self.size).map { x in self[x, y] ? "#" : "." })
        }.joined(separator: "\n")
    }
}

/// Renders a region by walking its rectangles, so a mismatch is a disagreement about pixels
/// and not about how the pixels were chopped up.
func rendered(_ region: Region) -> Bitmap {
    var bitmap = Bitmap()
    for r in region.rects { bitmap.add(r) }
    return bitmap
}

/// The canonical form from `raster.h`, checked literally. Returns a description of the first
/// violation, or nil.
func canonicalFormViolation(_ region: Region) -> String? {
    let rects = region.rects
    var index = 0
    var previousBand: ArraySlice<raster_rect>?

    while index < rects.count {
        let first = rects[index]
        if raster_rect_is_empty(first) { return "rect \(index) is empty" }

        let start = index
        while index < rects.count, rects[index].y0 == first.y0 {
            let r = rects[index]
            if r.y1 != first.y1 {
                return "rect \(index) shares y0=\(r.y0) but has y1=\(r.y1), not \(first.y1)"
            }
            if index > start, r.x0 <= rects[index - 1].x1 {
                return "rects \(index - 1),\(index) touch or overlap in x "
                     + "(\(r.x0) <= \(rects[index - 1].x1))"
            }
            index += 1
        }
        let band = rects[start..<index]

        if index < rects.count, rects[index].y0 < first.y1 {
            return "band at y=\(first.y0)..<\(first.y1) overlaps the next band at "
                 + "y=\(rects[index].y0)"
        }
        if let previous = previousBand,
           previous.first!.y1 == first.y0,
           previous.count == band.count,
           zip(previous, band).allSatisfy({ above, below in
               above.x0 == below.x0 && above.x1 == below.x1
           }) {
            return "bands at y=\(previous.first!.y0) and y=\(first.y0) are identical and "
                 + "vertically adjacent, so they should have been merged"
        }
        previousBand = band
    }
    return nil
}

/// Every promise the header makes, checked at once: the pixels, the canonical form, the exact
/// bounds, `isEmpty`, and `contains` over the whole grid plus a one-pixel margin on each side
/// so an out-of-range coordinate has to answer false rather than run off the rectangle list.
func expectMatches(_ region: Region, _ want: Bitmap, _ label: String,
                   sourceLocation: SourceLocation = #_sourceLocation) {
    let got = rendered(region)
    #expect(got.drawn == want.drawn, "\(label): pixels", sourceLocation: sourceLocation)

    if let violation = canonicalFormViolation(region) {
        Issue.record("\(label): canonical form: \(violation)", sourceLocation: sourceLocation)
    }

    #expect(region.isEmpty == want.isEmpty, "\(label): isEmpty", sourceLocation: sourceLocation)
    #expect(region.bounds == want.bounds, "\(label): bounds", sourceLocation: sourceLocation)

    for y in Int32(-1)...Bitmap.size {
        for x in Int32(-1)...Bitmap.size where region.contains(x, y) != want[x, y] {
            // Built as a String first: `Issue.record` takes a `Comment`, and concatenating
            // two literals with `+` resolves against the wrong overload.
            let detail: String = "\(label): contains(\(x),\(y)) says \(region.contains(x, y)), "
                       + "the bitmap says \(want[x, y])"
            Issue.record("\(detail)", sourceLocation: sourceLocation)
            return
        }
    }
}

/// The same generator the C property-test harness uses, so a failing Swift seed can be
/// reproduced in a standalone C program under valgrind.
struct Random {
    private var state: UInt32
    init(seed: UInt32) { state = seed }

    mutating func next() -> UInt32 {
        state = state &* 1_664_525 &+ 1_013_904_223
        return state >> 8
    }

    mutating func below(_ bound: UInt32) -> Int32 { Int32(next() % bound) }

    /// Widths lean small: many small rectangles produce far more bands, and bands are where
    /// the merging logic lives. Every eighth rectangle is allowed to span the grid.
    mutating func nextRect() -> raster_rect {
        let x0 = below(UInt32(Bitmap.size))
        let y0 = below(UInt32(Bitmap.size))
        var w = below(9) + 1
        var h = below(9) + 1
        if next() % 8 == 0 { w = below(UInt32(Bitmap.size)) + 1 }
        if next() % 8 == 0 { h = below(UInt32(Bitmap.size)) + 1 }
        return rect(x0, y0, min(x0 + w, Bitmap.size), min(y0 + h, Bitmap.size))
    }
}

/// Builds a region as the union of `count` random rectangles, alongside the matching bitmap.
/// Construction therefore exercises the union path on every single test.
func randomRegion(_ count: Int, _ random: inout Random) -> (Region, Bitmap, [raster_rect]) {
    var bitmap = Bitmap()
    var rects: [raster_rect] = []
    var region = Region()
    for _ in 0..<count {
        let r = random.nextRect()
        rects.append(r)
        bitmap.add(r)
        region = region.union(Region(r))
    }
    return (region, bitmap, rects)
}

// MARK: - Degenerate input

@Suite("region: empty and degenerate")
struct RegionDegenerateTests {
    @Test("a rectangle is empty when either extent is non-positive")
    func rectEmptiness() {
        // Also the first Swift caller of a `static inline` function from raster.h. If the
        // importer ever stops surfacing those, this is where it shows up, rather than in
        // CGContext where `raster_bytes_per_pixel` is on the hot path.
        #expect(raster_rect_is_empty(rect(5, 5, 5, 10)))
        #expect(raster_rect_is_empty(rect(5, 5, 10, 5)))
        #expect(raster_rect_is_empty(rect(10, 10, 5, 5)))
        #expect(raster_rect_is_empty(rect(0, 0, 0, 0)))
        #expect(!raster_rect_is_empty(rect(0, 0, 1, 1)))
    }

    @Test("a fresh region covers nothing")
    func fresh() {
        let region = Region()
        expectMatches(region, Bitmap(), "empty region")
        #expect(region.count == 0)
        #expect(region.bounds == rect(0, 0, 0, 0))
    }

    @Test("an empty rectangle produces an empty region rather than a degenerate one")
    func emptyRect() {
        for r in [rect(5, 5, 5, 10), rect(5, 5, 10, 5), rect(10, 10, 5, 5), rect(0, 0, 0, 0)] {
            let region = Region(r)
            expectMatches(region, Bitmap(), "region from (\(r.x0),\(r.y0),\(r.x1),\(r.y1))")
            #expect(region.count == 0, "an empty rectangle must not be stored")
        }
    }

    @Test("an out-of-range index answers with an empty rectangle")
    func outOfRange() {
        let region = Region(rect(1, 2, 3, 4))
        #expect(raster_rect_is_empty(region[1]))
        #expect(raster_rect_is_empty(region[99]))
    }

    @Test("NULL is a readable empty region, not a crash")
    func nullQueries() {
        #expect(raster_region_is_empty(nil))
        #expect(raster_region_count(nil) == 0)
        #expect(!raster_region_contains(nil, 0, 0))
        #expect(raster_rect_is_empty(raster_region_bounds(nil)))
        raster_region_destroy(nil)
    }

    @Test("NULL as an operand behaves as the empty region")
    func nullOperands() {
        let r = rect(2, 3, 8, 9)
        let one = Region(r)
        var full = Bitmap()
        full.add(r)

        // b = NULL: union, subtract and xor keep a; intersect empties it.
        expectMatches(Region(raster_region_union(one.handle, nil)!), full, "a | NULL")
        expectMatches(Region(raster_region_subtract(one.handle, nil)!), full, "a - NULL")
        expectMatches(Region(raster_region_xor(one.handle, nil)!), full, "a ^ NULL")
        expectMatches(Region(raster_region_intersect(one.handle, nil)!), Bitmap(), "a & NULL")

        // a = NULL: union and xor keep b; intersect and subtract are empty.
        expectMatches(Region(raster_region_union(nil, one.handle)!), full, "NULL | b")
        expectMatches(Region(raster_region_xor(nil, one.handle)!), full, "NULL ^ b")
        expectMatches(Region(raster_region_intersect(nil, one.handle)!), Bitmap(), "NULL & b")
        expectMatches(Region(raster_region_subtract(nil, one.handle)!), Bitmap(), "NULL - b")
    }

    @Test("a copy is equal and independent")
    func copying() {
        var random = Random(seed: 7)
        let (region, bitmap, _) = randomRegion(6, &random)
        let copy = region.copy()
        expectMatches(copy, bitmap, "copy")
        #expect(copy.rects == region.rects)

        let empty = Region().copy()
        #expect(empty.count == 0)
        #expect(empty.isEmpty)
    }
}

// MARK: - Named shapes

@Suite("region: band merging")
struct RegionBandMergingTests {
    @Test("two rectangles stacked exactly become one")
    func stacked() {
        let merged = Region(rect(4, 0, 10, 5)).union(Region(rect(4, 5, 10, 9)))
        #expect(merged.count == 1)
        #expect(merged[0] == rect(4, 0, 10, 9))
    }

    @Test("two rectangles touching side by side become one")
    func sideBySide() {
        let merged = Region(rect(0, 0, 5, 5)).union(Region(rect(5, 0, 11, 5)))
        #expect(merged.count == 1)
        #expect(merged[0] == rect(0, 0, 11, 5))
    }

    @Test("three stacked rectangles collapse all the way down")
    func threeStacked() {
        var region = Region()
        for i in Int32(0)..<3 {
            region = region.union(Region(rect(2, i * 4, 7, i * 4 + 4)))
        }
        #expect(region.count == 1)
        #expect(region[0] == rect(2, 0, 7, 12))
    }

    @Test("a plus sign is exactly three bands with an exact bounding box")
    func plus() {
        let plus = Region(rect(4, 0, 7, 12)).union(Region(rect(0, 4, 12, 7)))
        #expect(plus.count == 3, "one band above the crossbar, one across it, one below")
        #expect(plus.bounds == rect(0, 0, 12, 12))
        // The corners are outside the plus. A conservative bounding box would still be
        // right here; a wrong `contains` would not.
        #expect(!plus.contains(0, 0))
        #expect(!plus.contains(11, 11))
        #expect(plus.contains(5, 0))
        #expect(plus.contains(0, 5))
        #expect(plus.contains(5, 5))
    }

    @Test("punching the centre out of a plus leaves four arms")
    func plusMinusCentre() {
        let plus = Region(rect(4, 0, 7, 12)).union(Region(rect(0, 4, 12, 7)))
        let arms = plus.subtract(Region(rect(4, 4, 7, 7)))
        #expect(arms.count == 4)
        #expect(!arms.contains(5, 5))

        var want = Bitmap()
        want.add(rect(4, 0, 7, 12))
        want.add(rect(0, 4, 12, 7))
        for y in Int32(4)..<7 {
            for x in Int32(4)..<7 { want[x, y] = false }
        }
        expectMatches(arms, want, "plus minus centre")
    }

    @Test("a hole gives a four-band frame whose bounds stay exact")
    func frame() {
        let outer = Region(rect(0, 0, 20, 20))
        let inner = Region(rect(5, 5, 15, 15))
        let frame = outer.subtract(inner)
        #expect(frame.count == 4, "top band, two side rects, bottom band")
        #expect(frame.bounds == rect(0, 0, 20, 20))
        #expect(!frame.contains(10, 10))
        #expect(frame.contains(0, 10))

        // Filling the hole back in has to return the original single rectangle, which is
        // the canonical form doing its job: the same pixels, the same representation.
        let refilled = frame.union(inner)
        #expect(refilled.count == 1)
        #expect(refilled[0] == rect(0, 0, 20, 20))
        #expect(refilled.rects == outer.rects)
    }
}

// MARK: - Properties

@Suite("region: set operations against a bitmap")
struct RegionOperationTests {
    /// Rectangle counts chosen to span the interesting range: one rectangle exercises the
    /// trivial paths, two and three produce the first real bands, and nine produces regions
    /// whose bands genuinely need merging.
    @Test("union, intersect, subtract and xor agree with the bitmap",
          arguments: [1, 2, 3, 5, 9])
    func operations(_ rectCount: Int) {
        for seed in UInt32(1)...40 {
            var random = Random(seed: seed &* 2_654_435_761 &+ UInt32(rectCount))
            let (a, ma, _) = randomRegion(rectCount, &random)
            let (b, mb, _) = randomRegion(rectCount, &random)

            expectMatches(a, ma, "seed \(seed): built a")
            expectMatches(b, mb, "seed \(seed): built b")

            expectMatches(a.union(b), ma.combined(mb) { $0 || $1 }, "seed \(seed): union")
            expectMatches(a.intersect(b), ma.combined(mb) { $0 && $1 }, "seed \(seed): intersect")
            expectMatches(a.subtract(b), ma.combined(mb) { $0 && !$1 }, "seed \(seed): subtract")
            expectMatches(a.xor(b), ma.combined(mb) { $0 != $1 }, "seed \(seed): xor")
        }
    }

    @Test("the algebraic identities hold, including when both operands are the same region")
    func identities() {
        for seed in UInt32(1)...60 {
            var random = Random(seed: seed &* 40_503 &+ 17)
            let (a, ma, _) = randomRegion(6, &random)

            expectMatches(a.union(a), ma, "seed \(seed): a | a")
            expectMatches(a.intersect(a), ma, "seed \(seed): a & a")
            expectMatches(a.subtract(a), Bitmap(), "seed \(seed): a - a")
            expectMatches(a.xor(a), Bitmap(), "seed \(seed): a ^ a")

            let empty = Region()
            expectMatches(a.union(empty), ma, "seed \(seed): a | 0")
            expectMatches(a.intersect(empty), Bitmap(), "seed \(seed): a & 0")
            expectMatches(a.subtract(empty), ma, "seed \(seed): a - 0")
            expectMatches(empty.subtract(a), Bitmap(), "seed \(seed): 0 - a")
        }
    }

    @Test("intersect_rect matches intersecting with a one-rectangle region")
    func intersectRect() {
        for seed in UInt32(1)...60 {
            var random = Random(seed: seed &* 2_246_822_519 &+ 11)
            let (a, ma, _) = randomRegion(6, &random)
            let clip = random.nextRect()

            var mc = Bitmap()
            mc.add(clip)
            let clipped = a.intersecting(clip)
            expectMatches(clipped, ma.combined(mc) { $0 && $1 }, "seed \(seed): intersect_rect")
            #expect(clipped.rects == a.intersect(Region(clip)).rects,
                    "seed \(seed): the fast path must agree with the general one")

            // The hot path is `clip(to: rect)` inside a save/restore pair, so clipping twice
            // by the same rectangle has to be a no-op rather than an accumulating change.
            #expect(clipped.intersecting(clip).rects == clipped.rects,
                    "seed \(seed): intersect_rect is not idempotent")
        }
    }
}

@Suite("region: canonical form")
struct RegionCanonicalFormTests {
    @Test("the same pixels give the same rectangle list, whatever order they arrive in")
    func orderIndependence() {
        // `DocumentSelection` compares clips for equality, and the clip stack compares a
        // restored clip against the one it replaced. Both need one representation per set.
        for seed in UInt32(1)...80 {
            var random = Random(seed: seed &* 40_503 &+ 7)
            let (forward, bitmap, rects) = randomRegion(7, &random)

            var backward = Region()
            for r in rects.reversed() { backward = backward.union(Region(r)) }

            #expect(backward.rects == forward.rects, "seed \(seed): order changed the shape")
            expectMatches(backward, bitmap, "seed \(seed): backward build")
        }
    }

    @Test("translating every input translates the output and changes nothing else")
    func translationInvariance() {
        // Nothing may assume non-negative coordinates: clips live in device space and
        // TiledLayerRenderer asks for tiles left of and above the origin.
        let shift: Int32 = -17
        for seed in UInt32(1)...60 {
            var random = Random(seed: seed &* 3_266_489_917 &+ 23)
            let (positive, _, rects) = randomRegion(5, &random)

            var shifted = Region()
            for r in rects {
                shifted = shifted.union(
                    Region(rect(r.x0 + shift, r.y0 + shift, r.x1 + shift, r.y1 + shift)))
            }

            if let violation = canonicalFormViolation(shifted) {
                Issue.record("seed \(seed): canonical form: \(violation)")
            }
            #expect(shifted.rects == positive.rects.map {
                rect($0.x0 + shift, $0.y0 + shift, $0.x1 + shift, $0.y1 + shift)
            }, "seed \(seed): the shifted region is not a translate of the original")

            guard !positive.isEmpty else { continue }
            let want = positive.bounds
            #expect(shifted.bounds == rect(want.x0 + shift, want.y0 + shift,
                                           want.x1 + shift, want.y1 + shift),
                    "seed \(seed): bounds")
            for y in want.y0..<want.y1 {
                for x in want.x0..<want.x1 where
                    shifted.contains(x + shift, y + shift) != positive.contains(x, y) {
                    let detail: String = "seed \(seed): contains(\(x + shift),\(y + shift)) in the "
                               + "shifted region disagrees with contains(\(x),\(y)) in the "
                               + "original"
                    Issue.record("\(detail)")
                    return
                }
            }
        }
    }
}

@Suite("region: fill rules")
struct RegionFillRuleTests {
    @Test("accumulating with xor is the even-odd rule: an overlap is out")
    func evenOdd() {
        // LayerRenderer builds its brush-preview clip with addRect + evenOdd, so two added
        // rectangles that overlap leave a hole. Every other accumulation in the app is
        // winding, which is plain union.
        for seed in UInt32(1)...80 {
            var random = Random(seed: seed &* 2_246_822_519 &+ 11)
            var bitmap = Bitmap()
            var region = Region()
            for _ in 0..<5 {
                let r = random.nextRect()
                bitmap.toggle(r)
                region = region.xor(Region(r))
            }
            expectMatches(region, bitmap, "seed \(seed): even-odd accumulation")
        }
    }

    @Test("two overlapping rectangles differ between the two rules")
    func rulesDisagree() {
        let a = Region(rect(0, 0, 10, 10))
        let b = Region(rect(5, 5, 15, 15))

        let winding = a.union(b)
        #expect(winding.contains(7, 7), "under winding the overlap is inside")

        let evenOdd = a.xor(b)
        #expect(!evenOdd.contains(7, 7), "under even-odd the overlap is a hole")
        #expect(evenOdd.contains(2, 2))
        #expect(evenOdd.contains(12, 12))
        #expect(evenOdd.bounds == rect(0, 0, 15, 15), "the hole does not shrink the bounds")
    }
}

@Suite("region: at renderer scale")
struct RegionScaleTests {
    @Test("a tiled patch list clips without losing its canonical form")
    func patchList() {
        // The largest region the app can build: a painted layer's patch list on the
        // 256-pixel tile grid over a 4000-pixel canvas, then clipped to the viewport.
        var random = Random(seed: 99)
        var patches = Region()
        var tiles = 0
        for ty in Int32(0)..<16 {
            for tx in Int32(0)..<16 {
                guard random.next() % 3 == 0 else { continue }
                tiles += 1
                patches = patches.union(
                    Region(rect(tx * 256, ty * 256, tx * 256 + 256, ty * 256 + 256)))
            }
        }
        if let violation = canonicalFormViolation(patches) {
            Issue.record("patch list: \(violation)")
        }
        #expect(!patches.isEmpty)
        #expect(tiles == 78, "the generator is deterministic; a change here invalidates the rest")
        // Merging is the whole point of the representation: adjacent tiles have to collapse
        // rather than accumulate one rectangle each.
        #expect(patches.count < tiles, "\(tiles) tiles stayed as \(patches.count) rectangles")

        let view = rect(300, 700, 2500, 3100)
        let clipped = patches.intersecting(view)
        if let violation = canonicalFormViolation(clipped) {
            Issue.record("clipped patch list: \(violation)")
        }
        #expect(clipped.count <= patches.count)

        let bounds = clipped.bounds
        #expect(bounds.x0 >= view.x0 && bounds.y0 >= view.y0)
        #expect(bounds.x1 <= view.x1 && bounds.y1 <= view.y1)
        #expect(clipped.intersecting(view).rects == clipped.rects)
    }
}
