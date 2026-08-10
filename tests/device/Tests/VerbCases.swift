// A latency case per brush and sculpt verb.
//
// Every verb here is driven through the public C ABI, at the three document
// sizes the growth axis names, on the device. The voxel verbs should be flat
// in document size — they walk a footprint, not a document — and the SDF ones
// should not, because the tape recompiles per edit. That contrast is the
// point: it is what tells a growth regression from measurement noise.

import Foundation
import XCTest
import claycore

// MARK: - Shared fixtures

enum Fixture {

    static func brush(size: Int32 = 8,
                      falloff: clay_brush_falloff = CLAY_BRUSH_FALLOFF_SMOOTH)
        -> clay_brush_params {
        var b = clay_brush_params()
        b.struct_size = UInt32(MemoryLayout<clay_brush_params>.size)
        b.size = size
        b.shape = Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue)
        b.falloff = Int32(falloff.rawValue)
        b.strength = 1.0
        b.seed = 1
        return b
    }

    /// Cell coordinates for stamp `i`, matching SceneBuilder's world spread so
    /// the voxel and SDF cases traverse the same shape.
    static func cell(_ i: Int, scale: Float = 40) -> [Int32] {
        let (x, y, z) = SceneBuilder.stampPosition(i)
        return [Int32(x * scale), Int32(y * scale), Int32(z * scale)]
    }

    /// A document with a voxel layer carrying `stamps` stamps of material, so
    /// the sculpt verbs have something to reshape. Returns the borrowed grid;
    /// destroying the document frees it.
    static func voxelGrid(stamps: Int, voxelSize: Float = 0.02)
        -> (doc: OpaquePointer, grid: OpaquePointer)? {
        guard let doc = clay_document_create() else { return nil }
        var layer: clay_layer_id = 0
        var grid: OpaquePointer?
        guard clay_document_add_voxel_layer(doc, "bench", voxelSize, &layer, &grid) == CLAY_OK,
              let grid else {
            clay_document_destroy(doc); return nil
        }
        var b = brush()
        for i in 0..<max(stamps, 1) {
            var c = cell(i)
            _ = clay_voxel_set_brush(grid, &c, &b, 1)
        }
        return (doc, grid)
    }

    /// A volume item baked from a document of `stamps` stamps — what relax and
    /// flatten act on. Free with clay_item_destroy.
    static func volumeItem(stamps: Int, cellSize: Float = 0.05) -> OpaquePointer? {
        guard let (doc, _) = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
        defer { clay_document_destroy(doc) }
        var params = clay_volume_params()
        params.struct_size = UInt32(MemoryLayout<clay_volume_params>.size)
        params.cell_size = cellSize
        params.band = 0
        params.padding = 0
        params.beta = 0
        var item: OpaquePointer?
        guard clay_item_volume_from_document(doc, &params, nil, nil, &item) == CLAY_OK else {
            return nil
        }
        return item
    }

    static func strokePreset(radius: Float = 0.15) -> clay_stroke_preset {
        var preset = clay_stroke_preset()
        _ = clay_stroke_preset_defaults(&preset)
        preset.radius = radius
        return preset
    }

    /// Pointer-pressure-time samples along a short drag.
    static func strokeSamples(count: Int = 32) -> [Float] {
        var samples: [Float] = []
        samples.reserveCapacity(count * 5)
        for i in 0..<count {
            let t = Float(i) / Float(max(count - 1, 1))
            // x y z pressure time
            samples.append(contentsOf: [t * 1.2 - 0.6, sin(t * 3) * 0.2, 0, 1.0, t])
        }
        return samples
    }
}

// MARK: - The cases

final class VerbLatencyTests: XCTestCase {

    private var collector = RunCollector()

    private func abiVersion() -> String {
        var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
        clay_version(&major, &minor, &patch)
        return "\(major).\(minor).\(patch)"
    }

    /// Measure one verb across the growth axis.
    ///
    /// `prepare` builds the fixture for a document size and returns the timed
    /// body plus its teardown. Returning nil skips that size, which is a
    /// failure rather than a silent gap.
    private func measureAxis(
        name: String, verb: String, _ cls: BudgetClass, backend: String = "cpu",
        prepare: (Int) -> (body: () -> Void, reset: (() -> Void)?,
                           cleanup: () -> Void)?
    ) {
        var measurements: [Measurement] = []
        for stamps in LatencyTests.axis {
            guard let fixture = prepare(stamps) else {
                XCTFail("\(name): could not build a fixture at \(stamps) stamps")
                continue
            }
            let r = Timing.measure(reset: fixture.reset, fixture.body)
            fixture.cleanup()
            measurements.append(Measurement(stamps: stamps, p50Ms: r.p50,
                                            p95Ms: r.p95, samples: r.n))
        }
        collector.add(CaseResult(
            name: name, verb: verb, budgetClass: cls,
            backend: backend, servedBy: backend,
            measurements: measurements,
            growthExponent: Timing.growthExponent(measurements)))
    }

