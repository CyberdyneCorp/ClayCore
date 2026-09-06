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

- [x] 2.1 DECISION, taken in `design.md` and recorded here: NOT one
      `for_each_surface_neighbor(vertex, callback)`. One topology helper with
      several readers, because `smooth_targets` re-reads the same materialized
      CSR up to `kMaxSmoothIterations` times per stamp and a callback would
      re-walk per pass, and because a neighbour is nine different payloads over
      three topologies. `build_neighbors` already made this call and wrote the
      reason down
- [x] 2.2 DECISION: the helper answers in FACES, not vertices. Every reader that
      is wrong at a transition is a face or corner walk — `level_normals`,
      `class_normal`, `recompute_normals`, `expand_by_face_ring`,
      `is_boundary_class` — and a vertex ring falls out of the corners. Task 0.1
      measured that the defect IS the incomplete face ring
- [x] 2.3 Build it over `LevelTopology` and `LevelConnectivity`, keyed on
      (level, vertex), returning every incident face named by the level it lives
      at. Seed it from `patch_neighbours` and `effective_level`, which already
      answer "which patches touch this one" and "at which level do I read that
      one"; `ChildIndex` already answers "does the fine level hold the child of
      this coarse vertex"
- [x] 2.4 DECISION: the order is ascending level, then the level's own face
      order. Deterministic for free — faces are patch-major in the parent's
      order and `full_of` is ascending — but it must be STATED, because two
      sites already record that neighbourhood order is load-bearing (float
      addition is not associative)
- [x] 2.5 Cache it in `LevelCache` and nowhere else, so
      `drop_intermediate_caches` and `drop_all_caches` already release it.
      `cache_generation` must move on RELEASE as well as on create, or a host
      holding a pointer never rebinds
- [x] 2.6 GATE: the helper's answer at a boundary vertex equals the dense
      hierarchy's incident-face set, as a count and as a set. Today 64 of 289
      level-3 vertices have a smaller face ring than the dense hierarchy's
- [x] 2.7 GATE: the cached helper rebuilds bit-identically after
      `drop_all_caches`, and `cache_generation` differs across the drop
- [ ] 2.8 `expand_by_face_ring` uses it, so the propagation halo no longer stops
      at the region rim. This is the walk that exists precisely because a vertex
      whose position did not move still has a changed normal, frame and detail.
      LEFT FOR SECTION 1, deliberately: the halo decides which vertices have
      their normals and frames redone, and while that normal is still summed
      over the level's own faces alone a wider halo writes the same numbers to
      the same vertices and no test can see it. It becomes observable in the
      same change that makes the normal complete

## 3. The brush-side readers, kept separate

- [x] 3.1 DECISION: two normal evaluators stay two. `class_normal` is
      angle-weighted over triangles and shades the brush; `level_normals` is an
      unweighted Newell sum over faces and shades the display and builds the
      frames. One traversal cannot serve both without changing one of their
      results, and the header for the first records why area weighting was
      rejected
- [x] 3.2 `class_normal` reads the complete ring, which fixes
      `normal_of_item` (and therefore every verb with a per-vertex direction:
      relax, inflate, pinch, crease, nudge and the normal-angle automask) and
      `automask_reference` in one place
- [x] 3.3 `recompute_normals` reads it too, and keeps accumulating only into
      corners whose class matches — that is what preserves a hard edge and must
      not be relaxed to make a transition work
- [x] 3.4 `is_boundary_class` stops reading a transition as a border. GATE:
      today all 64 transition classes on the fixture report true and NONE is on
      the cage's own outer edge; after the fix the count of transition classes
      reporting true is 0 and the count on the real border is unchanged.
      `on_open_border` is the hook; the model's actual border still reports true
- [x] 3.5 GATE the visible consequence, not just the predicate: with
      `AutomaskFactor::Boundary` and 2 rings the regional stamp moves 47 classes
      against 58 without it, while the identical dense stamp stays at 107.
      Assert the class COUNT
- [x] 3.6 `laplacian_pass` divides by the ring size AS FOUND, which is short and
      one-sided at the rim. Give it the complete ring through
      `MeshWorkItemTopology::ring_slots` and `build_neighbors` rather than a
      special case inside the kernel — there is no branch to fix, the neighbour
      simply has no identity that can name another level
- [ ] 3.7 SMOOTH IS TWO IMPLEMENTATIONS, and section 3.4 of the predecessor
      named it once. Besides the kernel path, `smooth_detail` averages
      `LocalDetail` coefficients raw and `form_shift` (under `smooth_form`)
      averages `S(n)`. Averaging coefficients across a transition is doubly
      wrong while the two sides' frames differ, so 3.7 lands AFTER section 1
- [x] 3.8 Leave `refit_bvh` alone. It walks only the triangles this level has,
      which is correct at a transition; listed so nobody "fixes" it
- [x] 3.9 `euclidean_region` has no ring to hook into — it scans every class of
      the level. Whatever reaches it does so through the CANDIDATE SET, not
      through a neighbour reader. `geodesic_region` is the one with a frontier

### What the traversal stage landed, and where the tree corrected the plan

- The helper is `CrossLevelNeighborhood` in `include/clay/mesh/cross_level.h`,
  built by `build_cross_level` and reached through
  `MultiresSurface::cross_level_at`. `MultiresSculptor` hands it to the level
  sculptor with `MeshSculptor::set_cross_level`, and it is refreshed on EVERY
  bind rather than only on a rebuild: its outside positions are the level
  below's, and a stroke down there moves them without this level's binding
  going stale
- CORRECTION TO 2.3 AND 2.4. A face the level does not store is not a face of
  the COARSER level, and naming it one would have made every reader wrong: a
  coarse face's own Newell normal is not the sum of the two child normals a
  boundary vertex actually has. What is missing at a boundary is a face of THIS
  level that this level does not store, so the helper names each one by the
  index the DENSE level would have given it, and its corners by a joined
  numbering that runs past the level's own vertex count. The order that follows
  is this level's own dense face order — parent face, then corner — which is
  deterministic for the reason 2.4 gives
- CORRECTION TO 2.3. It is seeded from the parent's own face list and the
  level's `patch_kept` rather than from `patch_neighbours` and
  `effective_level`. Those two answer at PATCH granularity and the join needed
  is per vertex; `ChildIndex` already answers "does this level hold the child of
  that parent element", which is the whole of it
