// The heavy half of the verb cases: mask_extrude through armature_edit.
//
// Its own bundle, and therefore its own process, because together with the
// light half it exceeded this iPad's jetsam limit. See
// Shared/VerbHarness.swift for the measurements that led here.

import Foundation
import XCTest
import claycore

final class VerbHeavyLatencyTests: VerbCaseGroup {

    /// The volume bakes and the authoring verbs, in one run so they share a
    /// thermal window.
    func testEveryHeavyVerbOnDevice() throws {
        collector = RunCollector()

        measureAxis(name: "mask_extrude", verb: "document_mask_extrude",
                    .operation) { stamps in
            // Mask extrude pulls the masked patch of the SURFACE off as a
            // solid, so the mask has to reach the surface — and where the
            // surface IS depends on the document. Stamps spread over the
            // working volume merge into a blob as their count rises, which
            // put the stamp centres deep inside it and got the verb refused
            // at 100 and 1000 ("the mask does not reach the surface").
            //
            // So this fixture keeps a shell of known radius and packs the
            // stamps well inside it: the document still grows along the axis,
            // the surface stays where the mask is painted, and what is
            // measured is the verb rather than a refusal.
            guard let doc = clay_document_create(),
                  let mask = clay_mask_create(0.04) else { return nil }
            var layer: clay_layer_id = 0
            guard clay_add_sdf_layer(doc, "bench", &layer) == CLAY_OK else {
                clay_document_destroy(doc); return nil
            }
            let shellRadius: Float = 0.8
            var shellParams: [Float] = [shellRadius]
            guard let shell = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue),
                                               &shellParams, 1) else {
                clay_document_destroy(doc); return nil
            }
            var shellNode: clay_node_id = 0
            _ = clay_layer_add_item(doc, layer, shell, &shellNode)
            clay_item_destroy(shell)
            for i in 0..<stamps {
                var p: [Float] = [0.12]
                guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue),
                                                  &p, 1) else { break }
                let (x, y, z) = SceneBuilder.stampPosition(i)
                var pos: [Float] = [x * 0.4, y * 0.4, z * 0.4]  // inside the shell
                _ = clay_item_set_position(item, &pos)
                var node: clay_node_id = 0
                _ = clay_layer_add_item(doc, layer, item, &node)
                clay_item_destroy(item)
            }
            var b = Fixture.brush(size: 6)
            for i in 0..<24 {
                let (x, y, z) = SceneBuilder.stampPosition(i)
                let length = max(sqrt(x * x + y * y + z * z), 1e-6)
                var point: [Float] = [x / length * shellRadius,
                                      y / length * shellRadius,
                                      z / length * shellRadius]
                _ = clay_mask_paint(mask, &point, &b, 1.0)
            }
            var params = clay_mask_extrude_params()
            params.struct_size = UInt32(MemoryLayout<clay_mask_extrude_params>.size)
            params.thickness = 0.05
            params.threshold = 0.5
            params.cell_size = 0.04

            // Verify the verb actually SUCCEEDS before timing it. A mask that
            // covers no surface is refused, and a refusal returns fast — so
            // an unchecked case here would report the cost of an error path
            // as the cost of the work.
            var trial: OpaquePointer?
            let rc = clay_document_mask_extrude(doc, layer, mask, &params, &trial)
            if let trial { clay_item_destroy(trial) }
            let detail = clay_last_error().map { String(cString: $0) } ?? "<none>"
            XCTAssertEqual(rc, CLAY_OK,
                           "mask_extrude was refused at \(stamps) stamps (\(detail)); "
                           + "the timing below would be the refusal, not the verb")

            return ({
                var item: OpaquePointer?
                if clay_document_mask_extrude(doc, layer, mask, &params, &item) == CLAY_OK,
                   let item { clay_item_destroy(item) }
            }, nil, {
                _ = clay_mask_destroy(mask)
                clay_document_destroy(doc)
            })
        }

        // -- the SDF region verbs ------------------------------------------------

        // relax and flatten REWRITE the volume's stored samples in place, so
        // iteration 2 smooths an already-smoothed field. Rebuilding the volume
        // between iterations (untimed) is what keeps every sample measuring
        // the same operation on the same input.
        measureAxis(name: "sdf_relax", verb: "item_volume_relax", .operation) { stamps in
            guard var item = Fixture.volumeItem(stamps: stamps) else { return nil }
            var params = clay_relax_params()
            params.struct_size = UInt32(MemoryLayout<clay_relax_params>.size)
            params.strength = 0.5
            params.radius_cells = 1
            params.iterations = 1
            params.centre = (0, 0, 0)
            params.region_radius = 0.4
            params.falloff = 0.1
            return ({ _ = clay_item_volume_relax(item, &params) },
                    {
                        // build the replacement BEFORE releasing the original:
                        // a failed rebuild would otherwise leave a destroyed
                        // pointer in `item`, which is a use-after-free on the
                        // next iteration and a double free at cleanup
                        if let fresh = Fixture.volumeItem(stamps: stamps) {
                            clay_item_destroy(item)
                            item = fresh
                        }
                    },
                    { clay_item_destroy(item) })
        }

        measureAxis(name: "sdf_flatten", verb: "item_volume_flatten", .operation) { stamps in
            guard var item = Fixture.volumeItem(stamps: stamps) else { return nil }
            var params = clay_flatten_params()
            params.struct_size = UInt32(MemoryLayout<clay_flatten_params>.size)
            params.plane_point = (0, 0, 0)
            params.plane_normal = (0, 1, 0)
            params.strength = 0.5
            params.centre = (0, 0, 0)
            params.region_radius = 0.4
            params.falloff = 0.1
            params.mode = Int32(CLAY_FLATTEN_TWO_SIDED.rawValue)
            return ({ _ = clay_item_volume_flatten(item, &params) },
                    {
                        // build the replacement BEFORE releasing the original:
                        // a failed rebuild would otherwise leave a destroyed
                        // pointer in `item`, which is a use-after-free on the
                        // next iteration and a double free at cleanup
                        if let fresh = Fixture.volumeItem(stamps: stamps) {
                            clay_item_destroy(item)
                            item = fresh
                        }
                    },
                    { clay_item_destroy(item) })
        }

        // Move is a GESTURE, not a stamp: a drag is resolved as one unit, and
        // the drag coalescing that `add-move-drag-continuity` added exists
        // because treating each frame as its own move stacked a warp per frame.
        // Move APPENDS a warp to every item its region reaches, so iteration N
        // drags a chain N deep — which is the exact degradation
        // add-move-drag-continuity exists to stop, and measuring it here would
        // report that decay as the cost of one drag. Rebuild between samples.
        measureAxis(name: "sdf_move", verb: "layer_move_surface", .gesture) { stamps in
            guard var built = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
            var params = clay_move_params()
            params.struct_size = UInt32(MemoryLayout<clay_move_params>.size)
            params.radius = 0.4
            params.ease = 0
            params.front_only = 0
            var centre: [Float] = [0, 0, 0]
            var displacement: [Float] = [0.05, 0, 0]
            return ({
                var applied = 0
                _ = clay_layer_move_surface(built.0, built.1, &centre, &displacement,
                                            &params, &applied)
            },
            {
                // as above: a failed rebuild must not leave a destroyed
                // document behind
                if let fresh = SceneBuilder.sdfDocument(stamps: stamps) {
                    clay_document_destroy(built.0)
                    built = fresh
                }
            },
            { clay_document_destroy(built.0) })
        }

        // consolidate COLLAPSES the layer into one volume, so every iteration
        // after the first consolidates an already-consolidated layer — a
        // different and much cheaper operation. The whole document is rebuilt
        // between iterations, untimed, so each sample measures what the verb
        // costs on a real chain.
        measureAxis(name: "sdf_consolidate", verb: "layer_consolidate",
                    .operation) { stamps in
            guard var built = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
            var params = clay_consolidation_params()
            params.struct_size = UInt32(MemoryLayout<clay_consolidation_params>.size)
            params.cell_size = 0.05
            params.band = 0
            params.padding = 0
            // Not skipped: redistancing is what actually bounds the Lipschitz,
            // and skipping it would measure the cheap-and-unsound path.
            params.skip_redistance = 0
            return ({
                _ = clay_layer_consolidate(built.0, built.1, &params, nil, nil, nil)
            },
            {
                // as above: a failed rebuild must not leave a destroyed
                // document behind
                if let fresh = SceneBuilder.sdfDocument(stamps: stamps) {
                    clay_document_destroy(built.0)
                    built = fresh
                }
            },
            { clay_document_destroy(built.0) })
        }

        // -- the cut tool ----------------------------------------------------------

        measureAxis(name: "cut_create", verb: "cut_create", .gesture, batch: 2048) { _ in
            var desc = clay_cut_desc()
            desc.struct_size = UInt32(MemoryLayout<clay_cut_desc>.size)
            desc.origin = (0, 0, -1)
            desc.right = (1, 0, 0)
            desc.up = (0, 1, 0)
            desc.forward = (0, 0, 1)
            desc.shape = Int32(CLAY_CUT_RECT.rawValue)
            desc.half_width = 0.3
            desc.half_height = 0.3
            desc.radius = 0.3
            desc.rounding = 0
            desc.region_min = (-1, -1, -1)
            desc.region_max = (1, 1, 1)
            return ({
                if let item = clay_cut_create(&desc, nil, 0) { clay_item_destroy(item) }
            }, nil, {})
        }

        // Trim Curve: an OPEN stroke closed against the frame bounds. Two
        // calls, as a host makes them — size query, then tessellate — because
        // the size query is not free and a host cannot skip it.
        measureAxis(name: "trim_curve", verb: "cut_polygon_from_open_curve", .gesture,
                    batch: 4096) { _ in
            var points = Fixture.curvePoints()
            var types = [Int32](repeating: Int32(CLAY_POINT_SPLINE.rawValue),
                                count: points.count / 4)
            var extent: [Float] = [3.0, 3.0]
            let side = Int32(CLAY_TRIM_BELOW.rawValue)
            return ({
                var count = 0
                guard clay_cut_polygon_from_open_curve(&points, types.count, &types,
                                                       side, &extent, 0,
                                                       nil, &count) == CLAY_OK,
                      count > 0 else { return }
                var xy = [Float](repeating: 0, count: count * 2)
                _ = clay_cut_polygon_from_open_curve(&points, types.count, &types,
                                                     side, &extent, 0, &xy, &count)
            }, nil, {})
        }

        // -- the resolvers that turn a gesture into an item ------------------------

        // Nomad's Tube, and the swept-sphere half of SnakeHook. Round rather
        // than profiled: with no profile the tube is an exact distance field,
        // which is the configuration a host reaches for first.
        measureAxis(name: "tube_create", verb: "tube_create", .gesture, batch: 512) { _ in
            var path = Fixture.tubePath()
            var params = clay_tube_params()
            params.struct_size = UInt32(MemoryLayout<clay_tube_params>.size)
            params.point_type = Int32(CLAY_POINT_SPLINE.rawValue)
            params.radius_start = 0.05
            params.radius_mid = 0.12
            params.radius_end = 0.03
            params.closed = 0
            params.tolerance = 0
            params.blend_k = 0.02
            let count = path.count / 3
            return ({
                if let item = clay_tube_create(&path, count, &params, -1, nil, 0) {
                    clay_item_destroy(item)
                }
            }, nil, {})
        }

        // ZBrush's Rotate. A deformer rather than an entry point of its own, so
        // the timed body is what a host does per drag: build the item, put the
        // warp at the FRONT of its chain, place it.
        // The batched body needs a reset the single one did not. Each iteration
        // PLACES a node, so 512 of them per timed body would grow the document
        // by 512 while the axis point still claimed to name `stamps` — which is
        // the failure Timing.measure's own warm-up note describes. Rolled back
        // untimed between samples instead, so every sample starts at the size
        // its axis point names.
        measureAxis(name: "pose_region", verb: "pose", .gesture, batch: 512) { stamps in
            guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
            // centre(3), radius, axis(3), angle
            var pose: [Float] = [0, 0, 0, 0.6, 0, 1, 0, 0.4]
            var radius: Float = 0.35
            var placed: [clay_node_id] = []
            return ({
                guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue),
                                                  &radius, 1) else { return }
                defer { clay_item_destroy(item) }
                _ = clay_item_add_deformer(item, Int32(CLAY_DEFORM_POSE.rawValue),
                                           &pose, pose.count, CLAY_EASE_LINEAR)
                var node: clay_node_id = 0
                if clay_layer_add_item(doc, layer, item, &node) == CLAY_OK {
                    placed.append(node)
                }
            }, {
                for node in placed.reversed() { _ = clay_remove_node(doc, layer, node) }
                placed.removeAll(keepingCapacity: true)
            }, { clay_document_destroy(doc) })
        }

        // -- rigs -------------------------------------------------------------------

        // ZSpheres. One MOVE on a placed armature, which is the drag an artist
        // repeats: `value` is a delta and the target's whole subtree travels
        // with it, so the cost follows the subtree rather than the document.
        measureAxis(name: "armature_edit", verb: "layer_armature_edit", .gesture,
                    batch: 1024) { stamps in
            guard let f = Fixture.armatureLayer(stamps: stamps) else { return nil }
            var delta: [Float] = [0.01, 0, 0]
            return ({
                _ = clay_layer_armature_edit(f.doc, f.layer, f.node,
                                             CLAY_ARMATURE_MOVE, f.target,
                                             &delta, 0, 0)
                // Alternate the direction so a long run does not walk the rig
                // out of its own bounds and change what is being measured.
                delta[0] = -delta[0]
            }, nil, { clay_document_destroy(f.doc) })
        }

        finishAndCheck()
    }
}
