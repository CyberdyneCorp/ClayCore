// The latency cases themselves, and the scene-building they share.
//
// What counts as one "stamp" is the load-bearing definition here. For an SDF
// layer it is NOT just the edit: the tape recompiles on every edit and the
// host cannot draw until it has evaluated the result, so the interactive unit
// is edit-then-evaluate. Timing the edit alone would report a document that
// grows without bound as costing a constant, which is exactly the regression
// this harness exists to catch.

import Foundation
import XCTest
import claycore


// MARK: - The cases

final class LatencyTests: XCTestCase {

    /// Sizes spanning two orders of magnitude, per the growth-axis requirement.
    static let axis = GrowthAxis.standard

    /// Dabs in one stroke, for `testStrokeLatencyOnTheAppendPath`.
    ///
    /// Long enough that the first dab's full compile and upload are amortised
    /// rather than dominant — at 24 the chained dabs outnumber it 23 to 1, so
    /// a regression that put every dab back on the slow path moves the number
    /// by an order of magnitude, not by 4%. Short enough that a pass at the
    /// 1000-stamp end of the axis still costs about what the other cases do.
    static let strokeDabs = 24

    private func abiVersion() -> String {
        var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
        clay_version(&major, &minor, &patch)
        return "\(major).\(minor).\(patch)"
    }

    private func registeredBackends() -> [String] {
        var size = 0
        guard clay_list_backends(nil, &size) == CLAY_OK else { return [] }
        var buffer = [CChar](repeating: 0, count: size)
        guard clay_list_backends(&buffer, &size) == CLAY_OK else { return [] }
        return String(cString: buffer).split(separator: ",").map(String.init)
    }

    /// The benchmark document must be a VOLUME, not a surface.
    ///
    /// Every latency figure this file records is measured against
    /// `SceneBuilder.stampPosition`, and culling cost is entirely a question of
    /// how many item bounds overlap a region. A spread that collapses onto a
    /// plane has an order of magnitude more neighbours per brick than a real
    /// sculpt, so the budgets would be calibrated against a workload nobody
    /// runs. The previous golden-ratio walk did exactly that — see the note on
    /// `stampMultipliers`.
    ///
    /// Two independent checks, because either alone is cheap to satisfy by
    /// accident: every cell of a 4x4x4 partition of the working volume must be
    /// reached, and no pair of axes may be correlated. The old walk scored
    /// 9 cells of 64 and a correlation of -0.9985 between x and y.
    func testStampSpreadFillsTheVolume() throws {
        let n = 4000, bins = 4
        var seen = Set<Int>()
        var xs = [[Double]](repeating: [], count: 3)
        for i in 0..<n {
            let (x, y, z) = SceneBuilder.stampPosition(i)
            let c = [Double(x), Double(y), Double(z)]
            for a in 0..<3 { xs[a].append(c[a]) }
            // (p + 0.8) / 1.6 maps the working volume back to [0,1).
            let cell = c.map { min(bins - 1, max(0, Int(($0 + 0.8) / 1.6 * Double(bins)))) }
            seen.insert((cell[0] * bins + cell[1]) * bins + cell[2])
        }
        XCTAssertEqual(seen.count, bins * bins * bins,
                       "the stamp spread reached \(seen.count) of \(bins * bins * bins) cells; "
                       + "a spread that misses cells is lying about how dense a brick is")

        for (a, b) in [(0, 1), (0, 2), (1, 2)] {
            let ma = xs[a].reduce(0, +) / Double(n), mb = xs[b].reduce(0, +) / Double(n)
            var num = 0.0, da = 0.0, db = 0.0
            for i in 0..<n {
                num += (xs[a][i] - ma) * (xs[b][i] - mb)
                da += (xs[a][i] - ma) * (xs[a][i] - ma)
                db += (xs[b][i] - mb) * (xs[b][i] - mb)
            }
            let r = num / (da.squareRoot() * db.squareRoot())
            XCTAssertLessThan(abs(r), 0.05,
                              "axes \(a) and \(b) correlate at \(r); the multipliers are "
                              + "algebraically related and the cloud is not three-dimensional")
        }
    }

