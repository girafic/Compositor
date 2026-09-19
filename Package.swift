// swift-tools-version: 6.0
//
// Compositor for Linux.
//
// This is the Linux fork of Compositor. The macOS app lives on `main` and is built with
// Xcode; nothing here builds it. See README.md.
//
// Tools version note: 6.0 is deliberate — nothing in the current target graph needs a newer
// manifest API. Milestone 2c bumps this to 6.2 for `.defaultIsolation(MainActor.self)`,
// which is what reproduces the Xcode project's `SWIFT_DEFAULT_ACTOR_ISOLATION = MainActor`
// once CompositorCore exists.

import PackageDescription

// The kernels were written against `GCC_C_LANGUAGE_STANDARD = gnu17`. `gnu11` is the floor:
// HealPixels.c uses M_PI, which strict `-std=c11` hides behind __STRICT_ANSI__.
let cFlags: [CSetting] = [
    .unsafeFlags(["-std=gnu11", "-Wall", "-Wextra", "-Werror"])
]

// Tests do a lot of raw-pointer work against the C targets. Keeping them in Swift 5 mode
// leaves that ergonomic without relaxing the libraries, which build under Swift 6.
let testSettings: [SwiftSetting] = [.swiftLanguageMode(.v5)]

let package = Package(
    name: "Compositor",
    targets: [
        // The eight pixel kernels, carried over from the macOS app unchanged. They include
        // only <stdint.h> <stddef.h> <math.h> <stdlib.h> <string.h> <float.h> — no Apple
        // headers — so they are the one part of the renderer that ports verbatim. SwiftPM's
        // generated umbrella module map over `include/` replaces the Xcode bridging header.
        .target(
            name: "CCompositorKernels",
            cSettings: cFlags
        ),

        // The compositing engine underneath the CoreGraphics replacement. Two pixel layouts
        // and nothing else: premultiplied RGBA8, and opaque GRAY8 for masks and coverage.
        .target(
            name: "CCompositorRaster",
            cSettings: cFlags
        ),

        // The CoreGraphics replacement itself. The module name is deliberate — see
        // Sources/CoreGraphics/Exports.swift.
        .target(
            name: "CoreGraphics",
            dependencies: ["CCompositorRaster"]
        ),

        .testTarget(
            name: "CKernelTests",
            dependencies: ["CCompositorKernels"],
            swiftSettings: testSettings
        ),
        .testTarget(
            name: "CRasterTests",
            dependencies: ["CCompositorRaster"],
            swiftSettings: testSettings
        ),
        .testTarget(
            name: "CoreGraphicsTests",
            dependencies: ["CoreGraphics"],
            swiftSettings: testSettings
        ),
    ]
)
