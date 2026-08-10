// The parity corpus, on the iPad.
//
// The C++ suite in tests/unit/test_parity.cpp checks every registered backend
// against the scalar reference — but only where it runs, which is a developer's
// Mac and a CI runner that may expose no GPU at all. Metal is the iPad app's
// production path, and until this ran nothing had ever compared it against the
// CPU *on the hardware it ships to*.
//
// It also covers something the C++ corpus structurally cannot: scenes authored
// BY THE BRUSHES. A hand-written corpus proves the opcodes agree. It does not
// prove that what a stroke, a cut or a mask extrude actually emits is inside
// the set that was checked.

import Foundation
import XCTest
import claycore

final class DeviceParityTests: XCTestCase {

    /// The tolerance `evaluation-backends` documents for GPU backends.
    private let tolerance: Float = 1e-4

    private func registeredBackends() -> [String] {
        var size = 0
        guard clay_list_backends(nil, &size) == CLAY_OK else { return [] }
        var buffer = [CChar](repeating: 0, count: size)
        guard clay_list_backends(&buffer, &size) == CLAY_OK else { return [] }
        return String(cString: buffer).split(separator: ",").map(String.init)
    }

    /// Probe points spread through and well outside the shape. "Well outside"
    /// matters for the transitions, whose weight reaches arbitrarily far and
    /// which are never culled for exactly that reason.
    private func probes(extent: Float = 2.0, count: Int = 512) -> [Float] {
        var points: [Float] = []
        points.reserveCapacity(count * 3)
        var state: UInt64 = 0x9E3779B97F4A7C15
        for _ in 0..<(count * 3) {
            state = state &* 6364136223846793005 &+ 1442695040888963407
            let u = Float((state >> 40) & 0xFFFFFF) / Float(0x1000000)
            points.append((u * 2 - 1) * extent)
        }
        return points
    }

    /// Compare every registered GPU backend against the CPU on one document.
    private func assertParity(_ doc: OpaquePointer, _ name: String,
                              extent: Float = 2.0,
                              file: StaticString = #filePath, line: UInt = #line) {
        let points = probes(extent: extent)
        let count = points.count / 3
        var pts = points

        var cpu = [Float](repeating: 0, count: count)
        XCTAssertEqual(clay_eval_points(doc, "cpu", &pts, count, &cpu, nil), CLAY_OK,
                       "\(name): CPU evaluation failed", file: file, line: line)

        for backend in registeredBackends() where backend != "cpu" {
            var got = [Float](repeating: 0, count: count)
            XCTAssertEqual(clay_eval_points(doc, backend, &pts, count, &got, nil), CLAY_OK,
                           "\(name): \(backend) evaluation failed", file: file, line: line)
            var worst: Float = 0
            var worstAt = 0
            for i in 0..<count {
                // the suite's own scale-guarded relative error
                let scale = max(max(abs(cpu[i]), abs(got[i])), 1.0)
                let err = abs(cpu[i] - got[i]) / scale
                if err > worst { worst = err; worstAt = i }
            }
            XCTAssertLessThanOrEqual(
                worst, tolerance,
                "\(name): \(backend) disagrees with cpu by \(worst) at point "
                + "\(worstAt) (cpu \(cpu[worstAt]) vs \(got[worstAt]))",
                file: file, line: line)
        }
    }

    /// Build a one-layer document, hand it to `body` to populate, then check.
    private func withDocument(_ name: String, extent: Float = 2.0,
                              _ body: (OpaquePointer, clay_layer_id) -> Bool) {
        guard let doc = clay_document_create() else {
            return XCTFail("\(name): could not create a document")
        }
        defer { clay_document_destroy(doc) }
        var layer: clay_layer_id = 0
        guard clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK else {
            return XCTFail("\(name): could not add a layer")
        }
        guard body(doc, layer) else {
            return XCTFail("\(name): could not build the scene")
        }
        assertParity(doc, name, extent: extent)
    }

    private func addItem(_ doc: OpaquePointer, _ layer: clay_layer_id,
                         prim: clay_prim, params: [Float],
                         position: [Float] = [0, 0, 0],
                         configure: ((OpaquePointer) -> Void)? = nil) -> Bool {
        var p = params
        guard let item = clay_item_create(Int32(prim.rawValue), &p, p.count) else { return false }
        defer { clay_item_destroy(item) }
        var pos = position
        guard clay_item_set_position(item, &pos) == CLAY_OK else { return false }
        configure?(item)
        var node: clay_node_id = 0
        return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
    }

