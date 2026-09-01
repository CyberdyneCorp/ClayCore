# Tasks: add-mesh-sculpt-layers

- [x] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): after
      `add-mesh-multires`, whose detail representation this extends. The two
      SHALL NOT be designed independently — a layer contribution and a base
      detail coefficient are the same quantity under two owners
      — `add-mesh-multires` verified landed in the tree; design.md D5 composes
      one `DetailField` per layer into the level's own rather than adding a
      second displacement representation

## 1. Decide first

- [x] 1.1 DECIDE the naming so the collision cannot ship: `MeshBrush::Layer` is
      a brush ALGORITHM (deposit to a ceiling above the stroke-start surface)
      and a sculpt layer is a persistent artist CHANNEL. Neither name changes
      meaning; the API SHALL make the difference unmissable
      — design.md D1: the channel is never spelled `Layer` unqualified
      (`SculptLayer*`, `clay_multires_sculpt_layer_*`,
      `surface.sculpt_layer(...)`), the brush enumerator is untouched, and
      `tools/check_c_abi.py` gains the rule so the discipline is gated
- [x] 1.2 DECIDE whether layers exist on a fixed-topology mesh with no
      hierarchy. A sparse per-vertex offset needs no levels; the question is
      whether that is a product or a second code path
      — design.md D2: yes as a product, no as a second path. A one-level
      `MultiresSurface` IS a fixed-topology mesh, and base deformation layers
      (2.6) are how it gets a stack; a bare mesh has no frame to measure an
      offset from
- [x] 1.3 DECIDE the layer-kind enumeration now, even if only sampled layers
      ship, so a procedural layer does not need a format break later
      — design.md D3: `SculptLayerKind : uint16_t`, `Sampled = 0` shipping,
      `Procedural = 1` reserved and REFUSED by the decoder, each layer payload
      length-prefixed so a later format can choose to skip deliberately
- [x] 1.4 DECIDE whether a colour layer stack is in scope. Recommendation: not
      here — mesh paint and smear write vertex colours and a paint stack is the
      same idea under different arithmetic
      — design.md D4: out of scope. Colour BLENDS and blending does not
      commute, so including it would make requirement 3.1 conditional on a
      layer's kind; and a level's colours are a rebuildable subdivided
      attribute cache, not per-level authoritative state

## 2. The stack

- [x] 2.1 `include/clay/mesh/sculpt_layer.h` — stable 64-bit ids, name,
      strength, visible, locked, kind, per-level detail, byte accounting
      — `SculptLayerId`, `SculptLayerKind`, `SparseWeightField`, `SculptLayer`,
      `SculptLayerStack`, `SculptLayerDelta` and `SculptLayerProperty`; per-layer
      `bytes()` and `coverage_vertices()`, stack `memory()`
- [x] 2.2 IDs are never vector indices. Reordering changes indices; an id is
      what a host, a serialized document and the C ABI hold
      — ids are minted from a serialized counter, `index_of` is a lookup, and
      `move_to`/`remove` never renumber anything a host holds
- [x] 2.3 Stack operations: add, remove, move, merge down, rename, set
      strength, set visible, set lock, set active
      — all nine on `SculptLayerStack`, each forwarded through `MultiresSurface`
      so the per-level sizes and the surface revisions stay in step and an undo
      record can be captured on the way
- [x] 2.4 A locked layer refuses a sculpt write; property changes may still be
      allowed and the rule is stated rather than discovered
      — `MultiresSculptor::stamp` refuses before the brush moves anything and
      `absorb_level_edit` puts the level's mesh back for a direct caller;
      `rename`, `set_locked` and `set_active` are still allowed and say so
- [x] 2.5 Evaluation: `base detail + Σ visible layer detail × strength × layer
      mask`, per level
      — `recompose_block` in `src/mesh/sculpt_layer_eval.cpp`; a layer at zero
      effective strength is SKIPPED rather than multiplied by zero, so an
      invisible stack composes to the base field bit for bit
- [x] 2.6 Base deformation layers at level zero, so a non-destructive
      proportion pass is possible and not only a non-destructive detail one
      — `State::BaseRestFrames` — the cage's rest frames, built lazily and only
      when a level-0 layer exists; `absorb_base_edit` subtracts the layer's
      contribution so sculpting the form under a proportion pass does not bake it
- [x] 2.7 Per-layer mask, sparse, DISTINCT from the temporary brush gate — the
      gate says where a brush writes, the mask says where a stored layer
      contributes
      — `mesh::SparseWeightField`, identity 1.0, same blocking and same block
      index as `DetailField`; the brush gate stays `field::MaskGate`