- A corner the level does not store is positioned by `subdivide_positions`
  itself, through a `ChildIndex` over exactly those layout ids — the same call
  the level's own vertices came out of, so a boundary value is the dense
  hierarchy's bits and not a second copy of the four rules. It is the surface
  with NO DETAIL on it, which is the only answer there is: a vertex the level
  does not store has nowhere to hold a coefficient
- 2.5 was ALREADY TRUE on the release side. `release_generation` already moves
  `cache_generation` when a drop released anything, so the transition set
  inherited it by living in `LevelCache`. Gated anyway in 2.7, because the
  file's own record says what happened the one time only the create side bumped
- 3.6 landed through `build_neighbors` and NOT through `ring_slots`, and the
  reason is measured rather than assumed: a cross-level neighbour that this
  level STORES is already an adjacency ring neighbour, so the only thing a depth
  boundary adds is a vertex outside the level — which is never in a workset and
  so never a `ring_slots` answer. `test_multires_sculpt.cpp` gates that count at
  0 rather than leaving it as an argument
- 3.9 CONFIRMED and unchanged: `euclidean_region` scans the level's own classes,
  and a vertex the level does not store can never be a candidate because nothing
  can write to it. There was nothing to reach it with
- A `MeshSculptor::write` that also marked the corners of the derived faces was
  written and then DELETED. The set it adds — the two edge points either side of
  a concave corner of the refined region — is real and measured (1 pair on an
  L-shaped region, 0 on a square block), but a stamp's normal-refresh set is the
  union over every class it moved and already covered it: the count of re-shaded
  vertices was 67 with the walk and 67 without. An identical count after
  deleting a term is the proof the term is never reached
- WHAT THE GATES MEASURE, all of them counts or byte comparisons on a 6x6 cage
  with the middle 2x2 refined to level 3. The complete face set at every one of
  289 level-3 vertices equals the uniform hierarchy's, where the level's own
  connectivity is short at 64 of them. All 64 rim classes report
  `is_boundary_class`, none on the cage's own edge, and 0 do afterwards while
  the uniform hierarchy's 192 real border classes are unchanged. A Smooth stamp
  on the rim leaves 11 shared vertices up to 0.0185 from where the uniform
  hierarchy leaves them — 44% of the 0.0417 level-3 edge spacing — against 3 at
  3.0e-08, which is float rounding. With boundary automasking on, 71 classes
  move instead of 82 before, and 82 after
- PROVED BY REVERT, twice, each revert compiling. Dropping the derived-face
  emission fails 10 assertions across 7 cases; dropping only the derived term in
  `is_boundary_class` fails exactly the two that are about a border

## 4. Brushes across a transition (the predecessor's 5.3)

- [x] 4.1 DECISION to take with a measurement in hand, not before: whether a
      stamp that crosses a transition writes into TWO levels or is clamped to
      one. TAKEN, AND IT IS TWO — three, where the grading makes it three — and
      the measurement is what took it. One Draw stamp anchored on the rim of the
      middle 2x2 of a 6x6 cage refined to level 3, against the same stamp on a
      uniformly refined hierarchy, over the mixed-depth surface an artist is
      actually looking at:

      | radius | clamped to one level | written across levels |
      |---|---:|---:|
      | 0.25 | 5 vertices never move, worst 0.029781371 | 0, worst 0.001621436 |
      | 0.35 | 17 never move, worst 0.090758100 | 0, worst 0.003878876 |
      | 0.50 | 46 never move, worst 0.182510689 | 0, worst 0.005068991 |

      The level-3 edge spacing here is 0.0417, so the clamped column is up to 4.4
      EDGES out — a step in the displacement rather than a fade — and the written
      column is a tenth of one edge. Clamping was the honest alternative and the
      measurement refused it: what a clamped stamp leaves is not a rounding
      error, it is the whole coarse half of the footprint
- [x] 4.2 `build_multires_workset` already tags each item with a level, so the
      vocabulary exists. A cross-level workset extends it rather than replacing
      it. LANDED AS A PARTITION RATHER THAN A WIDER WORKSET, and the correction
      is recorded below: the write list is per level, and which level owns a
      vertex is a question the EXPORT already answers
- [x] 4.3 Any (level, vertex) pair that outlives a stamp carries the revision
      discipline `ConnectivitySeed` and `seed_revision` already have. Nothing
      here outlives a stamp: `last_write_vertices_at` is read after one and
      cleared by the next, and the list of levels that own vertices is rebuilt
      by `bind` on a level change AND on a cache-generation change, with the
      coarse `MeshSculptor` built for the stamp and dropped with it rather than
      held. GATED: a `drop_all_caches` between two stamps of one stroke moves
      the generation and the second stamp still writes both levels
- [x] 4.4 GATE: the same stroke across a transition and on a dense hierarchy
      finish in the same place where the levels agree. Asked as a RATIO rather
      than against a threshold, because the number that matters is the
      improvement: 18x, 23x and 36x closer at the three radii above, and the
      residual is 2% of the stamp's own peak displacement against 73% for the
      clamped stamp. Where the levels do NOT agree the residual is the coarse
      level's own spacing and is stated as such
- [x] 4.5 No fixture in `tests/unit/test_multires_sculpt.cpp` is mixed-depth
      today. It is now — `build_regional`, the one the section-2 gates already
      use, sized as this task asks: the middle 2x2 of a 6x6 cage, where 64 of
      289 level-3 vertices (22%) are on the rim, because a SMALL refined region
      has proportionally more boundary and a large one under-reports every
      number above

### What the brushes stage landed, and where the tree corrected the plan

- `MultiresSculptor::stamp` writes every level the footprint reaches, coarsest
  first, with the bound level written LAST as absolute positions. No new type
  and no new entry point: `stamp_coarse`, `bind_coarse`, `note_before` and
  `partition_coarse_write` in `src/mesh/multires_sculpt.cpp`, plus
  `MultiresSurface::restore_level_positions` — the other half of "the ONE write
  path", for a caller that moved a level's mesh and decided part of the move was
  not its to keep
