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

        // -- voxel: the display path ------------------------------------------
        // The half of a sculpt the gate never measured. Whole-grid meshing is
        // the export path and budgeted as an operation; meshing what a dab
        // dirtied is what has to fit a frame.
        .measured("voxel_mesh", by: "voxel_mesh_whole"),
        .measured("voxel_mesh_chunks", by: "voxel_mesh_dirty"),
        // Drained inside voxel_mesh_dirty's timed body rather than hoisted out
        // of it, because what a frame pays is whatever the set holds when it
        // asks — a host that skipped a frame coalesces.
        .measured("voxel_take_dirty_chunks", by: "voxel_mesh_dirty"),

        // -- voxel: multi-resolution -------------------------------------------
        .measured("voxel_add_level", by: "voxel_add_level"),
        // A verb with a level under it: a write costs 8^d cell writes for d
        // levels finer than the active one, so editing coarse with a stack is
        // the direction that pays — and it is the one #86's workaround asks a
        // host to take.
        .measured("voxel_sculpt_smooth_levels", by: "voxel_smooth_l2"),
        // The same verb at a radius a sculptor actually blocks out with. Every
        // other voxel case is size 8; cost is roughly cubic in radius.
        .measured("voxel_sculpt_smooth_large", by: "voxel_smooth_r32"),
        // Reading a level's own size and occupancy is an accessor, not a verb
        // a sculptor drives, and dropping a level is the undo of add_level
        // rather than an edit of its own.
        .exempt("voxel_drop_level", because:
            "the inverse of voxel_add_level, which is measured; it frees the "
            + "detail map of one level and writes no cells, so it cannot be the "
            + "expensive half of a subdivide/undo pair"),
        // The regional form of a verb whose whole-lattice form is measured.
        // Reported as a gap since it landed, because the pattern in
        // check_device_coverage.py names the _region suffix deliberately — the
        // level stack went missing from that list once by being covered by a
        // prefix, and this is the check working rather than a false alarm.
        .exempt("voxel_add_level_region", because:
            "voxel_add_level is the same refinement over the whole lattice and "
            + "is measured; the region form rounds OUT to whole chunks and "
            + "allocates a subset of what the whole-lattice form allocates, so "
            + "the measured case is its ceiling. A region smaller than one "
            + "32-cell chunk still costs one chunk, so there is no cheaper "
            + "regime below the measured one either"),

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
        // Dab after dab without an invalidation between them, which is the
        // only shape that reaches the compiled-prefix reuse and the GPU
        // suffix copy (#294, #296). Named against the METAL row rather than
        // the cpu one, unlike sdf_stamp above: the residency this exercises
        // is the GPU backend's, and a table entry pointing at cpu would be
        // satisfied by a run in which Metal never patched anything.
        .measured("sdf_stroke", by: "sdf_stroke_metal"),
        // The same unbroken stroke through the BRICK CACHE, which is a
        // different code path from sdf_stroke above and the only one that can
        // reach the resumed refill (#306: the seeded suffix, the cull-index
        // append, the brick seed store). sdf_stamp_incremental cannot: its
        // reset removes the node, and a removal is not an append, so the chain
        // breaks before the fast path is ever consulted. Named separately for
        // the same reason sdf_stroke is named apart from sdf_stamp -- one is
        // not evidence for the other.
        .measured("sdf_stroke_incremental", by: "sdf_stroke_bricks"),
        // The same stroke with its dabs INSIDE A GROUP, which is a different
        // path and not a variant of the row above. `tail_append` requires the
        // added node's parent to be the root list, so a dab placed into a
        // group misses the append fast path entirely: the edit is structural,
        // every prefix seed is retired, and the tape recompiles whole. An
        // artist who groups a head and keeps sculpting on it is on this path,
        // which is the ordinary way to work and was never measured.
        .measured("sdf_stroke_in_group", by: "sdf_stroke_in_group_bricks"),
        // The same stroke on a SMOOTH-blended document. Every other SDF row
        // here is hard-blended, and a hard blend contributes nothing to the
        // chain pad — so the pad resolves to a constant zero and the suite
        // could not see it at all. The clay and build brushes are smooth by
        // default; the hard-blended fixture is not the document a sculptor
        // makes.
        .measured("sdf_stroke_smooth", by: "sdf_stroke_smooth_bricks"),

        // -- placing what is already there ------------------------------------
        // The gizmo. Sixty-two cases and not one of them transformed anything,
        // so Move/Rotate/Scale had no number and no way to regress visibly.
        //
        // THREE entries for ONE pair of entry points, and the split is the
        // point rather than an accident of how the cases were written. What an
        // edit invalidates is derived from the edited node's ANCESTRY rather
        // than from the node, so a grouped document and a flat one are two
        // different costs through one call, and a single case would report
        // whichever shape the fixture happened to build.
        //
        // The drag rows themselves come out nearly equal, and that is a
        // finding rather than a defect: a drag walks, so each frame refills
        // roughly what the frame before it dirtied, and those bricks lose
        // their seeds either way. Where the ancestry is paid is in the two
        // `sdf_stamp_after_*_drag` rows below.
        .measured("layer_set_transform", by: "sdf_node_transform_bricks"),
        .measured("sdf_group_transform", by: "sdf_group_transform_bricks"),
        .measured("document_set_layer_transform", by: "sdf_layer_transform_bricks"),
        // What a drag costs the NEXT edit, which is where a wide invalidation
        // is actually paid and which the three rows above cannot see: a drag
        // frame refills what the frame before it dirtied, so grouped and flat
        // agree on the drag itself and diverge on everything that follows.
        // Two rows for the same reason the node rows are two — the ancestry is
        // the variable, and one row would report whichever the fixture built.
        .measured("sdf_stamp_after_drag", by: "sdf_stamp_after_drag_bricks"),
        .measured("sdf_stamp_after_group_drag", by: "sdf_stamp_after_group_drag_bricks"),
        // The per-axis form takes the same path as the uniform one on
        // everything this gate measures: it differs in what the tape records
        // about exactness, not in what an edit invalidates or what a refill
        // costs. Measuring it would duplicate sdf_node_transform_bricks.
        .exempt("layer_set_transform_nonuniform", because:
            "the same invalidation and the same refill as layer_set_transform, "
            + "which is measured; a per-axis scale changes what the tape records "
            + "about exactness (cfi_scale_nonuniform) rather than what a drag "
            + "frame costs"),

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
        // Trim Curve: an OPEN stroke closed against the frame bounds. A
        // separate brush from the lasso in docs/07's table, and a separate
        // tessellation.
        .measured("cut_polygon_from_open_curve", by: "trim_curve"),

        // -- the resolvers that turn a gesture into an item -------------------
        .measured("tube_create", by: "tube_create"),
        .measured("pose", by: "pose_region"),

        // -- rigs ---------------------------------------------------------------
        .measured("layer_armature_edit", by: "armature_edit"),

        // -- sessions: the same brushes over EIGHT strokes, accumulating -----
        // Everything above measures one application with the document held
        // still. These hold nothing still: stroke 8 lands on what strokes 1-7
        // left, which is the shape that degrades and the shape an artist
        // works in. Each one also renders its result to a PNG.
        .measured("session_stroke_build", by: "stroke_build"),
        .measured("session_stroke_carve", by: "stroke_carve"),
        .measured("session_move_drags", by: "move_drags"),
        .measured("session_cut_passes", by: "cut_passes"),
        .measured("session_snakehook", by: "snakehook_tendrils"),
        .measured("session_noise", by: "noise_detail"),
        .measured("session_magnify_pinch", by: "magnify_pinch"),

        // Volume verbs and masking, rendered. These had latency cases and no
        // picture, which for a brush whose job is a visible change of surface
        // leaves the only question that matters unanswered.
        .measured("session_volume_relax", by: "volume_relax"),
        // *_pass1 renders are attachments, not cases; the chaining pair is
        // two pictures of one case rather than two measurements.
        .measured("session_volume_flatten", by: "volume_flatten"),
        // hPolish is NOT its own entry point: it is clay_item_volume_flatten
        // in CUT_ONLY mode, and every other flatten case here uses TWO_SIDED,
        // so the mode that defines the Trim family was untested.
        .measured("session_volume_hpolish", by: "volume_hpolish"),
        // What a mask is FOR. Everything else measures how fast one paints.
        .measured("session_mask_freeze", by: "mask_freeze"),
        .measured("session_mask_extract", by: "mask_extract"),

        // Voxel sessions. Several voxel verbs can legally return CLAY_OK
        // having changed no cell — the ABI says so explicitly — and no timing
        // distinguishes that from working. The render does.
        .measured("session_voxel_build", by: "session_voxel_build"),
        .measured("session_voxel_erase", by: "session_voxel_erase"),
        .measured("session_voxel_paint", by: "session_voxel_paint"),
        .measured("session_voxel_smooth", by: "session_voxel_smooth"),
        .measured("session_voxel_inflate", by: "session_voxel_inflate"),
        .measured("session_voxel_flatten", by: "session_voxel_flatten"),
        .measured("session_voxel_pinch", by: "session_voxel_pinch"),
        .measured("session_voxel_magnify", by: "session_voxel_magnify"),
        .measured("session_voxel_scrape", by: "session_voxel_scrape"),
        .measured("session_voxel_grab", by: "session_voxel_grab"),
        .measured("session_voxel_smudge", by: "session_voxel_smudge"),
        .measured("session_voxel_fill_cavities", by: "session_voxel_fill_cavities"),
        .measured("session_voxel_carve_alpha", by: "session_voxel_carve_alpha"),

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
        // NOT a decision that these should never be measured — an unmeasured
        // gap, named so it is visible to the gate rather than invisible to it.
        // The alternative was leaving clay_mesh_sculptor_* out of
        // VERB_PATTERNS, which is exactly the failure that list exists to
        // prevent and is how tube, Trim Curve, pose and the level stack all
        // went missing without anything reporting them missing.
        //
        // Closing it needs a mesh-layer fixture in the harness — every case
        // here drives a field or a grid, and a mesh brush needs an imported
        // mesh, its adjacency built once, and a pick to aim from. The cost
        // shape is also different enough to be worth its own number rather
        // than an inherited one: a stamp is O(vertices the falloff reached)
        // after an O(vertices) adjacency build the session pays once.
        .exempt("mesh_sculptor_stamp",
                because: "unmeasured: the device harness has no mesh-layer "
                       + "fixture, so no case drives a mesh brush yet. Named "
                       + "here rather than left out of VERB_PATTERNS, which "
                       + "would make the whole family invisible to this gate. "
                       + "Closing it needs an imported mesh in the harness and "
                       + "a session that builds the adjacency once."),
        .exempt("mesh_sculptor_apply_stroke",
                because: "unmeasured, as mesh_sculptor_stamp is and for the "
                       + "same reason. The stroke path is the one worth "
                       + "measuring first when the fixture exists: it is what "
                       + "a drag actually drives, and it amortises the "
                       + "adjacency the stamp path pays for separately."),
        .exempt("mesh_sculptor_deform",
                because: "unmeasured, for the same missing mesh-layer fixture "
                       + "as mesh_sculptor_stamp — but it is a DIFFERENT cost "
                       + "shape and should not inherit that case's number when "
                       + "one exists. A stamp is O(the vertices a falloff "
                       + "reached) after an adjacency build the session pays "
                       + "once; a taper or a twist is O(every vertex in the "
                       + "mesh), every time, because a deformer acts on the "
                       + "whole form rather than under a cursor. That makes it "
                       + "the mesh case most likely to breach an interaction "
                       + "budget, and the one worth measuring first after the "
                       + "stroke path."),
        .exempt("cut_polygon_from_curve",
                because: "the CLOSED tessellation of the same control points the "
                       + "open variant measured by trim_curve tessellates. The "
                       + "shapes differ — one divides the frame, the other does "
                       + "not — but the work is the same curve flattening, so "
                       + "measuring the second adds a number, not a signal. "
                       + "Revisit if the two paths diverge."),
    ]

    /// Entries that must have produced a case in the run.
    static var required: [CoverageEntry] { table.filter { $0.exemption == nil } }
}
