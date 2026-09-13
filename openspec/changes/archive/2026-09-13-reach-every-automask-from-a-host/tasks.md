## 1. The gap, and its two witnesses

- [x] 1.1 `clay.h` recorded it at the field: CAVITY and SURFACE_GROUP "need an
      input this descriptor cannot carry … the descriptor that carries their
      inputs is a follow-up rather than a guess made against a sample of one"
- [x] 1.2 `tools/check_binding_parity.py` recorded the same thing as three
      exemptions, and that gate fails when an exemption becomes reachable — so
      the ABI addition and the exemption removal are one change by construction
- [x] 1.3 pyclay has been able to wire both since it shipped, which is what
      makes this reachability rather than a missing feature

## 2. What the inputs are

- [x] 2.1 The same two objects pyclay takes: a `clay_mask` for the cavity
      measure, the document's `clay_groups` for the surface groups
- [x] 2.2 NOT a host callback, and not for style: these are evaluated per vertex
      from worker threads. pyclay refuses a Python callable for exactly this
      reason and says so in its error message
- [x] 2.3 REFUTED — a `clay_document` and `clay_measure_params`, measuring the
      tape directly as the C++ path does. It makes a threading claim this change
      had not established, and it is a SECOND way to reach one estimator

## 3. The descriptor and the setters

- [x] 3.1 `clay_automask_sources`, `struct_size`-versioned like every other
- [x] 3.2 `clay_mesh_sculptor_set_automask_sources`, `_dynamic_`, `_multires_`
- [x] 3.3 Session state, not per stamp: the engine holds these as closures and
      rebuilding them per dab is an allocation per dab
- [x] 3.4 A null descriptor clears both
- [x] 3.5 Naming inputs enables nothing; the brush's bits still decide
- [x] 3.6 A bit with no input stays inert rather than becoming an error
- [x] 3.7 A borrowed lattice that has left its document is CLAY_ERROR_NOT_FOUND

## 4. The space, on the C path

- [x] 4.1 The two defects this found -- the unplaced sampling and the inverted
      cavity slider -- are `place-the-automask-lattices`, against `main`. Both
      are reachable from C++ and pyclay today and neither needs an entry point,
      so neither waits behind this
- [x] 4.2 What stays here is the C path's own placement:
      `read_automask_sources` needs the SCULPTOR's declared frame, because a
      single `clay_mesh_sculptor_stamp` builds no `MeshStrokeOptions` at all
- [x] 4.3 The frame is read when a lattice is asked, not captured, so the two
      calls may be made in either order
- [x] 4.4 REFUTED -- capturing the frame at set time. It is correct only in the
      order a host happened to use, and wrong silently in the other
- [x] 4.5 Only the fixed-mesh session declares a space; the adaptive and
      multiresolution sculptors pass null, which is the reading their mask gate
      already had. Stated in the header rather than left to be found

## 5. The reader

- [x] 5.1 `clay_layer_node_color`, the one setter with no reader
- [x] 5.2 CORRECTED — the first draft claimed an untouched node reads the
      engine's default. `clay_item_desc` carries a colour, so an item's is its
      descriptor's and a zeroed descriptor places a BLACK item. A group, which
      takes no colour at creation, is the one node that carries the default
- [x] 5.3 A group answers, where the transform reader refuses one

## 6. Tests

- [x] 6.1 `tests/unit/test_c_automask_reach.cpp`, written against `clay.h` and
      nothing else — an internal test reaches the feature the way the engine
      does and so cannot see this class of defect at all
- [x] 6.2 Both halves of every claim: the bit alone, the source alone, and both
- [x] 6.3 The cavity strength as a ratio rather than a magnitude
- [x] 6.4 The group case straddles a border, so a gate that passed everything
      would read the same as one that worked
- [x] 6.5 The frame regression proven by reverting the placement in
      `read_automask_sources`: 2 cases, 3 assertions, reporting "placed sample 0
      of an ungated 14.6555"
- [x] 6.6 The zero-strength case is NOT asserted here. It belongs to the change
      that fixes it, whose regression test is the brush-preset round trip --
      the path that could observe the defect while CAVITY was inert from C. Two
      answers about it would be one too many, and this one would be red until
      that lands

## 7. Gates

- [x] 7.1 `cpu-only` suite green
- [x] 7.2 `check_binding_parity.py` OK, three exemptions removed and aliased
- [x] 7.3 `release_check.py --skip-slow`
- [x] 7.4 ABI 0.95.0 -> 0.96.0 in all three version lines
