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


// MARK: - The light cases

/// The voxel, stroke and mask-paint verbs — everything up to `mask_extrude`.
/// The heavy half lives in VerbHeavyCases.swift, in its own bundle; see
/// Shared/VerbHarness.swift for why.
final class VerbLatencyTests: VerbCaseGroup {

    /// One run so they share a thermal window: cases measured minutes apart on
    /// a warming device are not comparable with each other.
    func testEveryVerbOnDevice() throws {
        collector = RunCollector()


        // -- voxel stamping ---------------------------------------------------

        for (name, verb, index) in [("voxel_stamp", "voxel_set_brush", Int32(1)),
                                    ("voxel_paint", "voxel_paint_brush", Int32(1))] {
            measureAxis(name: name, verb: verb, .interactive, batch: 128) { stamps in
                guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
                var b = Fixture.brush()
                var i = 0
                return ({
                    var c = Fixture.cell(stamps + i); i = (i + 1) % Self.walkWindow
                    if name == "voxel_paint" {
                        _ = clay_voxel_paint_brush(f.grid, &c, &b, index)
                    } else {
                        _ = clay_voxel_set_brush(f.grid, &c, &b, index)
                    }
                }, nil, { clay_document_destroy(f.doc) })
            }
        }

        measureAxis(name: "voxel_erase", verb: "voxel_erase_brush", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_erase_brush(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // -- voxel sculpt verbs ------------------------------------------------

        measureAxis(name: "voxel_smooth", verb: "voxel_sculpt_smooth", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_smooth(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_inflate", verb: "voxel_sculpt_inflate", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_inflate(f.grid, &c, &b, 1)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_flatten", verb: "voxel_sculpt_flatten", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var normal: [Float] = [0, 1, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_flatten(f.grid, &c, &b, &normal, 0)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_pinch", verb: "voxel_sculpt_pinch", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_pinch(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_magnify", verb: "voxel_sculpt_magnify", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_magnify(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // Grab and smudge need a displacement past the nearest-cell dead zone:
        // under half a cell on every axis they read each cell's source as
        // itself, return OK, and move nothing. Measuring that would time the
        // walk without the work.
        measureAxis(name: "voxel_grab", verb: "voxel_sculpt_grab", .gesture,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var displacement: [Float] = [0.06, 0, 0]  // 3 cells at 0.02
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_grab(f.grid, &c, &b, &displacement, 0)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_smudge", verb: "voxel_sculpt_smudge", .gesture,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var displacement: [Float] = [0.06, 0, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_smudge(f.grid, &c, &b, &displacement)
            }, nil, { clay_document_destroy(f.doc) })
        }

        measureAxis(name: "voxel_scrape", verb: "voxel_sculpt_scrape", .interactive,
                    batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var b = Fixture.brush()
            var normal: [Float] = [0, 1, 0]
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
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
                    .interactive, batch: 512) { stamps in
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
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
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
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
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
                    .interactive, batch: 128) { stamps in
            guard let f = Fixture.voxelGrid(stamps: stamps) else { return nil }
            var level = 0
            guard clay_voxel_add_level(f.grid, &level) == CLAY_OK,
                  clay_voxel_set_active_level(f.grid, 0) == CLAY_OK else {
                clay_document_destroy(f.doc); return nil
            }
            var b = Fixture.brush()
            var i = 0
            return ({
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
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
                var c = Fixture.cell(i); i = (i + 1) % Self.walkWindow
                _ = clay_voxel_sculpt_smooth(f.grid, &c, &b)
            }, nil, { clay_document_destroy(f.doc) })
        }

        // -- the stroke engine -------------------------------------------------

        measureAxis(name: "stroke_resolve", verb: "stroke_resolve", .interactive,
                    batch: 512) { _ in
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

        measureAxis(name: "mask_paint", verb: "mask_paint", .interactive, batch: 128) { _ in
            guard let mask = clay_mask_create(0.02) else { return nil }
            var b = Fixture.brush()
            var i = 0
            return ({
                let (x, y, z) = SceneBuilder.stampPosition(i)
                i = (i + 1) % Self.walkWindow
                var point: [Float] = [x, y, z]
                _ = clay_mask_paint(mask, &point, &b, 1.0)
            }, nil, { _ = clay_mask_destroy(mask) })
        }


        finishAndCheck()
    }
}
