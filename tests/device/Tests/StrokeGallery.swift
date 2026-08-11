// What a brush costs over a SESSION, and what it left behind.
//
// Every other case in this harness measures one application of a verb,
// repeated with a reset that undoes it — deliberately, so the document stays
// the size the growth axis claims. That answers "what does one dab cost on a
// document of size N" and says nothing about the shape an artist actually
// works in, which is stroke after stroke on a document THEY are growing.
//
// Two things are added here:
//
//  * per-stroke timing as strokes ACCUMULATE, so the cost of stroke 8 is
//    visible next to the cost of stroke 1. This is the shape that degrades:
//    `add-move-drag-continuity` exists because a Move stacked a warp per drag,
//    and hPolish corrupts by the third pass. A single-application measurement
//    cannot see either.
//  * a render of the result, so the numbers can be looked AT rather than only
//    read. A brush that got fast by doing nothing is a passing latency case
//    and an obviously wrong picture.

import Foundation
import XCTest
import UIKit
import claycore

// MARK: - Rendering

enum Render {
    /// Sphere-trace the document through the ABI's batch raycast and shade by
    /// normal. Rendering on the device is the point: this is the same field
    /// evaluation the latency cases time, so a picture and a number come from
    /// one run and cannot disagree about what was measured.
    static func image(of doc: OpaquePointer, size: Int = 320,
                      backend: String = "cpu") -> UIImage? {
        // A fixed three-quarter camera. Deliberately not derived from the
        // document's bounds: two runs of the same brush must frame the shape
        // identically or the pictures cannot be compared.
        let eye: (Float, Float, Float) = (2.2, 1.7, 2.2)
        let target: (Float, Float, Float) = (0, 0, 0)
        let up: (Float, Float, Float) = (0, 1, 0)

        func norm(_ v: (Float, Float, Float)) -> (Float, Float, Float) {
            let l = max(sqrt(v.0 * v.0 + v.1 * v.1 + v.2 * v.2), 1e-6)
            return (v.0 / l, v.1 / l, v.2 / l)
        }
        func cross(_ a: (Float, Float, Float), _ b: (Float, Float, Float))
            -> (Float, Float, Float) {
            (a.1 * b.2 - a.2 * b.1, a.2 * b.0 - a.0 * b.2, a.0 * b.1 - a.1 * b.0)
        }

        let forward = norm((target.0 - eye.0, target.1 - eye.1, target.2 - eye.2))
        let right = norm(cross(forward, up))
        let camUp = cross(right, forward)

        var rays = [Float]()
        rays.reserveCapacity(size * size * 6)
        let fov: Float = 0.62   // half-extent at unit distance
        for y in 0..<size {
            for x in 0..<size {
                let u = (Float(x) / Float(size - 1) * 2 - 1) * fov
                let v = (1 - Float(y) / Float(size - 1) * 2) * fov
                let d = norm((forward.0 + right.0 * u + camUp.0 * v,
                              forward.1 + right.1 * u + camUp.1 * v,
                              forward.2 + right.2 * u + camUp.2 * v))
                rays.append(contentsOf: [eye.0, eye.1, eye.2, d.0, d.1, d.2])
            }
        }

        let count = size * size
        var hits = [Int32](repeating: 0, count: count)
        var normals = [Float](repeating: 0, count: count * 3)
        guard clay_raycast_many(doc, &rays, count, &hits, nil, nil,
                                &normals) == CLAY_OK else { return nil }

        // Shade: a key light plus a fill, so curvature reads. RGBA8.
        var pixels = [UInt8](repeating: 0, count: count * 4)
        let key = norm((0.5, 0.8, 0.35))
        let fill = norm((-0.6, 0.2, -0.4))
        for i in 0..<count {
            let base = i * 4
            if hits[i] == 0 {
                pixels[base] = 24; pixels[base + 1] = 26
                pixels[base + 2] = 30; pixels[base + 3] = 255
                continue
            }
            let n = (normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2])
            let kd = max(n.0 * key.0 + n.1 * key.1 + n.2 * key.2, 0)
            let fd = max(n.0 * fill.0 + n.1 * fill.1 + n.2 * fill.2, 0)
            let lit = min(0.12 + 0.78 * kd + 0.25 * fd, 1.0)
            // tint slightly by normal so facing changes are visible in print
            pixels[base] = UInt8(min(lit * (0.80 + 0.20 * abs(n.0)) * 255, 255))
            pixels[base + 1] = UInt8(min(lit * (0.84 + 0.16 * abs(n.1)) * 255, 255))
            pixels[base + 2] = UInt8(min(lit * (0.90 + 0.10 * abs(n.2)) * 255, 255))
            pixels[base + 3] = 255
        }

