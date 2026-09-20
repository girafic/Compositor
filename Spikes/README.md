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

**Answered: yes. 1,252,637 checks, 0 failures**, on two compilers and five optimisation
levels, clean under valgrind and under gcc's AddressSanitizer + UndefinedBehaviorSanitizer.

Covers `coverage.c` (the device-pixel arithmetic), `context.c` (the graphics state stack,
the clip stack, snapshots, fill, clear and mask clips) and `draw_image.c` (the sampler)
against independent models:

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

The sampler adds a third stage, against the promises the app's own tests encode:

- **1:1 is exact** across all five interpolation qualities, with antialiasing on and off, on
  both pixel formats, at integer and at shifted-integer origins. This is the single hardest
  requirement in the slice: `RasterSnapshotTests` memcmps a `.high` render against a `.low`
  one over 64 MB, and those two paths differ in quality, antialiasing, geometry *and* the
  float expression used for the destination edge. Byte equality is only reachable if 1:1
  neutralises all four.
- **The nearest tie-break** on a ramp at 1:1, 2x and 0.5x. Both plausible wrong answers must
  fail, and 0.5x is what separates them: corner-to-corner mapping agrees with the correct
  rule at 1:1 and under magnification, and only diverges under reduction.
- **The sampler against a reference** that computes each destination pixel's source
  coordinate in closed form, so nothing it does can drift the way an incremental walk would.
- **Crop invariance**: one image drawn whole, then in strips with margin, compared byte for
  byte. Exact, not within a tolerance — the closed-form calculation gives that for free, and
  the app only asks for 2.
- **Reduction without aliasing**, edge replication on both sides, Catmull-Rom actually
  differing from the tent kernel, and premultiplied validity (no colour above its own alpha).

Mask clips and the coverage plane add a fourth stage. The first item carries most of it:

- **The mask idiom reproduces its mask.** `BrushRaster.draw(_:in:mask: true, context:)` is
  copied call for call and the output must equal the input mask byte for byte. That one
  sequence pins exact resampling at 1:1, the GRAY8 lerp, `setFillColor(gray:)` with `fill`,
  and the hard rectangle edge — and over a hundred exact pixel assertions across the app's
  own suite read their results back through it.
- **A 1x1 mask stretches flat**, which is what `LayerMask.solid(revealing:)` needs.
- **Nested masks multiply**, `a * b / 255`, and a `restoreGState` leaves the outer plane
  exactly as it was. Nothing copies to achieve that: a nested clip builds its own plane, so
  the parent is read-only by construction rather than by discipline.
- **The plane survives a later rectangle clip.** It sits at the clip region's bounds, so
  narrowing the region has to re-base it; a flat mask could not tell a correct re-base from a
  missing one, so the mask has to vary.
- **The rectangle clips hard even with antialiasing on**, and a soft fill edge crossing a
  mask multiplies both coverages.
- **The mask is placed exactly as a drawn image is**, first row at the rectangle's maximum y.
  That is not a guess: `BrushRaster.draw` uses one transform preamble for its mask branch and
  its image branch, and `LayerRenderer.drawCoverage` applies the same second flip.

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

17 more defects were injected with the sampler, into `draw_image.c`, `context.c` and
`surface.c`: rounding instead of flooring the source index, corner-to-corner mapping, a
kernel that never widens with the reduction factor, a tent that is really a box, unnormalised
weights, edge clamping that wraps instead, Catmull-Rom silently replaced by the tent, a
truncated rather than rounded output byte, a lost y flip, a draw that ignores the clip, the
alpha, the rectilinear check, the format check or the snapshot detach, a crop that refuses
an overhang again, and two forms of a missing premultiplied clamp. **All 17 were caught**,
one only after the test was improved:

- **Clamping only the red channel survived.** The validity test drove a single red spike
  through Catmull-Rom's negative lobes, so a clamp that looked at channel 0 and stopped was
  indistinguishable from a correct one. The image now spikes each of red, green and blue at
  a different distance from the alpha hole, which also catches a clamp that decides once and
  applies that answer to all three.

One mutant in that sweep is worth recording as a method note rather than a result: deleting
the clamp's loop body left `if (format == RASTER_RGBA8)` with no statement, so it failed to
compile. A mutant that does not build is not a surviving mutant and not a killed one — it is
a malformed experiment, and reading it as either would be wrong. It was rewritten to remove
the whole guard, and in that form it dies.

16 more came with the coverage plane: a plane that is ignored, indexed without its x or its
y offset, or that drops the antialiasing coverage instead of multiplying it; a product that
truncates rather than rounds; a nested clip that does not multiply the parent in, or reads it
at the wrong offset; a mask rectangle that clips soft instead of hard; a clip that accepts a
colour mask, accepts a rotation, or ignores the interpolation quality; a re-base that does
not crop, drops the plane, or discards it on construction; and fill and draw each failing to
apply it. **15 of the 16 were caught, but 5 only after four new tests were written** — and
none of those 5 was an equivalent mutant. The gaps were all of one kind: the first pass had
no test that combined antialiasing with a mask, none that nested masks at *different*
offsets, none with a fractional mask rectangle, and none that varied the interpolation
quality. Uniform masks over identical full-size rectangles cannot see any of that.

The remaining survivor is genuinely equivalent: the rotation check at the top of
`clip_mask` is repeated inside `raster_image_mapping`, so removing it changes no answer. It
stays — the refusal should be that function's own decision rather than a side effect of what
a callee happens to check — and the source now says so, as the two unreachable branches in
`region.c` do.

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
