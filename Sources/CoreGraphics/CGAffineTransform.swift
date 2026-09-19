/// An affine transformation, matching CoreGraphics' conventions exactly.
///
/// The matrix is
///
///     | a  b  0 |
///     | c  d  0 |
///     | tx ty 1 |
///
/// and a point maps as `x' = a·x + c·y + tx`, `y' = b·x + d·y + ty`.
///
/// The composition order is the part that matters and the part that compiles cleanly when
/// it is wrong. `rotated(by:)`, `scaledBy(x:y:)` and `translatedBy(x:y:)` *pre*-concatenate:
/// the new step is applied to a point **before** the receiver. `concatenating(_:)`
/// post-concatenates: the receiver first, then the argument. Swapping them still builds and
/// breaks every layer placement in the app.
public struct CGAffineTransform: Equatable, Sendable {
    public var a: CGFloat
    public var b: CGFloat
    public var c: CGFloat
    public var d: CGFloat
    public var tx: CGFloat
    public var ty: CGFloat

    public init(a: CGFloat, b: CGFloat, c: CGFloat, d: CGFloat, tx: CGFloat, ty: CGFloat) {
        self.a = a; self.b = b; self.c = c; self.d = d; self.tx = tx; self.ty = ty
    }

    public init() {
        self.init(a: 1, b: 0, c: 0, d: 1, tx: 0, ty: 0)
    }

    public init(translationX tx: CGFloat, y ty: CGFloat) {
        self.init(a: 1, b: 0, c: 0, d: 1, tx: tx, ty: ty)
    }

    public init(scaleX sx: CGFloat, y sy: CGFloat) {
        self.init(a: sx, b: 0, c: 0, d: sy, tx: 0, ty: 0)
    }

    /// Positive angles rotate from the positive x-axis towards the positive y-axis. In the
    /// app's top-left, y-down document space that reads as clockwise on screen, which is
    /// what `LayerTransform` documents.
    public init(rotationAngle angle: CGFloat) {
        let sine = sin(angle), cosine = cos(angle)
        self.init(a: cosine, b: sine, c: -sine, d: cosine, tx: 0, ty: 0)
    }

    public static let identity = CGAffineTransform(a: 1, b: 0, c: 0, d: 1, tx: 0, ty: 0)

    public var isIdentity: Bool { self == .identity }

    /// `self` applied first, then `t2`.
    public func concatenating(_ t2: CGAffineTransform) -> CGAffineTransform {
        CGAffineTransform(a: a * t2.a + b * t2.c,
                          b: a * t2.b + b * t2.d,
                          c: c * t2.a + d * t2.c,
                          d: c * t2.b + d * t2.d,
                          tx: tx * t2.a + ty * t2.c + t2.tx,
                          ty: tx * t2.b + ty * t2.d + t2.ty)
    }

    /// Translation applied *before* the receiver.
    public func translatedBy(x: CGFloat, y: CGFloat) -> CGAffineTransform {
        CGAffineTransform(translationX: x, y: y).concatenating(self)
    }

    /// Scale applied *before* the receiver.
    public func scaledBy(x: CGFloat, y: CGFloat) -> CGAffineTransform {
        CGAffineTransform(scaleX: x, y: y).concatenating(self)
    }

    /// Rotation applied *before* the receiver.
    public func rotated(by angle: CGFloat) -> CGAffineTransform {
        CGAffineTransform(rotationAngle: angle).concatenating(self)
    }

    /// The inverse, or the receiver unchanged when it is singular — CoreGraphics returns the
    /// original rather than trapping, and `TiledLayerRenderer.snapped` relies on being able
    /// to call this on any context transform.
    public func inverted() -> CGAffineTransform {
        let determinant = a * d - b * c
        guard determinant != 0, determinant.isFinite else { return self }
        return CGAffineTransform(a: d / determinant,
                                 b: -b / determinant,
                                 c: -c / determinant,
                                 d: a / determinant,
                                 tx: (c * ty - d * tx) / determinant,
                                 ty: (b * tx - a * ty) / determinant)
    }
}

public extension CGPoint {
    func applying(_ t: CGAffineTransform) -> CGPoint {
        CGPoint(x: t.a * x + t.c * y + t.tx, y: t.b * x + t.d * y + t.ty)
    }
}

public extension CGRect {
    /// The bounding box of the transformed rectangle. Under rotation that is larger than the
    /// rectangle itself, which is what CoreGraphics does too.
    func applying(_ t: CGAffineTransform) -> CGRect {
        if isNull || isInfinite { return self }
        let corners = [
            CGPoint(x: minX, y: minY).applying(t),
            CGPoint(x: maxX, y: minY).applying(t),
            CGPoint(x: maxX, y: maxY).applying(t),
            CGPoint(x: minX, y: maxY).applying(t),
        ]
        let xs = corners.map(\.x), ys = corners.map(\.y)
        guard let left = xs.min(), let right = xs.max(),
              let top = ys.min(), let bottom = ys.max() else { return self }
        return CGRect(x: left, y: top, width: right - left, height: bottom - top)
    }
}
