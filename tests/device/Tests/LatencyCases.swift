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
    @discardableResult
    static func addStamp(_ doc: OpaquePointer, _ layer: clay_layer_id, index: Int) -> Bool {
        var params: [Float] = [0.12]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return false
        }
        defer { clay_item_destroy(item) }
        let (x, y, z) = stampPosition(index)
        var position: [Float] = [x, y, z]
        if clay_item_set_position(item, &position) != CLAY_OK { return false }
        var node: clay_node_id = 0
        return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
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

                let r = Timing.measure {
                    // the interactive unit: one stamp, then the evaluation the
                    // host needs before it can draw the result
                    SceneBuilder.addStamp(doc, layer, index: index)
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

}