- [x] 2.8 Byte accounting per layer and per stack, and coverage per layer, so a
      strength change can dirty coverage rather than the model
      — `MultiresMemory::sculpt_layers` (authoritative) and `::composed`
      (rebuildable), plus `SculptLayer::coverage_vertices`

## 3. Semantics that must be written down, not discovered

- [x] 3.1 Additive layers COMMUTE. Reordering changes organisation and not
      geometry, and the requirement says so rather than implying an order
      dependence. This differs from voxel sculpt layers, whose replay of cell
      writes IS order-dependent and whose spec pins which order wins
      — composition visits a block's layers once and adds; `move_to` invalidates
      no block. `test_mesh_sculpt_layers` swaps two overlapping layers and
      compares the evaluated positions BIT for bit
- [x] 3.2 A stroke on a layer at strength 0.5 records its FULL contribution.
      Strength is composition, not a scale on the pen
      — `absorb_layered_detail` stores `ΔE = frame⁻¹(P_written) − E_before`;
      nothing in the change divides by a strength. Tested at 0.5 → 1.0
- [x] 3.3 Merge-down and bake-to-base are defined by VISUAL PARITY — evaluated
      surface before equals evaluated surface after — not by concatenating
      coefficients. The naive arithmetic divides by the lower layer's strength
      and fails exactly when it is zero
      — `SculptLayerStack::merge_down` sets the target to the identity it needs;
      `bake_sculpt_layer_to_base` is the same statement with the base as target.
      Parity tested at strengths 1.0, 0.37 and 0.0
- [x] 3.4 Removing a layer re-evaluates its coverage only; it does not replay
      strokes and does not touch other layers
      — `remove` notes the removed layer's coverage and nothing else; tested
- [x] 3.5 Under symmetry, every mirrored write enters the SAME active layer and
      one undo step, with the coverage as the union
      — falls out of the transaction, and `test_mesh_sculpt_layer_stroke.cpp`
      asserts both halves: a gesture whose stamps alternate between a place and
      its mirror image produces ONE delta naming ONE layer, both sides carry
      coefficients, and reverting that one delta clears both. The companion case
      is the one that found the bug — pinning at `begin` was a single write, so
      a host that changed the active layer mid-gesture split it across two
      channels; the transaction now re-asserts its target before every dab

## 4. Writing into a layer

- [x] 4.1 `mesh::LayeredMultiresSculptor` with a stroke transaction —
      begin, stamp, commit, cancel — following the shape the SDF sculpt
      transaction already established
      — `mesh::LayeredMultiresSculptor` in `include/clay/mesh/layered_sculpt.h`
- [x] 4.2 Cancel restores the layer exactly; commit produces ONE undo delta
      — `cancel` reverts the recorded `before` values rather than recomputing;
      `commit` hands over one `SculptLayerDelta` (or one `MultiresDelta` for a
      base-domain stroke). Asserted on CHECKSUMS rather than positions, in both
      domains: a cancelled gesture leaves the layer checksum, the base checksum
      and every evaluated position bit-identical, and one delta reverses a
      six-stamp gesture. This is the case that found the checksum bug — an
      undone stroke leaves the block allocated, and `sculpt_layer_checksum` was
      folding the vertex count a lazily-sized field had been sized to, so an
      exact restore hashed differently from never having written
- [x] 4.3 A hundred stamps over one vertex coalesce to one entry
      — `SculptLayerDelta::note_detail` keeps the FIRST `before` per (level,
      vertex) and `sync_after` rewrites the LAST `after`
- [x] 4.4 The active layer's blocks are writable; the evaluated lower stack is
      read-only and cached during the stroke
      — the composed field IS the cached lower stack, and
      `hold_sculpt_layer_composition` refuses a composition change while a
      stroke is open — which answers the design's open question
- [x] 4.5 Write domain: geometry at the active level, or detail relative to the
      subdivided parent, chosen explicitly by the caller. An automatic choice
      may be offered and SHALL NOT be the only one
      — `MultiresWriteDomain::{Automatic, Geometry, Detail}`, resolved once at
      `begin`; `Detail` with no active layer refuses rather than falling back
- [x] 4.6 An erase mode moves the active layer's detail toward zero and touches
      neither the base nor any other layer
      — `LayeredMultiresSculptor::erase` fades the ACTIVE layer toward zero and
      refuses when there is no target, so it can never reach the base