    // MARK: - Every primitive

    func testEveryPrimitiveAgreesOnDevice() throws {
        try XCTSkipUnless(registeredBackends().contains("metal"), "no GPU backend on device")

        // Same spread the Swift smoke walks, so the two stay in step.
        let primitives: [(clay_prim, [Float])] = [
            (CLAY_PRIM_SPHERE, [0.8]),
            (CLAY_PRIM_BOX, [0.6, 0.5, 0.4]),
            (CLAY_PRIM_ROUND_BOX, [0.5, 0.4, 0.3, 0.08]),
            (CLAY_PRIM_BOX_FRAME, [0.6, 0.5, 0.4, 0.08]),
            (CLAY_PRIM_TORUS, [0.7, 0.2]),
            (CLAY_PRIM_CAPSULE, [-0.4, 0, 0, 0.4, 0.2, 0, 0.25]),
            (CLAY_PRIM_CAPPED_CYLINDER, [0.4, 0.6]),
            (CLAY_PRIM_ROUNDED_CYLINDER, [0.4, 0.1, 0.6]),
            (CLAY_PRIM_CAPPED_CONE, [0.6, 0.5, 0.2]),
            (CLAY_PRIM_ROUND_CONE, [0.3, 0.12, 0.7]),
            (CLAY_PRIM_ELLIPSOID, [0.7, 0.4, 0.5]),
            (CLAY_PRIM_OCTAHEDRON, [0.7]),
            (CLAY_PRIM_HEX_PRISM, [0.5, 0.3]),
            (CLAY_PRIM_PYRAMID, [0.8]),
        ]

        for (prim, params) in primitives {
            withDocument("prim_\(prim.rawValue)") { doc, layer in
                addItem(doc, layer, prim: prim, params: params)
            }
        }
    }

    // MARK: - Every combine op and blend profile

