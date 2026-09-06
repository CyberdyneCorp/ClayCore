## 0. Read first — done by the audit, recorded so nobody repeats it

- [x] 0.1 MEASURE the boundary before designing for it. Positions at a region
      boundary are bit-identical to the dense hierarchy's (worst 0.000000000 at
      levels 1, 2 and 3) and NORMALS AND FRAMES ARE NOT: worst 0.104052432,
      0.048561923 and 0.029364093. They differ at exactly the vertices whose
      face ring is incomplete — the two predicates disagree on 0 vertices at
      every level. The defect is the incomplete FACE RING, which is why the
      helper in section 2 answers in faces
- [x] 0.2 PROVE it is a storage defect and not a shading one. With the same
      `LocalDetail` written into every level-3 vertex of a dense and a regional
      hierarchy, 63 of 289 evaluate to a different position (worst 0.00776535)
      while all 225 interior vertices stay bit-identical. `P(n) = S(n) +
      Frame * Detail`, so the shipped bit-identity holds only while the
      boundary detail is zero — the only case the existing gate exercises
- [x] 0.3 SIZE the emission set from real hierarchies rather than from the task
      wording. `resolve_keep` is stricter than 2:1 (a patch and its whole vertex
      ring resident one level down), so the max depth spread across a shared
      cage vertex is 1 — measured on five graded hierarchies and again on an
      awkward direct sequence where 2 of 4 requests were refused. One bridging
      case, and one incidence pattern OUTSIDE the 2:1 vocabulary: a corner-only
      coarse patch, no split edge, conflicting corner — 4 of 44 transition
      patches on a 2x2 region, 19 on three scattered ones
- [x] 0.4 CONFIRM the transition is derivable. `src/mesh/multires_serialize.cpp`
      writes only the per-level patch sets and replays the build; the face
      lists, `full_of` and the chunk tables are all rebuilt. So section 5 stores
      nothing and `kSurfaceVersion` does not move
- [x] 0.5 CONFIRM the existing traversal before proposing a second one.
      `SculptNeighbors` plus `build_neighbors` is already "one traversal that
      hides what kind of neighbour this is", materialized once per stamp because
      `smooth_targets` re-reads it once per pass, with `want_normals` opt-in
      because a per-neighbour normal costs a second-order walk. Extend its
      identity space; do not add a rival

## 1. The frame at a transition — ships first, has users today

- [ ] 1.1 REGRESSION GATE FIRST, because it fails today. A new case in
      `tests/unit/test_multires_regional.cpp`: build a dense and a regional
      hierarchy over the same cage, write identical `LocalDetail` into every
      vertex of the top level of BOTH, and assert the count of differing
      positions is 0 over every vertex the regional hierarchy shares with the
      dense one. It is 63 of 289 today. Assert the COUNT, not a tolerance
- [ ] 1.2 Fix the cause, which is one input and not a branch. `child_frame_of`
      takes `target_normal` from the child's own level normal, and
      `level_normals` sums only the faces THIS level has. Feed it the complete
      face ring from section 2 so the normal at a boundary vertex is the
      dense hierarchy's. Both `transport_frames` and `transport_frames_partial`
      go through it
- [ ] 1.3 The display normal takes the same input, in both forms:
      `level_normals` at `full_evaluate` and `level_normals_partial` at
      `partial_evaluate`. The partial form is the one a stamp actually takes, so
      a transition normal is re-derived on every dab and must be re-derived
      completely
- [ ] 1.4 PROVE 1.1 by reverting 1.2 and watching it fail. The revert must
      COMPILE, and the failure must be a moved POSITION rather than a moved
      normal — a frame fix that only changes shading has not fixed the storage
- [ ] 1.5 Do not weaken `tests/unit/test_multires_regional.cpp`'s existing
      bit-identity case. It authors no detail and therefore cannot see this bug;
      it encodes a true and separate claim about stored positions

## 2. One cross-level topology helper

- [ ] 2.1 DECISION, taken in `design.md` and recorded here: NOT one
      `for_each_surface_neighbor(vertex, callback)`. One topology helper with
      several readers, because `smooth_targets` re-reads the same materialized
      CSR up to `kMaxSmoothIterations` times per stamp and a callback would
      re-walk per pass, and because a neighbour is nine different payloads over
      three topologies. `build_neighbors` already made this call and wrote the
      reason down
- [ ] 2.2 DECISION: the helper answers in FACES, not vertices. Every reader that
      is wrong at a transition is a face or corner walk — `level_normals`,
      `class_normal`, `recompute_normals`, `expand_by_face_ring`,
      `is_boundary_class` — and a vertex ring falls out of the corners. Task 0.1
      measured that the defect IS the incomplete face ring
- [ ] 2.3 Build it over `LevelTopology` and `LevelConnectivity`, keyed on
      (level, vertex), returning every incident face named by the level it lives
      at. Seed it from `patch_neighbours` and `effective_level`, which already
      answer "which patches touch this one" and "at which level do I read that
      one"; `ChildIndex` already answers "does the fine level hold the child of
      this coarse vertex"
