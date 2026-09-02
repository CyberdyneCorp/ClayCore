// Adaptive topology on the device, which nothing here has ever measured.
//
// Every other sculpting path in this library is gated on hardware — 69 cases,
// a 1.4x tolerance, budgets per class, and a table in
// docs/09-brush-latency-and-coverage.md comparing them against ZBrush and
// Nomad. Dynamic topology had none of it. The desktop suite proves the
// operators correct and says nothing about whether a dab fits inside a frame
// on the tablet this exists to serve, which is the only question that decides
// whether the feature ships.
//
// WHAT THESE MEASURE, and why they are shaped as pairs. The fixture holds
// triangle SPACING constant and grows the EXTENT, so a dab covers the same
// number of triangles at every axis point (see Fixture.dynamicPatch). An
// adaptive dab should therefore be FLAT in document size, like the voxel verbs
// and unlike the SDF ones — the local remesher is bounded by the brush and the
// spatial index is meant to be queried, not walked. A slope is the regression
// `test_dynamic_scale.cpp` gates on desktop and nothing gated on hardware.
//
// Each adaptive case is paired with the same dab at `enabled: false`. The pair
// is what makes the number a statement about ADAPTATION rather than about the
// Clay brush: subtract one from the other and what is left is the remesh, the
// local normal recompute and the incremental index update.
//
// ITS OWN BUNDLE, and last in the run. A bundle is the process boundary that
// returns a high-water mark, and this harness has been jetsam-killed three
// times by suites sharing one; project.yml warns in as many words against
// adding a case to an existing bundle. Last, because the run order is worth
// 2.7x on a 1.4x tolerance by this project's own measurement, and a new suite
// has no business moving 69 baselines that were taken in the current order.

import Foundation
import XCTest
import claycore

final class DyntopoLatencyTests: VerbCaseGroup {

    func testAdaptiveTopologyOnDevice() throws {
        collector = RunCollector()

        // -- one adaptive dab --------------------------------------------------
        //
        // The interactive case: a pencil is down and the surface is gaining
        // geometry under it. Batched, because a single dab on the smallest
        // patch sits near the 0.05 ms floor below which the tolerance cannot
        // object to anything (#337).
        measureAxis(name: "dyntopo_stamp", verb: "dynamic_sculptor_stamp",
                    .interactive, batch: 8) { stamps in
            guard let f = Fixture.dynamicPatch(stamps: stamps) else { return nil }
            var brush = Fixture.patchBrush()
            var topo = Fixture.topology(enabled: true)
            var i = 0
            return ({
                brush.center = Fixture.patchCenter(i)
                i = (i + 1) % Self.walkWindow
                _ = clay_dynamic_sculptor_stamp(f.sculptor, &brush, &topo, nil, nil)
            }, nil, {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
            })
        }

        // -- the same dab with adaptation off ----------------------------------
        //
        // The other half of the pair. Deformation only, same brush, same
        // fixture, same walk.
        measureAxis(name: "dyntopo_stamp_fixed", verb: "dynamic_sculptor_stamp",
                    .interactive, batch: 8) { stamps in
            guard let f = Fixture.dynamicPatch(stamps: stamps) else { return nil }
            var brush = Fixture.patchBrush()
            var topo = Fixture.topology(enabled: false)
            var i = 0
            return ({
                brush.center = Fixture.patchCenter(i)
                i = (i + 1) % Self.walkWindow
                _ = clay_dynamic_sculptor_stamp(f.sculptor, &brush, &topo, nil, nil)
            }, nil, {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
            })
        }

        // -- a finer dab, which is where the budget actually binds --------------
        //
        // detail_resolution 8 rather than 4 halves the target edge length, so
        // the same footprint asks for roughly four times the triangles and the
        // per-dab operation budget starts to bite. A host that lets an artist
        // raise detail is asking for exactly this, and it is the setting most
        // likely to miss a frame.
        measureAxis(name: "dyntopo_stamp_fine", verb: "dynamic_sculptor_stamp",
                    .interactive, batch: 4) { stamps in
            guard let f = Fixture.dynamicPatch(stamps: stamps) else { return nil }
            var brush = Fixture.patchBrush()
            var topo = Fixture.topology(enabled: true, resolution: 8.0, maxOps: 400)
            var i = 0
            return ({
                brush.center = Fixture.patchCenter(i)
                i = (i + 1) % Self.walkWindow
                _ = clay_dynamic_sculptor_stamp(f.sculptor, &brush, &topo, nil, nil)
            }, nil, {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
            })
        }

        // -- the host's upload path --------------------------------------------
        //
        // A dab is only half of a frame. The host still has to get what changed
        // onto the GPU, and for an adaptive surface that is the chunk
        // transport: ask what a chunk needs, then copy it into memory the
        // caller owns. Measured per frame because that is when it happens, and
        // interactive for the same reason.
        //
        // Copying chunk 0 rather than draining the dirty set on purpose: the
        // dirty set's SIZE is what a dab decides, and timing it would fold the
        // brush's reach back into a number that is meant to be about transport.
        measureAxis(name: "dyntopo_chunk_copy", verb: "dynamic_surface_copy_chunk",
                    .interactive, batch: 16) { stamps in
            guard let f = Fixture.dynamicPatch(stamps: stamps) else { return nil }
            var brush = Fixture.patchBrush()
            var topo = Fixture.topology(enabled: true)
            // Adapt once, so the chunk being copied is one the remesher has
            // actually touched rather than the imported patch.
            for i in 0..<8 {
                brush.center = Fixture.patchCenter(i)
                _ = clay_dynamic_sculptor_stamp(f.sculptor, &brush, &topo, nil, nil)
            }
            var info = clay_dynamic_chunk_info()
            info.struct_size = UInt32(MemoryLayout<clay_dynamic_chunk_info>.size)
            guard clay_dynamic_surface_chunk_count(f.sculptor) > 0,
                  clay_dynamic_surface_copy_chunk(f.sculptor, 0, nil, 0, nil, 0, nil, 0,
                                                  &info) == CLAY_OK,
                  info.vertex_count > 0 else {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
                return nil
            }
            var positions = [Float](repeating: 0, count: Int(info.vertex_count) * 3)
            var indices = [UInt32](repeating: 0, count: Int(info.index_count))
            var written = clay_dynamic_chunk_info()
            written.struct_size = UInt32(MemoryLayout<clay_dynamic_chunk_info>.size)
            return ({
                positions.withUnsafeMutableBufferPointer { p in
                    indices.withUnsafeMutableBufferPointer { i in
                        _ = clay_dynamic_surface_copy_chunk(f.sculptor, 0,
                                                            p.baseAddress, p.count,
                                                            nil, 0,
                                                            i.baseAddress, i.count, &written)
                    }
                }
            }, nil, {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
            })
        }

        finishAndCheck()
    }
}