        let cs = CGColorSpaceCreateDeviceRGB()
        guard let provider = CGDataProvider(data: Data(pixels) as CFData),
              let cg = CGImage(width: size, height: size, bitsPerComponent: 8,
                               bitsPerPixel: 32, bytesPerRow: size * 4,
                               space: cs,
                               bitmapInfo: CGBitmapInfo(
                                   rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                               provider: provider, decode: nil,
                               shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        return UIImage(cgImage: cg)
    }

    /// Attach a render so `xcresulttool export attachments` can pull it out.
    static func attach(_ image: UIImage, named name: String, to testCase: XCTestCase) {
        guard let data = image.pngData() else { return }
        let attachment = XCTAttachment(data: data, uniformTypeIdentifier: "public.png")
        attachment.name = "gallery-\(name).png"
        attachment.lifetime = .keepAlways
        testCase.add(attachment)
    }
}

// MARK: - A session per brush

/// One brush, several strokes, timed as they accumulate — then rendered.
struct StrokeSession {
    let name: String
    /// Applies stroke `i` to the document. Returns false to fail the case.
    let stroke: (OpaquePointer, clay_layer_id, Int) -> Bool
}

final class StrokeGalleryTests: XCTestCase {

    /// Strokes per session. Eight because that is the length the roadmap's own
    /// degradation notes use — "2.7x over eight strokes", "79x by nine drags".
    static let strokeCount = 8

    private func abiVersion() -> String {
        var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
        clay_version(&major, &minor, &patch)
        return "\(major).\(minor).\(patch)"
    }

    /// A drag across the form, offset per stroke so a session covers the
    /// surface rather than carving the same groove eight times.
    private static func dragSamples(_ index: Int, count: Int = 24) -> [Float] {
        var samples: [Float] = []
        let phase = Float(index) * 0.7
        for i in 0..<count {
            let t = Float(i) / Float(count - 1)
            samples.append(contentsOf: [
                t * 1.3 - 0.65,
                sin(t * 2.6 + phase) * 0.35,
                cos(phase) * 0.3,
                1.0,
                t,
            ])
        }
        return samples
    }

    /// The sessions. One per brush that can be driven as a stroke on an SDF
    /// layer through the C ABI.
    private func sessions() -> [StrokeSession] {
        [
            StrokeSession(name: "stroke_build") { doc, layer, i in
                var preset = Fixture.strokePreset(radius: 0.16)
                var samples = Self.dragSamples(i)
                let sampleCount = samples.count / 5
                var capacity = 0
                guard clay_stroke_resolve(&samples, sampleCount, &preset, nil,
                                          &capacity) == CLAY_OK, capacity > 0 else {
                    return false
                }
                var stamps = [clay_stamp](repeating: clay_stamp(), count: capacity)
                guard clay_stroke_resolve(&samples, sampleCount, &preset, &stamps,
                                          &capacity) == CLAY_OK else { return false }
                for stamp in stamps.prefix(capacity) {
                    var r: [Float] = [stamp.radius]
                    guard let item = clay_item_create(
                        Int32(CLAY_PRIM_SPHERE.rawValue), &r, 1) else { return false }
                    var pos = [stamp.position.0, stamp.position.1, stamp.position.2]
                    _ = clay_item_set_position(item, &pos)
                    _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.06)
                    var node: clay_node_id = 0
                    let ok = clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
                    clay_item_destroy(item)
                    if !ok { return false }
                }
                return true
            },

            StrokeSession(name: "stroke_carve") { doc, layer, i in
                var preset = Fixture.strokePreset(radius: 0.10)
                var samples = Self.dragSamples(i + 3)
                let sampleCount = samples.count / 5
                var capacity = 0
                guard clay_stroke_resolve(&samples, sampleCount, &preset, nil,
                                          &capacity) == CLAY_OK, capacity > 0 else {
                    return false
                }
                var stamps = [clay_stamp](repeating: clay_stamp(), count: capacity)
                guard clay_stroke_resolve(&samples, sampleCount, &preset, &stamps,
                                          &capacity) == CLAY_OK else { return false }
                for stamp in stamps.prefix(capacity) {
                    var r: [Float] = [stamp.radius]
                    guard let item = clay_item_create(
                        Int32(CLAY_PRIM_SPHERE.rawValue), &r, 1) else { return false }
                    var pos = [stamp.position.0, stamp.position.1, stamp.position.2 + 0.55]
                    _ = clay_item_set_position(item, &pos)
                    _ = clay_item_set_op(item, Int32(CLAY_OP_SUBTRACT.rawValue))
                    _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.05)
                    var node: clay_node_id = 0
                    let ok = clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
                    clay_item_destroy(item)
                    if !ok { return false }
                }
                return true
            },

            // Move is a GESTURE per stroke, and the one the roadmap says
            // degrades: each drag prepends a warp to every item it reaches.
            StrokeSession(name: "move_drags") { doc, layer, i in
                var params = clay_move_params()
                params.struct_size = UInt32(MemoryLayout<clay_move_params>.size)
                params.radius = 0.45
                params.ease = 3
                params.front_only = 0
                let a = Float(i) * 0.8
                var centre: [Float] = [cos(a) * 0.4, sin(a) * 0.4, 0.35]
                var displacement: [Float] = [cos(a) * 0.09, sin(a) * 0.09, 0.05]
                var applied = 0
                return clay_layer_move_surface(doc, layer, &centre, &displacement,
                                               &params, &applied) == CLAY_OK
            },