- CORRECTION TO 4.2, AND IT MOVED WORK OUT. The plan called for a cross-level
  WORKSET. There is none, because the question a wider workset would have had to
  answer is one the export stage already answered: which level does the
  mixed-depth surface carry this vertex at? The answer is `ChildIndex::of(level
  above).stored(v) == kNoVertex`, one line, and it is the SAME predicate
  `mixed_mesh_at_level` emits its vertices with. So the two write lists are a
  partition of that surface by construction, and "no doubled contribution at the
  seam" is a property of the representation rather than a tolerance. The export
  half, which no host is waiting for, turns out to have a user: the brush
- ORDERING IS THE WHOLE ARITHMETIC, and it was measured rather than reasoned. A
  coarse write moves S(n) under the finer levels beside it, so a fine level
  absorbed FIRST keeps a coefficient that reconstructs to the asked-for position
  plus that ripple. Fine-first finishes 0.106460609 from the uniform hierarchy's
  answer at radius 0.50; coarsest-first with the fine level written as an
  absolute target finishes 0.005068991 from it — 21x — and clamping to one level
  finishes 0.182510689 from it
- WHAT IS NOT WRITTEN, and it is deliberate: a coarse vertex the level ABOVE
  carries is put back where it was rather than absorbed. The consequence is that
  the coarse form under the refined region does not follow the stroke, which is
  the honest reading of a regional hierarchy — the stroke lives at the sculpt
  level there, and an artist who wants it in the coarse form sculpts the coarse
  level. Absorbing those too was tried and measured: it leaks into level-3
  vertices outside the footprint, which the fine write list cannot compensate
- NO NEW ENTRY POINT, AND THE VERSION LINES DO NOT MOVE for this stage. A host's
  redraw path already works: `absorb_level_edit` marks BASE PATCHES dirty at
  whatever level it is given, and `clay_multires_dirty_blocks` reports patches
  rather than levels, so a host re-copying its dirty patches at their effective
  level sees the coarse write with no ABI change at all.
  `clay_multires_stamp_report.moved_vertices` now counts the whole stamp and
  `.level` still names the level the brush was bound to. The two new C++
  accessors — `last_write_levels` and `last_write_vertices_at` — get no C mirror
  until a host asks for one
- COMPLEXITY: `stamp` was 35 before this change and is 13 after it, with every
  new function at or below 10 (`note_before` 10, `partition_coarse_write` 9,
  `stamp_coarse` 7, `restore_level_positions` 7)
- PROVED BY REVERT, twice, each revert compiling, each one isolating one
  decision. Dropping the coarse write — every moved coarse vertex treated as the
  level above's — fails 39 assertions across 6 cases, including every
  `dropped == 0` gate, both write-list gates and the undo gate. Reverting ONLY
  the ordering — the bound level absorbed before the coarse passes — fails 9
  assertions across 2 cases and, tellingly, NOT one `dropped == 0`: nothing is
  dropped by the wrong order, it is counted twice

## 5. Mixed-depth export — last, and no user is waiting

- [x] 5.1 DECISION: derived, not stored. `kSurfaceVersion` does NOT move and
      there is no format-minor bump — the topology is a function of the cage,
      the rule and the per-level patch sets, all already in a version-3 stream.
      GATED: `encode()` is byte-identical before and after an export, and a
      hierarchy saved, decoded and re-exported gives the same mesh
- [x] 5.2 DECISION: the shared vertex takes the FINE side's value, and every
      coarse face incident to it adopts that value — including a corner-only
      patch with no split edge. Stopping at edge-adjacent faces relocates the
      crack one face over onto a coarse-coarse edge. The emission set is "every
      coarse face incident to a cage vertex shared with a finer patch".
      MEASURED: 12 of 144 patches are corner-only on the gate's fixture, and
      they emit only quads — the ripple reaches faces with no T-junction at all
- [x] 5.3 DECISION, AND THE ONE THE TREE OVERRULED. The plan said "quad-only
      templates, one per incidence pattern". There is no such template and there
      cannot be: a coarse quad with one split edge is a PENTAGON, and a polygon
      with an odd number of boundary vertices has no quadrangulation — four
      edges per quad counts every interior edge twice, so the boundary count
      must be even however many vertices are added inside. Making it even means
      splitting a second edge of that face, which its coarse neighbour must then
      carry too, and so on across the model. So the decision taken instead:
      `Mesh::quads` survives exactly while NO edge is split — every uniform
      export and every corner-only transition — and is dropped whole otherwise,
      because a quad list that does not describe `indices` is the lie
      `mesh_data.h` forbids. `level_faces_into` already makes that choice from
      the face list, so no new code decides it
- [x] 5.4 Emit for the whole-surface path AND for the per-patch path.
      `mixed_mesh_at_level` and `build_mixed_block`, and they agree: assembling
      every patch's block and welding at exactly zero gives the same vertex
      count as the whole-surface export at every display level (264, 472, 680).
      CORRECTION: as NEW entry points rather than as changes to `mesh_at_level`
      and `build_block`, which keep their meaning — a single named level — and
      whose every existing caller and golden would otherwise move. 5.7
      presupposes a new entry point in any case
- [x] 5.5 Attribute values for a synthesized corner. CORRECTION: there are no
      synthesized corners. Every emitted vertex is a vertex some level already
      stores, so the rule is "read the channel at the level that vertex lives
      at" and the existing `AttrLevel` answers it. What has no rule is a cage
      that SPLITS its attributes: there the attribute hierarchy is a second
      topology mapped face for face, and a mixed-depth face has no counterpart
      on either side. Refused by name — `AttributeSplitCage` — and the geometry
      comes back when the caller asks again with `uvs` and `colors` off
- [x] 5.6 A transition face inherits the `face_patch` of the coarse face it
      replaces. It does, through the emitted topology, and `build_mixed_block`
      is where a caller sees it: a patch's block holds exactly the faces that
      belong to it, transition faces included. `Mesh` has nowhere to carry a
      per-face group, so the whole-surface export states the commitment rather
      than transporting it — which is the whole commitment this change makes to
      the polygroup proposal
