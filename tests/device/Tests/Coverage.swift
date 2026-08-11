// The coverage table: every brush and sculpt verb, and the device case that
// measures it.
//
// This exists for the same reason `CAPABILITY_EXAMPLES` exists in
// examples/run_all.py — an uncovered capability should be an error, and an
// exemption should be a decision on the record rather than an omission nobody
// noticed. A latency suite that quietly skips half the brushes reads exactly
// like one that covers them.
//
// The table is the SOURCE OF TRUTH and is emitted into the run record, where
// tools/check_device_coverage.py checks it back against the entry points
// declared in bindings/c/clay.h. Two directions, so neither a verb added to
// the engine without a case, nor a table entry naming a case that never ran,
// survives.

import Foundation

struct CoverageEntry: Codable {
    /// Canonical verb name. For voxel and mask verbs this is the C entry point
    /// with the `clay_` prefix dropped, which is what the checker matches on.
    let verb: String
    /// The case that measures it, or nil when exempt.
    let caseName: String?
    /// Why this verb is not measured. Non-nil exactly when `caseName` is nil.
    let exemption: String?

    static func measured(_ verb: String, by caseName: String) -> CoverageEntry {
        CoverageEntry(verb: verb, caseName: caseName, exemption: nil)
    }
    static func exempt(_ verb: String, because reason: String) -> CoverageEntry {
        CoverageEntry(verb: verb, caseName: nil, exemption: reason)
    }
}

enum Coverage {
    static let table: [CoverageEntry] = [

        // -- voxel: stamping ------------------------------------------------
        .measured("voxel_set_brush", by: "voxel_stamp"),
        .measured("voxel_erase_brush", by: "voxel_erase"),
        .measured("voxel_paint_brush", by: "voxel_paint"),

        // -- voxel: the sculpt verbs ----------------------------------------
        .measured("voxel_sculpt_smooth", by: "voxel_smooth"),
        .measured("voxel_sculpt_inflate", by: "voxel_inflate"),
        .measured("voxel_sculpt_flatten", by: "voxel_flatten"),
        .measured("voxel_sculpt_pinch", by: "voxel_pinch"),
        .measured("voxel_sculpt_magnify", by: "voxel_magnify"),
        .measured("voxel_sculpt_grab", by: "voxel_grab"),
        .measured("voxel_sculpt_scrape", by: "voxel_scrape"),
        .measured("voxel_sculpt_smudge", by: "voxel_smudge"),
        .measured("voxel_sculpt_fill_cavities", by: "voxel_fill_cavities"),
        .measured("voxel_sculpt_carve_alpha", by: "voxel_carve_alpha"),

        // -- the stroke engine ----------------------------------------------
        .measured("stroke_resolve", by: "stroke_resolve"),
        .measured("voxel_apply_stroke", by: "voxel_apply_stroke"),
        // The SDF side of a stroke: one stamp becoming an item, then the
        // evaluation a host needs before it can draw. The case the whole
        // harness was built for — it is the only one paying a tape recompile.
        .measured("sdf_stamp", by: "sdf_stamp_cpu"),
        // The same stamp through the brick cache — the incremental path a
        // host actually drives, as opposed to re-evaluating everything.
        .measured("sdf_stamp_incremental", by: "sdf_stamp_bricks"),

        // -- masks ------------------------------------------------------------
        .measured("mask_paint", by: "mask_paint"),
        .measured("document_mask_extrude", by: "mask_extrude"),

        // -- the SDF region verbs ---------------------------------------------
        .measured("item_volume_relax", by: "sdf_relax"),
        .measured("item_volume_flatten", by: "sdf_flatten"),
        .measured("layer_move_surface", by: "sdf_move"),
        .measured("layer_consolidate", by: "sdf_consolidate"),

        // -- the cut tool -----------------------------------------------------
        .measured("cut_create", by: "cut_create"),

        // -- sessions: the same brushes over EIGHT strokes, accumulating -----
        // Everything above measures one application with the document held
        // still. These hold nothing still: stroke 8 lands on what strokes 1-7
        // left, which is the shape that degrades and the shape an artist
        // works in. Each one also renders its result to a PNG.
        .measured("session_stroke_build", by: "stroke_build"),
        .measured("session_stroke_carve", by: "stroke_carve"),
        .measured("session_move_drags", by: "move_drags"),
        .measured("session_cut_passes", by: "cut_passes"),

        // -- exemptions, each a decision rather than an omission --------------
        .exempt("snakehook",
                because: "no C entry point exists. The binding-parity table maps "
                       + "module.snakehook to clay_item_set_curve_points, so what "
                       + "reaches this ABI is the tapered stroke chain a snakehook "
                       + "RESOLVES INTO, not the resolver. That chain is measured by "
                       + "sdf_stamp. A host cannot call the verb, so there is no "
                       + "verb-shaped latency to measure until one is exposed."),
        .exempt("voxel_mask_extrude",
                because: "the voxel path of a verb whose SDF path is measured by "
                       + "mask_extrude. Both share the pocket-fill walk; measuring "
                       + "the second adds a number, not a signal. Revisit if the "
                       + "two implementations diverge."),
        .exempt("item_volume_move_topological",
                because: "reachable only on a volume item, and layer_move_surface "
                       + "is the verb a host drives for Move. Measured there."),
    ]

    /// Entries that must have produced a case in the run.
    static var required: [CoverageEntry] { table.filter { $0.exemption == nil } }
}