            StrokeSession(name: "cut_passes") { doc, layer, i in
                var desc = clay_cut_desc()
                desc.struct_size = UInt32(MemoryLayout<clay_cut_desc>.size)
                let a = Float(i) * 0.9
                desc.origin = (cos(a) * 0.5, sin(a) * 0.5, -1.4)
                desc.right = (1, 0, 0)
                desc.up = (0, 1, 0)
                desc.forward = (0, 0, 1)
                desc.shape = Int32(CLAY_CUT_CIRCLE.rawValue)
                desc.radius = 0.16
                desc.half_width = 0.16
                desc.half_height = 0.16
                desc.rounding = 0.03
                desc.region_min = (-1.4, -1.4, -1.4)
                desc.region_max = (1.4, 1.4, 1.4)
                guard let cut = clay_cut_create(&desc, nil, 0) else { return false }
                defer { clay_item_destroy(cut) }
                _ = clay_item_set_op(cut, Int32(CLAY_OP_SUBTRACT.rawValue))
                var node: clay_node_id = 0
                return clay_layer_add_item(doc, layer, cut, &node) == CLAY_OK
            },
        ]
    }

    /// A base form for the verbs that reshape rather than deposit.
    private func addBase(_ doc: OpaquePointer, _ layer: clay_layer_id) -> Bool {
        var r: [Float] = [0.62]
        guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &r, 1) else {
            return false
        }
        defer { clay_item_destroy(item) }
        var node: clay_node_id = 0
        return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
    }

    func testBrushSessionsAndGallery() throws {
        let collector = RunCollector()

        for session in sessions() {
            guard let doc = clay_document_create() else {
                XCTFail("\(session.name): no document"); continue
            }
            defer { clay_document_destroy(doc) }
            var layer: clay_layer_id = 0
            XCTAssertEqual(clay_add_sdf_layer(doc, "gallery", &layer), CLAY_OK)
            XCTAssertTrue(addBase(doc, layer), "\(session.name): no base form")

            // One measurement PER STROKE, in order, on a document that keeps
            // everything the previous strokes left. `stamps` carries the
            // stroke index rather than a document size here — the axis is
            // session progress, which is the thing that degrades.
            // Stroke AND the evaluation a host needs before it can draw the
            // result. Timing the edit alone measures nothing that grows —
            // depositing items is cheap, and the first version of this case
            // reported strokes getting FASTER as the document filled up,
            // which is the same edit-without-evaluate mistake the per-stamp
            // cases already had to correct.
            let points = SceneBuilder.drawLattice()
            let pointCount = points.count / 3
            var pts = points
            var out = [Float](repeating: 0, count: pointCount)

            var measurements: [Measurement] = []
            var failed = false
            for i in 0..<Self.strokeCount {
                let t0 = DispatchTime.now().uptimeNanoseconds
                let ok = session.stroke(doc, layer, i)
                let evaluated = clay_eval_points(doc, "cpu", &pts, pointCount,
                                                 &out, nil) == CLAY_OK
                let t1 = DispatchTime.now().uptimeNanoseconds
                if !ok { XCTFail("\(session.name): stroke \(i) failed"); failed = true; break }
                XCTAssertTrue(evaluated, "\(session.name): evaluation failed at stroke \(i)")
                let ms = Double(t1 - t0) / 1_000_000.0
                measurements.append(Measurement(stamps: i + 1, p50Ms: ms,
                                                p95Ms: ms, samples: 1))
            }
            if failed { continue }

            // What the marcher will pay for the result — the number that
            // actually degrades when a chain steepens, and the reason the
            // roadmap tracks safe_step_scale at all.
            var report = clay_field_report()
            report.struct_size = UInt32(MemoryLayout<clay_field_report>.size)
            if clay_layer_field_report(doc, layer, 0, &report) == CLAY_OK {
                print("\(session.name): after \(Self.strokeCount) strokes — "
                      + "lipschitz \(report.lipschitz), "
                      + "safe_step_scale \(report.safe_step_scale), "
                      + "items \(report.item_count), "
                      + "longest_deformer_chain \(report.longest_deformer_chain)")
            }

            let first = measurements.first?.p50Ms ?? 0
            let last = measurements.last?.p50Ms ?? 0
            print("\(session.name): stroke 1 \(String(format: "%.3f", first)) ms -> "
                  + "stroke \(Self.strokeCount) \(String(format: "%.3f", last)) ms "
                  + "(x\(String(format: "%.2f", first > 0 ? last / first : 0)))")

            collector.add(CaseResult(
                name: session.name,
                verb: "session.\(session.name)",
                budgetClass: .gesture,
                backend: "cpu",
                servedBy: "cpu",
                measurements: measurements,
                growthExponent: Timing.growthExponent(measurements)))

            // The picture. Rendered on the device, from the same document the
            // timings above came from.
            if let image = Render.image(of: doc) {
                Render.attach(image, named: session.name, to: self)
            } else {
                XCTFail("\(session.name): could not render the result")
            }
        }

        _ = collector.finish(abiVersion: abiVersion(), attachTo: self)
    }
}
