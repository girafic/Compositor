import Testing
import CCompositorRaster

/// Pixel-format numbering, mirroring `raster.h`. See the note in BlendTests.swift.
enum Format {
    static let rgba8: raster_format = 0
    static let gray8: raster_format = 1
}

/// Runs `body` with a surface and releases it afterwards, so a failing expectation cannot
/// leak the allocation.
func withSurface<T>(_ width: Int, _ height: Int, _ format: raster_format,
                    _ body: (OpaquePointer) throws -> T) rethrows -> T? {
    guard let surface = raster_surface_create(width, height, format) else { return nil }
    defer { raster_surface_release(surface) }
    return try body(surface)
}

@Suite("surface creation")
struct SurfaceCreationTests {
    @Test("an RGBA8 surface is four bytes per pixel and starts zeroed")
    func rgba() throws {
        let checked = withSurface(16, 8, Format.rgba8) { surface -> Bool in
            #expect(raster_surface_width(surface) == 16)
            #expect(raster_surface_height(surface) == 8)
            #expect(raster_surface_stride(surface) == 64)
            #expect(raster_surface_format(surface) == Format.rgba8)
            #expect(raster_surface_bytes(surface)?[0] == 0)
            #expect(raster_surface_is_unique(surface))
            return true
        }
        #expect(checked == true)
    }

    @Test("a GRAY8 surface is one byte per pixel")
    func gray() throws {
        let stride = withSurface(5, 3, Format.gray8) { raster_surface_stride($0) }
        #expect(stride == 5)
    }

    @Test("degenerate sizes are refused rather than allocated")
    func degenerate() {
        #expect(raster_surface_create(0, 4, Format.rgba8) == nil)
        #expect(raster_surface_create(4, 0, Format.rgba8) == nil)
    }
}

@Suite("cropping shares pixels")
struct SurfaceCropTests {
    @Test("a crop view reads the parent's pixels without copying")
    func zeroCopy() throws {
        let surface = try #require(raster_surface_create(8, 8, Format.gray8))
        defer { raster_surface_release(surface) }
        try #require(raster_surface_mutable_bytes(surface))[2 * 8 + 3] = 77

        let view = try #require(raster_surface_crop(surface, 2, 1, 4, 4))
        defer { raster_surface_release(view) }

        #expect(raster_surface_width(view) == 4)
        #expect(raster_surface_stride(view) == 8, "a view keeps the parent's row stride")
        #expect(raster_surface_bytes(view)?[1 * 8 + 1] == 77)
    }

    @Test("an overhanging crop is trimmed rather than refused")
    func overhangIsTrimmed() throws {
        // CGImageCreateWithImageInRect intersects with the image; it does not refuse. The
        // difference is not cosmetic: RasterSnapshot drops the whole piece when a crop comes
        // back nil, so refusing an overhang would silently lose painted pixels.
        let surface = try #require(raster_surface_create(8, 8, Format.gray8))
        defer { raster_surface_release(surface) }
        try #require(raster_surface_mutable_bytes(surface))[2 * 8 + 6] = 99

        let both = try #require(raster_surface_crop(surface, 4, 4, 8, 8))
        defer { raster_surface_release(both) }
        #expect(raster_surface_width(both) == 4 && raster_surface_height(both) == 4)

        // Trimming must move the window's size, never its origin: the view still starts at
        // the pixel asked for.
        let oneAxis = try #require(raster_surface_crop(surface, 6, 2, 10, 2))
        defer { raster_surface_release(oneAxis) }
        #expect(raster_surface_width(oneAxis) == 2 && raster_surface_height(oneAxis) == 2)
        #expect(raster_surface_bytes(oneAxis)?[0] == 99)
    }

    @Test("an empty crop, or one that misses the surface, returns nothing")
    func outOfBounds() throws {
        // nil means empty intersection and nothing else. Thirteen call sites branch on it.
        let surface = try #require(raster_surface_create(8, 8, Format.gray8))
        defer { raster_surface_release(surface) }
        #expect(raster_surface_crop(surface, 0, 0, 0, 4) == nil)
        #expect(raster_surface_crop(surface, 0, 0, 4, 0) == nil)
        #expect(raster_surface_crop(surface, 9, 0, 1, 1) == nil)
        #expect(raster_surface_crop(surface, 0, 9, 1, 1) == nil)
        #expect(raster_surface_crop(surface, 8, 0, 1, 1) == nil, "x == width is already past it")

        let edge = try #require(raster_surface_crop(surface, 4, 4, 4, 4), "flush to the edge is valid")
        raster_surface_release(edge)
    }

    @Test("borrowed memory is neither copied nor freed")
    func borrowed() throws {
        var bytes = [UInt8](repeating: 0, count: 16)
        bytes[5] = 42
        bytes.withUnsafeMutableBufferPointer { buffer in
            let surface = raster_surface_create_borrowed(buffer.baseAddress, 4, 4, 4, Format.gray8)
            #expect(raster_surface_bytes(surface)?[5] == 42)
            raster_surface_release(surface)
        }
        #expect(bytes[5] == 42, "releasing the surface must not free the caller's buffer")
    }
}

