// DOES A WARM DAB COST THE SAME AFTER THOUSANDS OF THEM, ON THE IPAD
// (gate-sustained-device-sculpting).
//
// Its own bundle, and therefore its own process, for the reason every split in
// this suite has: a bundle inherits no other bundle's high-water mark, and a
// case whose whole subject is memory over time must not be reading somebody
// else's peak.
//
// TWO THINGS THIS CLOSES.
//
// 1. THE SESSION AXIS. Every other case measures growth over DOCUMENT SIZE and
//    fails when cost scales faster than N^1.25. An artist's session is hours of
//    dabs on ONE model that is not growing, and the failures on that axis — a
//    leak, an unbounded cache, a history outgrowing its budget, an arena that
//    never converges — do not scale with the document, so no case on the other
//    axis can fail on them however large it is made.
//
// 2. THE FIXED-MESH PATH ON HARDWARE AT ALL. `mesh_sculptor_stamp` and
//    `mesh_sculptor_apply_stroke` were EXEMPT as unmeasured because the harness
//    had no mesh-layer fixture: every case drove a field or a grid. The
//    classical sculpting mode had never run on an iPad.

import Foundation
import XCTest
import claycore

final class SustainedLatencyTests: VerbCaseGroup {

    func testAMeshSessionDoesNotDriftOnDevice() throws {
        collector = RunCollector()

        // A patch sized so the dab's footprint is a few hundred vertices and
        // the whole run stays far inside the jetsam limit. THE VOLUME IS
        // BOUNDED ON PURPOSE: an unbounded walk here is what took the measure
        // bundle over that limit and killed it with a signal rather than a
        // failure.
        guard let fx = Fixture.meshLayerPatch(stamps: 400) else {
            XCTFail("sustained: could not build a mesh layer fixture")
            return
        }
        defer {
            clay_mesh_sculptor_destroy(fx.sculptor)
            clay_document_destroy(fx.doc)
        }

        // The counts are what make a window comparable across machines, so the
        // report has to be on for this case.
        XCTAssertEqual(clay_mesh_sculptor_set_stage_report_enabled(fx.sculptor, 1), CLAY_OK)

        var brush = clay_mesh_brush_desc()
        brush.struct_size = UInt32(MemoryLayout<clay_mesh_brush_desc>.size)
        XCTAssertEqual(clay_mesh_brush_defaults(&brush), CLAY_OK)
        brush.verb = Int32(CLAY_MESH_BRUSH_GRAB.rawValue)
        brush.radius = 0.10
        brush.strength = 1.0

        // A GRAB ON A LOOP WITH THE SIGN FLIPPING PER REVOLUTION, so the surface
        // returns to where it started and the workset is a property of the
        // fixture rather than of how long it has been running.
        //
        // A DRAW CANNOT DO THIS and the difference is not small: a draw builds a
        // bump, the bump lengthens the geodesic distance to the same world
        // radius, and a later dab genuinely reaches 2.1x fewer vertices.
        // Alternating its sign does not help either, because a draw deposits
        // along the region's averaged normal, which the deformation turns. A
        // grab displaces by `direction * weight` and names no normal.
        let perRevolution = 120

        let read: () -> (considered: UInt64, affected: UInt64, scratch: UInt64) = {
            var r = clay_sculpt_stage_report()
            r.struct_size = UInt32(MemoryLayout<clay_sculpt_stage_report>.size)
            guard clay_mesh_sculptor_stage_report(fx.sculptor, &r) == CLAY_OK else {
                return (0, 0, 0)
            }
            return (r.vertices_considered, r.vertices_affected, r.scratch_high_water)
        }

        measureSustained(name: "mesh_sustained_grab", verb: "mesh_sculptor_stamp",
                         .interactive, perWindow: perRevolution * 6,
                         counters: read) { i in
            let t = 2.0 * Float.pi * Float(i % perRevolution) / Float(perRevolution)
            brush.center = (0.25 * cos(t), 0.05 * (0.25 * cos(t)), 0.25 * sin(t))
            let sign: Float = ((i / perRevolution) % 2 == 0) ? 1.0 : -1.0
            brush.direction = (0, sign * 0.0008, 0)
            var moved: Int = 0
            _ = clay_mesh_sculptor_stamp(fx.sculptor, &brush, nil, nil, &moved)
        }

        finishAndCheck()
    }
}