- [ ] 2.4 DECISION: the order is ascending level, then the level's own face
      order. Deterministic for free — faces are patch-major in the parent's
      order and `full_of` is ascending — but it must be STATED, because two
      sites already record that neighbourhood order is load-bearing (float
      addition is not associative)
- [ ] 2.5 Cache it in `LevelCache` and nowhere else, so
      `drop_intermediate_caches` and `drop_all_caches` already release it.
      `cache_generation` must move on RELEASE as well as on create, or a host
      holding a pointer never rebinds
- [ ] 2.6 GATE: the helper's answer at a boundary vertex equals the dense
      hierarchy's incident-face set, as a count and as a set. Today 64 of 289
      level-3 vertices have a smaller face ring than the dense hierarchy's
- [ ] 2.7 GATE: the cached helper rebuilds bit-identically after
      `drop_all_caches`, and `cache_generation` differs across the drop
- [ ] 2.8 `expand_by_face_ring` uses it, so the propagation halo no longer stops
      at the region rim. This is the walk that exists precisely because a vertex
      whose position did not move still has a changed normal, frame and detail

## 3. The brush-side readers, kept separate

- [ ] 3.1 DECISION: two normal evaluators stay two. `class_normal` is
      angle-weighted over triangles and shades the brush; `level_normals` is an
      unweighted Newell sum over faces and shades the display and builds the
      frames. One traversal cannot serve both without changing one of their
      results, and the header for the first records why area weighting was
      rejected
- [ ] 3.2 `class_normal` reads the complete ring, which fixes
      `normal_of_item` (and therefore every verb with a per-vertex direction:
      relax, inflate, pinch, crease, nudge and the normal-angle automask) and
      `automask_reference` in one place
- [ ] 3.3 `recompute_normals` reads it too, and keeps accumulating only into
      corners whose class matches — that is what preserves a hard edge and must
      not be relaxed to make a transition work
- [ ] 3.4 `is_boundary_class` stops reading a transition as a border. GATE:
      today all 64 transition classes on the fixture report true and NONE is on
      the cage's own outer edge; after the fix the count of transition classes
      reporting true is 0 and the count on the real border is unchanged.
      `on_open_border` is the hook; the model's actual border still reports true
- [ ] 3.5 GATE the visible consequence, not just the predicate: with
      `AutomaskFactor::Boundary` and 2 rings the regional stamp moves 47 classes
      against 58 without it, while the identical dense stamp stays at 107.
      Assert the class COUNT
- [ ] 3.6 `laplacian_pass` divides by the ring size AS FOUND, which is short and
      one-sided at the rim. Give it the complete ring through
      `MeshWorkItemTopology::ring_slots` and `build_neighbors` rather than a
      special case inside the kernel — there is no branch to fix, the neighbour
      simply has no identity that can name another level
- [ ] 3.7 SMOOTH IS TWO IMPLEMENTATIONS, and section 3.4 of the predecessor
      named it once. Besides the kernel path, `smooth_detail` averages
      `LocalDetail` coefficients raw and `form_shift` (under `smooth_form`)
      averages `S(n)`. Averaging coefficients across a transition is doubly
      wrong while the two sides' frames differ, so 3.7 lands AFTER section 1
- [ ] 3.8 Leave `refit_bvh` alone. It walks only the triangles this level has,
      which is correct at a transition; listed so nobody "fixes" it
- [ ] 3.9 `euclidean_region` has no ring to hook into — it scans every class of
      the level. Whatever reaches it does so through the CANDIDATE SET, not
      through a neighbour reader. `geodesic_region` is the one with a frontier

## 4. Brushes across a transition (the predecessor's 5.3)

- [ ] 4.1 DECISION to take with a measurement in hand, not before: whether a
      stamp that crosses a transition writes into TWO levels or is clamped to
      one. `MultiresSculptor::stamp` expands its write region into level-local
      vertex ids and hands them to `absorb_level_edit`; there is no
      representation for "and these vertices one level down also moved". Two
      lists absorbed into two levels, with two frames and two detail fields, is
      the real shape
- [ ] 4.2 `build_multires_workset` already tags each item with a level, so the
      vocabulary exists. A cross-level workset extends it rather than replacing
      it
- [ ] 4.3 Any (level, vertex) pair that outlives a stamp carries the revision
      discipline `ConnectivitySeed` and `seed_revision` already have.
      `MultiresSculptor::bind` renumbers every weld class on a level change AND
      on a `cache_generation` change, and a regional level's numbering is
      compacted to what it stores
- [ ] 4.4 GATE: the same stroke across a transition and on a dense hierarchy
      finish in the same place where the levels agree. Today, a `MeshBrush`
      Smooth anchored on a transition vertex moves 107 classes densely and 58
      regionally, and 11 shared vertices finish up to 0.0255854 apart — about
      60% of the level-3 edge spacing
