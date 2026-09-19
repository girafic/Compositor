# Spikes

Experiments that answer a design question before the design depends on it. Each one is
self-contained and re-runnable; none of them is part of the shipping build.

## `halving-invariant.c` — does Lanczos-3 fit the tiling margin budget?

**Answered: yes, with 4–5× headroom at every level.**

`DownsampleCache` builds a chain of exactly-2× reductions, and `TiledLayerRenderer` depends on the
result being *position independent*: a piece of a layer rendered on its own with a margin, reduced
the same way, must come out bit-identical to the same region of the whole layer reduced. Otherwise
every painted layer shows a grid of hairlines.

`TiledLayerRenderer.support(level:)` is the margin budgeted for that — 8 grid pixels at level 0,
`16 << level` above it. On macOS the actual reach came from vImage's Lanczos. On Linux we choose the
kernel, so the budget has to be measured against it rather than assumed.

This builds the real thing — exactly-2× Lanczos-3, with `DownsampleCache.halve`'s geometry (pad by 8
transparent pixels, scale the padded buffer, clamp premultiplied ringing, crop back by `pad/2`) —
and then searches for the smallest margin at which a 256×256 piece reproduces the whole *exactly*,
on high-frequency content with varying alpha.

| level | `support()` | margin actually needed | headroom |
|------:|------------:|-----------------------:|---------:|
| 1 | 32 | 6 | 5.3× |
| 2 | 64 | 16 | 4.0× |
| 3 | 128 | 32 | 4.0× |
| 4 | 256 | 64 | 4.0× |
| 5 | 512 | 96 | 5.3× |
| 6 | 1024 | 192 | 5.3× |

What this does and does not establish: it shows the kernel is position independent well inside the
existing budget, which is exactly the property `TiledLayerTests` checks (piece against whole, within
one run, with a tolerance). It does **not** show agreement with vImage, and nothing requires that —
there is no stored macOS reference anywhere in the test suite.

Consequences for M2:

- `support(level:)` carries over unchanged. No re-derivation needed.
- The padding geometry is load-bearing and must be copied exactly, including the asymmetry: 8px
  transparent padding plus an alpha clamp for colour, edge replication for gray masks.
- Normalising the kernel weights over the taps that fall inside the buffer is required. Without it
  the outermost rows darken.
- This file is the prototype for `Sources/CCompositorRaster/resample.c`.

```sh
clang -std=gnu11 -O2 -Wall -Wextra Spikes/halving-invariant.c -lm -o /tmp/halving && /tmp/halving
```

## `probe-linux-swift.sh` — what does the Linux toolchain actually vend?

**Answered on Swift 6.4.0 / Ubuntu 24.04: 12 passed, 8 failed — and the failures are the useful
part.** Runs in CI as the `toolchain-probe` job, which reports and never gates.

### Foundation vends part of the geometry, not all of it

| | on Linux |
|---|---|
| `CGFloat`, `CGPoint`, `CGSize`, `CGRect` | **provided** |
| `CGRect.minX/maxY/midX/width/…`, `intersection`, `union`, `insetBy`, `offsetBy`, `isNull`, `isEmpty`, `integral`, `standardized`, `contains`, `intersects`, `.null`, `.zero` | **provided** |
| `CGAffineTransform` | **missing** |
| `CGVector` | **missing** |
| `CGRect.applying(_:)`, `CGPoint.applying(_:)` | **missing** (they need `CGAffineTransform`) |

So the split is now exact. `Sources/CoreGraphics/` must `@_exported import Foundation` to inherit
the rect and point algebra — roughly a thousand occurrences across the codebase that then need no
attention at all — and must itself declare `CGAffineTransform`, `CGVector`, and the two `applying`
extensions, on top of `CGContext`, `CGImage`, `CGPath`, `CGColor`, `CGColorSpace`, `CGGradient` and
`CGBlendMode`.

Note the trap: swift-corelibs-foundation *does* have `AffineTransform` (from `NSAffineTransform`).
It is a different type with different conventions and is not a substitute. The 26 `inverted()` and
8 `concatenating()` call sites want `CGAffineTransform` semantics.

### A SwiftPM target may be called `CoreGraphics`

Confirmed by building one. There is no system `CoreGraphics` module on Linux to collide with — the
`import CoreGraphics` probe fails, which is exactly what frees the name. The probe package also
checks the part that actually matters: a *consumer* doing a single `import CoreGraphics` sees both
our own declarations and everything Foundation re-exports through it. That is what lets ~800
`context.*` call sites and 288 tests port unmodified rather than being rewritten against a new name.

### Everything else M2 leans on works

`@Observable`, swift-testing, and `-default-isolation MainActor` (the flag standing in for the Xcode
project's `SWIFT_DEFAULT_ACTOR_ISOLATION = MainActor`) all pass.

Re-run after any toolchain bump; the answers are version-dependent.