- [ ] 5.7 The new entry point does NOT repeat `build_block`'s shape, which
      returns true with an empty block for a non-resident patch. A descriptor
      starting with `uint32_t struct_size`, grown by appending, a new field's
      zero meaning today's behaviour, caller-owned buffers, `BUFFER_TOO_SMALL`
      for a short buffer, and a header that says what the call does NOT promise.
      LEFT FOR 6.1, because this stage added no C symbol for those rules to
      apply to. The half that is C++ IS done and gated: a refusal has a name
      (`MultiresMixedStatus`), a refused export is empty rather than partial,
      and the header states what the call does not promise — no quad list at a
      split edge, no attributes on a split cage
- [x] 5.8 DECISION deferred to this stage with a measured peak in hand: whether
      a mixed export needs its own preflight. IT DOES NOT, and the reason is
      that `mesh_at_level` already walks every level below its own — both calls
      open with `evaluate_up_to(level)`. The mixed export then reads the
      evaluated positions and builds no level mesh, no adjacency and no chunk
      table, so its resident set is a SUBSET. GATED as a byte comparison:
      `memory().rebuildable` after a mixed export is <= after `mesh_at_level` on
      the same hierarchy, and both are above the cold figure
- [x] 5.9 GATE: assembling the export welds to 0 boundary edges. Measured on a
      closed torus cage, the loop `clay.h` tells a host to write today —
      `build_block(effective_level(patch, display), patch)` per patch, welded at
      exactly zero — leaves 72 open edges at display 1, 168 at display 2 and 264
      at display 3, against 0 at display 0 where the depths agree. The same loop
      over `build_mixed_block` leaves 0 at every one of them, and so does
      `mixed_mesh_at_level`, which needs no welding at all
- [x] 5.10 GATE: the quad list survives exactly as far as the split edges allow,
      which is 5.3's decision rather than the "quad-clean always" the plan
      asked for. A uniform export keeps `Mesh::quads` with the invariant
      holding; a mixed one with a split edge carries no quad list. The three
      patch counts that say why: 48 patches emit more triangles than twice their
      faces (a split edge), 12 emit exactly twice and still span two levels
      (corner-only, all quads), 84 are untouched
- [x] 5.11 GATE: stability under re-refinement. Refining a distant region leaves
      84 of 144 patches byte-identical — indices, vertex levels and positions —
      while 60 change, so the count is a comparison rather than a tautology. The
      transition beside the first region, pentagons included, is in the
      identical half. Asked between two hierarchies rather than across a
      mutation, because `refine_patches_to_level` builds levels rather than
      growing the ones that exist
- [x] 5.12 GATE: determinism, asked in the opposite order — the same hierarchy
      built from a reversed patch list emits a byte-identical mesh

### What the export stage landed, and where the tree corrected the plan

- `MultiresSurface::mixed_mesh_at_level` and `MultiresSurface::build_mixed_block`
  in `src/mesh/multires_mixed.cpp`, with `MultiresMixedStatus` and
  `Block::vertex_levels` added to `include/clay/mesh/multires.h`. No C symbol,
  no version-line move: 6.1 and 6.2 own those, and the predecessor stage left
  them for the same reason
- THERE IS NO CONFIGURATION TABLE, and the plan's 1-, 2-, 3-, 4-sided and
  corner-only cases are not five templates but five answers to the same two
  questions, asked of the level above each emitted face: does it store this
  corner's vertex point, and does it store this edge's edge point? Both are
  properties of the shared element rather than of the face asking, so two faces
  either side of a boundary get the same answer and the mesh is watertight by
  identity. The ripple of 5.2 is automatic rather than a case to remember,
  because the corner is resolved through the level above and not through an "is
  this a transition face" test
- CORRECTION TO 5.3, and it is arithmetic rather than a judgement: quad-only
  transition templates do not exist. See the task. The consequence is stated in
  the header, in the spec delta and in the gate, in both directions
- THE EXPORT IS A READ, which is the ClaySpaceDesktop question `design.md`
  records. Gated: `detail_checksum`, `base_revision` and `detail_revision` are
  unchanged across an export, `encode()` is byte-identical across one, and two
  exports of the same surface are the same bytes. It does EVALUATE — every level
  up to its own, because it reads each emitted vertex at the level that vertex
  lives at — and building a level cache moves `cache_generation`; the header says
  so rather than leaving a host to find out. IT PROMISES NO RESIDENCY: a level a
  trim released is brought back to answer it, which the same call one line below
  in the header does not do. What stood here said the export evaluates exactly
  the levels `mesh_at_level` evaluates, which is the sentence the residency fix
  removed from the header for being false; it was still in this record and is
  corrected for the same reason
- WHAT THE GATES MEASURE, all counts or byte comparisons, on a CLOSED torus
  cage of 144 patches with a 2x2 region refined to level 3. A closed cage on
  purpose: "0 boundary edges" is then a statement about the export and not about
  the cage's own rim, and the split-cage case gates the rim count (18) explicitly
  so the one open fixture cannot pass by accident
- PROVED BY REVERT, twice, each revert compiling, and each one isolating one of
  the two defects the audit measured. Dropping the CORNER promotion — the
  0.0434 gap, the larger half — fails 18 assertions across 5 cases, including
  `open_edges == 0` at all three mixed display levels. Dropping only the EDGE
  point — the 0.0232 T-junction — fails 12 across 5, including `open_edges == 0`
  again and, tellingly, `mixed.quads.empty()`: with no edge split every face is
  a quad, which is exactly 5.3's parity argument arriving from the other side

## 6. Surface, docs and versions

- [ ] 6.1 C ABI, pyclay and a mirrored entry point for the export, following
      `clay_multires_block_info` and `clay_multires_copy_block` in shape. The
      cross-level helper of section 2 is internal and gets no C entry point
      unless a host asks for one