- [ ] 4.5 No fixture in `tests/unit/test_multires_sculpt.cpp` is mixed-depth
      today — every one uses uniform `add_level` — so the suite is silent on
      transitions by construction rather than by luck. Add the mixed-depth
      fixture there and SIZE it by the transition fraction: 64 of 289 level-3
      vertices, 22%, because a small refined region has proportionally more
      boundary. A large refined region under-reports every effect above

## 5. Mixed-depth export — last, and no user is waiting

- [ ] 5.1 DECISION: derived, not stored. `kSurfaceVersion` does NOT move and
      there is no format-minor bump — the topology is a function of the cage,
      the rule and the per-level patch sets, all already in a version-3 stream
- [ ] 5.2 DECISION: the shared vertex takes the FINE side's value, and every
      coarse face incident to it adopts that value — including a corner-only
      patch with no split edge. Stopping at edge-adjacent faces relocates the
      crack one face over onto a coarse-coarse edge. The emission set is "every
      coarse face incident to a cage vertex shared with a finer patch"
- [ ] 5.3 DECISION: quad-only templates. `Mesh::quads` requires
      `quads.size() / 4 * 6 == indices.size()`, and `level_faces_into` already
      clears `quads` for any non-uniform face list, so one 5-gon turns the whole
      export into a triangle soup. Template each incidence pattern that occurs:
      1-sided, 2-sided, 3-sided, 4-sided, corner-only
- [ ] 5.4 Emit for `mesh_at_level` AND for `build_block`. They are separate code
      paths that both take a single `level`; a transition built into only one
      leaves the other cracked
- [ ] 5.5 Attribute values for a synthesized corner need an explicit rule.
      `build_attr_level` maps attribute vertices to geometric ones by a
      face-for-face correspondence between the two topologies, and a synthesized
      face has no counterpart on either side. The machinery is already regional;
      only the rule for a new face is missing
- [ ] 5.6 A transition face inherits the `face_patch` of the coarse face it
      replaces. That is the whole commitment to per-face grouping this change
      makes — the polygroup work is a design proposal that writes no code and
      whose unit is the triangle
- [ ] 5.7 The new entry point does NOT repeat `build_block`'s shape, which
      returns true with an empty block for a non-resident patch. A descriptor
      starting with `uint32_t struct_size`, grown by appending, a new field's
      zero meaning today's behaviour, caller-owned buffers, `BUFFER_TOO_SMALL`
      for a short buffer, and a header that says what the call does NOT promise.
      `MultiresExportOptions` has no `struct_size` today and
      `clay_multires_copy_level_mesh` takes no options struct at all
- [ ] 5.8 DECISION deferred to this stage with a measured peak in hand: whether a
      mixed export needs its own preflight. `mesh_at_level` is non-const and
      evaluates, so a mixed export forces levels 0..max simultaneously resident,
      and the peak-versus-persistent argument behind `preflight_add_level`
      applies. Decide against a number, not an assumption
- [ ] 5.9 GATE: assembling the export welds to 0 boundary edges. Today,
      assembling per-patch `clay_multires_copy_block` at
      `clay_multires_effective_level` gives 240 boundary edges at display 3, 144
      at display 2 and 48 at display 1, against 0 for one level everywhere
- [ ] 5.10 GATE: the export is quad-clean — `Mesh::quads` non-empty and the
      invariant holding — on a mixed-depth hierarchy
- [ ] 5.11 GATE: stability under re-refinement. Refining an unrelated region
      leaves the emitted faces of a coarse face whose vertex-ring residency did
      not change byte-identical. Refinement is monotonic, so the transition set
      can only shrink
- [ ] 5.12 GATE: determinism, asked in the opposite order — the same hierarchy
      built from a reversed patch list emits byte-identical transition faces

## 6. Surface, docs and versions

- [ ] 6.1 C ABI, pyclay and a mirrored entry point for the export, following
      `clay_multires_block_info` and `clay_multires_copy_block` in shape. The
      cross-level helper of section 2 is internal and gets no C entry point
      unless a host asks for one
- [ ] 6.2 VERSION LINES move together to 0.87.0 — NOT 0.86.0, which two other
      branches already claim — in the stage that first adds an entry point.
      `CMakeLists.txt` `project(VERSION)`, `CLAY_ABI_MAJOR` / `CLAY_ABI_MINOR` /
      `CLAY_ABI_PATCH` in `bindings/c/clay.h`, and `version` in `pyproject.toml`.
      A gate fails if the three disagree
- [ ] 6.3 `docs/09-brush-latency-and-coverage.md` states the export gap under
      "What is not done yet" and is correct today; update it to what landed
- [ ] 6.4 `examples/74_regional_multires.py` repeats the gap in the artist's
      vocabulary and calls the polygons "the next piece of this change". Update
      it, and say plainly there that the export half had no host waiting
- [ ] 6.5 Say in the docs who each half is for. `mesh_at_level` and
      `clay_multires_copy_level_mesh` are called only from tests, bindings and
      examples — no host loop — and the one host we can check does not export
      hierarchies at all. The frame and neighbourhood half is storage and does
      have users today
- [ ] 6.6 `python3 tools/check_task_symbols.py` and the OpenSpec strict
      validation both pass on this change before it is opened
