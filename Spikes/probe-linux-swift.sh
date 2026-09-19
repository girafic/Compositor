#!/usr/bin/env bash
# What the Linux Swift toolchain actually gives us.
#
# Milestone 2 replaces CoreGraphics with our own module. Two things decide its shape and neither can
# be settled by reading documentation:
#
#   1. Does swift-corelibs-foundation already vend the CG geometry types on Linux? If it does, our
#      module must re-export them rather than declare them, or every call site gets an ambiguity
#      error. If it does not, we declare them ourselves.
#   2. Can a SwiftPM target be called `CoreGraphics` on Linux without colliding with anything? That
#      is what lets ~800 call sites and 288 tests port unmodified instead of being rewritten.
#
# This probes each question by type-checking a snippet and reporting the result. It never fails the
# build: a FAIL here is data, not a broken tree. Re-run it after a toolchain bump.

set -uo pipefail

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
pass=0
fail=0

probe() {
    local name="$1" flags="$2" source="$3"
    printf '%s\n' "$source" > "$work/probe.swift"
    # shellcheck disable=SC2086
    if output=$(swiftc -typecheck $flags "$work/probe.swift" 2>&1); then
        printf '  \033[32mPASS\033[0m  %s\n' "$name"
        pass=$((pass + 1))
    else
        printf '  \033[31mFAIL\033[0m  %s\n' "$name"
        printf '%s\n' "$output" | grep -m2 'error:' | sed 's/^/          /'
        fail=$((fail + 1))
    fi
}

echo "== Toolchain =="
swift --version
echo

echo "== Is there a system CoreGraphics on Linux? =="
echo "   (expected to FAIL -- that absence is what frees the module name for us)"
probe "import CoreGraphics" "" 'import CoreGraphics
let r = CGRect(x: 0, y: 0, width: 1, height: 1)
_ = r.minX'
echo

echo "== Geometry types Foundation vends on its own =="
for type in CGFloat CGPoint CGSize CGRect CGVector CGAffineTransform; do
    probe "$type" "" "import Foundation
func use(_ value: $type) {}"
done
echo

echo "== CGRect members the codebase uses =="
probe "CGRect basics (min/max/mid, width, height)" "" 'import Foundation
let r = CGRect(x: 1, y: 2, width: 3, height: 4)
_ = (r.minX, r.minY, r.maxX, r.maxY, r.midX, r.midY, r.width, r.height, r.origin, r.size)'
probe "CGRect.intersection / union / insetBy / offsetBy" "" 'import Foundation
let a = CGRect(x: 0, y: 0, width: 10, height: 10), b = CGRect(x: 5, y: 5, width: 10, height: 10)
_ = (a.intersection(b), a.union(b), a.insetBy(dx: 1, dy: 1), a.offsetBy(dx: 1, dy: 1))'
probe "CGRect.isNull / isEmpty / integral / standardized" "" 'import Foundation
let r = CGRect(x: 0.5, y: 0.5, width: 3.2, height: 4.7)
_ = (r.isNull, r.isEmpty, r.integral, r.standardized, CGRect.null, CGRect.zero)'
probe "CGRect.contains / intersects" "" 'import Foundation
let r = CGRect(x: 0, y: 0, width: 10, height: 10)
_ = (r.contains(CGPoint(x: 1, y: 1)), r.contains(r), r.intersects(r))'
probe "CGRect.applying(CGAffineTransform)" "" 'import Foundation
let r = CGRect(x: 0, y: 0, width: 10, height: 10)
_ = r.applying(CGAffineTransform(scaleX: 2, y: 2))'
echo

echo "== CGAffineTransform members the codebase uses =="
probe "init(translationX:y:) / (scaleX:y:) / (rotationAngle:)" "" 'import Foundation
_ = (CGAffineTransform(translationX: 1, y: 2), CGAffineTransform(scaleX: 2, y: 3), CGAffineTransform(rotationAngle: 0.5))'
probe "init(a:b:c:d:tx:ty:) and the a...ty members" "" 'import Foundation
let t = CGAffineTransform(a: 1, b: 0, c: 0, d: 1, tx: 0, ty: 0)
_ = (t.a, t.b, t.c, t.d, t.tx, t.ty)'
probe "inverted / concatenating / scaledBy / translatedBy / rotated" "" 'import Foundation
let t = CGAffineTransform.identity
_ = (t.inverted(), t.concatenating(t), t.scaledBy(x: 2, y: 2), t.translatedBy(tx: 1, ty: 1), t.rotated(by: 0.5))'
probe "CGPoint.applying" "" 'import Foundation
_ = CGPoint(x: 1, y: 2).applying(CGAffineTransform.identity)'
echo

echo "== Language and library features M2 depends on =="
probe "Observation / @Observable" "" 'import Observation
@Observable final class Model { var value = 0 }'
probe "swift-testing" "" 'import Testing
@Test func works() { #expect(1 == 1) }'
probe "-default-isolation MainActor (SWIFT_DEFAULT_ACTOR_ISOLATION)" "-default-isolation MainActor" 'final class Thing { var value = 0 }'
echo

echo "== Can a SwiftPM target be named CoreGraphics? =="
pkg="$work/pkg"
mkdir -p "$pkg/Sources/CoreGraphics" "$pkg/Sources/User"
cat > "$pkg/Package.swift" <<'MANIFEST'
// swift-tools-version: 6.0
import PackageDescription
let package = Package(
    name: "NameProbe",
    targets: [
        .target(name: "CoreGraphics"),
        .target(name: "User", dependencies: ["CoreGraphics"]),
    ]
)
MANIFEST
cat > "$pkg/Sources/CoreGraphics/Shim.swift" <<'SOURCE'
@_exported import Foundation
public struct CGContext: Sendable { public init() {} }
SOURCE
cat > "$pkg/Sources/User/Use.swift" <<'SOURCE'
import CoreGraphics
// Both our own type and whatever Foundation re-exports have to be reachable through the one import.
public let context = CGContext()
public let rect = CGRect(x: 0, y: 0, width: 1, height: 1)
SOURCE
if output=$(cd "$pkg" && swift build 2>&1); then
    printf '  \033[32mPASS\033[0m  a target named CoreGraphics builds and is importable\n'
    pass=$((pass + 1))
else
    printf '  \033[31mFAIL\033[0m  a target named CoreGraphics builds and is importable\n'
    printf '%s\n' "$output" | grep -m3 'error:' | sed 's/^/          /'
    fail=$((fail + 1))
fi

echo
echo "== Summary: $pass passed, $fail failed =="
echo "(Failures are findings, not build breaks. This script always exits 0.)"
exit 0