- [x] 6.2 VERSION LINES move together, and they moved for a reason this task did
      not anticipate: not the new entry point of 6.1, which is still unbuilt,
      but a field of an EXISTING one that means something new.
      `clay_multires_stamp_report.moved_vertices` counted weld classes at the
      bound level, because a stamp only ever wrote one; it now sums the classes
      kept on every level a crossing stamp wrote. Same layout, same number, new
      meaning — which is worse than a new field, because nothing a host compiles
      against tells it to look. The alternative was to report the bound level
      alone, and that is the silent failure this change exists to remove: a
      stamp that moved only the coarse side would come back 0. So the meaning is
      stated on the field and the minor moves with it. THE NUMBER IS 0.88.0, not
      the 0.87.0 written here: this branch was cut when the tree was at 0.85.0,
      0.86.0 and 0.87.0 have both landed on main since, and 0.88.0 is the next
      free minor. `CMakeLists.txt` `project(VERSION)`, `CLAY_ABI_MAJOR` /
      `CLAY_ABI_MINOR` / `CLAY_ABI_PATCH` in `bindings/c/clay.h`, and `version`
      in `pyproject.toml`; `release_check.py` reads
      `cmake=0.88.0 abi=0.88.0 wheel=0.88.0`. 6.1 adds an entry point at this
      same minor and does not move it again
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
- [x] 6.6 `python3 tools/check_task_symbols.py` and the OpenSpec strict
      validation both pass on this change before it is opened — and so does
      every other gate CI runs against it, listed in the block below. 6.1 will
      add a C symbol and must re-run them; nothing here excuses that

### What the gate run landed

- WHAT THE GATES FOUND, and only the sanitizer job could have found it: both
  readers this change adds look at a level BELOW the one they were asked for,
  and `evaluate_up_to` does not promise that level is resident. It guarantees
  the cache of the level it was ASKED for and no other, on purpose —
  `drop_intermediate_caches` releases everything between the cage and the
  levels in use without marking anything pending, so the short circuit walks
  past them and a release STAYS released, which is the whole point of the trim.
  So `cross_level_at`, whose outside vertices are subdivided from the parent,
  and `mixed_mesh_at_level`, which reads each emitted vertex at the level that
  vertex lives at, both read a released cache through a null pointer. The
  ASan+UBSan preset reports `member access within null pointer of type 'struct
  LevelCache'` in the EXISTING case "the levels between the cage and the brush
  can be released and stay released"; an unsanitized build segfaults
- THE FIX IS TWO ANSWERS, not one, because the two readers differ in what they
  can do without. `cross_level_at` can answer a level that stores every child of
  every face of its parent without reading the parent at all — which is every
  level of a uniform hierarchy, so a trim there still holds — and
  `level_is_self_contained` in `cross_level.h` now names that rule once for both
  this caller and `build_cross_level` rather than leaving a second copy of it.
  Any other level brings the level below back, and deliberately: the outside
  positions are the parent's, a stroke down there moves them without this
  level's cache going stale, and the re-read on the way past is the whole reason
  they are not tracked — so handing back the last copy would be an answer a
  reader cannot tell from a current one, and handing back an empty
  neighbourhood would read as "no depth boundary here". The export has no
  cheaper case at all — a vertex emitted at level 2 is read out of level 2 — so
  it asks for the storage back through a new `evaluate_all_up_to`, whose comment
  says why `evaluate_up_to` is not it
- GATED as one case, "a level released between the cage and the brush is still
  readable", on a 144-patch closed torus with a 2x2 region at level 3. The two
  halves are trimmed SEPARATELY so each is exercised against a released parent
  on its own: the export after a trim answers the same mesh with 0 open edges,
  and the neighbourhood after a second trim is the same `corners`, `dense_face`,
  `outside_layout` and `outside_positions` bits. What it costs is asserted
  rather than left implied — `resident_levels` goes 1 -> 4 at each of them. The
  uniform half of the claim needs no new case: "the levels between the cage and
  the brush can be released and stay released" in `test_multires_dirty.cpp` is
  the gate that a uniform hierarchy still holds its trim at one resident level,
  and it is the case the sanitizer reported the null access on
- PROVED BY REVERT, twice, each revert compiling and each isolating one reader.
  Dropping the released-parent test in `cross_level_at` alone: SIGSEGV after 11
  assertions — the whole export half passes, and the crash lands on the first
  `cross_level_at` past a trim. Dropping only `evaluate_all_up_to` in the
  export, with the other fix left in: SIGSEGV after 7, at the export instead.
  The tree was restored from a saved patch after each and the suite re-run green
  (94 cases, 17877 assertions)
- THE HEADERS SAY WHAT THIS COSTS. `mixed_mesh_at_level` claimed it evaluated
  "exactly the levels `mesh_at_level(level)` evaluates", which stopped being
  true the moment a trim was in the picture; it now states that residency is not
  something it promises and that a host trimming between exports pays for the
  levels below again. `cross_level_at` says the same about its outside vertices
- NO TIMING WAS TAKEN, as with every stage before this one. The cost of paying a
  trim back is stated as a level count (`resident_levels` 1 -> 4) and not as a
  duration

### What the review of that gate run landed: a crash the guard missed, and six
### terms nothing executed

- THE GUARD WAS WRITTEN AGAINST THE POINTER AND THE STATE IS A FLAG. A level
  cache can be ALLOCATED and not evaluated: `ensure_cache` builds a level's
  connectivity for a caller that wanted only that, leaves `subdivided`, `frames`
  and `mesh.positions` empty, and `MultiresSurface::connectivity_at` is a public
  call that reaches it. So a trim followed by one connectivity question leaves a
  level whose cache is present and whose surface is not, and both readers walked
  straight past `!cache` into an empty position array. `level_is_evaluated` is
  now the one predicate all three sites ask — the two new guards and
  `below_is_current`, which already tested the flag and is where the shape came
  from. GATED as "a released level asked for its connectivity is still not
  evaluated", on the same 144-patch torus, and PROVED BY REVERT twice: the
  pointer test back in `evaluate_all_up_to` SIGSEGVs after 11 assertions against
  25, and back in `cross_level_at` after 19. The export half needs the
  connectivity asked for at every level below the target, because filling one
  hole marks the level `pending_all` and the mark is what makes the short
  circuit fall through and cover for the bug — that is written into the case
- A REUSED `Block` CARRIED A STALE LEVEL ARRAY. `build_block` cleared `vertices`
  and `indices` and not `vertex_levels`, and the emptiness of that array is the
  statement "every vertex is at `level`" — which `block_positions` and the
  header both tell a host to read. Invisible on a fresh block and visible on the
  second use, so the case reuses ONE block across `build_mixed_block` and
  `build_block`, as a host's loop does. Reverting the clear fails it twice: the
  emptiness gate and the value gate that reads the block back through the array
