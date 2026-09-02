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
// THE FLATNESS IS CHECKED, not asserted. These four cases have been run on an
// iPad Pro SIMULATOR — which answers no question about latency and every
// question about whether the fixture holds — and a host-side replay driving the
// same C ABI against a desktop build agrees with it. The growth exponents the
// harness recorded:
//
//     dyntopo_stamp         +0.067    p95  5.15 /  5.65 /  7.01 ms  (batch 8)
//     dyntopo_stamp_fixed   +0.497    p95  0.12 /  0.30 /  1.20 ms  (batch 8)
//     dyntopo_stamp_fine    +0.006    p95 25.9  / 26.9  / 26.7  ms  (batch 4)
//     dyntopo_chunk_copy    +0.027    p95  0.87 /  0.88 /  0.99 ms  (batch 256)
//
// The desktop replay puts the footprint at 751 / 790 / 784 splits across the
// three axis points, so the fixture is delivering the constant dab it promises.
//
// Three of the four are flat. The DEFORMATION-ONLY dab is not, and it is the
// one case with no remesher in it: +0.497, an 8.9x rise on a document a hundred
// times larger, for work whose footprint does not change. That is the shape
// `tests/unit/test_dynamic_scale.cpp` gates on desktop, and it is why the pair
// is worth more than either half of it. Whether it also holds on the tablet is
// what the baseline run is for.
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

    /// What the four cases below assume about their fixture, checked before any
    /// of them is timed.
    ///
    /// BOTH OF THESE HAVE BEEN WRONG IN THIS FILE, and neither showed up as a
    /// failure — they showed up as plausible numbers measuring the wrong thing,
    /// which is the only kind of bug a benchmark suite can have. On the first
    /// version of these cases a single probe stamp reported:
    ///
    ///     side=15   split=21  collapse=92   dirty x[-0.016  0.150]
    ///     side=50   split=0   collapse=120  dirty x[-0.010  0.210]
    ///     side=158  split=0   collapse=119  dirty x[-0.010  0.210]
    ///
    /// Two things are wrong there and this method fails on either. The dirty
    /// region at the smallest axis point stops at 0.150, which is exactly that
    /// patch's half-extent — the dab was hanging off the edge and being clipped,
    /// at one axis point and not the others, so the growth axis was reading the
    /// FIXTURE. And `split=0` at two of three points: the case named
    /// `dyntopo_stamp`, the one that exists to measure an adaptive dab, was
    /// doing no refinement at all, because a brush-relative target of
    /// radius/resolution = 0.03 sits above the fixture's 0.02 spacing and the
    /// remesher can only collapse.
    ///
    /// A probe stamp costs one dab on a throwaway fixture per axis point, which
    /// is nothing beside the run it protects.
    func checkFixtureInvariants() {
        for stamps in GrowthAxis.standard {
            guard let f = Fixture.dynamicPatch(stamps: stamps) else {
                XCTFail("fixture invariants: no patch at \(stamps) stamps")
                continue
            }
            defer {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
            }
            var brush = Fixture.patchBrush()
            var topo = Fixture.topology(enabled: true)
            brush.center = Fixture.patchCenter(0)
            var report = clay_dynamic_stamp_report()
            report.struct_size = UInt32(MemoryLayout<clay_dynamic_stamp_report>.size)
            let rc = clay_dynamic_sculptor_stamp(f.sculptor, &brush, &topo, nil, &report)
            XCTAssertEqual(rc, CLAY_OK, "fixture invariants: the probe stamp failed")

            XCTAssertGreaterThan(
                report.split_edges, 0,
                "fixture invariants at \(stamps) stamps: the adaptive dab SPLIT nothing. "
                + "radius / detail_resolution must land below the patch's 0.02 spacing "
                + "or this suite measures decimation under a name that says refinement.")

            // x and z only: the patch is flat in y and the deposit's own
            // displacement there says nothing about the footprint.
            let half = Fixture.patchHalfExtent(stamps: stamps)
            let reach = max(abs(report.dirty_min.0), abs(report.dirty_max.0),
                            abs(report.dirty_min.2), abs(report.dirty_max.2))
            XCTAssertLessThan(
                reach, half,
                "fixture invariants at \(stamps) stamps: the dab reached \(reach) on a "
                + "patch whose half-extent is \(half), so the boundary is clipping it. "
                + "A footprint that is clipped at one axis point and not another makes "
                + "the growth axis a measurement of this fixture.")
        }
    }

    func testAdaptiveTopologyOnDevice() throws {
        checkFixtureInvariants()
        collector = RunCollector()

        // -- one adaptive dab --------------------------------------------------
        //
        // The interactive case: a pencil is down and the surface is gaining
        // geometry under it. GAINING is checked rather than assumed — at
        // radius 0.04 and resolution 4 the brush-relative target is 0.01,
        // below the fixture's 0.02 spacing, and the replay grows the patch
        // 450 -> 1122 triangles. Read `Fixture.patchBrush` before changing
        // either number: a target ABOVE the spacing turns this case into a
        // decimation, which is a different experiment under the same name.
        //
        // Batched, because a single dab on the smallest patch sits near the
        // 0.05 ms floor below which the tolerance cannot object to anything
        // (#337).
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
        // per-dab operation budget starts to bite. Measured on the replay: the
        // base case adds 672 triangles to the smallest patch and this one adds
        // 3,032, a factor of 4.5, and `hit_budget` is set on 4 of 60 batched
        // bodies against the base case's 1. A host that lets an artist raise
        // detail is asking for exactly this, and it is the setting most likely
        // to miss a frame.
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
        // ONE chunk per timed body rather than draining the dirty set: the
        // dirty set's SIZE is what a dab decides, and timing the drain would
        // fold the brush's reach back into a number that is meant to be about
        // transport.
        //
        // The chunk is one the stamps above actually DIRTIED, asked for by
        // index. Chunk 0 was the first attempt and is the wrong chunk: the
        // largest patch is partitioned into 256 of them and the brush works at
        // the centre, so index 0 is a corner the remesher never reached. Its
        // vertex count came out non-monotonic across the axis (939 / 468 / 585)
        // and the harness recorded a growth exponent of -0.102 — a NEGATIVE
        // slope read straight off the chunker's boundaries. Selecting from
        // `dirty_chunks` gives 537 / 699 / 843 and +0.027, which is a statement
        // about transport.
        //
        // BATCH 256, and the size is the point. At 16 the case measured a p95
        // of 0.043 to 0.081 ms, and a regression has to clear BOTH 1.4x and an
        // absolute 0.05 ms before this gate will fail — so the case sat AT or
        // UNDER the noise floor and could not have objected to anything, which
        // is #337 happening again in a new suite. 256 puts its p95 near 1 ms,
        // where the declared tolerance is the one that actually applies.
        measureAxis(name: "dyntopo_chunk_copy", verb: "dynamic_surface_copy_chunk",
                    .interactive, batch: 256) { stamps in
            guard let f = Fixture.dynamicPatch(stamps: stamps) else { return nil }
            var brush = Fixture.patchBrush()
            var topo = Fixture.topology(enabled: true)
            // Adapt once, so the chunk being copied is one the remesher has
            // actually touched rather than the imported patch.
            for i in 0..<8 {
                brush.center = Fixture.patchCenter(i)
                _ = clay_dynamic_sculptor_stamp(f.sculptor, &brush, &topo, nil, nil)
            }
            var dirtyCount = 0
            _ = clay_dynamic_surface_dirty_chunks(f.sculptor, nil, &dirtyCount)
            var dirty = [UInt32](repeating: 0, count: max(dirtyCount, 1))
            let listed = dirty.withUnsafeMutableBufferPointer { d in
                clay_dynamic_surface_dirty_chunks(f.sculptor, d.baseAddress, &dirtyCount)
            }
            var info = clay_dynamic_chunk_info()
            info.struct_size = UInt32(MemoryLayout<clay_dynamic_chunk_info>.size)
            guard dirtyCount > 0, listed == CLAY_OK else {
                clay_dynamic_sculptor_destroy(f.sculptor)
                clay_dynamic_surface_destroy(f.surface)
                clay_mesh_destroy(f.mesh)
                return nil
            }
            let chunk = Int(dirty[0])
            guard clay_dynamic_surface_chunk_count(f.sculptor) > chunk,
                  clay_dynamic_surface_copy_chunk(f.sculptor, chunk, nil, 0, nil, 0, nil, 0,
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
                        _ = clay_dynamic_surface_copy_chunk(f.sculptor, chunk,
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