    /// One SDF brush stamp, edit-then-evaluate, on both backends.
    ///
    /// This is the case the whole harness exists for: it is the only one that
    /// pays the tape recompile, so it is where a growth regression shows.
    func testStampLatencyAcrossDocumentGrowth() throws {
        let collector = RunCollector()
        let available = registeredBackends()
        XCTAssertTrue(available.contains("cpu"))

        let points = SceneBuilder.drawLattice()
        let pointCount = points.count / 3

        for backend in ["cpu", "metal"] {
            guard available.contains(backend) else {
                XCTFail("backend '\(backend)' did not register on this device; "
                        + "a case cannot be attributed to a backend that is absent")
                continue
            }
            // Attribution rests on the ABI refusing an unknown backend rather
            // than falling back: `clay_eval_points` with an unregistered name
            // returns non-OK and writes no distances (measured — a bogus name
            // gives error 2). So an eval that returns OK for "metal" WAS
            // served by Metal, and the assert below is what makes `servedBy`
            // a fact rather than a restatement of the request.

            var measurements: [Measurement] = []
            let canaryBefore = collector.sampleCanaryNow()
            let caseStartedAtMs = collector.elapsedMs
            let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)
            for stamps in Self.axis {
                guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else {
                    XCTFail("could not build a \(stamps)-stamp document"); continue
                }
                defer { clay_document_destroy(doc) }

                var pts = points
                var out = [Float](repeating: 0, count: pointCount)
                var index = stamps
                var evalFailures = 0
                var lastNode: clay_node_id?

                let r = Timing.measureStable(reset: {
                    // Undo the stamp OUTSIDE the timing, so the document stays
                    // the size the axis says it is. Without this the document
                    // grows by one per iteration and the smallest axis point
                    // measures the average over a range instead of a size.
                    if let node = lastNode { _ = clay_remove_node(doc, layer, node) }
                }) {
                    // the interactive unit: one stamp, then the evaluation the
                    // host needs before it can draw the result
                    lastNode = SceneBuilder.addStampNode(doc, layer, index: index)
                    index += 1
                    if clay_eval_points(doc, backend, &pts, pointCount, &out, nil) != CLAY_OK {
                        evalFailures += 1
                    }
                }
                XCTAssertEqual(evalFailures, 0,
                               "\(backend) failed to evaluate at \(stamps) stamps")
                measurements.append(Measurement(stamps: stamps, p50Ms: r.p50,
                                                p95Ms: r.p95, samples: r.n,
                                                repeats: r.repeats,
                                                p95SpreadMs: r.spread))
            }

            collector.add(CaseResult(
                name: "sdf_stamp_\(backend)",
                verb: "sdf_stamp",
                budgetClass: .interactive,
                backend: backend,
                servedBy: backend,
                measurements: measurements,
                growthExponent: Timing.growthExponent(measurements),
                startedAtMs: caseStartedAtMs,
                thermalStateStart: caseThermalStart,
                thermalStateEnd: DeviceInfo.thermalName(
                    ProcessInfo.processInfo.thermalState),
                canaryBeforeMs: canaryBefore,
                canaryAfterMs: collector.sampleCanaryNow()))
        }