- A TERM NOTHING EXECUTES IS NOT A FIX, and the instrument is deletion: delete
  the term, run the suite, and an IDENTICAL assertion count says no case reached
  it. Six were found that way and all six are now reached, each proved by
  deleting it again. Counts are the whole suite, before -> after the case:
  - Task 3.3's cross-level contribution in `recompute_normals`: 16463619 ->
    16463619 identical. Now "a normal recompute completes the ring at a depth
    transition", which drives a DEFORMER so every class is touched at once and
    re-derives the angle-weighted fan from the mesh and the neighbourhood: 64 rim
    classes, worst 0.000000000 against the complete fan and 0.393747 against the
    level's own. Deleting the term: 3 failed assertions
  - The "outside positions are re-read on every access" guarantee: deleting
    `refresh_cross_level` changed nothing. Now "the outside positions follow a
    stroke on the level below" — a coarse stroke moves 23 of them, and what a
    reader gets is what a hierarchy with nothing cached would build. Deleting the
    call: 2 failed (23 -> 0 moved, and the value against the fresh build)
  - `CrossLevelNeighborhood::face_patch` had no reader that depended on its
    values. KEPT rather than deleted, because it is task 5.6's identity on the
    structure that holds the derived faces, and now asserted as one: the patch is
    the coarse parent face's, and it is always a patch this level does NOT
    refine. Deleting the `push_back`: 1 failed
  - Task 5.5's attribute gathering had no VALUE gate — the existing case checks
    that `colors.size() == positions.size()`, which an export of the right number
    of zeroes would pass. Now "an emitted vertex carries the ATTRIBUTES of the
    level it lives at": the export's numbering is rebuilt from
    `build_mixed_block`, checked against the positions first, and then both
    channels are read at the level each vertex lives at. Dropping the copy: 3
    failed
  - `append_outside_neighbors`'s `if (colors) return;`: identical count. Now "a
    COLOUR verb keeps the ring it already had at a transition" — a smear over a
    coloured copy of a level mesh with the neighbourhood bound writes the same
    bytes as one without it. Deleting the guard: 2 failed (44 classes against 42,
    and 8 colours differing), because `kernel_smear` reads `nb.colors[k]` at the
    index it reads `nb.positions[k]`
  - `append_outside_neighbors`'s `std::find`: identical count. Reachable only
    through a WELDED class, which needs two level vertices at one point — what
    `level_adjacency`'s exact 0.0f weld exists for. Built directly: two rim
    vertices sharing outside neighbours pinched onto one point, 8 outside entries
    and 7 distinct. One smooth pass moves a vertex ALONG (mean - p), so the
    direction is the part that does not depend on the falloff: sine 0.000000000
    with the guard, 0.069843 without
- ONE REPORTED FINDING IS NOT REAL AS STATED, and the probe is why. Deleting the
  `want_normals` output in `append_outside_neighbors` does NOT leave the count
  identical: it fails "every displacement verb crosses a depth boundary", because
  a shorter `nb_normals_` desynchronises from `nb_slots_`. What IS unreached is
  the VALUE — substituting a constant for the outside normal while keeping the
  slot changes nothing, because on the plane cage every normal is within a few
  degrees of +Y and `polish_gate` reads an ANGLE. Gated on a TORUS instead, where
  a wrong normal reads as a hard edge and shuts the gate: "a polish stamp reads
  the derived faces' OWN normals across a transition" holds `dropped == 0` at two
  radii, and the constant drops 3 and 1
- THE SUITE IS 2425 cases and 16463921 assertions, green, and green again under
  the ASan+UBSan preset over `*test_multires*.cpp` (103 cases, 18216
  assertions). `ctest` is 8 of 8. NO TIMING WAS TAKEN

### What the record review landed: a SHALL the code does not satisfy, a field
### that changed meaning under an unchanged number, and a gate nothing ran

- THE DELTA PROMISED A FRAME THIS CHANGE DOES NOT BUILD. It carried "A vertex's
  frame SHALL NOT depend on which patches are resident at its level" as a
  MODIFIED requirement, with a scenario saying the same coefficients reconstruct
  to the same position at a boundary. Section 1 is unticked and the code does
  not do it: `full_evaluate` and `partial_evaluate` still build the frame from
  `level_normals`, which sums a level's OWN faces. MEASURED rather than argued,
  on a 6x6 cage with the middle 2x2 refined to level 3, walking the resident
  patches face by face against the dense hierarchy: 124 of 1024 emitted corners
  carry a different frame, worst |Δnormal| 0.170116 at level 2 and 0.154028 at
  level 3, and the DISPLAY normal — the same sum — differs at the same corners
  by the same amounts. With identical `LocalDetail` written into every level-3
  vertex of both, 118 of 1024 shared corners land somewhere else, worst
  0.00570561. Level 1 differs at 0 corners, because nothing is missing there.
  The delta now says that, states the limit and says which three pieces have to
  land together (the complete normal, the wider halo of 2.8, and 3.7's
  coefficient averaging). The "display normals and transported frames agree"
  scenario in the ADDED requirement was the same promise in a second place and
  is corrected to what IS complete: the brush's readers. Measured with a probe
  case built for it and then deleted — it is task 1.1's gate, and 1.1 owns it
- `moved_vertices` CHANGED MEANING UNDER AN UNCHANGED VERSION. See 6.2. Stated
  on the field rather than beside it, and the minor moved to 0.88.0
- A STALE SENTENCE THIS RECORD STILL CARRIED. The export-is-a-read bullet said
  the export evaluates "the levels `mesh_at_level` already evaluates" — the
  exact sentence the residency fix removed from `multires.h` for being false,
  left standing here. Corrected the same way: it evaluates every level up to its
  own and promises no residency. (Reported as a header line; the header was
  already right, and this was the copy that was not.)
- THE AUTOMASK ADAPTER BUILT A SECOND, BLIND TOPOLOGY.
  `compute_automask(const Mesh&, const Adjacency&, ...)` is the overload a
  caller holding a mesh and an adjacency reaches, and it constructed its own
  `MeshWorkItemTopology` with no neighbourhood — so the one entry point that
  does not go through `MeshSculptor::gather` faded every class on the rim of a
  refined region as an open border. It now takes `cross` as a trailing default,
  which leaves the signature every existing caller passes. GATED by "the mesh
  adapter fades no seam when it is given the whole surface": 120 slots faded
  blind (the 64-class rim plus the 56 inside it, at 2 boundary rings), 0 with
  the neighbourhood, and the dense hierarchy's real outer edge faded identically
  either way. PROVED BY REVERT — dropping `cross` from the topology it builds,
  keeping the parameter so it compiles: `CHECK(whole == 0)` fails at 120
