// Shared by both device test bundles: the gallery needs the same scene
// spread the latency cases use, and a type cannot live in one bundle and
// be referenced from the other.

import Foundation
import XCTest
import claycore

enum SceneBuilder {

    /// Points a host would evaluate to redraw the region a stamp touched.
    /// A cube lattice rather than a random cloud: coherent access is what a
    /// brick refresh actually does, and a random cloud would measure cache
    /// misses the real path does not take.
    static func drawLattice(side: Int = 12, extent: Float = 1.0) -> [Float] {
        var points: [Float] = []
        points.reserveCapacity(side * side * side * 3)
        for i in 0..<side {
            for j in 0..<side {
                for k in 0..<side {
                    let f = { (n: Int) in Float(n) / Float(side - 1) * 2 * extent - extent }
                    points.append(f(i)); points.append(f(j)); points.append(f(k))
                }
            }
        }
        return points
    }

    /// Deterministic positions spread through the working volume, so a
    /// document of N stamps is the same document on every run and on every
    /// device. A random spread would make two runs incomparable.
    /// The three multipliers are the fractional parts of sqrt(2), sqrt(3) and
    /// sqrt(5), and the choice is load-bearing rather than decorative.
    ///
    /// This used to walk `g`, `g*g`, `g*g*g` for the golden ratio, described as
    /// "a low-discrepancy-ish walk". It was not one. `g` satisfies `g^2 = 1-g`,
    /// so `frac(i*g^2) == 1 - frac(i*g)` and therefore **y was exactly -x for
    /// every stamp**: the whole cloud lay on the plane x+y=0. `g^3 = 2g-1` tied
    /// z to x as well. The document was a curve in a plane, not a volume, and
    /// since culling cost is entirely a question of how many item bounds
    /// overlap a region, a sheet measures a density no sculpt produces —
    /// ~5,300 survivors per brick against ~200 for a real spread at 50k items.
    ///
    /// The powers of ANY algebraic number of low degree carry a relation like
    /// that; the plastic number's own powers fail the same way (`a^2 + a^3 = 1`
    /// exactly, so z would be -y). Distinct square-free radicands cannot: 1,
    /// sqrt(2), sqrt(3) and sqrt(5) are linearly independent over the
    /// rationals, so no relation of that shape exists to be tripped over.
    ///
    /// `testStampSpreadFillsTheVolume` holds this, and is the only reason the
    /// property is not one comment away from regressing again.
    static let stampMultipliers = (0.4142135624, 0.7320508076, 0.2360679775)

    static func stampPosition(_ index: Int) -> (Float, Float, Float) {
        let (mx, my, mz) = stampMultipliers
        let x = Float((Double(index) * mx).truncatingRemainder(dividingBy: 1.0)) * 1.6 - 0.8
        let y = Float((Double(index) * my).truncatingRemainder(dividingBy: 1.0)) * 1.6 - 0.8
        let z = Float((Double(index) * mz).truncatingRemainder(dividingBy: 1.0)) * 1.6 - 0.8
        return (x, y, z)
    }

    /// A document carrying `count` SDF stamps, as a stroke would leave it.
    static func sdfDocument(stamps count: Int) -> (OpaquePointer, clay_layer_id)? {
        guard let doc = clay_document_create() else { return nil }
        var layer: clay_layer_id = 0
        guard clay_add_sdf_layer(doc, "bench", &layer) == CLAY_OK else {
            clay_document_destroy(doc); return nil
        }
        for i in 0..<count {
            guard addStamp(doc, layer, index: i) else {
                clay_document_destroy(doc); return nil
            }
        }
        return (doc, layer)
    }

    /// Where dab `k` of a STROKE lands.
    ///
    /// Deliberately NOT `stampPosition`. That one fills the working volume on
    /// purpose — a spread is what the growth axis needs, and a spread that
    /// collapses onto a plane calibrates the budgets against a workload nobody
    /// runs. But a spread is not a stroke: consecutive dabs land nowhere near
    /// each other, so each one dirties a fresh set of bricks.
    ///
    /// That distinction is invisible to `sdf_stroke_cpu`/`_metal`, which
    /// measure the tape path, where an append is an append wherever it lands.
    /// It decides everything for the BRICK path: the resumed refill continues
    /// from the accumulator a previous refill of THAT BRICK left, so it fires
    /// when successive dabs overlap and does not when they scatter.
    ///
    /// Measured on an M2 Max through the C ABI, one dab of 24, per dab:
    ///
    ///   dabs scattered by stampPosition   1.36 -> 1.29 ms   1.05x
    ///   dabs along this path              1.80 -> 0.24 ms   7.6x
    ///
    /// at 1000 items, and 8.38 -> 0.87 (9.7x) at 5000 with the path. A case
    /// built on the spread would have reported the resumed refill as worth
    /// nothing and passed, which is the shape of gate this suite exists to
    /// stop shipping.
    ///
    /// A short march rather than a long one: 24 dabs 0.025 apart is 0.6 of a
    /// unit, comfortably inside the working volume at every axis size, and a
    /// step well under the 0.12 dab radius so consecutive dabs genuinely
    /// overlap the way a drag does.
    static func strokeDabPosition(_ k: Int) -> (Float, Float, Float) {
        (-0.30 + 0.025 * Float(k), 0.10, 0.05)
    }

    /// One dab of a stroke, at `strokeDabPosition(k)`.
    static func addStrokeDabNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                                 dab k: Int) -> clay_node_id? {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = strokeDabPosition(k)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        var node: clay_node_id = 0
        guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        return node
    }

    /// One stamp: a small blended sphere, which is what a stroke deposits.
    /// Returns the node it became, which the brick-cache case needs to dirty
    /// exactly the region the new item influences.
    static func addStampNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                             index: Int) -> clay_node_id? {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = stampPosition(index)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        var node: clay_node_id = 0
        guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        return node
    }

    @discardableResult
    static func addStamp(_ doc: OpaquePointer, _ layer: clay_layer_id, index: Int) -> Bool {
        addStampNode(doc, layer, index: index) != nil
    }
}