        let record = collector.finish(abiVersion: abiVersion(), attachTo: self)
        XCTAssertTrue(record.valid,
                      "thermal state was \(record.thermalStateStart) -> "
                      + "\(record.thermalStateEnd); a throttled run is a different "
                      + "experiment, not a slower result")
    }

    /// A STROKE, which is the only shape that reaches the append fast path.
    ///
    /// `sdf_stamp_*` above removes the stamp between iterations so the document
    /// stays the size the axis claims. That is right for the question it asks,
    /// and it is exactly what makes it blind to this one: a removal is a
    /// general invalidation, so every iteration recompiles the whole tape and
    /// re-uploads it. A host sculpting does not do that. It appends dab after
    /// dab, and since #294 the compiler reuses the compiled prefix and the
    /// GPU backend copies only the suffix onto a tape it already holds.
    ///
    /// THE UNIT IS A STROKE, NOT A DAB, and it has to be. The chain is what is
    /// under test: the first dab of a stroke pays a full compile and a full
    /// upload, and every dab after it is served from what the one before left
    /// behind. Timing a single dab with a reset would measure the first case
    /// over and over and never the second — and there is no reset that
    /// restores a document without breaking the chain, because breaking it is
    /// what a reset IS.
    ///
    /// The recorded numbers are per dab: the stroke's cost divided by its
    /// dabs. So `p95Ms` is the mean dab of the 95th-percentile STROKE rather
    /// than the 95th-percentile dab — a coarser statistic than the other
    /// cases', and the one the unit permits. It is directly comparable to
    /// `sdf_stamp_\(backend)`, and the gap between them is what a host gives
    /// up by invalidating instead of appending.
    ///
    /// TWO THINGS ABOUT THAT COMPARISON, both of which flatter nobody:
    ///
    /// The axis point is the size the stroke STARTS from, not the size it is
    /// measured at — a stroke adds `strokeDabs` items as it runs. At 1000 that
    /// is +2.4% and immaterial; at 10 the document more than triples, so the
    /// small end of this case measures a bigger document than the same point
    /// of `sdf_stamp_*` does and is the harder comparison of the two, not the
    /// easier one. Measured on the reference iPad: `sdf_stroke_cpu` reads
    /// 0.076 ms against `sdf_stamp_cpu`'s 0.038 ms at 10 stamps for exactly
    /// this reason, and the two converge by 1000 (2.370 against 2.419).
    ///
    /// And the gap is NOT the GPU patch alone. Appending is cheaper to compile
    /// as well as cheaper to transfer, and this case pays both — which is
    /// right, because a host pays both. What isolates the transfer is
    /// `BM_MetalStrokePatched`/`BM_MetalStrokeReupload`, which hold the compile
    /// fixed and strip the lineage on one side.
    func testStrokeLatencyOnTheAppendPath() throws {
        let collector = RunCollector()
        let available = registeredBackends()
        XCTAssertTrue(available.contains("cpu"))

        let points = SceneBuilder.drawLattice()
        let pointCount = points.count / 3

        for backend in ["cpu", "metal"] {
            guard available.contains(backend) else {
                XCTFail("backend '\(backend)' did not register on this device; "
                        + "a case cannot be attributed to a backend that is absent")
                continue
            }

            var measurements: [Measurement] = []
            let canaryBefore = collector.sampleCanaryNow()
            let caseStartedAtMs = collector.elapsedMs
            let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)
            for stamps in Self.axis {
                guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else {
                    XCTFail("could not build a \(stamps)-stamp document"); continue
                }
                defer { clay_document_destroy(doc) }

                var pts = points
                var out = [Float](repeating: 0, count: pointCount)
                var index = stamps
                var evalFailures = 0
                var addFailures = 0
                var stroke: [clay_node_id] = []

                let r = Timing.measureStable(reset: {
                    // Roll the stroke back OUTSIDE the timing, so every pass
                    // starts from the same document the axis names. This is
                    // also what re-arms the measurement: the removals break
                    // the append chain, so the next stroke's first dab pays a
                    // full compile again, exactly as a real one does.
                    for node in stroke.reversed() { _ = clay_remove_node(doc, layer, node) }
                    stroke.removeAll(keepingCapacity: true)
                }) {
                    for _ in 0..<Self.strokeDabs {
                        guard let node = SceneBuilder.addStampNode(doc, layer, index: index)
                        else { addFailures += 1; continue }
                        stroke.append(node)
                        index += 1
                        // The interactive unit, once per dab: the edit, then
                        // the evaluation the host needs before it can draw.
                        if clay_eval_points(doc, backend, &pts, pointCount, &out, nil)
                            != CLAY_OK {
                            evalFailures += 1
                        }
                    }
                }
                XCTAssertEqual(addFailures, 0,
                               "a dab could not be added at \(stamps) stamps")
                XCTAssertEqual(evalFailures, 0,
                               "\(backend) failed to evaluate at \(stamps) stamps")

                let perDab = Double(Self.strokeDabs)
                measurements.append(Measurement(stamps: stamps,
                                                p50Ms: r.p50 / perDab,
                                                p95Ms: r.p95 / perDab,
                                                samples: r.n,
                                                repeats: r.repeats,
                                                p95SpreadMs: r.spread / perDab))
            }

            collector.add(CaseResult(
                name: "sdf_stroke_\(backend)",
                verb: "sdf_stroke",
                budgetClass: .interactive,
                backend: backend,
                servedBy: backend,
                measurements: measurements,
                growthExponent: Timing.growthExponent(measurements),
                startedAtMs: caseStartedAtMs,
                thermalStateStart: caseThermalStart,
                thermalStateEnd: DeviceInfo.thermalName(
                    ProcessInfo.processInfo.thermalState),
                canaryBeforeMs: canaryBefore,
                canaryAfterMs: collector.sampleCanaryNow()))
        }

        let record = collector.finish(abiVersion: abiVersion(), attachTo: self)
        XCTAssertTrue(record.valid,
                      "thermal state was \(record.thermalStateStart) -> "
                      + "\(record.thermalStateEnd); a throttled run is a different "
                      + "experiment, not a slower result")
    }

    /// The brick refresh across an UNBROKEN stroke, which is the only case
    /// here that can reach the resumed-refill path at all.
    ///
    /// `sdf_stamp_bricks` drives the same three calls and cannot show this,
    /// because its reset REMOVES the node every iteration — and a removal is
    /// not an append, so the chain breaks and every timed dab starts from
    /// nothing. That is the harness gap this case exists to close: through
    /// v0.52.2 the suite had no case in which `clay_brick_cache_eval_requests`
    /// was ever asked to continue from a previous dab, so the whole of #306's
    /// work (#308 seeded suffix, #309 cull-index append, #310/#311 resumed
    /// refill) was invisible to the device gate. Measured off-device that work
    /// is 10x-93x on this exact call; measured here it was 1.00x, because the
    /// fast path never ran.
    ///
    /// The rollback is in `reset`, untimed, and it does two jobs: it keeps the
    /// document the size the axis names, and it re-arms the case — the
    /// removals break the append chain, so the next stroke's first dab pays a
    /// full refill again, exactly as a real first dab does.
    ///
    /// THE REMOVAL'S OWN REFRESH IS DRAINED IN `reset`, for the reason
    /// `sdf_stamp_bricks` records: left queued it lands in the next
    /// iteration's pump and every timed sample refreshes for a removal as well
    /// as for an addition, which measured ~2x high there.
    ///
    /// Distances only (nil colours), matching `sdf_stamp_bricks` so the two
    /// are comparable. #311 carries colour through the same path and a
    /// coloured case would measure a different thing; it is not added here
    /// because the pair that makes this number mean something is
    /// stamp-vs-stroke on ONE channel.
    func testStrokeRefreshOnTheAppendPath() throws {
        let collector = RunCollector()

        var measurements: [Measurement] = []
        let canaryBefore = collector.sampleCanaryNow()
        let caseStartedAtMs = collector.elapsedMs
        let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)
        for stamps in Self.axis {
            guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else {
                XCTFail("could not build a \(stamps)-stamp document"); continue
            }
            defer { clay_document_destroy(doc) }

            var config = clay_brick_config()
            config.struct_size = UInt32(MemoryLayout<clay_brick_config>.size)
            guard clay_brick_config_defaults(&config) == CLAY_OK else {
                XCTFail("no brick defaults"); continue
            }
            config.voxel_size = 0.05
            guard let cache = clay_brick_cache_create(&config) else {
                XCTFail("could not create a brick cache"); continue
            }
            defer { clay_brick_cache_destroy(cache) }

            let brickFloats = Int(config.dim * config.dim * config.dim)
            var requests = [clay_brick_request](repeating: clay_brick_request(), count: 4096)
            var values = [Float](repeating: 0, count: requests.count * brickFloats)

            func pump() -> Int {
                var refreshed = 0
                while true {
                    var count = requests.count
                    var remaining = 0
                    guard clay_brick_cache_take_dirty(cache, &requests, &count,
                                                      &remaining) == CLAY_OK,
                          count > 0 else { return refreshed }
                    _ = clay_brick_cache_eval_requests(doc, "cpu", requests, count,
                                                       &values, count * brickFloats,
                                                       nil, 0)
                    _ = clay_brick_cache_submit(cache, requests, count, values,
                                                count * brickFloats, nil, 0,
                                                nil, nil)
                    refreshed += count
                    if remaining == 0 { return refreshed }
                }
            }

            // The initial full fill is a load cost, not a stroke cost.
            _ = clay_brick_cache_mark_dirty_layer(cache, doc, layer)
            let initial = pump()
            XCTAssertGreaterThan(initial, 0,
                                 "the first fill refreshed no bricks at \(stamps) stamps; "
                                 + "the case below would measure nothing")

            var stroke: [clay_node_id] = []
            var starved = 0
            var refreshedTotal = 0
            var refreshCalls = 0
            var addFailures = 0

            let r = Timing.measureStable(reset: {
                guard !stroke.isEmpty else { return }
                var all = stroke
                _ = clay_brick_cache_mark_dirty_nodes(cache, doc, layer, &all, all.count, nil)
                for node in stroke.reversed() { _ = clay_remove_node(doc, layer, node) }
                stroke.removeAll(keepingCapacity: true)
                _ = pump()
            }) {
                for k in 0..<Self.strokeDabs {
                    // A STROKE, not a spread: SceneBuilder.strokeDabPosition
                    // marches the dabs along a short path so consecutive ones
                    // overlap. That is what the resumed refill continues from,
                    // and a scattered dab reaches a brick whose seed is from a
                    // different revision. Measured off-device, the spread
                    // reports this work as 1.05x and the path as 7.6x.
                    guard let node = SceneBuilder.addStrokeDabNode(doc, layer, dab: k)
                    else { addFailures += 1; continue }
                    stroke.append(node)
                    var one = [node]
                    _ = clay_brick_cache_mark_dirty_nodes(cache, doc, layer, &one, 1, nil)
                    let refreshed = pump()
                    refreshedTotal += refreshed
                    refreshCalls += 1
                    if refreshed == 0 { starved += 1 }
                }
            }
            XCTAssertEqual(addFailures, 0,
                           "a dab could not be added at \(stamps) stamps")
            XCTAssertLessThan(starved, refreshCalls,
                              "every dab refreshed zero bricks at \(stamps) stamps — "
                              + "this is measuring an empty loop")

            let perStamp = refreshCalls > 0
                ? Double(refreshedTotal) / Double(refreshCalls) : 0
            print("stroke bricks/dab at \(stamps) stamps: "
                  + "\(String(format: "%.1f", perStamp)) (initial fill \(initial))")

            let perDab = Double(Self.strokeDabs)
            measurements.append(Measurement(stamps: stamps,
                                            p50Ms: r.p50 / perDab,
                                            p95Ms: r.p95 / perDab,
                                            samples: r.n,
                                            repeats: r.repeats,
                                            p95SpreadMs: r.spread / perDab))
        }

        collector.add(CaseResult(
            name: "sdf_stroke_bricks",
            verb: "sdf_stroke_incremental",
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

    /// The same stamp, refreshed the way a host actually refreshes it.
    ///
    /// `sdf_stamp_*` above re-evaluates a lattice over the WHOLE working
    /// volume after every stamp, which is an upper bound rather than the
    /// interactive cost: it gives culling nothing to cull. A host drives the
    /// brick cache instead — dirty the bricks the new node's influence
    /// reaches, drain those requests, evaluate each against its own culled
    /// tape, submit. That is this case, and the gap between the two is what
    /// the incremental path is worth.
    ///
    /// The tape still recompiles per edit either way, so whatever growth
    /// survives here is the part culling cannot remove.
    func testStampLatencyThroughTheBrickCache() throws {
        let collector = RunCollector()

        var measurements: [Measurement] = []
        let canaryBefore = collector.sampleCanaryNow()
        let caseStartedAtMs = collector.elapsedMs
        let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)
        for stamps in Self.axis {
            guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else {
                XCTFail("could not build a \(stamps)-stamp document"); continue
            }
            defer { clay_document_destroy(doc) }

            var config = clay_brick_config()
            config.struct_size = UInt32(MemoryLayout<clay_brick_config>.size)
            guard clay_brick_config_defaults(&config) == CLAY_OK else {
                XCTFail("no brick defaults"); continue
            }
            config.voxel_size = 0.05
            guard let cache = clay_brick_cache_create(&config) else {
                XCTFail("could not create a brick cache"); continue
            }
            defer { clay_brick_cache_destroy(cache) }

            let brickFloats = Int(config.dim * config.dim * config.dim)
            var requests = [clay_brick_request](repeating: clay_brick_request(), count: 4096)
            var values = [Float](repeating: 0, count: requests.count * brickFloats)

            // Drain and fill whatever is queued. Returns the bricks refreshed.
            func pump() -> Int {
                var refreshed = 0
                while true {
                    var count = requests.count
                    var remaining = 0
                    guard clay_brick_cache_take_dirty(cache, &requests, &count,
                                                      &remaining) == CLAY_OK,
                          count > 0 else { return refreshed }
                    // "cpu" deliberately: the header records that Metal costs
                    // 288 us per brick against the CPU's 114 us and does not
                    // win at any thread count — 512 samples is too little work
                    // to cover a dispatch. This case measures the documented
                    // path, not a worse one.
                    // ABI 0.26.0 gave the cache a colour channel ("the cache
                    // becomes a GPU atlas"), so eval and submit each take a
                    // colours buffer and its capacity. Passing nil/0 asks for
                    // distances only, which is what this case measures.
                    _ = clay_brick_cache_eval_requests(doc, "cpu", requests, count,
                                                       &values, count * brickFloats,
                                                       nil, 0)
                    _ = clay_brick_cache_submit(cache, requests, count, values,
                                                count * brickFloats, nil, 0,
                                                nil, nil)
                    refreshed += count
                    if remaining == 0 { return refreshed }
                }
            }

            // The initial full fill is a load cost, not a stamp cost.
            _ = clay_brick_cache_mark_dirty_layer(cache, doc, layer)
            let initial = pump()
            XCTAssertGreaterThan(initial, 0,
                                 "the first fill refreshed no bricks at \(stamps) stamps; "
                                 + "the case below would measure nothing")

            var index = stamps
            var starved = 0
            var refreshedTotal = 0
            var refreshCalls = 0
            var lastNode: clay_node_id?
            let r = Timing.measureStable(reset: {
                // Same reason as the global case: keep the document the size
                // the axis claims.
                //
                // The removal's own refresh is drained HERE, untimed. Leaving
                // it queued would push it into the next iteration's pump, so
                // every timed sample would refresh the bricks for a removal
                // as well as for an addition — roughly twice the work a host
                // actually does, and the case measured ~2x high until this
                // drained it.
                if let node = lastNode {
                    var one = [node]
                    _ = clay_brick_cache_mark_dirty_nodes(cache, doc, layer, &one, 1, nil)
                    _ = clay_remove_node(doc, layer, node)
                    _ = pump()
                }
            }) {
                // one stamp, then the refresh a host would do for it
                guard let node = SceneBuilder.addStampNode(doc, layer, index: index) else {
                    return
                }
                lastNode = node
                index += 1
                // Dirty by NODE, so the cache derives the influence bound
                // itself. The bound is the thing most likely to be got
                // silently wrong by hand, and one that is too tight leaves
                // visibly stale bricks at a blend seam.
                var one = [node]
                _ = clay_brick_cache_mark_dirty_nodes(cache, doc, layer, &one, 1, nil)
                let refreshed = pump()
                refreshedTotal += refreshed
                refreshCalls += 1
                if refreshed == 0 { starved += 1 }
            }
            // How many bricks one stamp costs. This is what separates "the
            // incremental path refreshes too much" from "each brick is
            // expensive": eval_requests compiles a CULLED TAPE PER BRICK, so
            // the per-stamp cost is (bricks dirtied) x O(document).
            let perStamp = refreshCalls > 0
                ? Double(refreshedTotal) / Double(refreshCalls) : 0
            print("bricks/stamp at \(stamps) stamps: \(String(format: "%.1f", perStamp)) "
                  + "(initial fill \(initial))")
            XCTAssertLessThan(starved, r.n,
                              "every timed iteration refreshed zero bricks at "
                              + "\(stamps) stamps — this is measuring an empty loop")

            measurements.append(Measurement(stamps: stamps, p50Ms: r.p50,
                                            p95Ms: r.p95, samples: r.n,
                                            repeats: r.repeats,
                                            p95SpreadMs: r.spread))
        }

        collector.add(CaseResult(
            name: "sdf_stamp_bricks",
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