- `set_defer_normals` IS NOT FORWARDED TO THE COARSE SCULPTORS, and that is
  deliberate. Each of them exists for ONE stamp and is destroyed with it, so
  there is no stroke to defer into and `flush_normals` cannot reach it. MEASURED
  by forwarding it anyway: the coarse level's normals come back stale (its
  positions are unaffected) and 0 of its chunks are marked `ChunkDirty::Normals`
  where an immediate stamp marks 2 — a host draining the stroke would draw the
  coarse side of the transition with the normals it had before. Said in the
  header at `set_defer_normals` and at `stamp_coarse`, and GATED by "a crossing
  stamp's coarse side does not depend on the host deferring", which asserts the
  coarse normals byte for byte AND the dirty count, because a stale normal
  nothing tells the host about is the failure. PROVED BY REVERT — adding the
  forward: 2 assertions fail, the byte compare and `0 == 2`
- THE TASK-SYMBOLS GATE RAN ON NOTHING. `tools/check_task_symbols.py` shipped
  with a baseline and no CI step — this repository's own "a gate no change
  triggers", added by the change that wrote the taxonomy down. It is now a step
  in the `checks` job beside the other file gates, and self-tested: a tasks.md
  line citing an invented symbol and a missing path exits 1 naming both, and the
  tree exits 0 again with the line removed. It caught this very block twice —
  once for the invented symbol, once for naming the baselined spans in backticks
  — which is the rule working: cite what exists, describe what does not in prose
- AUDITING THE BASELINE FOUND THE GATE, NOT THE DEBT. Two defects, and the first
  hid the second. `tools/task_symbols_baseline.txt` lives under `tools/`, which
  is one of the directories the gate searches — so every name written into it
  resolved BY ITS OWN ROW, which made each row redundant and, worse, let any
  other change cite a name someone else had baselined. The haystack now excludes
  it. With that off, three rows resolved for a second reason: `backends/` was
  not a search directory at all, so `upload_tape`, `MetalBackend::upload_tape`
  and `wait_for` — all in `backends/metal/metal_backend.cpp` — were recorded as
  debt nobody owed. `backends` is now searched and those three rows are gone.
  The 12 that remain are debt in two honest shapes: a name belonging to another
  repository or to a build artefact — the xcframework under dist, the kernel
  header directory inside a built slice, an Xcode group, and the four bridge
  names — and a name a change has promised and not built, the SDF layer
  composition type and the lattice gizmo preview. The composition type resolves
  on main and not on this branch; the baseline file says so, so whoever rebases
  deletes that row rather than rediscovering it
- WHAT WAS NOT CHANGED, and why. `moved_vertices` was not restored to the bound
  level's count: a crossing stamp that moved only the coarse side would then
  report 0, which is the silent success this whole change exists to remove, and
  no call in the C ABI answers the old question anyway. Section 1, 2.8 and 3.7
  are still unticked and none of them was started here — the delta now describes
  the tree instead of describing them
- THE SUITE IS 2427 cases and 16463944 assertions, green — 2425 and 16463921
  before this stage's two cases. `ctest` is 8 of 8, `check_layering.py`,
  `check_binding_parity.py` (735 pyclay capabilities), `check_task_symbols.py`
  and `openspec validate --all --strict` (39 items) all pass, and
  `release_check.py --skip-slow` reports the three version lines agreeing at
  0.88.0. The CI job's NAME was left alone and only a step added: it is what a
  required status check is pinned to and has not moved since the file was
  scaffolded. NO TIMING WAS TAKEN

### Record — running every gate this change touches

- EVERY GATE THIS CHANGE TOUCHES WAS RUN AND IS GREEN. Measured at the commit
  that retires a paid baseline row: the suite is 2430 cases and 16464008
  assertions, 0 failed, and `ctest` is 9 of 9 — 8 before this stage registered
  the task-symbols self-test. `check_layering.py`, `check_kernel_dialect.py`,
  `check_c_abi.py`, `check_test_shards.py` (2430 cases across 4 shards, none
  duplicated, none unrun), `check_task_symbols.py` and `openspec validate --all
  --strict` (39 items) all pass. So does the rest of the `checks` job, which the
  ask did not list but this change edited: `package_kernels.py --verify`,
  `check_licenses.py`, `check_doc_latency.py` (46 figures),
  `check_binding_parity.py` (735 pyclay capabilities) and
  `check_swift_package.py`. THE COUNT IS PINNED TO A COMMIT ON PURPOSE: an
  earlier reading in this same stage was 2427 and 16463944, and it went stale
  while the stage ran. A bare number in a record decays; one that names the tree
  it describes does not. NO TIMING WAS TAKEN
- ASAN + UBSAN ARE CLEAN ON THE SANITIZER PRESET: 8042615 assertions, 0 failed,
  across 1951 cases, the regional and cross-level cases among them. This is the
  gate that matters most to a change whose first stage fixed a read past a
  released cache, because that defect was a SIGSEGV rather than a wrong value.
  One case reports CRASHED and it is not a finding — the run was sent SIGTERM
  and the signal landed in whichever case was executing, `test_sdf_prefix_cache`
- THE ONE GATE FAILURE THAT IS THIS BOX, PROVED RATHER THAN ASSUMED.
  `check_c_abi.py` dies with `GLIBCXX_3.4.31 not found`; it is anaconda's
  python being picked up, and the same command under
  `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6` prints
  `c-abi: OK (hygiene + ctypes FFI)`. The same single cause accounts for four
  `release_check.py` rows — `tests`, `bindings`, `abi` and `wheel`. The `tests`
  row is `pyclay_pytest`, which is registered only in a build with
  `CLAY_BUILD_PYTHON=ON` and so does not appear in the cpu preset at all; under
  the same LD_PRELOAD that one test passes. Nothing here is a defect in the tree
