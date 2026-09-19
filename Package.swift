// swift-tools-version: 6.0
//
// Compositor for Linux.
//
// This is the Linux fork of Compositor. The macOS app lives on `main` and is built with
// Xcode; nothing here builds it. See README.md.
//
// Tools version note: 6.0 is deliberate — nothing in the current target graph needs a newer
// manifest API. Milestone 2 bumps this to 6.2 for `.defaultIsolation(MainActor.self)`, which
// is what reproduces the Xcode project's `SWIFT_DEFAULT_ACTOR_ISOLATION = MainActor`.

import PackageDescription

// The kernels were written against `GCC_C_LANGUAGE_STANDARD = gnu17`. `gnu11` is the floor:
// HealPixels.c uses M_PI, which strict `-std=c11` hides behind __STRICT_ANSI__.
let kernelCFlags: [CSetting] = [
    .unsafeFlags(["-std=gnu11", "-Wall", "-Wextra", "-Werror"])
]

let package = Package(
    name: "Compositor",
    targets: [
        // The eight pixel kernels, carried over from the macOS app unchanged. They include only
        // <stdint.h> <stddef.h> <math.h> <stdlib.h> <string.h> <float.h> — no Apple headers — so
        // they are the one part of the renderer that ports verbatim. SwiftPM's generated umbrella
        // module map over `include/` replaces the Xcode bridging header.
        .target(
            name: "CCompositorKernels",
            cSettings: kernelCFlags
        ),
        .testTarget(
            name: "CKernelTests",
            dependencies: ["CCompositorKernels"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