    func testEveryCombineOpAgreesOnDevice() throws {
        try XCTSkipUnless(registeredBackends().contains("metal"), "no GPU backend on device")

        let ops: [(clay_op, Float, Float)] = [   // op, blend k, rounding
            (CLAY_OP_ADD, 0.12, 0),
            (CLAY_OP_SUBTRACT, 0.12, 0),
            (CLAY_OP_INTERSECT, 0.12, 0),
            (CLAY_OP_PAINT, 0.10, 0),
            (CLAY_OP_GROOVE, 0.18, 0.09),   // consumes rounding as channel half-width
            (CLAY_OP_TONGUE, 0.18, 0.09),
            (CLAY_OP_PIPE, 0.14, 0),
            (CLAY_OP_ENGRAVE, 0.14, 0),
            (CLAY_OP_EMBOSS, 0.14, 0),
            (CLAY_OP_INSET, 0.12, 0),
            (CLAY_OP_SHELL, 0.10, 0),
            (CLAY_OP_REPLACE, 0.10, 0),
            (CLAY_OP_RELIEF, 0.13, 0.20),
            (CLAY_OP_INCISE, 0.13, 0.20),
        ]

        for (op, k, rounding) in ops {
            withDocument("op_\(op.rawValue)") { doc, layer in
                guard addItem(doc, layer, prim: CLAY_PRIM_SPHERE, params: [0.9],
                              position: [-0.15, 0, 0]) else { return false }
                return addItem(doc, layer, prim: CLAY_PRIM_BOX, params: [0.55, 0.75, 0.5],
                               position: [0.35, 0.1, 0.1]) { item in
                    _ = clay_item_set_op(item, Int32(op.rawValue))
                    _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), k)
                    _ = clay_item_set_rounding(item, rounding)
                }
            }
        }

        // The transitions are NON-LOCAL: their weight reaches arbitrarily far,
        // so they are probed over a wider extent than anything else here.
        for radial in [false, true] {
            withDocument("transition_\(radial ? "radial" : "linear")", extent: 3.0) { doc, layer in
                guard addItem(doc, layer, prim: CLAY_PRIM_SPHERE, params: [0.8],
                              position: [0, -0.5, 0]) else { return false }
                return addItem(doc, layer, prim: CLAY_PRIM_BOX, params: [0.6, 0.6, 0.6],
                               position: [0, 0.5, 0]) { item in
                    if radial {
                        _ = clay_item_set_op(item, Int32(CLAY_OP_TRANSITION_RADIAL.rawValue))
                        _ = clay_item_set_transition_radial(item, 0.2, 1.4, 3)
                    } else {
                        _ = clay_item_set_op(item, Int32(CLAY_OP_TRANSITION_LINEAR.rawValue))
                        var a: [Float] = [0, -1, 0], b: [Float] = [0, 1, 0]
                        _ = clay_item_set_transition_linear(item, &a, &b, 3)
                    }
                }
            }
        }

        for blend in [CLAY_BLEND_HARD, CLAY_BLEND_QUADRATIC, CLAY_BLEND_CUBIC,
                      CLAY_BLEND_CIRCULAR, CLAY_BLEND_CHAMFER] {
            withDocument("blend_\(blend.rawValue)") { doc, layer in
                guard addItem(doc, layer, prim: CLAY_PRIM_SPHERE, params: [0.8],
                              position: [-0.4, 0, 0]) else { return false }
                return addItem(doc, layer, prim: CLAY_PRIM_BOX, params: [0.6, 0.5, 0.7],
                               position: [0.5, 0.2, 0]) { item in
                    _ = clay_item_set_blend(item, Int32(blend.rawValue), 0.15)
                }
            }
        }
    }

    // MARK: - Every deformer

    func testEveryDeformerAgreesOnDevice() throws {
        try XCTSkipUnless(registeredBackends().contains("metal"), "no GPU backend on device")

        let deformers: [(clay_deform, [Float], clay_prim, [Float])] = [
            (CLAY_DEFORM_TWIST, [1.1], CLAY_PRIM_BOX, [0.5, 1.0, 0.5]),
            (CLAY_DEFORM_BEND, [0.9], CLAY_PRIM_BOX, [1.0, 0.35, 0.35]),
            (CLAY_DEFORM_TAPER, [-1.0, 1.0, 1.3, 0.4], CLAY_PRIM_CAPPED_CYLINDER, [0.5, 1.0]),
            (CLAY_DEFORM_DISPLACE, [0.08, 4.0], CLAY_PRIM_SPHERE, [0.9]),
            (CLAY_DEFORM_WRAP_AROUND, [-3.14159, 3.14159], CLAY_PRIM_BOX, [3.14, 0.2, 0.5]),
            (CLAY_DEFORM_ELONGATE, [0.8, 0.0, 0.3], CLAY_PRIM_SPHERE, [0.5]),
            (CLAY_DEFORM_ELONGATE_AXIS, [0.7, 0.0, 0.3], CLAY_PRIM_CAPPED_CONE, [0.6, 0.5, 0.1]),
            (CLAY_DEFORM_BEND_RADIAL, [0.2, 1.2, 0.6], CLAY_PRIM_CAPPED_CYLINDER, [1.2, 0.15]),
            (CLAY_DEFORM_GRAB, [1.0, 0, 0, 0.8, 0.4, 0.2, 0], CLAY_PRIM_SPHERE, [1.0]),
            (CLAY_DEFORM_POSE, [0, 0.8, 0, 1.0, 0, 0, 1, 0.7],
             CLAY_PRIM_CAPPED_CYLINDER, [0.3, 1.0]),
            (CLAY_DEFORM_MAGNIFY, [0.3, 0.2, 0, 0.7, 0.45],
             CLAY_PRIM_ROUND_BOX, [0.5, 0.4, 0.5, 0.12]),
            (CLAY_DEFORM_NOISE, [0.09, 5.0, 4, 0.5, 17], CLAY_PRIM_SPHERE, [0.8]),
        ]

        for (deform, params, prim, primParams) in deformers {
            withDocument("deform_\(deform.rawValue)") { doc, layer in
                addItem(doc, layer, prim: prim, params: primParams) { item in
                    var p = params
                    _ = clay_item_add_deformer(item, Int32(deform.rawValue), &p, p.count, 3)
                }
            }
        }
    }

    // MARK: - Scenes the brushes authored

    /// The gap a hand-written corpus cannot close: what a brush actually emits.
    func testBrushAuthoredScenesAgreeOnDevice() throws {
        try XCTSkipUnless(registeredBackends().contains("metal"), "no GPU backend on device")

        // A stroke, as the stroke engine leaves it: spaced stamps blended
        // along a drag.
        withDocument("authored_stroke") { doc, layer in
            var preset = Fixture.strokePreset(radius: 0.2)
            var samples = Fixture.strokeSamples(count: 24)
            let sampleCount = samples.count / 5
            var capacity = 0
            guard clay_stroke_resolve(&samples, sampleCount, &preset, nil, &capacity) == CLAY_OK,
                  capacity > 0 else { return false }
            var stamps = [clay_stamp](repeating: clay_stamp(), count: capacity)
            guard clay_stroke_resolve(&samples, sampleCount, &preset,
                                      &stamps, &capacity) == CLAY_OK else { return false }
            // Every stamp becomes an ordinary item, which is the engine's
            // whole claim about strokes: resolution is pure, and a stamp is a
            // plain edit-list node.
            for stamp in stamps.prefix(capacity) {
                let ok = addItem(doc, layer, prim: CLAY_PRIM_SPHERE,
                                 params: [stamp.radius],
                                 position: [stamp.position.0, stamp.position.1,
                                            stamp.position.2]) { item in
                    _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.05)
                }
                if !ok { return false }
            }
            return true
        }

        // A cut, as the cut tool resolves it: a drawn shape swept into a prism.
        withDocument("authored_cut") { doc, layer in
            guard addItem(doc, layer, prim: CLAY_PRIM_BOX,
                          params: [0.7, 0.7, 0.7]) else { return false }
            var desc = clay_cut_desc()
            desc.struct_size = UInt32(MemoryLayout<clay_cut_desc>.size)
            desc.origin = (0, 0, -1.5)
            desc.right = (1, 0, 0)
            desc.up = (0, 1, 0)
            desc.forward = (0, 0, 1)
            desc.shape = Int32(CLAY_CUT_CIRCLE.rawValue)
            desc.radius = 0.35
            desc.half_width = 0.35
            desc.half_height = 0.35
            desc.rounding = 0.05
            desc.region_min = (-1, -1, -1)
            desc.region_max = (1, 1, 1)
            guard let cut = clay_cut_create(&desc, nil, 0) else { return false }
            defer { clay_item_destroy(cut) }
            _ = clay_item_set_op(cut, Int32(CLAY_OP_SUBTRACT.rawValue))
            var node: clay_node_id = 0
            return clay_layer_add_item(doc, layer, cut, &node) == CLAY_OK
        }

        // A mask extrude: a painted patch pulled off as a solid, which lands
        // as a sampled VOLUME — the sparse narrow-band structure every backend
        // has to walk identically.
        withDocument("authored_mask_extrude") { doc, layer in
            guard addItem(doc, layer, prim: CLAY_PRIM_SPHERE, params: [0.8]),
                  let mask = clay_mask_create(0.04) else { return false }
            defer { _ = clay_mask_destroy(mask) }
            // Paint ON the surface, not inside it. Mask extrude pulls the
            // masked patch of the SOURCE SURFACE off as a solid, so a mask
            // that only covers interior cells describes no patch and the call
            // is refused — which is a valid refusal, and useless as a scene.
            var brush = Fixture.brush(size: 6)
            let radius: Float = 0.8
            for i in 0..<24 {
                let (x, y, z) = SceneBuilder.stampPosition(i)
                let length = max(sqrt(x * x + y * y + z * z), 1e-6)
                var point: [Float] = [x / length * radius,
                                      y / length * radius,
                                      z / length * radius]
                _ = clay_mask_paint(mask, &point, &brush, 1.0)
            }
            var params = clay_mask_extrude_params()
            params.struct_size = UInt32(MemoryLayout<clay_mask_extrude_params>.size)
            params.thickness = 0.06
            params.threshold = 0.5
            params.cell_size = 0.04
            var item: OpaquePointer?
            guard clay_document_mask_extrude(doc, layer, mask, &params, &item) == CLAY_OK,
                  let item else { return false }
            defer { clay_item_destroy(item) }
            var node: clay_node_id = 0
            return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
        }
    }
}
