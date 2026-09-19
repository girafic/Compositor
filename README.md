# Compositor for Linux

A Linux port of [Compositor](https://github.com/robbietilton/Compositor), a free and open-source
image editor built around a Photoshop-style compositing workflow — layers, masks, selections,
non-destructive transforms, adjustment layers and filters.

> **This branch is a hard fork.** It targets Linux only. The macOS app lives on `main` and is built
> with Xcode; nothing on this branch builds it, and fixes do not flow between the two.
>
> **It does not run yet.** The port is staged, and only the pixel kernels are wired up so far. See
> [Status](#status).

## Status

The macOS app is ~24,000 lines of Swift on AppKit, SwiftUI, CoreGraphics, CoreImage, Vision, Metal
and Accelerate. Replacing that stack is a long job, so it is being done in milestones, each of them
verified by CI rather than by assertion.

| Milestone | What it delivers | State |
|---|---|---|
| **M1** | SwiftPM build, CI, the eight C pixel kernels building and tested on Linux | **done** |
| M2 | A CoreGraphics replacement (own compositor in C, Cairo for path coverage) and the headless document renderer | next |
| M3 | Image codecs (libpng/libjpeg-turbo/libtiff/lcms2), the CoreImage filters as C kernels, `.comp` v8 | |
| M4 | `compositor-cli` — open a project, render it, export PNG/JPEG — plus a golden-image determinism suite | |
| M5 | GTK4 shell and canvas | |
| M6 | Panels and the layer list | |
| M7 | Flatpak/AppImage packaging, ONNX background removal, Vulkan compute brush | |

Nothing here is a working editor before M5. M4 is the first artifact that does something useful.

## Building

Needs a Swift 6 toolchain. Nothing else yet — M1 has no system dependencies.

```sh
swift build
swift test
```

Later milestones add `libcairo2-dev libpng-dev libturbojpeg0-dev libtiff-dev liblcms2-dev
libzip-dev` and, from M5, `libgtk-4-dev`. All of them are packaged in Ubuntu 24.04 and Debian 13.

CI runs on a digest-pinned `swift:6.4.0-noble` container, in both debug and release — the kernels
are hand-written pointer code, and optimised builds are where aliasing assumptions bite.

## Layout

```
Sources/CCompositorKernels/   The eight pixel kernels, carried over from macOS unchanged.
                              Plain C99 with no Apple headers, so they port verbatim; SwiftPM's
                              umbrella module map over include/ replaces the Xcode bridging header.
Tests/CKernelTests/           Behavioural tests pinning those kernels before the renderer is
                              rebuilt on top of them.
Compositor/                   The macOS sources still awaiting a port: the document model
                              (Document/), the renderer's portable half (Rendering/) and
                              persistence (IO/). M2 and M3 move these into Sources/.
Attic/appkit/                 AppKit- and SwiftUI-bound code kept as a reference for the GTK
                              rewrite. Not compiled, and deleted at 1.0.
docs/                         Project format and notes. project-format.md is behind the code
                              (it documents v1–6; ProjectStore writes v7) and is corrected in M3.
```

## What changes on Linux

Some things cannot be carried across, and are better stated up front than discovered:

- **Remove Background** used Vision's foreground-*instance* mask. No open model does instance
  segmentation of generic foregrounds; the replacement (ONNX + BiRefNet) produces a single saliency
  map, so choosing between subjects goes away. Landing in M7.
- **`.comp` becomes a ZIP archive** (manifest version 8) instead of a directory. macOS could make a
  directory look like one file; Linux cannot, and a single file is what makes an atomic
  save genuinely atomic. Projects from versions 1–7 still open.
- **Color Dodge and Color Burn render correctly.** CoreGraphics blends both wrong with respect to
  source alpha — the macOS app already works around it — so output differs from macOS for those two
  modes over translucent pixels.
- **No auto-update.** Sparkle has no Linux counterpart; updates come from Flatpak or the distro.
- **Cursors are the theme's**, not the hand-drawn set the macOS build composes from SF Symbols.

## License

MIT — see [LICENSE](LICENSE).
