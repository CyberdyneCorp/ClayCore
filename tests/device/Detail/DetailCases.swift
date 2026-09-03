// The DETAIL-RESOLUTION case, alone in a bundle and last in the run.
//
// ALONE, and it earned that the way every other split here did: measured.
// Added to ClayCoreDeviceMeasureTests it ran last, sorted after every existing
// case so none of them moved -- and the session that had ended `nominal` for
// as long as the gate has existed ended `serious` on both sides of an A/B,
// which marks the whole run invalid. The heat is the fixture: a whole-form
// fill at voxel_size 0.01 is 3,332 to 5,508 bricks of untimed full-tape
// evaluation per axis point, against the 216 a 0.05 fixture pays, and the
// timed body is 180 ms at the far end rather than 0.05.
//
// A process boundary would not have helped, because heat is not a high-water
// mark: what a bundle split fixes is memory, and this is not a memory problem.
// A SESSION boundary is what makes a suite cold, so this takes one, with
// tools/run_device_bench.sh's cooldown ahead of it.
//
// LAST, for the reason ClayCoreDeviceDyntopoTests is last: the committed
// baselines were all taken in the existing order, project.yml measures
// ordering at 2.7x against a 1.4x tolerance, and a new suite has no business
// moving them. The cost is that this case is measured at the warm end of the
// run, which can only make its own figures pessimistic -- the safe direction
// for a suite whose baseline does not exist yet.

import Foundation
import XCTest
import claycore

final class DetailLatencyTests: XCTestCase {

    /// The axis for `sdf_stamp_detail_bricks`, which stops at 1,000.
    ///
    /// SHORTER than `brickAxis` on purpose, and the reason is the fill rather
    /// than the measurement. This case works at voxel_size 0.01, where the
    /// form's own bound holds 5,832 bricks against the 216 a 0.05 fixture
    /// holds; the untimed whole-form fill that sets the case up therefore costs
    /// 0.2 s at 10 dabs and 1.9 s at 1,000 on an M2 Max, and a 5,000-dab point
    /// costs 8.4 s of setup for a timed body of 255 ms. Two orders of magnitude
    /// is what the growth-axis requirement asks for and this spans them; the
    /// third would be paid almost entirely in setup, on a device whose whole
    /// budget for this bundle is thermal.
    static let detailAxis = [10, 100, 1_000]

    private func abiVersion() -> String {
        var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
        clay_version(&major, &minor, &patch)
        return "\(major).\(minor).\(patch)"
    }

    /// ONE DETAIL DAB ON A FORM, at a resolution that resolves it.
    ///
    /// The same three calls as `sdf_stamp_bricks` — add a node, dirty what it
    /// influences, drain the refill — on a different FIXTURE and at a different
    /// RESOLUTION, and both differences are the point.
    ///
    /// `sdf_stamp_bricks` scatters radius-0.12 spheres through empty space and
    /// evaluates them at voxel_size 0.05, where a brick spans 0.4 units. Its
    /// dabs are smaller than a brick, so every brick a dab dirties straddles
    /// that dab's surface and the refill has to walk all 512 samples of every
    /// one of them. That is a blockout, and it is the only regime this suite
    /// has ever measured.
    ///
    /// A detail pass is the other one: a small dab on a form already there, at
    /// a resolution fine enough to resolve it. The brick is then small against
    /// the FORM, and most of what a dab dirties is solid clay a band or more
    /// inside the surface — bricks that store nothing and are, in principle,
    /// free. Measured on an M2 Max, one dab's dirty set at 1,000 items: 64
    /// bricks, of which 9 straddle the surface and 55 are interior. The same
    /// dab on the blockout fixture: 27 dirtied, 21 straddling, 6 interior.
    ///
    /// SO THE TWO CASES BOUND THE REFILL FROM BOTH ENDS. Anything that changes
    /// what an interior brick costs — a classification shortcut, a cheaper
    /// uniform path, a change to how the band is decided — moves this case and
    /// leaves `sdf_stamp_bricks` flat, and this suite could not previously see
    /// such a change at all. Anything that changes the per-sample walk moves
    /// both.
    ///
    /// NOT BATCHED, for `sdf_stamp_bricks`'s reason: the reset removes the
    /// previous dab, so every timed body starts from a general invalidation and
    /// pays a full recompile. Dabs 2..K inside one body would append onto an
    /// un-invalidated document, which is the suffix path `sdf_stroke_bricks`
    /// owns. The dab positions spread over the form rather than walking it, so
    /// nothing here can be served from what the previous iteration left.
    ///
    /// The timed body is tens of milliseconds at every axis point, so there is
    /// no question of the 0.125 ms floor and no reason to batch for one.
    func testSurfaceDetailStampThroughTheBrickCache() throws {
        let collector = RunCollector()

        var measurements: [Measurement] = []
        let canaryBefore = collector.sampleCanaryNow()
        let caseStartedAtMs = collector.elapsedMs
        let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)

