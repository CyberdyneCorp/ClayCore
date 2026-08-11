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

    /// The same camera, against a voxel grid.
    ///
    /// `clay_voxel_raycast` is per-ray and reports the CELL and the FACE hit
    /// rather than a normal, so shading is by face — which is what a voxel
    /// surface looks like anyway, and makes the difference between a smoothed
    /// grid and a raw one visible rather than averaged away.
    static func imageVoxel(of grid: OpaquePointer, size: Int = 300) -> UIImage? {
        // Closer than the SDF camera: a voxel groove is a few cells wide and
        // was unreadable at the wider framing.
        let eye: (Float, Float, Float) = (1.15, 0.9, 1.15)
        let target: (Float, Float, Float) = (0, 0, 0)

        func norm(_ v: (Float, Float, Float)) -> (Float, Float, Float) {
            let l = max(sqrt(v.0 * v.0 + v.1 * v.1 + v.2 * v.2), 1e-6)
            return (v.0 / l, v.1 / l, v.2 / l)
        }
        func cross(_ a: (Float, Float, Float), _ b: (Float, Float, Float))
            -> (Float, Float, Float) {
            (a.1 * b.2 - a.2 * b.1, a.2 * b.0 - a.0 * b.2, a.0 * b.1 - a.1 * b.0)
        }
        let forward = norm((target.0 - eye.0, target.1 - eye.1, target.2 - eye.2))
        let right = norm(cross(forward, (0, 1, 0)))
        let camUp = cross(right, forward)

        // Per-face brightness: +X/-X, +Y/-Y, +Z/-Z. Faces come back as an
        // axis-and-sign index; anything unexpected shades mid-grey rather than
        // black, so a decoding mistake looks wrong instead of looking empty.
        let faceShade: [Float] = [0.82, 0.55, 1.00, 0.40, 0.68, 0.48]

        var pixels = [UInt8](repeating: 0, count: size * size * 4)
        let fov: Float = 0.62
        var origin: [Float] = [eye.0, eye.1, eye.2]

        for y in 0..<size {
            for x in 0..<size {
                let u = (Float(x) / Float(size - 1) * 2 - 1) * fov
                let v = (1 - Float(y) / Float(size - 1) * 2) * fov
                let d = norm((forward.0 + right.0 * u + camUp.0 * v,
                              forward.1 + right.1 * u + camUp.1 * v,
                              forward.2 + right.2 * u + camUp.2 * v))
                var dir: [Float] = [d.0, d.1, d.2]
                var hit: Int32 = 0
                var cell: [Int32] = [0, 0, 0]
                var face: Int32 = 0
                var adjacent: [Int32] = [0, 0, 0]
                var t: Float = 0
                let base = (y * size + x) * 4
                let ok = clay_voxel_raycast(grid, &origin, &dir, &hit, &cell,
                                            &face, &adjacent, &t) == CLAY_OK
                if !ok || hit == 0 {
                    pixels[base] = 24; pixels[base + 1] = 26
                    pixels[base + 2] = 30; pixels[base + 3] = 255
                    continue
                }
                let idx = Int(face)
                let lit = (idx >= 0 && idx < faceShade.count) ? faceShade[idx] : 0.6
                pixels[base] = UInt8(min(lit * 232, 255))
                pixels[base + 1] = UInt8(min(lit * 238, 255))
                pixels[base + 2] = UInt8(min(lit * 246, 255))
                pixels[base + 3] = 255
            }
        }

        let cs = CGColorSpaceCreateDeviceRGB()
        guard let provider = CGDataProvider(data: Data(pixels) as CFData),
              let cg = CGImage(width: size, height: size, bitsPerComponent: 8,
                               bitsPerPixel: 32, bytesPerRow: size * 4, space: cs,
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
    ///
    /// Appended rather than written as one array literal: seven closures in a
    /// single expression is more than the Swift type-checker will take, and it
    /// fails with a timeout rather than a useful message.
    private func sessions() -> [StrokeSession] {
        var out: [StrokeSession] = []
        out.append(StrokeSession(name: "stroke_build") { doc, layer, i in
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
            })

        out.append(StrokeSession(name: "stroke_carve") { doc, layer, i in
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
            })

            // Move is a GESTURE per stroke, and the one the roadmap says
            // degrades: each drag prepends a warp to every item it reaches.
        out.append(StrokeSession(name: "move_drags") { doc, layer, i in
                var params = clay_move_params()
                params.struct_size = UInt32(MemoryLayout<clay_move_params>.size)
                params.radius = 0.40
                params.ease = 3
                params.front_only = 0
                // The first version dragged 0.09 from a centre 0.4 out on a
                // 0.62 sphere — a centre INSIDE the form, and a displacement
                // the documented falloff shrinks further ("a drag of 0.5 over
                // a radius of 0.8 moves a tip about 0.31"). It reported a
                // deformer chain 8 deep and safe_step_scale 0.027 while the
                // render stayed a plain sphere: the engine paid for warps that
                // moved the surface by less than a pixel.
                //
                // Pull from ON the surface, and far enough to see.
                let a = Float(i) * 0.8
                let r: Float = 0.62
                var centre: [Float] = [cos(a) * r, sin(a) * r * 0.6, 0.25]
                var displacement: [Float] = [cos(a) * 0.16, sin(a) * 0.16, 0.06]
                var applied = 0
                let rc = clay_layer_move_surface(doc, layer, &centre, &displacement,
                                                 &params, &applied)
                // `applied` is how many items took a warp. A drag that reaches
                // nothing SUCCEEDS and changes nothing, so the return code
                // alone cannot tell "moved the form" from "moved nothing" —
                // which is the distinction two failed fixtures turned on.
                var lo: [Float] = [0, 0, 0], hi: [Float] = [0, 0, 0]
                var hasBounds: Int32 = 0
                _ = clay_layer_bounds(doc, layer, &lo, &hi, &hasBounds)
                let box = hasBounds != 0
                    ? String(format: "[%.2f %.2f %.2f]..[%.2f %.2f %.2f]",
                             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2])
                    : "<no bounds: the layer shows nothing>"
                print("  move drag \(i): applied=\(applied) rc=\(rc.rawValue) \(box)")
                return rc == CLAY_OK
            })

            // A snakehook is a tapered stroke chain: the resolver is
            // Python-only, so what reaches this ABI is the chain it produces.
        out.append(StrokeSession(name: "snakehook_tendrils") { doc, layer, i in
                guard let item = clay_item_create(
                    Int32(CLAY_PRIM_STROKE.rawValue), nil, 0) else { return false }
                defer { clay_item_destroy(item) }
                let a = Float(i) * 0.8
                var chain: [Float] = []
                for k in 0..<6 {
                    let s = Float(k) / 5.0
                    chain.append(contentsOf: [
                        cos(a) * (0.45 + s * 0.5),
                        sin(a) * (0.25 + s * 0.45),
                        sin(a * 1.7) * s * 0.35,
                        0.13 * (1 - s * 0.8),      // the taper IS the tendril
                    ])
                }
                guard clay_item_set_stroke_points(item, &chain, 6) == CLAY_OK,
                      clay_item_set_stroke_blend_k(item, 0.05) == CLAY_OK else {
                    return false
                }
                _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.07)
                var node: clay_node_id = 0
                return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
            })

            // Noise, applied as a deformer to a fresh item per stroke.
        out.append(StrokeSession(name: "noise_detail") { doc, layer, i in
                var r: [Float] = [0.30]
                guard let item = clay_item_create(
                    Int32(CLAY_PRIM_SPHERE.rawValue), &r, 1) else { return false }
                defer { clay_item_destroy(item) }
                let a = Float(i) * 0.85
                var pos: [Float] = [cos(a) * 0.5, sin(a) * 0.5, 0.25]
                _ = clay_item_set_position(item, &pos)
                var params: [Float] = [0.05, 6.0, 3, 0.5, Float(17 + i)]
                _ = clay_item_add_deformer(item, Int32(CLAY_DEFORM_NOISE.rawValue),
                                           &params, params.count, 0)
                _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.07)
                var node: clay_node_id = 0
                return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
            })

            // Magnify and pinch are ONE deformation with a signed strength,
            // so alternating the sign per stroke exercises both.
        out.append(StrokeSession(name: "magnify_pinch") { doc, layer, i in
                var r: [Float] = [0.34]
                guard let item = clay_item_create(
                    Int32(CLAY_PRIM_SPHERE.rawValue), &r, 1) else { return false }
                defer { clay_item_destroy(item) }
                let a = Float(i) * 0.9
                var pos: [Float] = [cos(a) * 0.45, sin(a) * 0.45, 0.3]
                _ = clay_item_set_position(item, &pos)
                let strength: Float = (i % 2 == 0) ? 0.5 : -0.5
                var params: [Float] = [0, 0, 0, 0.4, strength]
                _ = clay_item_add_deformer(item, Int32(CLAY_DEFORM_MAGNIFY.rawValue),
                                           &params, params.count, 2)
                _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.06)
                var node: clay_node_id = 0
                return clay_layer_add_item(doc, layer, item, &node) == CLAY_OK
            })

        out.append(StrokeSession(name: "cut_passes") { doc, layer, i in
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
            })
        return out
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

    /// A voxel session: a verb applied along a few drags across a base blob.
    ///
    /// Every voxel verb is here, because the render is the only thing that
    /// distinguishes "smoothed the surface" from "did nothing" — several of
    /// them legally return CLAY_OK having changed no cell, which the ABI is
    /// explicit about and which no timing can catch.
    struct VoxelSession {
        let name: String
        let apply: (OpaquePointer, [Int32], inout clay_brush_params) -> Void
    }

    private func voxelSessions() -> [VoxelSession] {
        var normal: [Float] = [0, 1, 0]
        var push: [Float] = [0.10, 0.02, 0]     // well past the nearest-cell dead zone
        var alphaDir: [Float] = [0, 1, 0]
        var alpha = [Float](repeating: 0, count: 16 * 16)
        for y in 0..<16 { for x in 0..<16 { alpha[y * 16 + x] = ((x / 2 + y / 2) % 2 == 0) ? 1 : 0 } }

        return [
            VoxelSession(name: "session_voxel_build") { g, c, b in
                var cell = c; _ = clay_voxel_set_brush(g, &cell, &b, 1)
            },
            VoxelSession(name: "session_voxel_erase") { g, c, b in
                var cell = c; _ = clay_voxel_erase_brush(g, &cell, &b)
            },
            VoxelSession(name: "session_voxel_paint") { g, c, b in
                var cell = c; _ = clay_voxel_paint_brush(g, &cell, &b, 2)
            },
            VoxelSession(name: "session_voxel_smooth") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_smooth(g, &cell, &b)
            },
            VoxelSession(name: "session_voxel_inflate") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_inflate(g, &cell, &b, 1)
            },
            VoxelSession(name: "session_voxel_flatten") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_flatten(g, &cell, &b, &normal, 0)
            },
            VoxelSession(name: "session_voxel_pinch") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_pinch(g, &cell, &b)
            },
            VoxelSession(name: "session_voxel_magnify") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_magnify(g, &cell, &b)
            },
            VoxelSession(name: "session_voxel_scrape") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_scrape(g, &cell, &b, &normal, 0)
            },
            VoxelSession(name: "session_voxel_grab") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_grab(g, &cell, &b, &push, 0)
            },
            VoxelSession(name: "session_voxel_smudge") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_smudge(g, &cell, &b, &push)
            },
            VoxelSession(name: "session_voxel_fill_cavities") { g, c, b in
                var cell = c; _ = clay_voxel_sculpt_fill_cavities(g, &cell, &b, 1)
            },
            VoxelSession(name: "session_voxel_carve_alpha") { g, c, b in
                var cell = c
                _ = clay_voxel_sculpt_carve_alpha(g, &cell, &b, &alpha, 16, 16, &alphaDir, 0)
            },
        ]
    }

    /// Voxel verbs, a few drags each, timed and rendered.
    func testVoxelSessionsAndGallery() throws {
        let collector = RunCollector()
        let voxelSize: Float = 0.03

        for session in voxelSessions() {
            guard let doc = clay_document_create() else {
                XCTFail("\(session.name): no document"); continue
            }
            defer { clay_document_destroy(doc) }
            var layer: clay_layer_id = 0
            var gridOut: OpaquePointer?
            XCTAssertEqual(
                clay_document_add_voxel_layer(doc, "gallery", voxelSize, &layer, &gridOut),
                CLAY_OK)
            guard let grid = gridOut else { XCTFail("\(session.name): no grid"); continue }

            // A base to reshape. A SMOOTH SOLID BALL is the wrong one, and
            // the renders showed why: `smooth` changed 1 cell in 8 strokes
            // because a sphere is already smooth, and `fill_cavities` changed
            // 0 because a solid ball has no cavities. Both were the verb
            // correctly doing nothing — the ABI is explicit that they may —
            // so the fixture was measuring the wrong input, not the engine
            // failing.
            //
            // So: a ball, roughened with bumps and pits, so every verb has
            // something to act on and the render shows whether it acted.
            var fill = Fixture.brush(size: 26, falloff: CLAY_BRUSH_FALLOFF_CONSTANT)
            var centre: [Int32] = [0, 0, 0]
            _ = clay_voxel_set_brush(grid, &centre, &fill, 1)

            var bump = Fixture.brush(size: 5, falloff: CLAY_BRUSH_FALLOFF_CONSTANT)
            for k in 0..<40 {
                let (x, y, z) = SceneBuilder.stampPosition(k)
                let l = max(sqrt(x * x + y * y + z * z), 1e-6)
                // ride the surface: the fill spans 26 cells, so radius ~13
                let rc: Float = 12.5
                var cell: [Int32] = [Int32(x / l * rc), Int32(y / l * rc), Int32(z / l * rc)]
                // alternate adding bumps and punching pits, so smooth has
                // spurs AND notches and fill_cavities has real cavities
                if k % 2 == 0 {
                    _ = clay_voxel_set_brush(grid, &cell, &bump, 1)
                } else {
                    _ = clay_voxel_erase_brush(grid, &cell, &bump)
                }
            }

            // Enclosed voids, for fill_cavities. Surface pits are not
            // cavities — the first version punched craters open to the air and
            // the verb correctly changed 0 cells over 8 strokes. A pocket has
            // to be INSIDE the solid, so erase well within the ball.
            // ...and they have to lie UNDER THE PATH the strokes sweep, or the
            // verb's footprint never reaches them. The sweep runs at cell
            // y = 10; these sit at y = 9, inside a ball whose surface is at
            // about 13, so each is a genuine enclosed pocket within reach.
            // SINGLE-CELL pockets. The rule is that an empty cell with at
            // least four of its six face neighbours occupied is inside a
            // pocket — so a 4x4x4 void qualifies nowhere: its corner cells see
            // three occupied neighbours and three empty ones, and its interior
            // sees none. A one-cell void sees six. That is why the previous
            // fixture reported 0 changes over 8 strokes while looking like it
            // had carved plenty of holes.
            var pocket = Fixture.brush(size: 1, falloff: CLAY_BRUSH_FALLOFF_CONSTANT)
            for k in 0..<24 {
                let s = Float(k) / 23.0
                var cell: [Int32] = [Int32((s * 1.2 - 0.6) / voxelSize), 9,
                                     Int32(sin(s * 3.0) * 0.25 / voxelSize)]
                _ = clay_voxel_erase_brush(grid, &cell, &pocket)
            }

            var before: UInt64 = 0
            _ = clay_voxel_change_count(grid, &before)

            var brush = Fixture.brush(size: 9)
            var measurements: [Measurement] = []
            for i in 0..<Self.strokeCount {
                // a drag across the ball's upper surface
                let t = Float(i) / Float(max(Self.strokeCount - 1, 1))
                let cell: [Int32] = [Int32((t * 1.2 - 0.6) / voxelSize),
                                     Int32(0.32 / voxelSize),
                                     Int32(sin(t * 3.0) * 0.25 / voxelSize)]
                let t0 = DispatchTime.now().uptimeNanoseconds
                session.apply(grid, cell, &brush)
                let t1 = DispatchTime.now().uptimeNanoseconds
                let ms = Double(t1 - t0) / 1_000_000.0
                measurements.append(Measurement(stamps: i + 1, p50Ms: ms,
                                                p95Ms: ms, samples: 1))
            }

            var after: UInt64 = 0
            _ = clay_voxel_change_count(grid, &after)
            let changed = after &- before
            // A verb that legally did nothing is the case the render exists to
            // expose; report it rather than letting a green timing imply work.
            print("\(session.name): \(changed) cell change(s) over "
                  + "\(Self.strokeCount) strokes, "
                  + "stroke 1 \(String(format: "%.3f", measurements.first?.p50Ms ?? 0)) ms -> "
                  + "stroke \(Self.strokeCount) "
                  + "\(String(format: "%.3f", measurements.last?.p50Ms ?? 0)) ms")

            collector.add(CaseResult(
                name: session.name, verb: "session.\(session.name)",
                budgetClass: .gesture, backend: "cpu", servedBy: "cpu",
                measurements: measurements,
                growthExponent: Timing.growthExponent(measurements)))

            if let image = Render.imageVoxel(of: grid) {
                Render.attach(image, named: session.name, to: self)
            } else {
                XCTFail("\(session.name): could not render the grid")
            }
        }

        _ = collector.finish(abiVersion: abiVersion(), attachTo: self)
    }

    /// The verbs that act on a BAKED VOLUME, plus masking.
    ///
    /// These were measured for latency and never rendered, which left the
    /// question the renders exist to answer — did the verb do the thing —
    /// unanswered for exactly the brushes whose whole job is a visible change
    /// of surface. hPolish in particular was not covered anywhere: it is not
    /// its own entry point, it is `clay_item_volume_flatten` in CUT_ONLY mode,
    /// and every flatten case used TWO_SIDED.
    func testVolumeVerbsAndMaskingGallery() throws {
        let collector = RunCollector()

        /// A bumpy form worth polishing: a ball with blobs stuck to it.
        func bumpyDocument() -> (OpaquePointer, clay_layer_id)? {
            guard let doc = clay_document_create() else { return nil }
            var layer: clay_layer_id = 0
            guard clay_add_sdf_layer(doc, "gallery", &layer) == CLAY_OK else {
                clay_document_destroy(doc); return nil
            }
            var base: [Float] = [0.55]
            if let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &base, 1) {
                var node: clay_node_id = 0
                _ = clay_layer_add_item(doc, layer, item, &node)
                clay_item_destroy(item)
            }
            for k in 0..<14 {
                var r: [Float] = [0.17]
                guard let item = clay_item_create(
                    Int32(CLAY_PRIM_SPHERE.rawValue), &r, 1) else { continue }
                let (x, y, z) = SceneBuilder.stampPosition(k)
                let l = max(sqrt(x * x + y * y + z * z), 1e-6)
                var pos: [Float] = [x / l * 0.55, y / l * 0.55, z / l * 0.55]
                _ = clay_item_set_position(item, &pos)
                _ = clay_item_set_blend(item, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.05)
                var node: clay_node_id = 0
                _ = clay_layer_add_item(doc, layer, item, &node)
                clay_item_destroy(item)
            }
            return (doc, layer)
        }

        /// Bake the bumpy form to a volume, run `apply` over a few passes,
        /// place the result and render it.
        func volumeSession(_ name: String, passes: Int = 4,
                           _ apply: (OpaquePointer, Int) -> Bool) {
            guard let (src, _) = bumpyDocument() else {
                XCTFail("\(name): no source document"); return
            }
            defer { clay_document_destroy(src) }
            var params = clay_volume_params()
            params.struct_size = UInt32(MemoryLayout<clay_volume_params>.size)
            params.cell_size = 0.015
            // A WIDE band, because flatten is only "accurate while the surface
            // stays near the band it came from — past that a volume reports a
            // bound rather than a distance, and the facet is placed against
            // the bound". The default three cells (0.045) against a facet that
            // moves 0.1+ is past it, and the render showed exactly that: the
            // form came apart on the FIRST pass, not on the third. examples/
            // 28_hpolish.py uses band 0.09 for the same reason.
            params.band = 0.12
            params.padding = 0.14
            var item: OpaquePointer?
            guard clay_item_volume_from_document(src, &params, nil, nil, &item) == CLAY_OK,
                  let volume = item else {
                XCTFail("\(name): could not bake a volume"); return
            }
            defer { clay_item_destroy(volume) }

            var measurements: [Measurement] = []
            for i in 0..<passes {
                let t0 = DispatchTime.now().uptimeNanoseconds
                let ok = apply(volume, i)
                let t1 = DispatchTime.now().uptimeNanoseconds
                XCTAssertTrue(ok, "\(name): pass \(i) was refused")
                measurements.append(Measurement(stamps: i + 1,
                                                p50Ms: Double(t1 - t0) / 1_000_000.0,
                                                p95Ms: Double(t1 - t0) / 1_000_000.0,
                                                samples: 1))

                // Render after the FIRST pass too. `add-consolidation-policy`
                // records that these verbs do not chain — a second pass samples
                // the first pass's VOLUME rather than the document, and "by the
                // third the form is visibly corrupt rather than merely
                // expensive". One picture at the end cannot tell that apart
                // from a verb that never worked; two can.
                if i == 0, let doc1 = clay_document_create() {
                    var l1: clay_layer_id = 0
                    _ = clay_add_sdf_layer(doc1, "pass1", &l1)
                    var n1: clay_node_id = 0
                    if clay_layer_add_item(doc1, l1, volume, &n1) == CLAY_OK,
                       let first = Render.image(of: doc1) {
                        Render.attach(first, named: "\(name)_pass1", to: self)
                    }
                    clay_document_destroy(doc1)
                }
            }

            // place the result so it can be rendered
            guard let doc = clay_document_create() else { return }
            defer { clay_document_destroy(doc) }
            var layer: clay_layer_id = 0
            _ = clay_add_sdf_layer(doc, "out", &layer)
            var node: clay_node_id = 0
            guard clay_layer_add_item(doc, layer, volume, &node) == CLAY_OK else {
                XCTFail("\(name): could not place the result"); return
            }

            print("\(name): pass 1 \(String(format: "%.2f", measurements.first?.p50Ms ?? 0)) ms "
                  + "-> pass \(passes) \(String(format: "%.2f", measurements.last?.p50Ms ?? 0)) ms")
            collector.add(CaseResult(
                name: name, verb: "session.\(name)", budgetClass: .operation,
                backend: "cpu", servedBy: "cpu", measurements: measurements,
                growthExponent: Timing.growthExponent(measurements)))
            if let image = Render.image(of: doc) {
                Render.attach(image, named: name, to: self)
            } else {
                XCTFail("\(name): could not render")
            }
        }

        // Relax: the smooth verb. Bumps should soften pass by pass.
        volumeSession("volume_relax") { volume, _ in
            var p = clay_relax_params()
            p.struct_size = UInt32(MemoryLayout<clay_relax_params>.size)
            p.strength = 0.9
            p.radius_cells = 2
            p.iterations = 2
            p.centre = (0, 0, 0)
            p.region_radius = 1.2
            p.falloff = 0.2
            return clay_item_volume_relax(volume, &p) == CLAY_OK
        }

        // Flatten, two-sided: material above the plane goes, hollows below fill.
        volumeSession("volume_flatten") { volume, _ in
            var p = clay_flatten_params()
            p.struct_size = UInt32(MemoryLayout<clay_flatten_params>.size)
            // The form is radius ~0.55-0.72. A region_radius of 0.75 swallowed
            // ALL of it, and where flatten's weight is 1 the result IS the
            // plane — so the render came back as a sliver instead of a planed
            // ball. Plane a facet on top instead.
            p.plane_point = (0, 0.60, 0)
            p.plane_normal = (0, 1, 0)
            p.strength = 1.0
            p.centre = (0, 0.60, 0)
            p.region_radius = 0.34
            p.falloff = 0.12
            p.mode = Int32(CLAY_FLATTEN_TWO_SIDED.rawValue)
            return clay_item_volume_flatten(volume, &p) == CLAY_OK
        }

        // hPolish / Planar / the Trim family: CUT ONLY. Planing a facet
        // WITHOUT filling what is beside it — the distinction the two-sided
        // mode erases, and the reason this mode exists.
        volumeSession("volume_hpolish") { volume, i in
            var p = clay_flatten_params()
            p.struct_size = UInt32(MemoryLayout<clay_flatten_params>.size)
            let a = Float(i) * 1.1
            // Same correction, and four passes around the crown so the
            // cut-only behaviour shows as facets rather than one slice.
            p.plane_point = (cos(a) * 0.30, 0.56, sin(a) * 0.30)
            p.plane_normal = (cos(a) * 0.42, 0.91, sin(a) * 0.42)
            p.strength = 1.0
            p.centre = (cos(a) * 0.30, 0.56, sin(a) * 0.30)
            p.region_radius = 0.30
            p.falloff = 0.10
            p.mode = Int32(CLAY_FLATTEN_CUT_ONLY.rawValue)
            return clay_item_volume_flatten(volume, &p) == CLAY_OK
        }

        // -- masking, which is the point of masks ---------------------------
        //
        // A mask is measured for paint speed and never for the thing it is
        // FOR: freezing a verb. This paints a mask over half the form and
        // sculpts across the whole of it — the render shows the frozen half
        // untouched, or shows the freeze not working.
        do {
            let voxelSize: Float = 0.03
            guard let doc = clay_document_create() else { return }
            defer { clay_document_destroy(doc) }
            var layer: clay_layer_id = 0
            var gridOut: OpaquePointer?
            XCTAssertEqual(
                clay_document_add_voxel_layer(doc, "masked", voxelSize, &layer, &gridOut),
                CLAY_OK)
            guard let grid = gridOut else { XCTFail("no grid"); return }

            var fill = Fixture.brush(size: 26, falloff: CLAY_BRUSH_FALLOFF_CONSTANT)
            var origin: [Int32] = [0, 0, 0]
            _ = clay_voxel_set_brush(grid, &origin, &fill, 1)

            guard let mask = clay_mask_create(voxelSize) else { XCTFail("no mask"); return }
            defer { _ = clay_mask_destroy(mask) }

            // The ball's fill spans 26 cells per axis, so its surface is about
            // 13 cells (0.39 world) from the centre. The first version carved
            // at y = 6 cells — INSIDE the solid — so 336 cells changed and the
            // silhouette did not move at all. Cut across the TOP instead,
            // where a groove is visible, and freeze the half of that groove
            // where x > 0.
            let surfaceCell: Int32 = 11
            let surfaceY = Float(surfaceCell) * voxelSize
            var maskBrush = Fixture.brush(size: 14, falloff: CLAY_BRUSH_FALLOFF_CONSTANT)
            for k in 0..<14 {
                let x = Float(k) / 13.0 * 0.36          // x > 0 only
                var point: [Float] = [x, surfaceY, 0]
                _ = clay_mask_paint(mask, &point, &maskBrush, 1.0)
            }
            var painted = 0
            _ = clay_mask_painted_count(mask, &painted)
            XCTAssertGreaterThan(painted, 0, "the mask painted nothing to freeze")

            var before: UInt64 = 0
            _ = clay_voxel_change_count(grid, &before)

            // Erase straight across the form WITH the mask attached. The
            // frozen side must survive.
            var carve = Fixture.brush(size: 11)
            carve.mask = mask   // clay_mask* is an OpaquePointer in Swift
            var measurements: [Measurement] = []
            for i in 0..<Self.strokeCount {
                let t = Float(i) / Float(max(Self.strokeCount - 1, 1))
                // sweep the groove right across the ball, through the frozen half
                var cell: [Int32] = [Int32((t * 0.72 - 0.36) / voxelSize),
                                     surfaceCell, 0]
                let t0 = DispatchTime.now().uptimeNanoseconds
                _ = clay_voxel_erase_brush(grid, &cell, &carve)
                let t1 = DispatchTime.now().uptimeNanoseconds
                measurements.append(Measurement(stamps: i + 1,
                                                p50Ms: Double(t1 - t0) / 1_000_000.0,
                                                p95Ms: Double(t1 - t0) / 1_000_000.0,
                                                samples: 1))
            }
            var after: UInt64 = 0
            _ = clay_voxel_change_count(grid, &after)
            print("mask_freeze: \(painted) mask cell(s) painted, "
                  + "\(after &- before) cell change(s) through a mask")

            collector.add(CaseResult(
                name: "mask_freeze", verb: "session.mask_freeze",
                budgetClass: .interactive, backend: "cpu", servedBy: "cpu",
                measurements: measurements,
                growthExponent: Timing.growthExponent(measurements)))
            if let image = Render.imageVoxel(of: grid) {
                Render.attach(image, named: "mask_freeze", to: self)
            }
        }

        // -- mask extrude, rendered ------------------------------------------
        do {
            guard let (doc, layer) = bumpyDocument(),
                  let mask = clay_mask_create(0.03) else { return }
            defer { clay_document_destroy(doc); _ = clay_mask_destroy(mask) }
            // Paint the mask WHERE THE SURFACE ACTUALLY IS, found by casting
            // a ray inward per sample rather than guessing a radius. The
            // bumpy form's surface runs from 0.55 between the blobs to ~0.72
            // on one, so any fixed radius paints partly inside the solid and
            // partly in open air — which is how this verb got refused twice
            // ("the mask does not reach the surface"), both times because the
            // fixture was wrong rather than the engine.
            var brush = Fixture.brush(size: 9)
            var painted = 0
            for k in 0..<40 {
                let (x, y, z) = SceneBuilder.stampPosition(k)
                let l = max(sqrt(x * x + y * y + z * z), 1e-6)
                let dir = (x / l, y / l, z / l)
                if dir.1 < 0 { continue }          // a patch on the upper half
                var origin: [Float] = [dir.0 * 3, dir.1 * 3, dir.2 * 3]
                var into: [Float] = [-dir.0, -dir.1, -dir.2]
                var hit: Int32 = 0
                var tHit: Float = 0
                var position: [Float] = [0, 0, 0]
                var normal: [Float] = [0, 0, 0]
                guard clay_raycast(doc, &origin, &into, &hit, &tHit,
                                   &position, &normal) == CLAY_OK, hit != 0 else {
                    continue
                }
                _ = clay_mask_paint(mask, &position, &brush, 1.0)
                painted += 1
            }
            XCTAssertGreaterThan(painted, 0, "no surface found to mask")
            var params = clay_mask_extrude_params()
            params.struct_size = UInt32(MemoryLayout<clay_mask_extrude_params>.size)
            params.thickness = 0.09
            params.threshold = 0.5
            params.cell_size = 0.03
            var shell: OpaquePointer?
            let t0 = DispatchTime.now().uptimeNanoseconds
            let rc = clay_document_mask_extrude(doc, layer, mask, &params, &shell)
            let t1 = DispatchTime.now().uptimeNanoseconds
            XCTAssertEqual(rc, CLAY_OK, "mask extrude was refused")
            guard rc == CLAY_OK, let shell else { return }
            defer { clay_item_destroy(shell) }

            // render the SHELL alone, which is what extract produces
            guard let out = clay_document_create() else { return }
            defer { clay_document_destroy(out) }
            var outLayer: clay_layer_id = 0
            _ = clay_add_sdf_layer(out, "shell", &outLayer)
            var node: clay_node_id = 0
            _ = clay_layer_add_item(out, outLayer, shell, &node)

            let ms = Double(t1 - t0) / 1_000_000.0
            print("mask_extract: \(String(format: "%.1f", ms)) ms")
            collector.add(CaseResult(
                name: "mask_extract", verb: "session.mask_extract",
                budgetClass: .operation, backend: "cpu", servedBy: "cpu",
                measurements: [Measurement(stamps: 1, p50Ms: ms, p95Ms: ms, samples: 1)],
                growthExponent: nil))
            if let image = Render.image(of: out) {
                Render.attach(image, named: "mask_extract", to: self)
            }
        }

        _ = collector.finish(abiVersion: abiVersion(), attachTo: self)
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

                // The first stroke, rendered. For most sessions this is only
                // a before-picture; for Move it is the whole finding, because
                // by the eighth the layer cannot be drawn at all.
                if i == 0, let early = Render.image(of: doc) {
                    Render.attach(early, named: "\(session.name)_pass1", to: self)
                }
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

            // Move earns a third picture, because the second one is BLACK and
            // that is the finding rather than a failure.
            //
            // Every drag applies (applied=1, CLAY_OK) and the layer's bounds
            // grow from ±0.62 to ±1.42, so the surface is genuinely moving.
            // But eight stacked warps take the declared Lipschitz to 333 and
            // safe_step_scale to 0.003, and at that step size the engine's own
            // raycast runs out of iterations before it reaches the surface:
            // the shape is there and cannot be drawn. That is exactly what
            // add-consolidation-policy was written for, and consolidating is
            // the documented cure — so render that too, and let the pair say
            // whether the cure works.
            if session.name == "move_drags" {
                var params = clay_consolidation_params()
                params.struct_size = UInt32(MemoryLayout<clay_consolidation_params>.size)
                params.cell_size = 0.012
                params.band = 0.06
                params.padding = 0.08
                params.skip_redistance = 0        // redistancing is the point
                let rc = clay_layer_consolidate(doc, layer, &params, nil, nil, nil)
                XCTAssertEqual(rc, CLAY_OK, "consolidate was refused")

                var after = clay_field_report()
                after.struct_size = UInt32(MemoryLayout<clay_field_report>.size)
                if clay_layer_field_report(doc, layer, 0, &after) == CLAY_OK {
                    print("move_drags: after consolidate — lipschitz \(after.lipschitz), "
                          + "safe_step_scale \(after.safe_step_scale), "
                          + "items \(after.item_count), "
                          + "chain \(after.longest_deformer_chain)")
                }
                if let cured = Render.image(of: doc) {
                    Render.attach(cured, named: "move_drags_consolidated", to: self)
                }
            }
        }

        _ = collector.finish(abiVersion: abiVersion(), attachTo: self)
    }
}
