// The first claycore code to run on a real iPad.
//
// `evaluation-backends` calls Metal "the iPad app's production path", and until
// this ran, no iPad had ever executed it — the metallib was compiled against
// the macOS SDK and the xcframework shipped CPU-only, so the claim was
// unverifiable rather than merely untested.
//
// What this asserts is deliberately narrow: that the backend registers on the
// device, and that it agrees with the CPU on the device. Latency lives in its
// own cases so a correctness failure never reads as a slow result.

import XCTest
import claycore

final class ClayCoreDeviceTests: XCTestCase {

    /// Names of every backend the library registered on this device.
    private func registeredBackends() -> [String] {
        // size-query pattern: a null buffer reports the length it needs
        var size = 0
        XCTAssertEqual(clay_list_backends(nil, &size), CLAY_OK)
        var buffer = [CChar](repeating: 0, count: size)
        XCTAssertEqual(clay_list_backends(&buffer, &size), CLAY_OK)
        return String(cString: buffer).split(separator: ",").map(String.init)
    }

    func testMetalRegistersOnDevice() {
        var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
        clay_version(&major, &minor, &patch)

        let device = UIDevice.current
        let backends = registeredBackends()
        print("claycore ABI \(major).\(minor).\(patch) on \(device.model), "
              + "iOS \(device.systemVersion): backends = \(backends.joined(separator: ", "))")

        XCTAssertTrue(backends.contains("cpu"), "the CPU backend is always present")
        XCTAssertTrue(
            backends.contains("metal"),
            "Metal did not register on device — either the slice was built "
            + "without the backend, or its metallib was compiled for the wrong "
            + "SDK and failed to load. Both look identical from here.")
    }

    /// Metal must agree with the CPU *on this hardware*, not only on a Mac.
    func testMetalAgreesWithCPUOnDevice() throws {
        try XCTSkipUnless(registeredBackends().contains("metal"),
                          "no Metal backend registered to compare against")

        guard let doc = clay_document_create() else {
            return XCTFail("could not create a document")
        }
        defer { clay_document_destroy(doc) }

        var layer: clay_layer_id = 0
        XCTAssertEqual(clay_add_sdf_layer(doc, "device", &layer), CLAY_OK)

        var params: [Float] = [0.8]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
            return XCTFail("could not build the sphere")
        }
        var node: clay_node_id = 0
        XCTAssertEqual(clay_layer_add_item(doc, layer, item, &node), CLAY_OK)
        clay_item_destroy(item)

        // a lattice through the surface, so both backends are asked about
        // points inside, outside, and near the zero set
        var points: [Float] = []
        for i in 0..<12 {
            for j in 0..<12 {
                points.append(Float(i) / 11.0 * 2.0 - 1.0)
                points.append(Float(j) / 11.0 * 2.0 - 1.0)
                points.append(0.25)
            }
        }
        let count = points.count / 3

        var cpu = [Float](repeating: 0, count: count)
        var metal = [Float](repeating: 0, count: count)
        XCTAssertEqual(clay_eval_points(doc, "cpu", &points, count, &cpu, nil), CLAY_OK)
        XCTAssertEqual(clay_eval_points(doc, "metal", &points, count, &metal, nil), CLAY_OK)

        // the tolerance `evaluation-backends` documents for GPU backends
        var worst: Float = 0
        for i in 0..<count {
            worst = max(worst, abs(cpu[i] - metal[i]) / max(1, abs(cpu[i])))
        }
        print("worst CPU-vs-Metal relative error on device: \(worst)")
        XCTAssertLessThanOrEqual(worst, 1e-4, "Metal disagrees with CPU on device")
    }
}