- WHAT THE PORTABILITY GATES THIS BOX CANNOT RUN WOULD SEE. The local build
  already carries `-Wall -Wextra -Wpedantic -Wshadow -Werror`, so the clean
  build is the GCC reading. The two traps that have cost this repository a CI
  round before were checked directly instead of guessed at: there is no
  `CHECK` on a smart pointer (the one pointer assertion is a raw
  `MeshSculptor*` against `nullptr`, which MSVC accepts), and no new C entry
  point at all — `bindings/c/clay.h` gained a comment and a version bump and no
  out-parameter, so there is nothing for AppleClang's distinct `size_t` to
  disagree about. All eight changed translation units were then compiled a
  second time with clang 21 under the project's own flags plus
  `-Wshorten-64-to-32`, the analogue of the MSVC C4267 narrowing warning that
  the GCC flag set does not include: all eight clean
- THE STALE-BASELINE HOLE, FOUND AND CLOSED. `check_task_symbols.py` consumed a
  baseline row and never asked whether the row still recorded anything, and it
  reported the SIZE of the file rather than how much of it was needed. So a row
  went on exempting its name after the debt was paid, and if that name were
  later renamed the gate would stay silent about exactly what it exists to
  catch — the same shape as the defect that put the baseline inside the tree it
  searched. `stale_rows` now fails the gate and names the row to delete when the
  change stops citing the span or the span starts resolving. The baseline file's
  "whoever rebases past that merge should delete its row" was advice to a human;
  it is now enforced, and the file says so
- THE FIXTURE CAUGHT ITSELF, which is the rule working on its author for the
  third time in this change. The self-test first used a real baselined name as
  its fixture symbol — and because this gate searches `tools/`, where its own
  source lives, writing that name into the self-test made a genuine row resolve
  and the gate demanded its deletion. Renamed to `ClaySelfTestOnlyMarker`, with
  the reason written beside the fixture
- GATED BY `clay_task_symbols_selftest`, a ctest, because the stale-row rule
  CANNOT FIRE ON A HEALTHY TREE — it only speaks once a recorded debt is paid,
  which has not happened on this branch, so running the gate in CI proves
  nothing about that half. `--self-test` builds a throw-away git tree where the
  rule must fire and where it must stay quiet, and runs the real gate over it;
  it needs no build artefact. PROVED BY REVERT — replacing the `stale_rows` call
  with an empty list: checks 3 and 4 fail, both reporting
  `task symbols resolve in 1 change(s), 1 baselined` and exit 0 where exit 1 was
  wanted. Checks 1 and 2, which cover the rule that already existed, still pass,
  which is what says the self-test is not merely asserting the whole gate
- WHAT WAS NOT FIXED, AND WHY IT IS NOT THIS CHANGE'S. `release_check.py` also
  fails `device` and `benchmarks`. `device` is the hardware gate and reports the
  engine moved since it last ran, which is what it should say on a feature
  branch. `benchmarks` was run inadvertently — `release_check.py` invokes one,
  and this stage was told to run none — and its result is NOT relied on here:
  the ratio it gates is between two voxel-remesh cases, and this change touches
  no code either of them reaches. Every source file it edits is under
  `src/mesh/` on the multires, cross-level, automask and sculpt path; the remesh
  benchmarks leave `build_multires_levels` at 0, which is the only place the
  remesh path mentions multires at all, and it is a refusal branch

### Record — four minors, three of them in the gates this stage had just written

- THE GATE ADVERTISED QUALIFIED NAMES AND READ NONE OF THE QUALIFICATION.
  `check_task_symbols.py` searched for the trailing identifier alone, so an
  invented class in front of a real member resolved off the member. That is
  load-bearing for the audit recorded above: `MetalBackend::upload_tape` was
  struck from `tools/task_symbols_baseline.txt` on the grounds that adding
  `backends/` made it resolve, and the only evidence gathered was about
  `upload_tape`. A qualified name now needs ONE file to hold every component as a
  word — what a class and its member look like on disk, where a literal search
  for the joined span would find nothing. RE-AUDITED against it: all three
  strikes hold, all twelve remaining rows still fail to resolve, and repo-wide
  every qualified span and every bare filename in all 18 in-flight changes
  resolves, so the stricter matcher records no new debt
- AND A BACKTICKED BARE FILENAME WAS CHECKED BY NEITHER BRANCH — no "/" for the
  path shape, a "." the identifier shape forbids — so nine citations in this
  file alone were skipped in silence, which is the one thing a gate must never
  do. Checked now as a basename anywhere in the tracked tree, recognised by an
  extension this repository uses so that a version number and a struct field are
  not mistaken for files. BOTH PROVED BY REVERT, one per property: restoring the
  trailing-identifier search fails self-test check 5 alone, dropping the filename
  branch fails check 7 alone, and the self-test is 8 checks
- `CrossLevelNeighborhood::bytes()` WAS EXECUTED AND NOTHING ASSERTED ON IT.
  `LevelCache::byte_split` prices the neighbourhood into `runtime_index`, and
  deleting that term left the whole suite green with an identical assertion
  count: the cases that build a non-empty neighbourhood and then read `memory()`
  assert `resident_levels` only, and the two that check byte figures run on
  uniform hierarchies where the neighbourhood is empty. GATED in
  `test_multires_regional.cpp` on a regionally refined cage with a brush bound
  and a stamp taken, then `drop_all_caches` and a read back — which is both what
  a host acting on this report does and what makes the measurement ONE term: a
  crossing stamp writes the coarse side too and builds the neighbourhood of every
  level it writes. The figure compared against is counted from the CSR arrays
  `build_cross_level` filled, at their sizes, so it is a floor the code under
  test did not compute: 8944 bytes against a measured 10704. PROVED BY REVERT —
  the revert compiles and fails this case alone, runtime row flat at 84832
- THE PROPOSAL'S IMPACT DESCRIBED A DIFFERENT CHANGE. It had the ABI growing by
  an export entry point and the version lines moving to 0.87.0 "because two
  branches already claim 0.86.0"; 6.1 is unbuilt, the `clay.h` diff adds no
  function, and the lines are at 0.88.0 for the reason 6.2 records. Its file list
  named three files this branch never touched — `surface_frame.cpp` among them,
  which is section 1 and still owed — and none of the ones the work landed in.
  Rewritten from the diff. Three claims in `design.md` were overruled by what
  landed and are corrected where they stand: a missing face is not "named by the
  level it lives at", quad-only transition templates do not exist, and a mixed
  export owes no preflight
