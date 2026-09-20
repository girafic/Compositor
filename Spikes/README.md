# Spikes

Experiments that answer a design question before the design depends on it, plus the standalone
C harnesses that verify the engine in ways CI cannot. Each one is self-contained and
re-runnable; none of them is part of the shipping build.

CI runs the Swift test suite under a Swift toolchain, which is the wrong place to reach for
valgrind, AddressSanitizer or a five-way optimisation sweep. Those live here instead, as plain
C programs that link one engine source directly.

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

## `region-oracle.c` — does `region.c` agree with a brute-force bitmap?

**Answered: yes. 60,085 checks, 0 failures**, on two compilers and five optimisation levels,
clean under valgrind and under gcc's AddressSanitizer + UndefinedBehaviorSanitizer.

Random rectangles on a 32×32 grid, with every region operation checked against a plain
`bool[32][32]`: union, intersect, subtract, xor, `bounds`, `contains` (probed one pixel outside
the grid on every side), `is_empty`, `copy` and `intersect_rect`. After every operation the
result is also checked against the canonical form the header promises — sorted bands, no
touching or overlapping rectangles within a band, no two vertically adjacent bands with
identical x-intervals — because that form is what makes `raster_region_bounds` exact and what
makes two regions covering the same pixels compare equal rectangle-for-rectangle.

`Tests/CRasterTests/RegionTests.swift` carries the same assertions into CI. This file exists
for what CI cannot do:

```sh
clang -std=gnu11 -Wall -Wextra -Werror -O1 -ISources/CCompositorRaster/include \
  Spikes/region-oracle.c Sources/CCompositorRaster/region.c -o /tmp/region-oracle
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all -q /tmp/region-oracle

# clang in this container ships no sanitizer runtimes; gcc does.
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ISources/CCompositorRaster/include \
  Spikes/region-oracle.c Sources/CCompositorRaster/region.c -o /tmp/region-asan && /tmp/region-asan
```

### It was mutation-tested, because passing on the first run is not evidence

Twenty-four single-defect mutants were injected into `region.c` and the harness re-run.
**Seventeen were caught**: every fill-rule predicate, a `bounds` that stops after the first
rectangle, two off-by-ones in `contains`, dropping the band merge or any of its three
conditions, leaving the band edges unsorted, a stale previous-band index, `copy` losing a
rectangle, `intersect_rect` dispatching to the wrong operation, and dropping the requirement
that a rectangle must span a strip to contribute to it — that last one does not fail so much as
hang, on a rectangle count that never stops growing.

The seven survivors are all genuinely undetectable rather than untested:

- **Four are redundant guards.** Empty rectangles are rejected in both
  `raster_region_create_rect` and `append`; zero-height strips are prevented by deduplicating
  band edges, *and* skipped in the strip loop, *and* rejected by `append`. Removing both
  empty-rectangle guards together **is** caught. Removing both zero-height guards is not — the
  third one, in `append`, still covers it.
- **Two are unreachable by construction**, and now say so in the source: the run-coalescing
  branch in `append`, and the loop (rather than `if`) form of the edge toggles in
  `combine_band`. Both are correct for inputs the current sweep cannot produce. They stay so the
  helpers are right on their own terms instead of only in combination with the one caller that
  happens to respect the invariant. The reachable half of that toggle logic — deciding only
  after *both* interval lists are advanced past a shared coordinate — is very much covered:
  breaking it fails 56,124 of the 60,085 checks.
- **One is the `break` in `contains`**, a pure optimisation over `continue`.

Four further mutants removed whole guard pairs at once, to tell a redundant guard from an
untested one; the two results that matter are quoted above.

## `context-oracle.c` — does the drawing context agree with a brute-force model?

**Answered: yes. 1,065,645 checks, 0 failures**, on two compilers and five optimisation
levels, clean under valgrind and under gcc's AddressSanitizer + UndefinedBehaviorSanitizer.

Covers `coverage.c` (the device-pixel arithmetic) and `context.c` (the graphics state stack,
the clip stack, snapshots, fill and clear) against independent models:

- **The edge rule** against a direct centre test, over random rectilinear matrices from both
  families, with negative scales and negative device origins. The input space is seeded with
  exact integers, exact half-integers, and values one ulp either side of a half-integer,
  because uniform random doubles would essentially never reach the cases that can break it.
- **The partition property** — splitting a range at any real point gives two ranges that tile
  it exactly. This is "piecewise == whole" reduced to integers and checked exhaustively.
- **The clip stack** against a `bool[20][14]` stack, reusing `region-oracle.c`'s oracle.
- **Every fill** against a reference that loops over each pixel and calls `raster_blend_*`
  one at a time. Nothing in that reference knows about bands, strides or row pointers, which
  is exactly where the engine's bugs would be.
- **Copy-on-write**, including the case where the snapshot was released before the draw (it
  must cost a refcount decrement, not a canvas copy) and the borrowed target (which can never
  detach, so its snapshot is copied eagerly).

```sh
clang -std=gnu11 -Wall -Wextra -Werror -O1 -ISources/CCompositorRaster/include \
  Spikes/context-oracle.c Sources/CCompositorRaster/*.c -lm -o /tmp/context-oracle
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all /tmp/context-oracle

gcc -std=gnu11 -Wall -Wextra -Werror -O2 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ISources/CCompositorRaster/include \
  Spikes/context-oracle.c Sources/CCompositorRaster/*.c -lm -o /tmp/context-asan && /tmp/context-asan
```

### What it caught, and what mutation testing then caught

The oracle found a real defect on its first run, before any of this reached Swift:
`ceil(t - 0.5)` loses its last bit when `t` is within an ulp of a half-integer, and `ceil`
then lands a column out. `raster_pixel_edge` now corrects the seed against the definition
directly, which also makes the tie-break impossible to drift — any seed within one converges
to the same answer, so replacing `ceil(t - 0.5)` with `round(t)` changes nothing.

33 defects were then injected deliberately, 14 into `coverage.c` and 19 into `context.c`.
**All 33 were caught**, but four of them only after the tests were improved, and those four
are the interesting ones:

- Two tie-break mutants survived because the correction step *is* the specification. That is
  an equivalent-mutant result rather than a gap, and it is stronger than the design intended.
- "Truncate instead of round" in the coverage ramp slipped under a sum-based tolerance, since
  the two differ by at most one part in 255 per partial pixel. Fixed by asserting exact
  coverage bytes for intervals in eighths — not tenths, because 0.1 is not representable and
  an "exact" expectation written with it tests the author's arithmetic rather than the code's.
- A missing `clip_release` in `restoreGState` is a leak, not a wrong answer, so no behavioural
  oracle could see it. Valgrind does: 242 KB definitely lost, exit 99.
- Copying a snapshot eagerly is always *correct*, just slower, so nothing noticed a
  full-canvas memcpy per `makeImage`. Fixed by asserting that a fresh snapshot shares the
  context's pixel pointer.

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
