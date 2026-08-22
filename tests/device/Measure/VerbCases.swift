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
    /// `axis` overrides the shared growth axis for a case whose smallest point
    /// its fixture cannot feed. Only `voxel_mesh_dirty` uses it; the reason is
    /// recorded there.
    private func measureAxis(
        name: String, verb: String, _ cls: BudgetClass, backend: String = "cpu",
        axis: [Int] = GrowthAxis.standard,
        prepare: (Int) -> (body: () -> Void, reset: (() -> Void)?,
                           cleanup: () -> Void)?
    ) {
        var measurements: [Measurement] = []
        collector.sampleCanaryIfDue()
        let caseStartedAtMs = collector.elapsedMs
        let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)
        for stamps in axis {
            guard let fixture = prepare(stamps) else {
                XCTFail("\(name): could not build a fixture at \(stamps) stamps")
                continue
            }
            let r = Timing.measureStable(reset: fixture.reset, fixture.body)
            fixture.cleanup()
            measurements.append(Measurement(stamps: stamps, p50Ms: r.p50,
                                            p95Ms: r.p95, samples: r.n,
                                            repeats: r.repeats,
                                            p95SpreadMs: r.spread))
        }
        collector.add(CaseResult(
            name: name, verb: verb, budgetClass: cls,
            backend: backend, servedBy: backend,
            measurements: measurements,
            growthExponent: Timing.growthExponent(measurements),
            startedAtMs: caseStartedAtMs,
            thermalStateStart: caseThermalStart,
            thermalStateEnd: DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)))
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

        // On a PERFORATED BLOCK rather than the scattered spheres every other
        // voxel case uses, because this verb only acts on a narrow pocket and
        // a sphere's surface has none. See Fixture.perforatedBlock: on the old
        // fixture this case measured 0 of 200 dabs changing a single cell.
        //
        // The reset re-opens the holes. The verb FILLS them, so without it the
        // first pass would close the block and every later iteration would time
        // an empty search — which is exactly the failure the fixture change is
        // fixing, reintroduced one level down.
        measureAxis(name: "voxel_fill_cavities", verb: "voxel_sculpt_fill_cavities",
                    .operation) { holes in
            guard let f = Fixture.perforatedBlock(holes: holes) else { return nil }
            var b = Fixture.brush()
            var i = 0
            var landed = 0
            var did = 0
            return ({
                var c = Fixture.holeCell(i % max(holes, 1)); i += 1
                var before: UInt64 = 0, after: UInt64 = 0
                clay_voxel_change_count(f.grid, &before)
                _ = clay_voxel_sculpt_fill_cavities(f.grid, &c, &b, 1)
                clay_voxel_change_count(f.grid, &after)
                landed += 1
                if after != before { did += 1 }
            }, {
                Fixture.punch(f.grid, holes: holes)
            }, {
                // The guard that would have caught this case being empty for as
                // long as it has been. change_count answers it exactly, which
                // the header says is the only way to know.
                XCTAssertGreaterThan(did, landed / 2,
                                     "over half the dabs at \(holes) holes filled nothing — "
                                     + "this case is timing a search rather than a fill")
                clay_document_destroy(f.doc)
            })
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

        // -- the display path ---------------------------------------------------
        //
        // The gate measured the voxel EDIT verbs and nothing that shows their
        // result, which is how a ~130x display gap (#86) stayed invisible here
        // while every verb above reported two orders of magnitude of headroom.
        // A sculpt nobody can see inside a frame is not interactive, however
        // cheap the edit was.
        //
        // Two cases because there are two paths, and they carry different
        // budgets on purpose. Whole-grid meshing is the EXPORT path — its
        // header says so, and it keeps the tighter merge for that reason — so
        // it is an `operation`. Meshing what a dab dirtied is the DISPLAY path
        // and has to fit a frame, so it is `interactive`.

        measureAxis(name: "voxel_mesh_whole", verb: "voxel_mesh", .operation) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            return ({
                var mesh: OpaquePointer?
                if clay_voxel_mesh(f.grid, &mesh) == CLAY_OK, let mesh {
                    clay_mesh_destroy(mesh)
                }
            }, nil, { clay_document_destroy(f.doc) })
        }

        // One dab and the display it costs, timed together, because that pair
        // is the interactive unit — the same reason the SDF stamp case times
        // edit-then-evaluate rather than the edit alone.
        //
        // The drain is INSIDE the timed body. It has to be: a host that
        // skipped a frame coalesces, so what a frame pays is whatever the set
        // holds when it asks, and hoisting the drain out would measure a
        // hand-picked key list instead of the one a session produces.
        //
        // No reset, for the reason the other voxel cases have none: the verbs
        // are flat in document size, so an accumulating grid does not drift the
        // number. It is load-bearing HERE for a second reason — draining every
        // iteration is what keeps the unit one dab. Without it the set would
        // accumulate and iteration N would mesh what N-1 also dirtied, which
        // reads as a display path that grows with the session.
        // A SHIFTED AXIS. The shared one starts at 10, and this fixture cannot
        // feed that point: the dab index walks 0...199 over up to 200 timed
        // iterations while a 10-stamp fixture holds ten spots of material, so
        // most dabs land in empty space, dirty nothing and mesh nothing.
        // Measured: 0.1 chunks per dab there, which is 10/200.
        //
        // That mattered because growthExponent divides the largest point by
        // the smallest, so the near-zero p95 at 10 stamps WAS the N^1.28 this
        // case reported — 0.0060 -> 2.1235 ms is N^1.27 whatever the engine
        // does. Starting at 100 gives every point material to work on and the
        // same measurement reads N^0.68.
        //
        // There is no scaling defect underneath it. Verified on the Mac
        // through the C ABI: meshing a fixed chunk count costs the same on a
        // grid of 30 chunks and one of 51, the dab and the drain are flat
        // across the axis, and all of the growth is in the mesh call, which is
        // O(keys). What remains is density — this fixture holds the volume
        // fixed, so more stamps means a denser solid — and that is N^0.33.
        measureAxis(name: "voxel_mesh_dirty", verb: "voxel_mesh_chunks", .interactive,
                    axis: [100, 1000]) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            var keys = [Int32](repeating: 0, count: 4096 * 3)
            // Drain what building the fixture dirtied, so the first timed
            // iteration pays for its own dab rather than for the whole grid.
            var drained = keys.count / 3
            var remaining = 0
            _ = clay_voxel_take_dirty_chunks(f.grid, &keys, &drained, &remaining)
            var meshedChunks = 0
            var calls = 0
            var starved = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_smooth(f.grid, &c, &b)

                var count = keys.count / 3
                var left = 0
                guard clay_voxel_take_dirty_chunks(f.grid, &keys, &count, &left) == CLAY_OK
                else { return }
                calls += 1
                meshedChunks += count
                if count == 0 { starved += 1 }
                var mesh: OpaquePointer?
                if clay_voxel_mesh_chunks(f.grid, &keys, count, nil, &mesh) == CLAY_OK,
                   let mesh {
                    clay_mesh_destroy(mesh)
                }
            }, nil, {
                // The same guard the brick-cache case carries. A dab that
                // dirties nothing meshes nothing, and a case that measured an
                // empty loop would report the display path as free.
                XCTAssertLessThan(starved, max(calls, 1),
                                  "every timed iteration at \(stamps) stamps dirtied zero "
                                  + "chunks — this is measuring an empty loop")
                let perDab = calls > 0 ? Double(meshedChunks) / Double(calls) : 0
                print("chunks/dab at \(stamps) stamps: \(String(format: "%.1f", perDab))")
                clay_document_destroy(f.doc)
            })
        }

        // -- what a level costs ------------------------------------------------
        //
        // add-multi-resolution landed the level stack and nothing here measured
        // it (#89). Three costs were unmeasured on device; these are the two
        // that land on the frame path.

        // Subdivision itself. Reset drops the level again, so every iteration
        // measures an add against the same one-level grid rather than adding a
        // fourth level to a stack the previous iteration deepened — and the
        // stack is capped, so without the reset later iterations would measure
        // a refusal.
        measureAxis(name: "voxel_add_level", verb: "voxel_add_level", .operation) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            return ({
                var level = 0
                _ = clay_voxel_add_level(f.grid, &level)
            }, {
                _ = clay_voxel_drop_level(f.grid)
            }, { clay_document_destroy(f.doc) })
        }

        // A verb with a level under it. The design charges a write `8^d` cell
        // writes for `d` levels finer than the active one, so editing COARSE
        // with a stack is the expensive direction — and it is also the one the
        // #86 workaround recommends, since a host displaying a coarse level
        // wants to keep editing there. Same verb and same brush as
        // `voxel_smooth`, so the pair is the measurement.
        measureAxis(name: "voxel_smooth_l2", verb: "voxel_sculpt_smooth_levels",
                    .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var level = 0
            guard clay_voxel_add_level(f.grid, &level) == CLAY_OK,
                  clay_voxel_set_active_level(f.grid, 0) == CLAY_OK else {
                clay_document_destroy(f.doc); return nil
            }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_smooth(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // -- what a big brush costs ---------------------------------------------
        //
        // Every voxel case above uses Fixture.brush(), which is size 8 — about
        // 2,100 cells. Cost is roughly cubic in radius, so the gate has been
        // reporting the cheap end of the range a sculptor actually uses: a
        // radius-32 smooth is ~200x the footprint of a radius-8 one, and
        // nothing here would have said so.
        measureAxis(name: "voxel_smooth_r32", verb: "voxel_sculpt_smooth_large",
                    .interactive) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush(size: 32)
            var i = 0
            return ({
                var c = Fixture.cell(i); i += 1
                _ = clay_voxel_sculpt_smooth(f.grid, &c, &b)
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

        // Trim Curve: an OPEN stroke closed against the frame bounds. Two
        // calls, as a host makes them — size query, then tessellate — because
        // the size query is not free and a host cannot skip it.
        measureAxis(name: "trim_curve", verb: "cut_polygon_from_open_curve", .gesture) { _ in
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
        measureAxis(name: "tube_create", verb: "tube_create", .gesture) { _ in
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
        measureAxis(name: "pose_region", verb: "pose", .gesture) { stamps in
            guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
            // centre(3), radius, axis(3), angle
            var pose: [Float] = [0, 0, 0, 0.6, 0, 1, 0, 0.4]
            var radius: Float = 0.35
            return ({
                guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue),
                                                  &radius, 1) else { return }
                defer { clay_item_destroy(item) }
                _ = clay_item_add_deformer(item, Int32(CLAY_DEFORM_POSE.rawValue),
                                           &pose, pose.count, CLAY_EASE_LINEAR)
                var node: clay_node_id = 0
                _ = clay_layer_add_item(doc, layer, item, &node)
            }, nil, { clay_document_destroy(doc) })
        }

        // -- rigs -------------------------------------------------------------------

        // ZSpheres. One MOVE on a placed armature, which is the drag an artist
        // repeats: `value` is a delta and the target's whole subtree travels
        // with it, so the cost follows the subtree rather than the document.
        measureAxis(name: "armature_edit", verb: "layer_armature_edit", .gesture) { stamps in
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