- [x] 4.7 Height stamps and tangent-space vector displacement, sampled through
      the SAME alpha sampler and orientation rules the existing mesh alpha
      uses, with image data borrowed and never copied into a preset
      — `include/clay/mesh/detail_stamp.h`; the placement is
      `kernel::calpha_frame` and the read is `kernel::calpha_sample`, the same
      two the scalar alpha uses. Images are planar and borrowed
- [x] 4.8 Vector displacement is interpreted in the tangent frame, never in
      world space — a world-space stamp is orientation-dependent and unusable
      over a curved surface
      — `DetailStampMode::Vector` returns three components in the vertex's
      transported frame; nothing is read or written in world space

## 5. Caching and scale

- [x] 5.1 Blocked detail storage, with the block size chosen by measurement
      — the stack shares `DetailField`'s blocking and its measured 1024-vertex
      default rather than choosing a second one — the shared block index is what
      makes 5.4 and 5.5 arithmetic
- [x] 5.2 An evaluated-detail block cache keyed on a stack revision; a rename
      SHALL NOT invalidate geometry
      — per-level dirty block sets on the stack; `rename` and `set_active` bump
      only `metadata_revision` and mark nothing. Tested
- [x] 5.3 Separate revisions for metadata, composition and content, so the
      three kinds of change invalidate what they actually affect
      — `metadata_revision`, `composition_revision`, `content_revision`, with
      `geometry_bumps` folded into the surface's own two so a pre-existing host
      keeps working
- [x] 5.4 THE GATE: a strength change on a layer touching a small fraction of a
      large surface costs its coverage, not the surface
      — tested: a layer inside one block of a five-block level recomposes exactly
      one block on a strength change
- [x] 5.5 THE GATE: a stamp on the top of a deep stack does not sum every layer
      beneath it over unrelated geometry. Prefix checkpoints if the measurement
      requires them; the cache keys SHALL be designed so they are possible
      — tested: sixteen layers over two disjoint blocks, a write into one visits
      the eight layers that reach it and none of the others
- [x] 5.6 Benchmarks over 1, 4, 16, 64 and 128 layers with local, overlapping
      and dense coverage
      — `BM_SculptLayerCompose*`, `BM_SculptLayerStrengthChange*` and
      `BM_SculptLayerStampOnStack*` over 1/4/16/64/128 with local, overlapping
      and dense coverage; the counters are the reading, not the clock.
      MEASURED (linuxdev, 21 repetitions each, P50 / P95 / max; load average
      4.19 before and 1.93 after, so the ratios below are the reading rather
      than the absolutes):

        stamp on a LOCAL stack, 1 -> 128 layers
          347.1 / 378.2 / 383.3 us   ->   365.0 / 415.7 / 417.1 us     1.05x
          layer_blocks_visited 2898 at both ends — task 5.5's gate: a 128x
          deeper stack is 1.05x the dab, because the layers that do not reach a
          block are an O(1) miss and are never summed
        stamp on an OVERLAPPING stack, 1 -> 128
          349.7 / 371.8 / 407.4 us   ->   515.9 / 558.3 / 565.2 us     1.48x
          layer_blocks_visited 2898 -> 14109 (4.9x), so the cost follows the
          layers that actually cover what the stamp touched
        strength change, LOCAL, 1 -> 128
          728.3 / 767.0 / 780.8 us   ->  1165.4 / 1232.9 / 1252.2 us   1.60x
          blocks_recomposed 200 at BOTH ends — task 5.4's gate; the pair count
          rises 200 -> 25600 because in this shape every layer covers the same
          footprint, which is the worst case by construction
        cold whole-level compose, LOCAL, 1 -> 128
          15.26 / 16.02 / 16.75 ms   ->   15.88 / 16.35 / 16.57 ms     1.04x
        dense coverage, every layer over the whole level, 1 -> 16
          compose 16.80 -> 18.02 ms; strength 5.80 -> 7.03 ms; stamp 1.66 ->
          2.16 ms — reported rather than optimised, because a host should be
          told this shape is expensive
- [x] 5.7 Memory never silently stops recording. Report the budget and let a
      host merge, bake, delete or compact — a cap that silently stopped
      recording would leave the pass on the surface and un-dialable, which is a
      correctness bug wearing a memory limit's clothes
      — nothing is capped anywhere; `MultiresMemory` reports layer content apart
      from the composed cache and the comment says why a cap would be a
      correctness bug

