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
    static func stampPosition(_ index: Int) -> (Float, Float, Float) {
        // A low-discrepancy-ish walk: cheap, deterministic, and it does not
        // pile every stamp on one spot the way a modulo grid would.
        let g = 0.6180339887
        let x = Float((Double(index) * g).truncatingRemainder(dividingBy: 1.0)) * 1.6 - 0.8
        let y = Float((Double(index) * g * g).truncatingRemainder(dividingBy: 1.0)) * 1.6 - 0.8
        let z = Float((Double(index) * g * g * g).truncatingRemainder(dividingBy: 1.0)) * 1.6 - 0.8
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

    /// One stamp: a small blended sphere, which is what a stroke deposits.
    /// Returns the node it became, which the brick-cache case needs to dirty
    /// exactly the region the new item influences.
    static func addStampNode(_ doc: OpaquePointer, _ layer: clay_layer_id,
                             index: Int) -> clay_node_id? {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return nil
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = stampPosition(index)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return nil }
        var node: clay_node_id = 0
        guard clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else { return nil }
        return node
    }

    @discardableResult
    static func addStamp(_ doc: OpaquePointer, _ layer: clay_layer_id, index: Int) -> Bool {
        addStampNode(doc, layer, index: index) != nil
    }
}

// MARK: - The cases

final class LatencyTests: XCTestCase {

    /// Sizes spanning two orders of magnitude, per the growth-axis requirement.
    static let axis = [10, 100, 1000]

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

                let r = Timing.measure(reset: {
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
                                                p95Ms: r.p95, samples: r.n))
            }

            collector.add(CaseResult(
                name: "sdf_stamp_\(backend)",
                verb: "sdf_stamp",
                budgetClass: .interactive,
                backend: backend,
                servedBy: backend,
                measurements: measurements,
                growthExponent: Timing.growthExponent(measurements)))
        }

        let record = collector.finish(abiVersion: abiVersion(), attachTo: self)
        XCTAssertTrue(record.valid,
                      "thermal state was \(record.thermalStateStart) -> "
                      + "\(record.thermalStateEnd); a throttled run is a different "
                      + "experiment, not a slower result")
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
        for stamps in Self.axis {
            guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else {
                XCTFail("could not build a \(stamps)-stamp document"); continue
            }
            defer { clay_document_destroy(doc) }

            var config = clay_brick_config()
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
            let r = Timing.measure(reset: {
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
                                            p95Ms: r.p95, samples: r.n))
        }

        collector.add(CaseResult(
            name: "sdf_stamp_bricks",
            verb: "sdf_stamp_incremental",
            budgetClass: .interactive,
            backend: "cpu",
            servedBy: "cpu",
            measurements: measurements,
            growthExponent: Timing.growthExponent(measurements)))

        _ = collector.finish(abiVersion: abiVersion(), attachTo: self)
    }
}
