# Tasks

## 1. The carrier

- [x] 1.1 `SessionFrame` — `has_frame` plus `math::Transform frame` (local →
      world) — extracted from `clay_mesh_sculptor` and inherited by
      `clay_mesh_sculptor`, `clay_dynamic_sculptor`, `clay_multires_sculptor`
      and `clay_multires_sculpt_layer_stroke`. A BASE AND NOT A MEMBER because
      every helper takes it alone: a member would name the carrier twice at
      every call site, and inheriting keeps `sculptor->has_frame` reading the
      same on the handle that had it first, which is what makes this a
      no-behaviour-change base for the three that did not
- [x] 1.2 The seven helpers retargeted from `const clay_mesh_sculptor&` to
      `const SessionFrame&`: `world_point_to_local`, `world_vector_to_local`,
      `world_length_to_local`, `brush_settings_to_local`, `stroke_to_local`,
      `mask_gate_for`, `reject_conflicting_frame`
- [x] 1.3 `set_session_frame`, `fill_session_frame` and
      `adopt_layer_transform` factored so the four handles cannot drift. The
      mesh sculptor's own setter and readback now call them, which is the
      cheapest available proof that the shared bodies are the ones it had

## 2. The three sites

- [x] 2.1 `clay_multires_sculptor_stamp` — `mask_gate_for` and
      `brush_settings_to_local`
- [x] 2.2 `clay_dynamic_sculptor_stamp` — the same pair
- [x] 2.3 `read_layer_stroke_stamp` — the same pair, and it serves FIVE entry
      points (`_stamp`, `_stamp_detail`, `_smooth`, `_erase`, `_restore`), so
      placing it there places all five and none can drift from the others
- [x] 2.4 `clay_mesh_sculptor_deform` — A FIFTH SITE, AND NOT ONE OF #506's
      THREE. It is on `clay_mesh_sculptor`, which has carried a frame since
      `define-carried-mesh-transform-semantics`, and it sampled its mask
      unplaced anyway *and* converted none of `MeshDeformSettings`' world
      values (`origin`, `axis`, `span`). On a placed layer a taper's gizmo box
      sat somewhere other than where the artist drew it and the freeze
      protected the wrong region. `deform_settings_to_local` beside
      `mask_gate_for`
- [x] 2.5 THE SWEEP THAT FOUND FOUR COULD NOT HAVE FOUND THE FIFTH, which is
      why the gate in 6.1 is structural. `grep` for the unplaced
      `field_mask->sample(p)` returns four sites — the three in 2.1–2.3 plus
      `mask_gate_for`'s own correct `!has_frame` fallback — and the deform site
      spells its pointer `m`. A sweep for one expression is a better bound than
      an enumeration and is still a bound on that expression, not on the defect

## 6. The bound, enforced

- [x] 6.1 Every `field::MaskGate` local must be sourced from `mask_gate_for`,
      or from a helper that is (`read_layer_stroke_stamp`, serving the five
      stroke verbs) — checked in `check_c_abi.py`'s hygiene pass, which
      `release_check` and CI already run. STRUCTURAL RATHER THAN TEXTUAL: the
      failure is "a gate that did not come from the helper", so a new site is
      caught however it samples. It found `clay_mesh_sculptor_deform` on its
      first run
- [x] 6.2 Comments stripped before the scan. Not tidiness: "remove the code and
      leave a note about what it used to do" is ordinary, and a note mentioning
      the sampled expression would have held a raw count at its expected value
      while the guarantee was gone
- [x] 6.3 Corroboration in a DIFFERENT SHAPE so the two cannot fail together:
      whatever the spelling, `field_mask->sample(` may appear only inside
      `mask_gate_for`
- [x] 6.4 The identity branch is checked too, and this is the direction that
      protects a shipping host. Phrased so it cannot be followed into
      inertness: no "update the constant if it moved", because tightening a
      gate announces itself and loosening one is silent forever
- [x] 6.5 PROVING A GATE FIRES IS NOT PROVING IT WATCHES THE RIGHT THING, and
      conflating the two is how the first version of this gate shipped. It
      counted one exact expression, was proved to fail in both directions, and
      was still the wrong invariant — a string count for a defect that is "an
      unplaced sample by any spelling". Both checks are needed and they are
      separate

## 3. The ABI

- [x] 3.1 `clay_multires_sculptor_set_world_frame` / `_use_layer_transform` /
      `_world_frame`
- [x] 3.2 `clay_dynamic_sculptor_set_world_frame` / `_world_frame`, with NO
      `_use_layer_transform` and the reason in the header: an adaptive surface
      is not a document layer and there is no transform to read
- [x] 3.3 `clay_multires_sculpt_layer_stroke_set_world_frame` /
      `_use_layer_transform` / `_world_frame`