## 6. Detail-aware verbs

- [x] 6.1 Smooth gains modes: geometry as today, detail-only, and
      preserve-detail. A plain Laplacian over pores removes the pores, which is
      rarely what was asked
      — `MultiresSmoothMode::{Geometry, DetailOnly, PreserveDetail}` on the
      layered sculptor; `PreserveDetail` smooths the form and folds the change
      into the level's own detail, leaving every layer's contribution intact
- [x] 6.2 A restore/morph mode — toward zero on the active layer, or toward the
      base — distinct from undo and documented as such
      — `erase` (the active channel toward zero) and `restore` (the level's own
      detail toward the pure subdivision); both are brushes and both are
      recorded as gestures that undo, which is what makes them not-undo

## 7. Undo, history, serialization

- [x] 7.1 `mesh::SculptLayerDelta` — layer id, level, changed entries or
      blocks, optional mask changes, with existence flags on each side
      — `mesh::SculptLayerDelta` — layer id, level, coefficient entries and mask
      entries, coalesced, with its own byte form
- [x] 7.2 Layer PROPERTY operations are undoable — rename, strength,
      visibility, reorder, lock, add, remove, merge, bake. Voxel sculpt-layer
      property changes are still outside the history; this is the change that
      does better rather than repeating it
      — `mesh::SculptLayerProperty`: scalar sides for rename/strength/visible/
      lock/active, and a whole-stack snapshot on each side for add, remove,
      move, merge and bake — plus the base detail and cage positions a bake
      wrote outside the stack
- [x] 7.3 `session::History` gains the kind and a resolver, through the same
      inversion the other kinds use
      — `Step::Kind::MultiresLayer` and `MultiresLayerProperty`, applied through
      `MultiresSurface::apply_sculpt_layer_{delta,property}` and the existing
      `set_multires_resolver`; no fifth resolver
- [x] 7.4 Journal encode, decode, replay; older journals still replay; a
      malformed delta is refused
      — `test_mesh_sculpt_layer_history.cpp`: a whole layered session (add,
      stroke, dial) journalled and replayed onto a fresh hierarchy reproduces
      the ids, the slider, the layer checksum and every position bit for bit; a
      journal written before either kind existed still replays; a corrupted
      payload stops the replay; and the delta, the property record and the stack
      snapshot each refuse a foreign magic, an unknown version, an unknown op
      code, a truncated stream and an absurd declared count — the last refused
      by arithmetic BEFORE the array is reserved. A record naming a layer the
      stack does not hold, or a vertex past the level, changes NOTHING rather
      than half of something
- [x] 7.5 Versioned serialization of the stack — id, name, kind, visible,
      locked, strength, per-level blocks, masks — inside the multires format
      rather than in the mesh stream
      — `kSurfaceVersion = 2`, the stack chunk inside the multires stream, and
      version 1 still accepted as a hierarchy with no layers
- [x] 7.6 A layer id survives a save, a load and a reorder
      — tested: save, load and a reorder before the save; ids and names survive

## 8. Bindings and gates

- [x] 8.1 C ABI: layer ids as `uint64`, stack and property operations, layer
      info with `struct_size`, name retrieval into caller buffers rather than
      pointers into engine strings
      — `clay_sculpt_layer_id`, `clay_sculpt_layer_info` and
      `clay_sculpt_layer_stats` with the `struct_size` prefix and bounded fills;
      add/remove/move/merge/bake/rename/set-active and the three sliders, each
      with an `int32_t* out_error` carrying a `clay_multires_error` so a host can
      tell "no such layer" from "locked" from "finish the stroke first".
      `clay_multires_sculpt_layer_name` is the size-query pattern into the
      caller's buffer. D1's naming rule is now gated by
      `tools/check_c_abi.py::sculpt_layer_naming` — which caught this change's
      own first spelling, `clay_multires_layered_sculptor_*`, and is why the
      transaction is `clay_multires_sculpt_layer_stroke_*`
- [x] 8.2 C ABI: high-detail stamp descriptors, borrowed image data, changed
      block readback, revisions
      — `clay_detail_stamp_desc` (planar, borrowed `const float*`) and
      `clay_detail_stamp_report`; `clay_multires_sculpt_layer_revision` for the
      three, and `clay_multires_memory` grown by `sculpt_layers` and `composed`.
      Changed blocks read back through the EXISTING `clay_multires_dirty_blocks`
      and `clay_multires_copy_block`: a layered write marks the same base
      patches, so a second transport would have been a second answer