@Suite("copy-on-write")
struct SurfaceCopyOnWriteTests {
    @Test("sharing an allocation makes both views read-only")
    func sharingBlocksWrites() throws {
        let surface = try #require(raster_surface_create(8, 8, Format.gray8))
        defer { raster_surface_release(surface) }
        let view = try #require(raster_surface_crop(surface, 0, 0, 4, 4))
        defer { raster_surface_release(view) }

        #expect(!raster_surface_is_unique(surface))
        #expect(!raster_surface_is_unique(view))
        #expect(raster_surface_mutable_bytes(view) == nil)
    }

    @Test("makeImage() then draw again — the DownsampleCache case")
    func snapshotThenDraw() throws {
        // DownsampleCache.halve snapshots a context, crops one row out of the snapshot, and
        // draws that row back into the same context. Without copy-on-write that is a
        // self-overlapping blit with undefined results.
        let context = try #require(raster_surface_create(8, 8, Format.gray8))
        defer { raster_surface_release(context) }
        try #require(raster_surface_mutable_bytes(context))[0] = 11

        let snapshot = try #require(raster_surface_crop(context, 0, 0, 8, 8))  // = makeImage()
        defer { raster_surface_release(snapshot) }
        #expect(raster_surface_bytes(snapshot)?[0] == 11)
        #expect(raster_surface_mutable_bytes(context) == nil, "shared, so not writable in place")

        #expect(raster_surface_make_unique(context), "drawing separates them")
        try #require(raster_surface_mutable_bytes(context))[0] = 22

        #expect(raster_surface_bytes(context)?[0] == 22)
        #expect(raster_surface_bytes(snapshot)?[0] == 11, "the snapshot stays frozen")
    }

    @Test("separating a crop copies only its own window")
    func cropDoesNotRetainTheParent() throws {
        // BrushStroke.paintSnapshot works around the opposite behaviour by hand today: a
        // CGImage crop retains the whole parent allocation, so small painted layers hold
        // large empty buffers alive.
        let surface = try #require(raster_surface_create(64, 64, Format.gray8))
        defer { raster_surface_release(surface) }
        let view = try #require(raster_surface_crop(surface, 8, 8, 4, 4))
        defer { raster_surface_release(view) }

        #expect(raster_surface_make_unique(view))
        #expect(raster_surface_stride(view) == 4, "tightly packed, not the parent's 64")
        #expect(raster_surface_is_unique(surface), "the parent is sole owner again")
    }

    @Test("retaining the same surface does not trigger a copy")
    func retainIsNotSharing() throws {
        // A CGContext held in two places is still one context; only a second *view* of the
        // pixels should force copy-on-write.
        let surface = try #require(raster_surface_create(4, 4, Format.rgba8))
        _ = raster_surface_retain(surface)
        #expect(raster_surface_is_unique(surface))
        raster_surface_release(surface)
        raster_surface_release(surface)
    }

    @Test("a shared borrowed surface refuses to detach")
    func borrowedNeverDetaches() throws {
        // The eyedropper (ColorPalette.sampleCompositeColor) is the app's one context over
        // caller-owned memory: it hands CGContext a 1x1 buffer, draws the whole document
        // through a translated CTM, and reads the pixel back out of *its own* array. If
        // copy-on-write quietly moved that context onto a private allocation, every draw
        // would land somewhere the caller never looks and the eyedropper would return the
        // untouched buffer — no error, no failing draw, just the wrong colour forever.
        var bytes = [UInt8](repeating: 0, count: 16)
        bytes.withUnsafeMutableBufferPointer { buffer in
            let surface = raster_surface_create_borrowed(buffer.baseAddress, 4, 4, 4, Format.gray8)
            defer { raster_surface_release(surface) }
            let view = raster_surface_crop(surface, 0, 0, 2, 2)
            defer { raster_surface_release(view) }

            #expect(!raster_surface_is_unique(surface), "the crop shares the borrowed store")
            #expect(!raster_surface_make_unique(surface),
                    "detaching a borrowed store would strand every later write")
            #expect(raster_surface_bytes(surface) == UnsafePointer(buffer.baseAddress!),
                    "and the surface still points at the caller's buffer")
        }
    }

    @Test("an explicit copy is independent and tightly packed")
    func explicitCopy() throws {
        let surface = try #require(raster_surface_create(8, 4, Format.gray8))
        defer { raster_surface_release(surface) }
        try #require(raster_surface_mutable_bytes(surface))[3] = 55

        let copy = try #require(raster_surface_copy(surface))
        defer { raster_surface_release(copy) }
        #expect(raster_surface_bytes(copy)?[3] == 55)
        #expect(raster_surface_is_unique(copy))

        try #require(raster_surface_mutable_bytes(copy))[3] = 66
        #expect(raster_surface_bytes(surface)?[3] == 55, "the original is untouched")
    }
}