    /// Every voxel verb and every mask/SDF verb, in one run so they share a
    /// thermal window: cases measured minutes apart on a warming device are
    /// not comparable with each other.
    func testEveryVerbOnDevice() throws {
        collector = RunCollector()

        // -- voxel stamping ---------------------------------------------------

        for (name, verb, index) in [("voxel_stamp", "voxel_set_brush", Int32(1)),
                                    ("voxel_paint", "voxel_paint_brush", Int32(1))] {
            measureAxis(name: name, verb: verb, .interactive) { stamps in
                guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
                var b = Fixture.brush()
                var i = stamps
                return ({
                    var c = Fixture.cell(i); i += 1
                    if name == "voxel_paint" {
                        _ = clay_voxel_paint_brush(f.grid, &c, &b, index)
                    } else {
                        _ = clay_voxel_set_brush(f.grid, &c, &b, index)
                    }
                }, nil, { clay_document_destroy(f.doc) })
            }
        }

        measureAxis(name: "voxel_erase", verb: "voxel_erase_brush", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_erase_brush(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // -- voxel sculpt verbs ------------------------------------------------

        measureAxis(name: "voxel_smooth", verb: "voxel_sculpt_smooth", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_smooth(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_inflate", verb: "voxel_sculpt_inflate", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_inflate(f.grid, &c, &b, 1)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_flatten", verb: "voxel_sculpt_flatten", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var normal: [Float] = [0, 1, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_flatten(f.grid, &c, &b, &normal, 0)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_pinch", verb: "voxel_sculpt_pinch", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_pinch(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_magnify", verb: "voxel_sculpt_magnify", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_magnify(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // Grab and smudge need a displacement past the nearest-cell dead zone:
        // under half a cell on every axis they read each cell's source as
        // itself, return OK, and move nothing. Measuring that would time the
        // walk without the work.
        measureAxis(name: "voxel_grab", verb: "voxel_sculpt_grab", .gesture) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var displacement: [Float] = [0.06, 0, 0]  // 3 cells at 0.02
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_grab(f.grid, &c, &b, &displacement, 0)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_smudge", verb: "voxel_sculpt_smudge", .gesture) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var displacement: [Float] = [0.06, 0, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_smudge(f.grid, &c, &b, &displacement)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_scrape", verb: "voxel_sculpt_scrape", .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var normal: [Float] = [0, 1, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_scrape(f.grid, &c, &b, &normal, 0)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_fill_cavities", verb: "voxel_sculpt_fill_cavities",
                    .operation) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_fill_cavities(f.grid, &c, &b, 1)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_carve_alpha", verb: "voxel_sculpt_carve_alpha",
                    .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            // A host with an alpha has already loaded the PNG; the engine
            // decodes no images, so this is a plain sample grid.
            let side = 16
            var alpha = [Float](repeating: 0, count: side * side)
            for y in 0..<side {
                for x in 0..<side {
                    alpha[y * side + x] = Float((x + y) % 2)
                }
            }
            var direction: [Float] = [0, 1, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_carve_alpha(f.grid, &c, &b, &alpha,
                                                  Int32(side), Int32(side), &direction, 0)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // -- the stroke engine -------------------------------------------------

        measureAxis(name: "stroke_resolve", verb: "stroke_resolve", .interactive) { _ in
            var preset = Fixture.strokePreset()
            var samples = Fixture.strokeSamples()
            let sampleCount = samples.count / 5
            var stamps = [clay_stamp](repeating: clay_stamp(), count: 4096)
            return ({
                var capacity = stamps.count
                _ = clay_stroke_resolve(&samples, sampleCount, &preset, &stamps, &capacity)
            }, nil, {})
        }

        measureAxis(name: "voxel_apply_stroke", verb: "voxel_apply_stroke",
                    .gesture) { stampCount in
            guard let f = Fixture.voxelGrid(stamps: stampCount) else { return nil }
            var preset = Fixture.strokePreset()
            var samples = Fixture.strokeSamples()
            let sampleCount = samples.count / 5
            return ({
                var applied = 0
                _ = clay_voxel_apply_stroke(
                    f.grid, &samples, sampleCount, &preset, 1,
                    Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue),
                    Int32(CLAY_BRUSH_FALLOFF_SMOOTH.rawValue), nil, &applied)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // -- masks --------------------------------------------------------------

        measureAxis(name: "mask_paint", verb: "mask_paint", .interactive) { _ in
            guard let mask = clay_mask_create(0.02) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                let (x, y, z) = SceneBuilder.stampPosition(i); i += 1
                var point: [Float] = [x, y, z]
                _ = clay_mask_paint(mask, &point, &b, 1.0)
            }, nil, { _ = clay_mask_destroy(mask) })
        }

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

        measureAxis(name: "cut_create", verb: "cut_create", .gesture) { _ in
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

        // -- the coverage guard ------------------------------------------------------

        let record = collector.finish(abiVersion: abiVersion(), attachTo: self)

        // A case that ran but names no verb in the table is checkable here.
        // The converse — every non-exempt entry produced a measurement — is
        // NOT: `sdf_stamp_cpu` runs in LatencyTests, and each test emits its
        // own attachment, so no single test sees the whole run. That check
        // belongs to tools/check_device_coverage.py, which reads the merged
        // record and clay.h together.
        let known = Set(Coverage.table.compactMap(\.caseName))
        for result in record.cases {
            XCTAssertTrue(known.contains(result.name),
                          "coverage: case '\(result.name)' is in no table entry")
        }
        XCTAssertTrue(record.valid,
                      "thermal state \(record.thermalStateStart) -> "
                      + "\(record.thermalStateEnd); a throttled run is a different "
                      + "experiment, not a slower result")
    }
}