- [x] 3.4 `reject_conflicting_frame` on `clay_multires_sculptor_apply_stroke`,
      the one call able to spell the frame twice. REFUSED, not resolved by
      precedence: a host passing both means one of the two is what it believes
- [x] 3.5 A readback on all four. A declared frame a host cannot read back
      makes "did that take?" answerable only by stamping and inspecting the
      result — a query that hides its own state
- [x] 3.6 Version to 0.97.0 in all three places: `CMakeLists.txt`,
      `CLAY_ABI_MINOR`, `pyproject.toml`

## 4. The header sentences

- [x] 4.1 REPLACED, not supplemented: "the identity is the truth rather than a
      default" was accurate about the code and wrong about the geometry, which
      is the expensive combination because a reader who checked it found it
      confirmed. It now says the identity was the only thing expressible, and
      names the storage that makes the surfaces layer-local
- [x] 4.2 "ONE TRANSFORM, NOT TWO" recorded beside it: two places build these
      inputs and the second `set_automask_inputs` REPLACES rather than composes,
      so a host on both paths gets one placement. Previously established only by
      reading two files

## 5. Proof

- [x] 5.1 `tests/unit/test_c_place_every_surface.cpp`, eight cases. A PAINTED
      BOX rather than a cavity field, because `MaskField::sample` is
      `get(cell_at(world_p))` — a nearest-cell lookup, no derivative — so
      `clay_mask_fill` makes "was the gate asked here or there" a question with
      two answers and no middle. A cavity estimator would instead make the
      test's power depend on a crevice being deep enough that the two samples
      fall on opposite sides of a threshold: a sensitivity to bound rather than
      a question to settle
- [x] 5.2 THE MARGIN. Non-overlap in SPACE is not the property needed. Cells
      are half-open `[x, x+1) * cell_size`, so a point one hair outside a filled
      box can quantise into the boundary cell and read 1. The separation clears
      the box extent PLUS ONE MASK CELL, asserted as the relation
      (`kPlacedX > 2 * kBoxHalf + kMaskCell`) rather than as numbers that happen
      to satisfy it
- [x] 5.3 THE VACUITY GUARD, its own case: both candidate points are sampled on
      both masks and asserted to read 1 and 0 the two ways round. A run where
      both fall inside the box, or both outside, would pass for a reason
      unrelated to the frame
- [x] 5.4 Assertions on POSITIONS, never on `moved_vertices`, which is
      documented to mean "reached nothing, fully masked, or no displacement" —
      three ordinary outcomes behind one number
- [x] 5.5 BOTH HALVES at every site: the box at the placed point masks, AND the
      box at the layer-local point does not. Either alone is satisfied by a mask
      that is simply never consulted
- [x] 5.6 The identity negative control: with no frame declared the box at the
      origin is the one that gates, which is the answer every existing host
      already gets
- [x] 5.7 REVERT PROOF, ONE REVERT PER PROPERTY — four reverts, each of which
      COMPILED and each of which failed exactly one case and no others:
      the multires gate (`at_placed` read 9.86 where it should read ~0 and
      `at_local` read 0 where it should read 9.86 — exactly inverted, which is
      the signature of a placement bug rather than of an inert mask), the
      layer-stroke gate, the dynamic gate, and the `clay_multires_sculptor_apply_stroke` refusal.
      A first attempt reverted the gate AND `brush_settings_to_local` together
      and failed at the precondition instead, proving nothing about the mask;
      that is why the reverts are one property each
- [x] 5.8 A REVERT THAT DOES NOT BUILD HANDS YOU THE PREVIOUS BINARY. Removing
      `deform_settings_to_local`'s call left the helper unused under `-Werror`,
      the suite ran the old binary, and the case appeared to fail for the right
      reason. With the helper removed too the revert built — and the case
      PASSED, which is what showed the gizmo property was untested:
      `plain != undeformed` holds either way, because an unconverted origin
      still deforms, just wrongly. The assertion that pins it is the
      equivalence — a declared frame with a world gizmo is the same operation
      as no frame with a local one — and it fails against a compiling revert.
      Checking the revert COMPILES caught an ambiguous failure earlier and a
      FALSE one here, which is the worse of the two
- [x] 5.9 A CONTROL THAT DID NOT CONTROL, recorded because it nearly shipped:
      the deform case first used a gizmo aimed 100 units away as its
      "untouched" baseline. A taper scales the cross-section ABOUT ITS AXIS, so
      a distant axis puts every vertex far from it and AMPLIFIES the result —
      10837.5 against an undeformed 153. Used as the frozen baseline it
      asserted roughly the opposite of the property. The baseline is the
      undeformed plane, measured directly
