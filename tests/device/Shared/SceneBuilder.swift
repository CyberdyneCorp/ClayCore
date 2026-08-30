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
    ///
    /// `group` places the dab inside a group rather than at the layer root,
    /// which is not a cosmetic difference: the compiler's append fast path
    /// (`tail_append`) requires a root-list parent, so a dab added into a
    /// group takes a structural invalidation and a full recompile instead.
    /// `sdf_stroke_in_group_bricks` is what measures that, against
    /// `sdf_stroke_bricks` which is the same stroke at the root.
    static func addStrokeDabNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                                 dab k: Int, group: clay_node_id? = nil) -> clay_node_id? {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = strokeDabPosition(k)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        var node: clay_node_id = 0
        if let group {
            guard clay_layer_add_item_in_group(doc, layer, group, -1, item, &node) == CLAY_OK
            else { return nil }
        } else {
            guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        }
        return node
    }

    /// The blend a SMOOTH-blended fixture uses.
    ///
    /// Every SDF fixture here was hard-blended until 2026-08-29, and the
    /// default `clay_item_create` blend is `Hard` with k = 0. A hard blend
    /// contributes NOTHING to the chain pad — `profile_chain_pad` returns 0
    /// for k <= 0 — so the pad resolved to a constant zero and the whole suite
    /// was blind to every effect that depends on it, however many cases it
    /// carried. The clay and build brushes are smooth by default, so the
    /// hard-blended document is also not the one a sculptor makes.
    static let smoothBlendK: Float = 0.05

    /// One stamp: a small blended sphere, which is what a stroke deposits.
    /// Returns the node it became, which the brick-cache case needs to dirty
    /// exactly the region the new item influences.
    ///
    /// `group` places the stamp inside a group instead of at the layer root.
    /// It defaults to nil, so every existing caller is unchanged — the grouped
    /// document the drag cases need differs from the flat one in that argument
    /// and in nothing else, which is what makes the two cases comparable.
    static func addStampNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                             index: Int, group: clay_node_id? = nil) -> clay_node_id? {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = stampPosition(index)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        var node: clay_node_id = 0
        if let group {
            guard clay_layer_add_item_in_group(doc, layer, group, -1, item, &node) == CLAY_OK
            else { return nil }
        } else {
            guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        }
        return node
    }

    /// Where frame `k` of a GIZMO DRAG places the node being dragged.
    ///
    /// A drag is not a stroke and it is not a spread. It walks: consecutive
    /// frames land near each other, so the region a frame dirties overlaps the
    /// one before it, and the brick store is asked the question a real drag
    /// asks — re-evaluate what moved, keep what did not.
    ///
    /// It must WALK rather than alternate between two placements. A two-point
    /// flip dirties the same pair of regions on every frame and the second
    /// frame can be served from what the first left behind, which measures a
    /// resume the artist never gets. `strokeDabPosition` exists for the same
    /// reason and this is its drag-shaped sibling: a short march, 0.02 a
    /// frame, well inside the working volume at every axis size and a step far
    /// under the dragged node's own radius so the reaches genuinely overlap.
    ///
    /// The path wraps at 32 frames so a long timed pass cannot walk the node
    /// out of the volume the document occupies — a node dragged into empty
    /// space stops dirtying anything, and the case would then time an empty
    /// loop while looking healthy.
    static func dragPosition(_ k: Int) -> (Float, Float, Float) {
        let step = k % 32
        return (-0.30 + 0.02 * Float(step), 0.10, 0.05)
    }

    /// The radius of the node a drag case drags.
    ///
    /// Larger than a stamp's 0.12 because a gizmo moves an OBJECT, not a dab,
    /// and because the region a drag dirties is what these cases are about: a
    /// node too small to reach past one brick would measure the store's
    /// bookkeeping rather than the refill.
    static let dragNodeRadius: Float = 0.25

    /// The node a drag case drags, added at `dragPosition(0)`.
    static func addDragNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                            group: clay_node_id? = nil) -> clay_node_id? {
        var params: [Float] = [dragNodeRadius]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = dragPosition(0)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        if clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.05) != CLAY_OK {
            return nil
        }
        var node: clay_node_id = 0
        if let group {
            guard clay_layer_add_item_in_group(doc, layer, group, -1, item, &node) == CLAY_OK
            else { return nil }
        } else {
            guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        }
        return node
    }

    /// The same document `sdfDocument` builds, with every item inside ONE
    /// group — which is what an artist leaves behind on a document worth
    /// grouping, and a different cost from the same items at the layer root.
    ///
    /// What an edit invalidates is derived from the edited node's ANCESTRY, not
    /// from the node, so the two shapes are separate cases rather than one case
    /// with an incidental fixture. Building the grouped form here rather than
    /// in the case keeps the two documents identical in everything else: same
    /// stamps, same positions, same radii, same blends.
    static func sdfDocumentGrouped(stamps count: Int)
        -> (OpaquePointer, clay_layer_id, clay_node_id)? {
        guard let doc = clay_document_create() else { return nil }
        var layer: clay_layer_id = 0
        guard clay_add_sdf_layer(doc, "bench", &layer) == CLAY_OK else {
            clay_document_destroy(doc); return nil
        }
        var group: clay_node_id = 0
        guard clay_layer_add_group(doc, layer, 0, -1,
                                   Int32(CLAY_OP_ADD.rawValue),
                                   Int32(CLAY_BLEND_QUADRATIC.rawValue),
                                   0.05, 0.0, &group) == CLAY_OK else {
            clay_document_destroy(doc); return nil
        }
        for i in 0..<count {
            guard addStampNode(doc, layer, index: i, group: group) != nil else {
                clay_document_destroy(doc); return nil
            }
        }
        return (doc, layer, group)
    }

    /// The same dab, carrying a smooth blend. Beside the hard-blended one
    /// rather than replacing it: both are worth measuring and neither stands
    /// for the other.
    static func addSmoothStrokeDabNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                                       dab k: Int) -> clay_node_id? {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = strokeDabPosition(k)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        if clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), smoothBlendK)
            != CLAY_OK { return nil }
        var node: clay_node_id = 0
        guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        return node
    }

    /// A document of `count` SMOOTH-blended stamps.
    static func smoothSdfDocument(stamps count: Int) -> (OpaquePointer, clay_layer_id)? {
        guard let doc = clay_document_create() else { return nil }
        var layer: clay_layer_id = 0
        guard clay_add_sdf_layer(doc, "bench", &layer) == CLAY_OK else {
            clay_document_destroy(doc); return nil
        }
        for i in 0..<count {
            var params: [Float] = [0.12]
            guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
                clay_document_destroy(doc); return nil
            }
            let (x, y, z) = stampPosition(i)
            var position: [Float] = [x, y, z]
            let ok = clay_item_set_position(item, &position) == CLAY_OK
                && clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue),
                                       smoothBlendK) == CLAY_OK
                && clay_layer_add_item(doc, layer, item, nil) == CLAY_OK
            clay_item_destroy(item)
            guard ok else { clay_document_destroy(doc); return nil }
        }
        return (doc, layer)
    }

    @discardableResult
    static func addStamp(_ doc: OpaquePointer, _ layer: clay_layer_id, index: Int) -> Bool {
        addStampNode(doc, layer, index: index) != nil
    }
}