- [x] 8.3 pyclay, with a context manager for a stroke transaction — the voxel
      sculpt layer's `with grid.sculpt_layer(name):` is the precedent, and it
      is the only form that cannot leave a surface recording when a stroke loop
      raises
      — the stack on `MultiresSurface` (ids, sliders, info, mask, coefficients,
      the three revisions, stats, compact) plus `SculptLayerStroke`, which
      `surface.sculpt_layer_stroke()` returns: `__enter__` begins, a clean exit
      commits and a RAISING BLOCK CANCELS. Height and vector maps are numpy
      arrays borrowed for the call, `(H, W)` and `(3, H, W)` — planar, because a
      plane is the buffer the alpha sampler already reads
- [x] 8.4 `tools/check_binding_parity.py` green
      — 622 capabilities against the IMPORTED module (`--pyclay`), 29 exempt,
      no new exemption: every sculpt-layer capability pyclay exposes resolves
      onto a C entry point. The three string-valued axes (write domain, smooth
      mode, stamp mode) are registered in `STRING_CHOICES`, so a fourth
      smoothing mode invented in Python has to exist in `clay.h` too
- [ ] 8.5 Swift smoke on macOS and in the simulator
      NOT DONE — NEEDS macOS. There is no `swift` or `swiftc` on this Linux box,
      so the file below has never been type-checked, let alone run, and neither
      the macOS nor the simulator run has happened. Left unticked deliberately.
      — what IS written: `tests/swift/smoke.swift` gained the section (identity
      across a reorder, the name into a caller buffer, the typed refusals, the
      gesture holding the composition, a height stamp, and the stack through a
      save and a load), and `tools/check_swift_package.py` is green — but that
      gate reads the package manifest and the sources TEXTUALLY, so it proves
      the file is declared and referenced and proves nothing about whether it
      compiles.
      PARTLY CLOSED BY CI, and the record should say which part. PR #417's
      `build+test (macos, +metal, parity)` job ran
      `tools/check_swift_smoke.sh typecheck` and reported "typecheck OK
      (smoke.swift compiles against bindings/c/clay.h)", so the new section
      DOES type-check against the header on a real Swift toolchain — and it
      found something this box could not: one `var coefficients` that is never
      mutated, now a `let`. That closes the failure the type-check exists to
      catch (a merge that makes the source stop compiling) and closes nothing
      else. `check_swift_smoke.sh` deliberately keeps `macos` and `sim` —
      building against the real xcframework slices and RUNNING the binary, the
      simulator one under `simctl spawn` — at release time, so what this task
      asks for has still not happened. To finish: `./tools/check_swift_smoke.sh
      macos` and `./tools/check_swift_smoke.sh sim` on a machine with Xcode and
      a booted simulator, which is the release workflow's job and not a PR's
- [x] 8.6 THE MILESTONE, as a numbered example that renders and asserts: a
      wrinkle pass dialled 0 → 50% → 100% over a form that never changes, plus
      one layer removed with the others untouched
      — `examples/69_mesh_sculpt_layers.py`, registered in `run_all.py`'s
      `EXAMPLES` and as `mesh-sculpt-layers`' capability example. Five tiles:
      the form alone, three passes at 0% (byte-identical to it), at 50%, at
      100%, and 'pores' removed. Every claim raises `SystemExit`: at 0 the
      surface is byte-identical to the form, at 0.5 the mean offset is 0.5000 of
      full over 310 moved vertices, dialling back to 1.0 reproduces the surface
      exactly with the stack checksum unmoved, 40 stamps over one place record
      the same 55 entries as one, a slider on 'scar' recomposes exactly the 11
      blocks that layer has allocated of the 21 at the level, removing a layer
      leaves the other two byte-identical and the base checksum unmoved, a
      raising stroke leaves nothing behind, and the stack survives a save