        for dabs in Self.detailAxis {
            guard let (doc, layer) = SceneBuilder.sculptDocument(dabs: dabs) else {
                XCTFail("could not build a \(dabs)-dab sculpt"); continue
            }
            defer { clay_document_destroy(doc) }

            var config = clay_brick_config()
            config.struct_size = UInt32(MemoryLayout<clay_brick_config>.size)
            guard clay_brick_config_defaults(&config) == CLAY_OK else {
                XCTFail("no brick defaults"); continue
            }
            config.voxel_size = SceneBuilder.sculptVoxelSize
            guard let cache = clay_brick_cache_create(&config) else {
                XCTFail("could not create a brick cache"); continue
            }
            defer { clay_brick_cache_destroy(cache) }

            let brickFloats = Int(config.dim * config.dim * config.dim)
            var requests = [clay_brick_request](repeating: clay_brick_request(), count: 4096)
            var values = [Float](repeating: 0, count: requests.count * brickFloats)

            // Drain and fill whatever is queued. Returns the bricks the cache
            // ACCEPTED, which is not the same as the number handed out.
            //
            // `out_accepted` is passed, and that is the one line where this
            // case departs from `sdf_stamp_bricks`'s otherwise identical pump.
            // The ABI refuses a submit that asks for neither `out_results` nor
            // `out_accepted` -- "with neither, a caller cannot tell an accepted
            // brick from a stale one" -- and it has refused since the commit
            // that first exposed the cache, which predates every brick case
            // here. The five older ones pass nil for both, discard the result
            // with `_ =`, and have therefore never stored a brick: their timed
            // work is the eval, which is real and dominant, but their caches
            // are empty from the first fill to the last dab. That is a
            // pre-existing defect in those cases rather than in the engine, and
            // fixing them moves five committed baselines, so it is not done
            // here. It is done HERE because this case cannot state its own
            // invariant without a populated cache: a fixture with no interior
            // is precisely what it exists to avoid measuring.
            func pump() -> Int {
                var accepted = 0
                while true {
                    var count = requests.count
                    var remaining = 0
                    guard clay_brick_cache_take_dirty(cache, &requests, &count,
                                                      &remaining) == CLAY_OK,
                          count > 0 else { return accepted }
                    let e = clay_brick_cache_eval_requests(doc, "cpu", requests, count,
                                                           &values, count * brickFloats,
                                                           nil, 0)
                    XCTAssertEqual(e, CLAY_OK, "eval_requests refused \(count) requests")
                    var took = 0
                    let sub = clay_brick_cache_submit(cache, requests, count, values,
                                                      count * brickFloats, nil, 0,
                                                      nil, &took)
                    XCTAssertEqual(sub, CLAY_OK, "submit refused \(count) bricks")
                    XCTAssertEqual(took, count,
                                   "the cache accepted \(took) of \(count) bricks — a refill "
                                   + "that stores nothing measures an empty cache")
                    accepted += took
                    if remaining == 0 { return accepted }
                }
            }

            // The whole-form fill is a load cost, not a dab cost. It is what
            // gives the case its interior: without it the dab's neighbours are
            // never-evaluated rather than solid, and the refill measures a
            // first fill instead of a detail pass.
            _ = clay_brick_cache_mark_dirty_layer(cache, doc, layer)
            let initial = pump()
            XCTAssertGreaterThan(initial, 0,
                                 "the first fill refreshed no bricks at \(dabs) dabs; "
                                 + "the case below would measure nothing")

            // THE FIXTURE HAS TO HAVE AN INTERIOR, and that is an invariant
            // rather than a hope: it is the whole difference between this case
            // and `sdf_stamp_bricks`, it depends on three constants that could
            // each drift, and a fixture that lost it would keep passing while
            // measuring the case we already have. A form of radius 0.5 filled
            // at voxel 0.01 is six bricks thick, so most of what it stores is
            // not surface.
            var stats = clay_brick_stats()
            stats.struct_size = UInt32(MemoryLayout<clay_brick_stats>.size)
            XCTAssertEqual(clay_brick_cache_stats(cache, &stats), CLAY_OK)
            print("detail fill at \(dabs) dabs: \(stats.surface_bricks) surface of "
                  + "\(stats.tracked_bricks) tracked")
            // BOTH ENDS, because each catches a different way of measuring
            // nothing. A form that is all surface is the blockout fixture again
            // at a finer resolution; a form with no surface at all is an empty
            // cache, which is what a submit whose result went unchecked leaves
            // behind and what this assertion would have caught on the five
            // cases above.
            XCTAssertGreaterThan(stats.surface_bricks, 0,
                                 "at \(dabs) dabs the filled form stores no surface at all")
            XCTAssertLessThan(Double(stats.surface_bricks),
                              0.75 * Double(stats.tracked_bricks),
                              "at \(dabs) dabs the filled form is \(stats.surface_bricks) "
                              + "surface bricks of \(stats.tracked_bricks) tracked — this "
                              + "fixture is supposed to be mostly interior, and one without an "
                              + "interior measures what sdf_stamp_bricks already does")

            var index = dabs
            var starved = 0
            var refreshedTotal = 0
            var refreshCalls = 0
            var lastNode: clay_node_id?
            let r = Timing.measureStable(reset: {
                // Keep the document the size the axis claims, and drain the
                // removal's own refresh here, untimed — `sdf_stamp_bricks`
                // records what leaving it queued costs.
                if let node = lastNode {
                    var one = [node]
                    _ = clay_brick_cache_mark_dirty_nodes(cache, doc, layer, &one, 1, nil)
                    _ = clay_remove_node(doc, layer, node)
                    _ = pump()
                }
            }) {
                guard let node = SceneBuilder.addSculptDabNode(doc, layer, dab: index) else {
                    return
                }
                lastNode = node
                index += 1
                var one = [node]
                _ = clay_brick_cache_mark_dirty_nodes(cache, doc, layer, &one, 1, nil)
                let refreshed = pump()
                refreshedTotal += refreshed
                refreshCalls += 1
                if refreshed == 0 { starved += 1 }
            }
            let perDab = refreshCalls > 0
                ? Double(refreshedTotal) / Double(refreshCalls) : 0
            print("bricks/dab at \(dabs) dabs: \(String(format: "%.1f", perDab)) "
                  + "(initial fill \(initial))")
            XCTAssertLessThan(starved, r.n,
                              "every timed iteration refreshed zero bricks at \(dabs) dabs — "
                              + "this is measuring an empty loop")

            measurements.append(Measurement(stamps: dabs, p50Ms: r.p50,
                                            p95Ms: r.p95, samples: r.n,
                                            repeats: r.repeats,
                                            p95SpreadMs: r.spread))
        }

        collector.add(CaseResult(
            name: "sdf_stamp_detail_bricks",
            verb: "sdf_stamp_incremental",
            budgetClass: .interactive,
            backend: "cpu",
            servedBy: "cpu",
            measurements: measurements,
            growthExponent: Timing.growthExponent(measurements),
            startedAtMs: caseStartedAtMs,
            thermalStateStart: caseThermalStart,
            thermalStateEnd: DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState),
            canaryBeforeMs: canaryBefore,
            canaryAfterMs: collector.sampleCanaryNow()))

        _ = collector.finish(abiVersion: abiVersion(), attachTo: self)
    }
}