- [x] 8.7 Version lines together; four presets green plus `release_check`;
      `tsan` under `setarch -R`; `check_layering.py` green
      — the four version lines move as one at 0.76.0 (`CMakeLists.txt` VERSION,
      `CLAY_ABI_MINOR 76`, `pyproject.toml`, and `release_check`'s `version`
      row, which compares exactly those and PASSES). Every preset this box can
      build is green, each the whole `ctest` suite and not a filter:
      `cpu-only` 4/4 in 154.6 s, `asan-ubsan` 4/4 in 3284.1 s with no sanitizer
      report, and `tsan` under `setarch -R` 4/4 in 1229.9 s, likewise clean —
      2,032 unit cases and 14.9 M assertions each. `metal` is Darwin-only and
      `cuda`/`opencl`/`vulkan` want hardware this box does not have.
      `check_layering.py`, `check_c_abi.py`, `check_binding_parity.py`,
      `check_gallery.py` and `check_swift_package.py` are all OK.
      `release_check.py` is 13 PASS / 2 FAIL, and NEITHER failure is this
      change's:
        * `device` needs the hardware gate — "engine changed since the gate ran
          at 39c244209", which is the pre-existing state of this environment;
        * `benchmarks` is not reproducible on this shared box, and MAIN FAILS IT
          TOO. Three runs of the same gate on this branch failed on three
          different rows (`BM_MeshBricksGradDenseDoc` at 17.17x once, six
          unrelated SDF/voxel/brick rows the next, one SDF row the third), a
          fourth run of the SAME pair measured 9.01x against the 14.0 ceiling,
          and a back-to-back run of main's own `clay_bench` on the same box
          failed on `BM_SdfHistoryPrefixPiled5000`. Load average moved 4.33 ->
          7.40 during the branch run and 7.40 -> 14.38 during main's. No failing
          row touches `mesh/`, and the change's own benchmarks are in 5.6.
      RE-RUN at the end of the branch, after the documentation pass, and the
      diagnosis above held: `release_check.py` is now **14 PASS / 1 FAIL** —
      `benchmarks` came back `bench-gate: OK` on a box whose load average went
      11.00 -> 19.51 across the run, without a line of benchmark code changing
      between the two results, which is what "not reproducible on this box"
      means. `device` is the only remaining failure and it is the environment's:
      "engine changed since the gate ran at 39c244209". `cpu-only` re-ran 4/4
      (clay_unit_tests 286.97 s under that load), and `check_layering`,
      `check_c_abi`, `check_binding_parity` (622 capabilities, 29 exempt,
      against the IMPORTED module), `check_gallery` (251 tracked outputs) and
      `check_swift_package` (textual) are all OK
- [x] 8.8 Docs: `docs/07-brushes-and-features.md` gains the stack and the
      distinction from `MeshBrush::Layer`; the README's sculpt-layer claim is
      widened from voxels to the representations that actually have them
      — §8b gains "Sculpt layers over the hierarchy": the one-line model, a
      table of the THREE things `Layer` means and why the brush enumerator was
      not renamed, strength-as-composition with the divide-by-zero trap named,
      the three revisions and what each invalidates, the transaction's three
      reasons, the detail-aware verbs as a table, and the memory rows with the
      reason there is no cap. The README's claim now reads "on TWO
      representations" and states the difference that matters — voxel layers
      replay cell writes and are order-dependent, additive displacement
      commutes.
      Four more documents this change made stale, found by reading rather than
      by a gate, since none of them has one:
        * `docs/09-brush-latency-and-coverage.md` — "Sculpt layers, measured",
          the 5.6 table with the counters called out as the actual claim, the
          tiering, and why these rows are deliberately NOT in `check_bench.py`
          (the claim is an exact integer asserted in a unit test, not a ratio
          between two clocks on a shared runner). Plus a "Named gaps" row: the
          device gate's `VERB_PATTERNS` matches no `clay_multires_*` name at
          all, so two changes' worth of ABI is invisible to it — not exempt,
          absent — and the same missing mesh fixture is the blocker
        * `docs/05-claycore-library.md` — the "what is genuinely not undoable"
          list said sculpt-layer property changes are not steps, which is now
          true of the VOXEL stack only; and the document memory section gained
          the note that a `MultiresSurface` is a standalone handle no document
          can walk to, with `clay_multires_memory`'s own authoritative /
          rebuildable split and the four levers it has instead of a cap
        * `docs/sculpt_comparison.md` — the ZBrush/Blender/3DCoat rows said
          sculpt layers were voxel-only in four places, including the Tier 1
          entry, which now also records where the mesh stack matches ZBrush's
          arithmetic literally and the two places it deliberately differs
        * `openspec/ROADMAP.md` — Phase 5 row 4 marked **Landed** with what
          shipped and the three decisions worth carrying, in the shape row 3
          used; the review's P0 "Sculpt layers" row widened to two
          representations; `unify-the-undo-history`'s "still open" narrowed to
          the voxel half; the morph-target gap re-rated, since a base
          deformation layer at level 0 is its storage; and `add-field-stamps`
          told that the tangent-space stamp vocabulary now exists to be read
