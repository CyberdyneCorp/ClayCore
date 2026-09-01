# Tasks: add-shared-brush-runtime

- [x] 0.1 SEQUENCING: the residual of `add-shared-brush-kernels` (merged,
      41/41). Consumes `add-dynamic-topology` and `add-mesh-multires`, both of
      which are landed on main. Assigned version **0.75.0 /
      `CLAY_ABI_MINOR 75`**; the four version lines move together
      (`CMakeLists.txt`, `bindings/c/clay.h`, `pyproject.toml`,
      `tools/release_check.py`)
- [x] 0.2 VERIFY the merged change's claims against the tree before planning
      against them. Done — see the table at the top of `design.md`; all seven
      hold

## 1. Decide before writing a line

- [x] 1.1 DECIDE where the neutral runtime lives: `mesh`, or a new leaf module
      below both. **`mesh`** — D1. `tools/check_layering.py` is NOT edited
- [x] 1.2 DECIDE the neutral work-item identity and its width. **64-bit
      `WorkItemId`**, chosen by `VertexId` and (level, vertex) — D2
- [x] 1.3 DECIDE what the arena is and what it refuses. **Bump, trivially
      destructible only, one per sculptor, no destructor tracking** — D3
- [x] 1.4 DECIDE the `BrushFrame` name collision. **The enum keeps its name**
      (it is serialized at preset version 1 and mirrored as
      `clay_brush_frame`); the new basis is `StampFrame` — D4
- [x] 1.5 DECIDE how a zero azimuth is handled. **Branch, do not multiply by
      `cos 0`/`sin 0`** — `-0.0f + 0.0f == +0.0f` moves a golden — D5
- [x] 1.6 DECIDE how the automask reaches three representations. **A neutral
      core over a two-query `WorkItemTopology`, the Mesh/Adjacency overload
      kept as the fixed adapter** — D6
- [x] 1.7 DECIDE what the three workset builders share. **Only
      `compose_workset`; the walks stay where they are** — D7
- [x] 1.8 DECIDE what cross-representation parity can assert. **P1 kernels,
      P2 weight, P3 identical-vertex-set byte equality for normal-free verbs,
      P4 the named differences asserted AS differences** — D8
- [x] 1.9 DECIDE the C ABI's share. **One appended `stamp_azimuth`, three
      arena-stat calls, no tuning knobs, no new automask surface** — D9

## 2. The arena

- [x] 2.1 `include/clay/mesh/brush_arena.h` + `src/mesh/brush_arena.cpp` —
      `BrushScratchArena`: `allocate<T>(count)`, `reset()`, `capacity_bytes()`,
      `high_water_bytes()`, `growths()`. `static_assert` on trivial
      destructibility; alignment honoured per `T`
- [x] 2.2 A `ScratchVector<T>` view over an arena block — size, `push_back` up
      to a reserved capacity, and a hard refusal past it. The automask's
      frontiers and the region sort want a growable thing with no allocator
- [x] 2.3 A member of `MeshSculptor`, `DynamicSculptor` and (forwarded to the
      level sculptor) `MultiresSculptor`. NEVER a process-global
- [x] 2.4 Move `compute_automask`'s five per-stamp vectors onto it —
      `depth`, `frontier`, `next`, `reached`, `stack`
- [x] 2.5 Move `DynamicSculptor`'s per-stamp vectors onto it — the sort
      permutation and its two sorted copies in both region functions, the ball
      query's face list, and `write_positions`' per-moved-vertex incident-face
      vector (which is an allocation PER VERTEX, not per stamp).
      DONE, with one deviation recorded rather than papered over: the sort
      permutation and its two copies are the arena's; the ball query's face list
      became a member (`ball_faces_`), because `DynamicBvh::faces_in_ball` takes
      a `std::vector<FaceId>*` and its size is not knowable before the query;
      and the per-vertex incident-face vector was REMOVED rather than moved — it
      existed only to call `incident_faces`, whose signature is a `std::vector`,
      so `write_positions` walks the borrowed half-edge fan directly and needs
      no buffer of its own. A bump block cannot be sized from a valence the
      caller has not asked for yet, and the requirement — no allocation after
      warm-up — is met either way. Measured 14 allocations per warm adaptive
      stamp before, 0 after
- [x] 2.6 `DynamicSurface::refresh_normals` takes the touched-vertex buffer
      from the caller instead of building one. It is called once per stamp from
      the sculptor and once per remesh
- [x] 2.7 The arena's persistent-buffer boundary is respected: the sculptors'
      `std::vector` members stay members. The arena is for what is transient
      WITHIN one stamp — D3

## 3. The neutral work item and workset

- [x] 3.1 `include/clay/mesh/work_item.h` — `WorkItemId` with
      `weld_class` / `surface_vertex` / `level_vertex` constructors and
      readbacks, and the `WorkItemTopology` interface (`ring_slots`,
      `on_open_border`)
- [x] 3.2 `SculptWorkset::classes` becomes `items`, typed `WorkItemId`.
      `BrushRegion` stays as the alias it already is
- [x] 3.3 `MeshSculptor::write_region()` widens to
      `const std::vector<WorkItemId>&`. Two call sites —
      `src/mesh/multires_sculpt.cpp:179` and one assertion in
      `tests/unit/test_mesh_sculpt.cpp`
- [x] 3.4 `compose_workset(...)` in `sculpt_workset.h` — the five factors in
      the one fixed order, the zero drop, the automask, the second drop, and
      the frame resolution. Names no representation
- [x] 3.5 `build_fixed_mesh_workset` (declared in `sculpt.h`),
      `build_dynamic_surface_workset` (`dynamic_sculpt.h`),
      `build_multires_workset` (`multires_sculpt.h`). Each walks its own
      representation and ends in `compose_workset` — D7
- [x] 3.6 `DynamicSculptor`'s five parallel arrays and its `slot_` become the
      shared `SculptWorkset`. `last_region()` keeps returning `VertexId`s,
      projected out of it

## 4. The automask reaches every representation

- [x] 4.1 Split `compute_automask` into the neutral core over
      `WorkItemTopology` and the `Mesh`/`Adjacency` overload that adapts to it.
      No factor arithmetic changes — D6
- [x] 4.2 `DynamicSurfaceTopology` in `src/mesh/dynamic_sculpt.cpp`:
      `ring_slots` from `one_ring` through the workset's slot map,
      `on_open_border` from `DynamicSurface::is_boundary_edge`
- [x] 4.3 `DynamicSculptor::set_automask_inputs` / `automask_inputs()`, matching
      `MeshSculptor`'s signature exactly. Set per STROKE — they hold
      `std::function`s
- [x] 4.4 `DynamicSculptor::gather` reads `brush.automask` and applies it as
      the LAST factor, dropping a fully masked entry from the workset entirely
      so it is bit-identical to its input rather than merely close
- [x] 4.5 REGRESSION TEST for the divergence this change exists to close: a
      stamp on an adaptive surface with `AutomaskFactor::NormalAngle` set must
      move fewer vertices than the same stamp without it, and the difference
      must be on the side facing away from the brush. It fails on main.
      DONE — `tests/unit/test_dynamic_shared_brush_parity.cpp`, two cases. On a
      cube-sphere under a 1.6 brush the stamp moves 365 unmasked and 149 with
      the factor, and every survivor faces within `cos(2 * 0.5) = 0.5403` of the
      brush where the unmasked stamp reached -0.272. ONE TRAP RECORDED IN THE
      TEST: at the 60-degree DEFAULT angle nothing on that sphere turns far
      enough away to reach zero, so the first version read 365 against 365 for a
      reason that had nothing to do with the defect — the angle is tightened to
      0.5 rad so the gate actually closes. PROVEN by reverting
      `in.topology = &topology` to null in `DynamicSculptor::gather` (which
      reinstates exactly the pre-change behaviour and COMPILES): 5 test cases
      and 129 assertions fail, and pass again on restore
- [x] 4.6 REGRESSION TEST at the C ABI, where the contract is written down:
      `clay_dynamic_sculptor_stamp` with `automask_factors` set must produce a
      different report from the same call with 0, because
      `clay_dynamic_sculptor_stamp`'s own header says the descriptor is "the
      same descriptor the fixed path takes". It fails on main.
      DONE — `tests/unit/test_c_shared_brush_runtime.cpp`, plus the positive
      half: one descriptor through `clay_mesh_sculptor_stamp` and
      `clay_dynamic_sculptor_stamp` gives the same 365 / 149 on both. Also
      covered from Python in `bindings/python/tests/test_shared_brush_runtime.py`,
      which is where the claim about the SHIPPED WHEEL lives
- [x] 4.7 `brush::apply_to_mesh`'s adaptive counterpart wires the cavity and
      group callbacks the same way `src/brush/stroke.cpp:543` does for the
      fixed path, or the change states in the delta why an adaptive stroke does
      not get them yet. THE SECOND BRANCH: there is no `brush::apply_to_dynamic`
      to wire them in — an adaptive stroke is driven by the host calling
      `DynamicSculptor::stamp` directly — and writing one owns spacing, drag
      re-anchoring, the snakehook anchor and the remesh schedule, none of which
      is about the automask. Stated in `specs/brush-engine/spec.md`, with the
      scenario that the host sets them on the sculptor itself, which it now can
      and before this change could not

## 5. The stamp frame

- [x] 5.1 `include/clay/mesh/stamp_frame.h` + `src/mesh/stamp_frame.cpp` —
      `StampFrame`, `make_stamp_frame(origin, normal, azimuth,
      explicit_rotation)`, `stamp_uv(frame, p, extent)`
- [x] 5.2 The zero-azimuth branch, with the `-0.0f` reasoning in the comment —
      D5. A test asserts the unrotated basis is byte-identical to the basis
      built with `azimuth = 0`.
      DONE in `tests/unit/test_stamp_frame.cpp`, as a PAIR, because the obvious
      single assertion is a tautology: one case says a zero azimuth returns
      `kernel::calpha_frame`'s output untouched, and the other says the
      `cos 0` / `sin 0` form is NOT that — on `direction = (-1,-1,0)`,
      `hint = (-1,0,0)` the branch keeps `bitangent.x == -0.0f` where the
      multiplication clears the sign bit. Found by sweeping the exactly
      representable directions and hints: 1876 of 16464 combinations disagree.
      PROVEN by deleting the branch (compiles): 3 cases, 4 assertions fail.
      AND ONE HONEST FINDING RECORDED IN THE FILE: with the branch deleted
      every golden in `mesh_sculpt_goldens_*.inc` still PASSES, so this file is
      the only gate on D5 — no golden fixture happens to pair such a direction
      with an alpha sample near a texel boundary
- [x] 5.3 `alpha_frame_for` reimplemented over `make_stamp_frame`, reaching
      `kernel::calpha_frame` with the same two arguments exactly once.
      `AlphaFrame` and `alpha_at` unchanged
- [x] 5.4 `MeshBrushSettings::stamp_azimuth` (radians, default 0), read by all
      three sculptors when they build the stamp frame
- [x] 5.5 The three "frame" headers each name the other two —
      `brush_model.h`'s enum, `stamp_frame.h`'s basis, `surface_frame.h`'s
      transported detail frame — so a reader who meets one knows there are
      three — D4

## 6. The gates

- [x] 6.1 `tests/unit/test_brush_arena.cpp` — bump order, alignment, `reset`
      keeps capacity, `high_water_bytes` is a maximum and not a current, and
      `growths` stops growing over a stroke of similar stamps.
      DONE, 17 cases. `growths` is asserted as a CONTRAST rather than against a
      magnitude — twelve stamps at the same size take 1 growth, twelve that
      climb to it take 4 — because a bare number would be a test of the doubling
      constant. Two behaviours were measured and written down rather than
      asserted away: `high_water_bytes` OVERSTATES a stamp that overflowed (an
      overflow block leaves its predecessor's tail unusable until the reset,
      which is the conservative direction for a budget), and
      `ScratchVector::overflowed()` is STICKY across `clear()`, because it
      reports a wrong bound rather than a condition. `WorkItemId`'s encoding is
      gated here too — the low half is the dense index in all three identities,
      which is the rule `slot[item.key()]` rests on
- [x] 6.2 `tests/unit/test_sculpt_allocation.cpp` EXTENDED: every verb with
      `Boundary | TopologyConnected | NormalAngle` set, which is the coverage
      hole that let `compute_automask`'s five vectors through. It fails on main.
      DONE, all 13 verbs. ONE TRAP: at radius 0.9 on a patch of half-extent 1.0
      the brush does not REACH the border, the boundary fade spreads from an
      empty frontier and the gate passes on code that never ran — the radius is
      1.2 and a companion case asserts the automask actually removed vertices
      from the same fixture, so the zero is one the automask earned
- [x] 6.3 `tests/unit/test_sculpt_allocation.cpp` EXTENDED to
      `DynamicSculptor` (topology disabled, so the surface is stable) and to
      `MultiresSculptor`. A warm stamp allocates nothing on all three
      representations, which is the requirement's actual wording.
      DONE — three new cases: 9 verbs on the adaptive surface, an automasked
      adaptive stamp, and an automasked multiresolution stamp. PROVEN by
      reverting `apply_boundary`'s four arena blocks to the local
      `std::vector`s they were (compiles): 3 cases, 15 assertions fail
- [x] 6.4 `tests/unit/test_dynamic_shared_brush_parity.cpp` — P1, P2, P3 and
      P4 for the adaptive surface, on a plane grid converted with
      `DynamicSurface::from_mesh` and topology disabled.
      DONE, 13 cases. P1/P2 are asserted in the strongest form available — not a
      synthetic snapshot fed to both, but the two representations' OWN worksets
      after the same stamp, compared entry for entry: items, positions, normals,
      weights, the resolved frame and the write region all agree BIT FOR BIT on
      the plane grid, and every neighbour-free kernel run over both writes
      identical displacements. P4 needs CURVATURE — on the plane the two normal
      estimators produce byte-identical normals, so Draw agrees there and the
      row would be vacuous; it is asserted on a cube-sphere, where Draw,
      Inflate and Clay differ at 9, 7 and 9 of 21 moved vertices by at most
      1.2e-9, 6.0e-8 and 1.4e-9. Nudge is separated from Grab deliberately:
      `kernel_nudge` projects its direction against the vertex normal, so it is
      a P3 verb on the plane and a P4 verb on the sphere, and the file says
      which is which. Also gated: two sculptors stamping CONCURRENTLY on two
      threads produce the single-threaded answer, which is the per-sculptor
      arena claim in the form only TSan can check
- [x] 6.5 `tests/unit/test_multires_shared_brush_parity.cpp` — the same, at
      level 0 where an identical vertex set exists; P1 and P2 above it.
      DONE, 9 cases, INCLUDING a three-way P3: one source model becomes a fixed
      mesh, an adaptive surface and a cage, and a Grab writes byte-identical
      positions on all three. TWO CLAIMS THIS FILE DELIBERATELY DOES NOT MAKE,
      both measured first. Above the cage the level-2 reconstruction is
      BIT-EXACT to the fixed sculptor for Grab, Clay, Draw, Inflate, Nudge and
      Crease on a planar cage — the transported frame is axis-aligned there and
      the encode/decode pair is exact — so P4 is stated as the difference that
      IS real (a fine stamp writes a coefficient and leaves the cage untouched
      where the fixed sculptor writes a position) rather than as a tolerance
      nothing measured. And the boundary automask leaves 49 of 81 cage vertices
      moving at EVERY `boundary_rings` setting, because the fade ramps and only
      the border ring reaches a hard zero — the ring count is visible in the
      WEIGHTS, which a separate case reads, and asserting 25 here was the first
      version measuring the wrong quantity
- [x] 6.6 `tests/unit/test_stamp_frame.cpp` — the basis is orthonormal and
      right-handed, `stamp_uv` inverts it, the azimuth rotates in-plane, and
      an explicit rotation replaces the azimuth rather than composing.
      DONE, 10 cases, including the D5 pair described at 5.2 and the claim that
      `alpha_frame_for` IS `make_stamp_frame` plus an extent — byte-compared,
      because a re-derived basis there would drift at the last bit and nothing
      else in the suite would notice. The rotation's SENSE is asserted as well
      as its magnitude: `cos` is even, so an angle check alone passes on a
      rotation the wrong way round
- [x] 6.7 THE ACCEPTANCE GATE, run after every commit in this branch:
      `mesh_sculpt_goldens_linux_x64.inc`, `..._macos_arm64.inc` and
      `..._msvc_x64.inc` are UNCHANGED in the diff. A moved golden is a failed
      refactor and never a new baseline.
      HELD. `git diff a44b1f5 -- tests/unit/mesh_sculpt_goldens_*.inc` is empty
      and was re-checked after every build this stage. See 5.2 for what that
      does NOT prove: the goldens also stay green with D5's branch deleted, so
      they are not the gate on it
- [x] 6.8 A benchmark case for the neutral automask's virtual — a
      boundary-automasked stamp, before and after, on the same fixture. The
      measured number goes in the commit message; a ceiling goes in
      `tools/check_bench.py` only if it can be set from the runner rather than
      from a development machine.
      DONE — four cases in `benchmarks/bench_main.cpp`, the fixed and adaptive
      stamps with the factor off and on, and the same four built against
      a44b1f5 to compare. Nine repetitions of 200 iterations, P50 in us, load
      average 7-12 before and after both runs. THE VIRTUAL IS NOT WHAT IT
      LOOKED LIKE: on the fixed path — the only one where the automask ran on
      main at all — the neutral rewrite is 2.0x and 1.6x FASTER than the direct
      `Mesh`/`Adjacency` implementation (350.28 -> 170.39 at n=224,
      2527.86 -> 1615.53 at n=707), because five per-stamp `std::vector`s
      became arena blocks and an indirect call per entry is cheaper than a
      malloc per stamp. THE ADAPTIVE ROWS ARE THE DEFECT, NOT A REGRESSION:
      main's automasked adaptive stamp costs the same as its unmasked one
      (144.65 against 153.23) because the factor never ran, so the 3.80x and
      8.08x here are what an automask costs on a representation that has
      started honouring it. NO CEILING was added to `check_bench.py`, on that
      file's own rule that a ceiling must be read off the runner. ONE THING
      FOUND AND VERIFIED AGAINST MAIN RATHER THAN ASSUMED: the plain fixed
      stamp is proportional to the MODEL at an identical 114-entry workset —
      10x the surface costs 7.96x on main and 8.22x here — so it is older than
      this change and is not this change's to fix, but it is recorded in the
      benchmark's comment where it will show up next

## 7. The ABI and the bindings

- [x] 7.1 `bindings/c/clay.h`: `CLAY_ABI_MINOR 77` (written as 75, moved by the
      merge at 11.7); `stamp_azimuth` appended to
      `clay_mesh_brush_desc` under the `struct_size` rule;
      `clay_brush_arena_stats` and the three `*_arena_stats` calls — D9.
      The stats descriptor's byte counts are `uint64_t` rather than `size_t`:
      it is a versioned descriptor with a fixed layout, and `size_t` would make
      that layout depend on the host's word size
- [x] 7.2 `bindings/c/clay_c.cpp`: `read_mesh_brush` carries the azimuth,
      `clay_mesh_brush_defaults` and `to_c_brush` report it, the three stat
      calls are implemented over one `write_arena_stats` filler.
      TWO THINGS WORTH RECORDING. The azimuth is passed STRAIGHT THROUGH,
      including zero — every other appended scalar here reads zero as "an older
      host declared the shorter layout, give it the engine default", and this
      one must not, because zero is the value that means unrotated and the
      engine branches on exactly it (D5). And the three queries do NOT go
      through `resolve_sculptor`, on the same footing as
      `clay_mesh_sculptor_has_colors`: what an arena owns is a fact about the
      SCULPTOR rather than about the mesh it is bound to, so a sculptor whose
      layer was rebuilt under it still spent the memory and a host winding down
      still has to account for it
- [x] 7.3 `bindings/python/pyclay_module.cpp`. WIDER THAN THE TASK ASKED, and
      deliberately: the brush-engine delta says the estimators SHALL be settable
      on EVERY sculptor that offers the automask with the same signature, and
      `MeshSculptor.stamp` turned out to have no `automask` argument either —
      so the factor was unreachable from Python on the fixed path too, not only
      the adaptive one. All three `stamp` calls (and both `apply_stroke` forms)
      now take `automask` and `stamp_azimuth`, threaded through the one
      `mesh_brush_settings` helper so they cannot drift; `set_automask_inputs`
      and `arena_stats` are on all three sculptors.
      `set_automask_inputs` TAKES OBJECTS, NOT CALLABLES — `cavity` is a
      MaskField (`document.mask_from_surface('cavity', ...)`, which bakes the
      same estimator the engine measures with) and `groups` is the document's
      own lattice. A Python callable is not an option rather than a nicety: a
      stamp releases the GIL and evaluates these per workset entry from a
      worker thread, where calling back into the interpreter crashes
- [x] 7.4 `python3 tools/check_binding_parity.py` — OK, 590 pyclay
      capabilities, 32 exempt. `arena_stats` maps to
      `clay_*_sculptor_arena_stats` under the existing `CLASS_PREFIX` rule with
      no table edit, as predicted. Two entries were added:
      `MeshBrushSettings.stamp_azimuth` aliases `clay_mesh_brush_defaults`
      exactly as its twelve siblings do, and the three `set_automask_inputs`
      are EXEMPT with the reason D9 already gives — the two estimators are
      `std::function`s, which is what the C ABI cannot carry, while the three
      input-free factors do cross on `clay_mesh_brush_desc`
- [ ] 7.6 `tests/swift/smoke.swift` — the appended field and the arena
      statistics through the SwiftPM surface, which is where a descriptor's
      layout is checked by a compiler that is not the one that built the
      library. ADDED IN RECONCILIATION: the work was done and no task named it.
      Three claims: `clay_mesh_brush_defaults` reports `stamp_azimuth == 0`,
      a turned stamp is the SAME descriptor rather than a second entry point,
      and `clay_dynamic_sculptor_arena_stats` /
      `clay_multires_sculptor_arena_stats` read back a high water no larger
      than the capacity.
      NOT RUN ON THIS BOX — there is no Swift toolchain here (`which swift`
      and `which swiftc` both fail), so the file is written and compiled by
      CI's macOS job and by nothing I ran. That is the one gate in this change
      whose result I am taking on trust.
      LEFT OPEN IN RECONCILIATION — needs macOS. The file is written and
      committed; the gate this task exists for is "a descriptor's layout
      checked by a compiler that is not the one that built the library", and
      no such compiler ran here. `which swift` and `which swiftc` both fail on
      this box and `tools/check_swift_package.py` is textual only —
      `check_swift_smoke.sh` prints "skipped (no swiftc on PATH)" and exits 0,
      which is a skip and not a pass.
      WHERE IT ACTUALLY CLOSES, read out of `.github/workflows/ci.yml` rather
      than assumed: the macOS job on THIS PR runs
      `./tools/check_swift_smoke.sh typecheck`, which type-checks the file
      against `clay.h` alone — so the appended field and the three
      `*_arena_stats` signatures are checked by a Swift compiler on every push.
      What stays release-time is `check_swift_smoke.sh all`: building against
      the real xcframework slices and RUNNING the binary, macOS and inside a
      booted iOS Simulator. So the three claims the file asserts at runtime —
      `stamp_azimuth == 0` from the defaults, a turned stamp being the same
      descriptor, and a high water no larger than the capacity — are gated on
      the tag's workflow and by nothing on the PR

- [x] 7.5 The version lines, in lockstep, at 0.77.0 / 77 (written at 0.75.0 / 75
      and moved by the merge — 11.7 has the why). THREE literals, not
      four: `CMakeLists.txt`, `bindings/c/clay.h` and `pyproject.toml` carry the
      number, and `tools/release_check.py` DERIVES it from all three and checks
      they agree (`check_versions`) rather than carrying a row of its own. The
      task said four because the other stacked changes' notes do; the file has
      no literal to edit
- [x] 7.7 GATE THE PARITY EXEMPTION. `set_automask_inputs` is the only call
      this change adds that pyclay has and the C ABI does not, and 7.4 records
      all three of them as EXEMPT — an exemption being a promise that the Python
      side does something real. Nothing kept that promise: the suite asserted
      that the method existed and that it refused a callable, so a binding that
      accepted a MaskField and a GroupField and dropped both on the floor passed
      every case in the file.
      DONE — four cases in `bindings/python/tests/test_shared_brush_runtime.py`.
      The surface-group estimator is the one that says the wiring is real rather
      than that the argument parsed: the answer lives on the document's world
      lattice, which a mesh module structurally cannot compute, and all three
      representations read 289 open and 153 isolated to group 7 — the SAME two
      numbers, which is the change's headline claim measured from the wheel.
      Asserted as a PARTITION and not as "fewer moved": isolating group 7 and
      isolating the ungrouped remainder give 153 and 136, and 153 + 136 = 289, a
      claim a wiring returning a constant would fail while a bare inequality
      would let through. The cavity estimator is gated on the two overlapping
      spheres of `examples/64_measuring_the_surface.py` — with the mask's
      `painted_count` asserted first, since the whole case is otherwise
      satisfied by a mask that painted nothing — and as a SUBSET rather than a
      count, because a subset is what a multiplicative mask MEANS and a count is
      a marching-cubes vertex count. Clearing the inputs is gated too: an
      estimator that outlived its stroke would mask the next one against a
      lattice the artist has moved on from.
      PROVEN NOT VACUOUS: with the two lambdas in `automask_inputs_of` deleted
      (it compiles), all four fail and the fourteen cases that were already
      there still pass — which is the coverage hole, exactly measured.
      ONE CLAIM STATED RATHER THAN PROVEN, in the test's own docstring: the
      `nb::keep_alive` case drops the caller's handle and stamps again, and
      without the keep_alive that reads freed memory — which a sanitizer catches
      and an ordinary pytest run may not. It gates the SHAPE, and says so

## 8. The demonstration

- [x] 8.1 `examples/70_shared_brush_runtime.py` — ONE
      `brush.reference_preset(...)` gesture, resolved once, replayed over a
      fixed mesh, an adaptive surface and a multires hierarchy built from the
      same source model.
      DONE. The 'Move' preset's stroke resolves to 41 stamps of 'grab' at
      spacing 0.050, replayed identically on all three. The fixture is a
      quad-gridded DOME rather than a cube-sphere: it has no duplicated
      vertices, so the three numberings coincide and a byte comparison is a byte
      comparison — and it is curved, so the P4 row below has something to
      report. `DynamicSurface.to_mesh()` emits in POOL order, so the example
      builds the pairing once from the untouched surface and says why
- [x] 8.2 It ASSERTS what must match: the normal-free verb writes identical
      positions on all three (P3); the topological automask reaches the same
      vertex set; the arena's `growths` stops climbing over the stroke.
      MEASURED FROM PYTHON DURING 7.3, so the example does not have to
      rediscover it: on a 20-per-face cube-sphere at radius 0.9, Draw at
      (0,0,0.9) with radius 1.2, the fixed and adaptive sculptors move 1081
      vertices unmasked and 849 with
      `NormalAngle|TopologyConnected|Boundary` — the same two numbers on both,
      which is the P3 automask row. AND A TRAP FOR THE ARENA CLAIM: the FIXED
      sculptor's arena reads all zeroes for a stamp whose automask needs no
      flood. NormalAngle reads the workset's own normals and TopologyConnected
      returns early on a region that is already one component, so only
      Boundary's frontiers reach the arena there; a `growths` assertion on the
      fixed path with those two factors alone is trivially true and proves
      nothing. The adaptive sculptor's arena is exercised by every stamp
      (25960 B capacity unmasked, 103840 B with the three factors), and the
      hierarchy's reads zero until a level is bound and 105648 B after — which
      is the documented behaviour, not a gap.
      DONE, and THE TRAP WAS TAKEN SERIOUSLY: the example's arena section drives
      `Boundary` on every representation precisely so the fixed and hierarchy
      columns are not asserting a trivially-true zero, and it refuses to run if
      the adaptive arena spent nothing. Its own measured numbers, on the dome:
      the gesture moves 935 vertices on all three and writes byte-identical
      positions; the automask leaves 613 open and 529 masked, the same pair on
      each; over 48 dabs the three arenas settle at 3, 4 and 3 growths and take
      nothing more
- [x] 8.3 It ASSERTS what legitimately differs, and by how much: Draw diverges
      between fixed and adaptive because the normal estimators differ (D8/P4),
      and the example says so in prose rather than hiding it.
      DONE — measured at 2.660e-10 against a displacement of 2.000e-01, and
      asserted in BOTH directions: it raises if Draw ever becomes byte-identical
      between the two (an estimator silently unified) and if the divergence
      exceeds a thousandth of the displacement (one representation drifting).
      The hierarchy's cage is asserted byte-identical to the fixed mesh, because
      at level 0 it IS the fixed sculptor rather than a second implementation
- [x] 8.4 `raise SystemExit` when a claim stops holding, which is this
      repository's example convention.
      DONE — eleven `raise SystemExit` sites, every one of them on a claim the
      prose makes
- [x] 8.5 Registered: `EXAMPLES` in `examples/run_all.py`, the render committed
      under `examples/output/`, and a line in
      `docs/07-brushes-and-features.md`. No new `CAPABILITY_EXAMPLES` key —
      this change adds requirements to `meshing` and `brush-engine`, which
      already have examples.
      DONE. The docs line is a new subsection, "The runtime the three mesh
      representations share", under §8a's automasking — the neutral workset, the
      automask reaching all three, the arena, the stamp azimuth, and the three
      things in `mesh` called a frame
- [x] 8.6 `python3 tools/check_gallery.py` — OK, 251 tracked outputs (three
      new: the contact sheet, the exported .obj and its .mtl)

## 9. Ship

- [x] 9.1 `cmake --build --preset cpu-only -j 8` clean;
      `ctest --preset cpu-only --output-on-failure` green.
      Build clean with `CLAY_WERROR=ON`, zero warnings from any file this change
      touches. ctest 4/4; `clay_unit_tests` is 2084 cases and 14,857,096
      assertions, up from 2022 cases — 62 new.
      CITATION CORRECTED IN RECONCILIATION: this line and 9.2 originally read
      "at af531ff", which is an object that exists in the repository and is
      reachable from no branch — an amended commit. The work it names is
      e85b2d8 ("test(mesh): gate the shared brush runtime on all three
      representations"), and the figures were superseded by 10.1's re-run at
      the branch tip either way
- [x] 9.2 `asan-ubsan` green; `setarch -R ctest --preset tsan` green — the
      arena is per-sculptor and the claim that two sculptors never alias it is
      a threading claim.
      ASAN-UBSAN: the full `ctest --preset asan-ubsan` is 4/4 green,
      `clay_unit_tests` in 3305.73 s, zero sanitizer reports. The new cases were
      re-run on their own against a build at e85b2d8 (see 9.1 on the corrected
      citation) with `ASAN_OPTIONS=detect_leaks=1` — 69 cases, 5370 assertions,
      clean.
      TSAN: `setarch -R` over the new cases plus every pre-existing `C ABI*`
      case — 207 cases, 108,074 assertions, zero race reports.
      AND THE THREADING CLAIM IS NOW A THREADING TEST rather than a sequential
      one: `two sculptors stamp concurrently without aliasing` runs two
      `DynamicSculptor`s on two `std::thread`s and asserts both produce the
      single-threaded answer byte for byte. Sequentially that assertion is weak
      — a racing bump allocator corrupts scratch rather than reliably changing
      an answer — so the test says in its own comment that TSan is the gate and
      it is only what gives TSan something to watch
- [x] 9.3 `check_layering.py`, `check_binding_parity.py`, `check_c_abi.py`,
      `check_gallery.py`, `release_check.py`. The four known-failing
      `release_check` rows in this environment (`bindings`, `abi`, `tests`
      from the anaconda GLIBCXX_3.4.31 mismatch, `device` from the hardware
      gate) are pre-existing; anything else is verified against main before it
      is called that.
      ALL GREEN. `check_layering.py` OK. `check_binding_parity.py` OK (590
      pyclay capabilities, 32 exempt) — re-run after the binding fix, no table
      edit needed because the fix changed error handling and added no symbol.
      `check_c_abi.py` OK against the built `libclay_shared.so`, run under
      `/usr/bin/python3` to sidestep the anaconda loader.
      `check_gallery.py` OK, 251 tracked outputs.
      `release_check.py` reports FIVE failures, not four, and the fifth is the
      same cause: `wheel` fails because the venv is built from `sys.executable`
      (anaconda), so the wheel builds and installs and only `import pyclay`
      inside it hits GLIBCXX_3.4.31. `tests` was traced to its root rather than
      assumed — the only failing ctest entry in `build/release` is
      `pyclay_pytest`, failing at IMPORT, and it PASSES under
      `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6` (53.65 s). Under that
      preload the whole pyclay suite is 607 passed, 1 skipped
- [x] 9.4 Every new `src/**.cpp` in `CMakeLists.txt`, every new
      `tests/unit/test_*.cpp` in `tests/CMakeLists.txt`
- [x] 9.5 `openspec validate add-shared-brush-runtime --strict`

## 10. Proving the gates, and the hole they left

- [x] 10.1 RE-RUN, not re-read. Everything section 9 claims was measured again
      from this worktree rather than trusted. `cmake --build build/cpu-only -j 8`
      clean with zero warnings; `ctest` 4/4; `clay_unit_tests` 2089 cases and
      14,860,258 assertions, up from the 2084 section 9.1 recorded — the five
      new ones are 10.2 and 10.3. `check_layering.py`, `check_binding_parity.py`
      (590 pyclay capabilities, 32 exempt), `check_c_abi.py`, `check_gallery.py`
      (251 tracked outputs) all OK. `release_check.py` fails the same five rows
      and no others — `tests` (only `pyclay_pytest`, at import),
      `bindings`, `abi` and `wheel` from the anaconda GLIBCXX_3.4.31 mismatch,
      and `device` from the hardware gate. Under
      `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6` the pyclay suite is 620
      passed, 1 skipped. The acceptance gate holds:
      `git diff a44b1f5 -- 'tests/unit/mesh_sculpt_goldens_*.inc'` is empty
- [x] 10.2 `tests/unit/test_shared_brush_determinism.cpp` — THE DEFECT CLASS
      THE ARENA REFACTOR COULD HAVE INTRODUCED EVERYWHERE AT ONCE, and which
      nothing in the suite was watching for. A `std::vector<int> depth(n, -1)`
      is value-initialized and an arena block is not: `reset()` is a pointer
      store, so a stamp is handed whatever the previous stamp left behind. Every
      other fixture in the tree runs on a COLD arena — build a sculptor, stamp
      once or twice at one size, read the answer — so a path that read scratch
      before writing it would be correct in the whole suite and wrong for an
      artist whose stroke changed size.
      Three cases. Two of them stamp the same stroke on identical geometry with
      a cold arena and with one grown and dirtied by a much larger stamp first
      (fixed, through `VertexDeltas::revert`; adaptive, through a snapshot of
      the slots) and compare positions, normals, moved counts and write regions
      BYTE FOR BYTE. Each asserts the warmed arena is genuinely larger before it
      compares anything, so a refactor that stopped the arena growing fails here
      rather than quietly turning the two runs into one run twice
- [x] 10.3 THE UNGATED CLAIM 10.2's third case closes, measured rather than
      assumed. Task 4.4 says a fully masked entry leaves the workset ENTIRELY
      "so it is bit-identical to its input rather than merely close". Replacing
      that drop with a keep-and-multiply-by-zero COMPILES, and against the whole
      2089-case suite exactly ONE case noticed — `multires parity: the boundary
      ring count is visible in the weights`, which noticed incidentally, by
      reading a weight it was checking for another reason.
      The first version of the new case did not notice either, and the reason is
      worth keeping: a kept entry at weight 0.0f produces a displacement of
      exactly zero, so it never enters the WRITE REGION and never reaches the
      undo record — the two places the obvious test looks. What a kept entry
      does do is sit in `items` with a zero weight and hold a live slot in the
      map its neighbours are looked up through, so that is where the assertion
      went. With it, the revert fails 233 assertions
- [x] 10.4 A BUG FOUND AND FIXED: `BrushPreset` did not carry
      `settings.stamp_azimuth`. Task 5.4 added the field to `MeshBrushSettings`
      and the preset schema — which serializes the identity subset of exactly
      that struct, field by field — was never revisited, so a saved brush came
      back unturned and looked like it had worked. It is identity and not
      placement, which is the whole reason it belongs there and `direction` does
      not: `direction` says where the finger went, the azimuth says how the
      brush's own pattern is turned, and a rotated chisel is not something the
      stroke engine derives — nothing in `brush` resolves an azimuth from the
      direction of travel yet, so a preset library is the ONLY place an artist
      can put one.
      FIXED at `kBrushPresetVersion = 2`, appended, with the read gated on the
      version so a version-1 record still loads and takes the default. Nothing
      else moves: the C ABI's `clay_brush_preset_version`, pyclay's
      `BrushPreset.version` and both "a newer version is refused" tests all
      DERIVE the number
- [x] 10.5 THE REGRESSION TESTS FOR 10.4, and why the existing round-trip case
      could not have caught it: it walks `reference_presets()`, and every
      preset in the library has an azimuth of zero, so a default round-trips to
      the default whether the schema knows the field or not. Three C++ cases in
      `tests/unit/test_brush_preset.cpp` — a NON-default azimuth survives, the
      default comes back as an exact `+0.0f` (which is the value
      `make_stamp_frame` branches on, D5), and a version-1 record loads at the
      default while a version-1 record that is also truncated is still refused.
      Three pyclay cases in `bindings/python/tests/test_shared_brush_runtime.py`,
      because that is the surface a host saves a library through and NO Python
      test had touched `BrushPreset` at all — the whole format was reachable
      from the wheel and ungated in it
- [x] 10.6 THE REVERT PROOFS, one per property, each confirmed to COMPILE.
      (1) `depth.assign_all(-1)` deleted from `apply_boundary`: 7 cases fail
      including 10.2's fixed-mesh one — recorded honestly, since this is the one
      probe where the new file is a gate rather than THE gate.
      (2) the automask's zero-drop replaced by a keep: 1 case, 233 assertions —
      see 10.3.
      (3) the preset fix undone in full (version back to 1, the `put_f32` and
      the gated read removed): 2 cases fail, both new.
      (4) `in.topology = &topology` set to null in `DynamicSculptor::gather`,
      pyclay rebuilt against it, and `examples/70_shared_brush_runtime.py` run:
      it raises `SystemExit` reading `(529, 613, 529)` where it needs
      `(529, 529, 529)`. That is the change's headline claim failing through the
      shipped wheel, which is the only place the example could have caught it
- [x] 10.7 SANITIZERS, over the new cases and the change's own.
      ASAN-UBSAN with `ASAN_OPTIONS=detect_leaks=1`: 59 cases, 10,015
      assertions, zero sanitizer reports.
      TSAN under `setarch -R`: 204 cases, 112,934 assertions, zero race
      reports — including `adaptive parity: two sculptors stamp concurrently
      without aliasing`, which is the per-sculptor arena claim in the only form
      TSan can check. Neither is the full suite; both are the new cases plus
      every `C ABI`, arena, stamp-frame and parity case, and that is what is
      claimed
- [x] 10.8 THE BENCHMARK, RE-MEASURED AS A DISTRIBUTION rather than as the
      single P50 6.8 recorded. Forty repetitions of 200 iterations,
      `--benchmark_report_aggregates_only=false`, percentiles taken over the
      forty repetition means (so they are percentiles of means, not of
      individual stamps — the case fixes `Iterations(200)`). Load average 2.06
      before and 1.60 after, both read on the box; the RATIOS are the reading.
      All times in microseconds:

      | case | P50 | P95 | P99 | max |
      |---|---|---|---|---|
      | fixed, no automask, n=224 | 66.89 | 76.85 | 83.18 | 86.99 |
      | fixed, boundary automask, n=224 | 161.36 | 169.28 | 172.43 | 173.25 |
      | fixed, no automask, n=707 | 552.17 | 602.63 | 659.90 | 666.04 |
      | fixed, boundary automask, n=707 | 1456.72 | 1523.41 | 1551.08 | 1557.71 |
      | adaptive, no automask, n=224 | 131.06 | 137.41 | 151.48 | 159.14 |
      | adaptive, boundary automask, n=224 | 350.57 | 360.87 | 361.98 | 362.59 |
      | adaptive, no automask, n=707 | 127.59 | 131.72 | 134.96 | 136.25 |
      | adaptive, boundary automask, n=707 | 1021.87 | 1105.52 | 1723.21 | 1789.59 |

      The automask's P50 cost is 2.41x and 2.64x on the fixed path and 2.67x and
      8.01x on the adaptive one, which agrees with 6.8's reading of what those
      two columns mean. TWO THINGS ONLY THE TAIL SHOWS. Seven of the eight rows
      have a P99 within 15% of their P50; the adaptive automasked stamp at n=707
      is 1.69x, and its max is 1.75x — so the one case whose cost is dominated
      by a breadth-first walk over an adaptive surface is also the one with a
      tail, which a mean would have hidden entirely. And the arena counters the
      cases carry confirm 8.2's trap rather than restating it: the plain FIXED
      stamp reports `arena_growths = 0` and `arena_high_water = 0` — its
      automask-free path never touches the arena at all — where every other row
      reports 1 growth and 1856 bytes
- [x] 10.9 THE EXAMPLE, RUN. `examples/70_shared_brush_runtime.py` exits 0 and
      reproduces its committed render and .obj BYTE-IDENTICALLY (`git status`
      over `examples/` is empty after the run), which is a determinism claim the
      gallery gate cannot make on its own. Its numbers are the ones 8.2 and 8.3
      record: 935 moved on all three with byte-identical positions, 613 open
      against 529 automasked on each, arenas settling at 3, 4 and 3 growths, and
      Draw diverging by 2.660e-10 against a 2.000e-01 displacement

## 11. Documentation, reconciliation and the PR

- [x] 11.1 THE DOCS THIS CHANGE MAKES STALE, each edited where the untrue
      sentence actually is rather than by appending a section somewhere.
      `docs/07-brushes-and-features.md`: a new subsection, "The runtime the
      three mesh representations share" — the neutral workset, the automask
      that used to reach two representations out of three, the arena and why
      `growths` is the counter to watch, the stamp's grain, and the three
      things in `mesh` that are called a frame. Preset version 2 recorded
      there and in the brush-engine delta spec.
      `docs/05-claycore-library.md`: why a sculptor's arena is ABSENT from
      `clay_document_memory` and is not an omission — a sculptor is a handle
      held beside a document, on `MultiresSurface::memory()`'s footing — with
      the three `*_arena_stats` calls and the "may you release it?" answer the
      table around it asks of every row.
      `docs/09-brush-latency-and-coverage.md`: the main-vs-branch P50 table,
      the 40-repetition distribution beside it, and TWO new rows in the
      missing-device-coverage list — the mesh automask on any representation,
      and the adaptive and multiresolution stamps.
      `README.md`: the Sculpt row, and the mesh-representation bullet.
      `openspec/ROADMAP.md`: row 1b for this change, the automasking row closed,
      and the stroke-stabilization row closed IN TWO HALVES — its "one line
      wide" gap (`apply_to_mesh` never reads `Stamp::rotation`) was already
      closed by `add-shared-brush-kernels` and the row had not been updated,
      which was verified against main rather than assumed before the row moved
- [x] 11.2 THE PHANTOM COMMIT, corrected rather than left standing. 9.1 and 9.2
      cited results "at af531ff", an object that exists in this repository and
      is reachable from no branch — an amended commit from an earlier stage.
      Both citations now name e85b2d8, which is the reachable commit that did
      that work, and say why they were changed. The figures themselves were
      superseded by 10.1's re-run at the branch tip
- [x] 11.3 THE `Files` TABLES RECONCILED WITH THE DIFF. Nine paths the change
      touches had no row: `test_c_shared_brush_runtime.cpp`, the pyclay suite,
      the three committed example outputs, `dynamic_bvh` and `slot_pool`,
      `test_sculpt_allocation.cpp` (which is THE GATE and was missing from the
      table listing it), `test_mesh_sculpt.cpp` / `test_dynamic_sculpt.cpp`,
      `bench_main.cpp`, `check_binding_parity.py`, `smoke.swift` and the five
      documentation files. The version row was wrong in the other direction and
      now agrees with 7.5: THREE literals, not four
- [ ] 11.4 `tests/swift/smoke.swift` COMPILED — see 7.6, which this stage
      UNTICKED after walking it.
      needs macOS. There is no Swift toolchain on this box and
      `tools/check_swift_package.py` is textual only. CI's macOS job is the
      only thing that can close it
- [x] 11.5 THE FINAL GATES, re-run from this worktree at the docs tip and
      recorded as output rather than as a tick.
      `cmake --build --preset cpu-only -j 8`: exit 0, zero warnings.
      `ctest --preset cpu-only`: 4/4, 149.54 s. `clay_unit_tests` on its own:
      2089 cases, 14,860,258 assertions, 0 failed.
      `check_layering.py` OK. `check_binding_parity.py` OK (590 pyclay
      capabilities, 32 exempt). `check_c_abi.py` OK (hygiene + ctypes FFI).
      `check_gallery.py` OK (251 tracked outputs).
      THE ACCEPTANCE GATE: `git diff a44b1f5 -- 'tests/unit/mesh_sculpt_goldens_*.inc'`
      empty; `tools/check_layering.py` byte-unchanged from main, which is D1's
      visible form.
      `release_check.py`, run under `/usr/bin/python3` rather than the
      anaconda python on PATH, fails THREE rows and no others — not the five
      the previous stage recorded, and the difference is the INTERPRETER
      rather than the tree: release_check's own subprocesses inherit the
      python it was started with, so `bindings` and `abi` PASS here and fail
      under anaconda. `version` passes: cmake=0.75.0 abi=0.75.0 wheel=0.75.0.
      Each of the three traced rather than assumed:
      `tests` — the only failing ctest entry in `build/release` is
      `pyclay_pytest`, and it fails at IMPORT on GLIBCXX_3.4.31. Under
      `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6` that suite is
      620 passed, 1 skipped in 57.31 s.
      `device` — the gate's snapshot is 39c244209, which IS AN ANCESTOR OF
      MAIN, and main already differs from it in `CMakeLists.txt`,
      `bindings/c/clay.h` and 45 more engine files. The row therefore fails on
      main as well; it re-runs on the reference iPad and nowhere else.
      `wheel` — NOT the GLIBCXX mismatch the previous stage attributed it to,
      which was worth checking because that explanation stopped applying the
      moment the venv came from `/usr/bin/python3`. Reproduced on its own
      outside release_check: `python3 -m venv` cannot bootstrap pip on this
      box because `ensurepip` is missing (`python3.12-venv` is not
      installed), so the gate never reaches a wheel to build.
      `benchmarks` PASSES — `check_bench.py` OK against a fresh
      `clay_bench --benchmark_format=json` run.
      AND THE EXAMPLE, RE-RUN AT THE DOCS TIP:
      `examples/70_shared_brush_runtime.py` exits 0 and leaves
      `git status examples/` empty, so it reproduces its committed PNG and
      `.obj` byte for byte
- [x] 11.6 THE PR, opened against `main` — #419.
      THE STACK IS NOT IN THE ORDER IT WAS PLANNED IN, measured after opening
      it rather than assumed from the plan. The branch was cut from a44b1f5 as
      the BOTTOM of three — 0.75.0 here, 0.76.0 on `feat/mesh-sculpt-layers`,
      0.77.0 on `feat/extreme-poly-runtime` — and `feat/mesh-sculpt-layers`
      merged FIRST, as #417. `main` is 1c230df4 at 0.76.0, so this branch's
      number is BEHIND main rather than ahead of it and GitHub reports the PR
      CONFLICTING. No CI has run and none will until that is resolved
- [x] 11.7 THE REBASE ONTO `main`, WHICH LANDED AS A MERGE — 9bb7181, onto
      `origin/main` at 428f7362. The number this change ships under is
      **0.77.0 / `CLAY_ABI_MINOR 77`**, not the 0.75.0 it was assigned: both
      branches below it merged first, so 0.75.0 and 0.76.0 were spent and the
      next free literal was 0.77.0. The three literals agree
      (`CMakeLists.txt`, `bindings/c/clay.h`, `pyproject.toml`), and
      `release_check`'s `version` row derives from them.
      THE EXAMPLE RENUMBERED to `examples/70_shared_brush_runtime.py`, because
      `add-mesh-sculpt-layers` took 69. Two renumber residues were still in the
      tree afterwards and are recorded at 12.3 and 12.4 rather than quietly
      fixed. The three golden `.inc` files are byte-unchanged against the new
      base, which is the acceptance gate re-measured rather than re-asserted.
      THE MERGE ALSO NEEDED AN ENGINE FIX, fcb8d174: `main` added
      `src/mesh/layered_sculpt.cpp`, whose region walk reads `workset.classes`,
      which this branch had already replaced with the neutral `items` array.
      The two sides never touched the same file, so git resolved it clean and
      the result did not compile. What the merge could NOT catch is at 12.2
      THE PRE-MERGE READING THIS SUPERSEDES, kept because it was right about
      the cost: `git merge-tree` named five conflicting files and nothing in
      the engine — the three version literals, `examples/run_all.py`, and
      `docs/09`, where both branches appended to one section and both
      paragraphs had to be kept. What it could not name is the file neither
      side touched twice, which is fcb8d174 above. And its closing warning
      stands and is section 12: every gate in 11.5 was measured against
      a44b1f5 and none of those readings survive the merge.

## 12. The proving run, at the merged tip

Everything sections 9, 10 and 11 measured was measured against `a44b1f5`. The
merge at 9bb7181 moved the base to 428f7362, took the version to 0.77.0,
renumbered the example and needed an engine fix to compile. None of those
readings survive that, so this section is the whole set taken again from the
merged tree — and two things it found that the merge could not.

- [x] 12.1 THE SUITE AND THE GATES, RE-RUN AT THE MERGED TIP.
      `cmake --build build/cpu-only -j 8` exit 0, zero warning lines with
      `CLAY_WERROR=ON`. `ctest --test-dir build/cpu-only` 4/4 in 143.77 s
      (`clay_unit_tests` 143.36 s), and the header names THIS worktree.
      `clay_unit_tests` on its own, BEFORE this section added anything:
      **2158 cases, 15,649,159 assertions, 0 failed** — against the
      2089 / 14,860,258 section 10.1 recorded, the difference being what the
      merge brought in. After 12.2's case, re-run at the tip:
      **2159 cases, 15,649,881 assertions, 0 failed**, `ctest` 4/4 in
      146.21 s.
      `check_layering.py` OK. `check_binding_parity.py` OK (631 pyclay
      capabilities, 32 exempt, imported from the built wheel).
      `check_c_abi.py` OK (hygiene + ctypes FFI) against
      `build/cpu-only/libclay_shared.so`. `check_gallery.py` OK, 254 tracked
      outputs. `check_swift_package.py` OK (textual — still no toolchain).
      THE ACCEPTANCE GATE HOLDS AGAINST THE NEW BASE:
      `git diff 428f7362 -- 'tests/unit/mesh_sculpt_goldens_*.inc'` is empty,
      and `tools/check_layering.py` is byte-unchanged from main, which is D1's
      visible form.
      pyclay: 632 passed, 1 skipped in 65.47 s;
      `test_shared_brush_runtime.py` alone 21 passed.
- [x] 12.2 THE GAP THE MERGE OPENED, FOUND AND GATED. `main` brought a FOURTH
      consumer of this runtime — `LayeredMultiresSculptor`, the layered stroke
      transaction — and this change's headline claim is stated over three.
      That sculptor reaches the runtime by a route none of the other three
      take: `gather()` builds no region of its own, it takes a zero-strength
      `Draw` through the level's `MeshSculptor` and reads `workset()` back,
      precisely so the falloff, the mask gate, the alpha and the composed
      automask are ONE answer. Nothing was watching that route — no test in
      the tree mentioned `LayeredMultiresSculptor` and an automask together —
      and it is exactly where the divergence this change exists to close would
      come back: `stamp` routes through `MultiresSculptor` and keeps its
      automask, so masking would appear to work for the sixteen ordinary verbs
      and silently stop for `erase`, `restore`, `smooth` and `stamp_detail`,
      the five verbs that exist only on this representation.
      `REGRESSION: the automask reaches the layered sculptor's own region walk`
      in `test_multires_shared_brush_parity.cpp` closes it. Measured on an 8x8
      quad cage at level 1: an eraser at radius 4.0 reaches all **289**
      vertices of the 17x17 level and leaves none unchanged; with
      `AutomaskFactor::Boundary` it reaches **225** and leaves exactly **64**,
      and those 64 are asserted to BE the geometric border rather than merely
      to number the same — a wrong address would leave a scrambled 64 that
      every count would still accept. The unmasked run is asserted first, as
      the control that keeps "the border survived" from being a statement
      about the brush's reach.
      PROVEN NON-VACUOUS by `probe.automask.factors = 0` in
      `LayeredMultiresSculptor::gather` — which compiles, and is the
      "optimisation" a reader would actually reach for. Against the whole
      2159-case suite exactly ONE case notices, and it is this one.
- [x] 12.3 A BUG THE RENUMBER LEFT, FOUND BY RUNNING THE EXAMPLE.
      `examples/output/70_shared_brush_adaptive.obj` was committed carrying
      `mtllib 69_shared_brush_adaptive.mtl` — a material file that does not
      exist in the tree, because the export was renamed after it was written.
      Any viewer opening the committed export finds no material. Regenerated
      and committed. THE GATE ALREADY EXISTED and is what caught it: 10.9's
      rule that the example must leave `git status examples/` empty. It is
      `check_gallery.py` that could NOT catch it — the manifest checks that a
      tracked output exists, not that it refers to a file that does.
- [x] 12.4 A SECOND RENUMBER RESIDUE, FIXED, AND THE GATE FOR IT DELIBERATELY
      NOT ADDED. The example announced itself as `R.banner("69 shared brush
      runtime ...")`, which is another example's number. Fixed.
      A repository-wide check that an example's banner names its own number is
      the obvious gate and was measured before being proposed: three examples
      on `main` — `33_mask_extrude` (says 26), `37_groups` (says 36) and
      `38_consolidation` (says 36) — fail it today. Adding the check here
      would either fail on main or drag three unrelated examples into this
      change's diff, so it is reported rather than taken.
- [x] 12.5 THE EXAMPLE, RUN AND RE-RUN. `examples/70_shared_brush_runtime.py`
      exits 0 against the wheel built from this tree, and reproduces all three
      of its outputs BYTE-IDENTICALLY on a second run (md5 over the .png, the
      .obj and the .mtl) — which is the determinism claim `check_gallery`
      cannot make. Its numbers at the merged tip are unchanged from 8.2 and
      8.3: 935 moved on all three with byte-identical positions, 613 open
      against 529 automasked on each, arenas settling at 3 / 4 / 3 growths and
      taking nothing more over 40 further dabs, and Draw diverging by
      2.660e-10 against a 2.000e-01 displacement.
      AND IT STILL CATCHES: with `in.topology = nullptr` in
      `DynamicSculptor::gather` (compiles) and pyclay rebuilt against it, the
      example exits 1 on `(529, 613, 529)` where it needs `(529, 529, 529)`.
      The same probe fails FIVE C++ cases — the two C ABI automask cases, P3's
      vertex-set case and both adaptive automask regressions — so the example
      is the wheel's gate on a property the test binary also holds.
- [x] 12.6 SANITIZERS, RE-RUN AT THE MERGED TIP over the change's own cases
      plus `test_sculpt_allocation`, `test_brush_preset` and the fixed-mesh
      golden parity case.
      ASAN-UBSAN with `ASAN_OPTIONS=detect_leaks=1`: exit 0, **82 cases,
      12,098 assertions, 0 failed**, and zero lines matching
      AddressSanitizer / UndefinedBehaviorSanitizer / LeakSanitizer /
      "runtime error".
      TSAN under `setarch -R`: exit 0, the same 82 cases and 12,098
      assertions, **zero `WARNING: ThreadSanitizer` lines**. That
      `adaptive parity: two sculptors stamp concurrently without aliasing`
      really ran under it was checked on its own rather than inferred from the
      total — 586 assertions, passing. Neither run is the whole suite and
      neither is claimed to be.
- [x] 12.7 THE BENCHMARK, RE-MEASURED, AND WHAT THIS BOX COULD NOT MEASURE.
      Forty repetitions of 200 iterations,
      `--benchmark_report_aggregates_only=false`; percentiles are over the
      forty repetition means. Load average 3.45 before and 5.59 after, both
      read on the box. All times in microseconds:

      | case | P50 | P95 | P99 | max | growths | high water |
      |---|---|---|---|---|---|---|
      | fixed, no automask, n=224 | 69.30 | 73.47 | 78.19 | 80.46 | 0 | 0 |
      | fixed, boundary automask, n=224 | 167.66 | 169.84 | 171.06 | 171.72 | 1 | 1856 |
      | fixed, no automask, n=707 | 570.74 | 575.70 | 615.38 | 640.29 | 0 | 0 |
      | fixed, boundary automask, n=707 | 1511.38 | 1545.44 | 1569.96 | 1575.74 | 1 | 1856 |
      | adaptive, no automask, n=224 | 135.15 | 147.45 | 157.62 | 158.63 | 1 | 1856 |
      | adaptive, boundary automask, n=224 | 351.71 | 358.53 | 385.15 | 400.99 | 1 | 1856 |
      | adaptive, no automask, n=707 | 131.73 | 150.58 | 153.20 | 153.96 | 1 | 1856 |
      | adaptive, boundary automask, n=707 | 1075.31 | 1803.49 | 4681.49 | 6234.81 | 1 | 1856 |

      THE MEDIAN RATIOS REPRODUCE 10.8 ALMOST EXACTLY, which is the reading:
      the automask costs x2.42 and x2.65 on the fixed path (10.8 measured 2.41
      and 2.64) and x2.60 and x8.16 on the adaptive one (2.67 and 8.01). The
      arena counters reproduce 8.2's trap too — the plain fixed stamp is the
      one row reporting `growths = 0`, because its automask-free path never
      touches the arena.
      THE TAIL ON ONE ROW IS NOT MEASURABLE ON THIS BOX TODAY, and is reported
      as that rather than as a number. `adaptive, boundary automask, n=707`
      shows P99/P50 = 4.35x here where 10.8 read 1.69x at load ~2. Re-run
      alone at load 10.3 rising to 15.3 it reads P50 1183.21, P99 17616.63,
      max 24406.12 — a 14.89x tail. The P50 moved 10% across a 3x load change
      and the tail moved 3x, so the tail is the box and not the feature. Seven
      of the eight rows are within 15% of their own P50 at both loads. NO
      CEILING added to `tools/check_bench.py`, on that file's own rule.
- [x] 12.8 A GOTCHA WORTH THE NEXT STAGE'S TIME, because it looked like a
      failure and was not. `bindings/python/tests/test_c_abi_parity.py::
      test_mesh_layer_written_by_pyclay_reads_back_through_c` failed with
      `clay_document_load` returning 4 on a document pyclay had just written.
      It is not a defect: that suite loads `build/release/libclay_shared.so`
      through ctypes, and building `--target pyclay` alone leaves that library
      STALE — a wheel at one format minor reading through a loader at another.
      `cmake --build build/release` in full, then 632 passed, 1 skipped.
      Building the pyclay target on its own is enough to run
      `test_shared_brush_runtime.py` and is NOT enough to run the ctypes
      parity suite.
- [x] 12.9 `release_check.py` AT THE MERGED TIP, run under `/usr/bin/python3`
      rather than the anaconda python on PATH, and every failing row traced
      rather than attributed. FOUR fail — `tests`, `device`, `benchmarks`,
      `wheel` — and `version` passes at cmake=0.77.0 abi=0.77.0 wheel=0.77.0.
      `tests` — the only failing ctest entry in `build/release` is
      `pyclay_pytest`, and the log names the cause: `ImportError:
      .../anaconda3/lib/libstdc++.so.6: version GLIBCXX_3.4.31 not found`.
      Under `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6` the same suite is
      632 passed, 1 skipped.
      `device` — the gate's snapshot is 39c244209 and main already differs from
      it in 72 engine files; it fails on main too and re-runs on the reference
      iPad.
      `wheel` — `python3 -m venv` cannot bootstrap pip here (`python3.12-venv`
      is not installed), so the gate never reaches a wheel.
      `benchmarks` — THE ONE THAT WAS NOT ON 11.5'S LIST, and it is the shared
      box rather than the tree. It failed
      `BM_MoveDrag1000: 0.7 ms above ceiling` on a run taken while another
      worktree's TSan and ASan suites had the load average at 15. Measured on
      its own at load 11.5: **0.086 ms median over 15 repetitions against a
      0.6 ms ceiling**, seven times the headroom. A second whole-suite run
      failed a DIFFERENT row (`BM_VolumeBakeCulledDoc not faster than
      BM_VolumeBakeWholeTapeDoc`, a ratio gate), and a third started at load
      3.93 and reports `bench-gate: OK`. Three runs, three different answers,
      none of the rows touched by this change: the gate needs a quiet runner,
      which is the same rule that kept a ceiling out of `check_bench.py` at
      6.8.

## 13. The documentation pass, and what CI closed that this box could not

Section 11 wrote the documentation against `a44b1f5` and section 12 re-measured
the code at the merged tip. What neither did is re-read the PROSE at the merged
tip, and the merge moved three things the prose names: the version, the example
number, and the count of things that consume this runtime.

- [ ] 13.1 THE DOCS RE-READ AT THE MERGED TIP, and three statements the merge
      made false, each fixed where the false sentence is rather than by
      appending a correction.
      `docs/05-claycore-library.md` said the arena arrived "since 0.75.0",
      which is the number this branch was ASSIGNED and not the one it ships
      under — 0.77.0, for the reason 11.7 records. A reader checking the claim
      against `clay.h` would have found 77 and concluded the arena predates it.
      `docs/07-brushes-and-features.md` said the runtime is read by three
      sculptors, full stop. It is three REPRESENTATIONS and FOUR consumers:
      `LayeredMultiresSculptor` reaches it through a route none of the other
      three take, and 12.2's regression case pins that route without any
      document saying the route exists. A new paragraph says what the route is,
      why it is shaped that way (a layered stroke must weight a vertex by
      exactly what an unlayered stamp would), and what breaks asymmetrically if
      it drifts — masking keeps working for the sixteen ordinary verbs and
      stops for `erase`, `restore`, `smooth` and `stamp_detail`, the four that
      exist only on that path. THE FOUR WAS COUNTED rather than copied: those
      are the four public verbs whose implementations call `gather()`
      (`stamp_detail`, `smooth` through both modes, and `erase` / `restore`
      through `fade_toward_zero`); 12.2's note says five and is one out.
      `docs/09-brush-latency-and-coverage.md` had the new section running
      straight into the next `###` with no blank line between them, which is a
      heading this file's own renderer would have swallowed.
- [ ] 13.2 THE OPENSPEC ARTIFACTS RECONCILED WITH THE NUMBER THEY SHIP UNDER.
      `proposal.md`'s Impact paragraph still promised 0.75.0 / minor 75, "a
      host compiled against 74", and "`examples` gains 69" — every one of which
      is what was planned and none of which is what merged. Corrected, with a
      sentence saying the two numbers moved because the stack merged out of
      order rather than because anyone revised the design; 11.7 keeps the
      detail. `tasks.md` 7.1, 7.5 and two `Files` rows carried 75 in the same
      way and now carry 77 with the same pointer. Task 0.1's "assigned 0.75.0"
      is LEFT STANDING — it is the assignment as it was given, and rewriting it
      would erase the fact 11.7 exists to record.
- [ ] 13.3 THE FINAL GATES, from this worktree at the documentation tip, as
      output rather than as a tick.
- [ ] 13.4 THE SWIFT GATE, CLOSED BY CI RATHER THAN BY THIS BOX — which is
      where 7.6 and 11.4 always said it would close, and it has.
- [ ] 13.5 THE PR, updated rather than opened. #419 already existed from 11.6
      and was CONFLICTING then; the merge at 9bb7181 resolved that.


## Files

**Added**

| Path | Why |
|---|---|
| `include/clay/mesh/brush_arena.h` | `BrushScratchArena` and `ScratchVector`; template `allocate<T>` has to be visible |
| `src/mesh/brush_arena.cpp` | growth policy, alignment arithmetic and the statistics, kept out of the header |
| `include/clay/mesh/work_item.h` | `WorkItemId` and `WorkItemTopology` — the two types the workset and the automask both need without either including the other |
| `include/clay/mesh/stamp_frame.h` | the orthonormal stamp basis, below `sculpt_kernels.h` so the sculptors can build one without the kernels |
| `src/mesh/stamp_frame.cpp` | `make_stamp_frame`, including the zero-azimuth branch D5 turns on |
| `tests/unit/test_brush_arena.cpp` | the arena's own contract, including that it STOPS growing |
| `tests/unit/test_stamp_frame.cpp` | orthonormality, `stamp_uv` inversion, and azimuth-zero byte equality |
| `tests/unit/test_dynamic_shared_brush_parity.cpp` | P1–P4 against the adaptive surface |
| `tests/unit/test_multires_shared_brush_parity.cpp` | P1–P4 against the hierarchy at level 0 |
| `examples/70_shared_brush_runtime.py` | one preset gesture over three representations, asserted and rendered |
| `tests/unit/test_shared_brush_determinism.cpp` | history-independence of the arena, and the automask drop said where it is visible |
| `tests/unit/test_c_shared_brush_runtime.cpp` | the automask divergence and the arena statistics at the ABI, where the contract is written down |
| `bindings/python/tests/test_shared_brush_runtime.py` | the same claims from the wheel, including the two estimators the C ABI cannot carry |
| `examples/output/70_shared_brush_runtime.png`, `..._adaptive.obj`, `..._adaptive.mtl` | the committed render and mesh the gallery gate compares against |

**Changed**

| Path | Why |
|---|---|
| `include/clay/mesh/sculpt_workset.h` | `items` replaces `classes`; `compose_workset` declared here because it is the one genuinely neutral step |
| `include/clay/mesh/automask.h`, `src/mesh/automask.cpp` | the neutral core over `WorkItemTopology`; the fixed overload becomes an adapter; the five vectors move to the arena |
| `include/clay/mesh/sculpt.h`, `src/mesh/sculpt.cpp` | arena member, `WorkItemId` write region, `build_fixed_mesh_workset` |
| `include/clay/mesh/dynamic_sculpt.h`, `src/mesh/dynamic_sculpt.cpp` | the shared workset replaces five private arrays; `set_automask_inputs`; the automask factor; `DynamicSurfaceTopology`; the per-stamp vectors move to the arena |
| `include/clay/mesh/multires_sculpt.h`, `src/mesh/multires_sculpt.cpp` | `build_multires_workset` re-tags items as (level, vertex); the arena is forwarded to the bound level sculptor |
| `include/clay/mesh/dynamic_surface.h`, `src/mesh/dynamic_surface.cpp` | `refresh_normals` takes its touched-vertex buffer from the caller |
| `include/clay/mesh/sculpt_kernels.h`, `src/mesh/sculpt_kernels.cpp` | `alpha_frame_for` reimplemented over `make_stamp_frame`; `AlphaFrame` unchanged |
| `include/clay/mesh/sculpt_common.h` | `stamp_azimuth` |
| `include/clay/brush/preset.h`, `src/brush/preset.cpp` | preset version 2: the azimuth crosses the format — 10.4 |
| `tests/unit/test_brush_preset.cpp` | the three regression cases for 10.4 |
| `bindings/python/tests/test_shared_brush_runtime.py` | the same three through the wheel, on a format no Python test had touched |
| `include/clay/mesh/brush_model.h`, `include/clay/mesh/surface_frame.h` | each names the other two frames — D4 |
| `bindings/c/clay.h`, `bindings/c/clay_c.cpp` | minor 77, `stamp_azimuth`, the arena statistics |
| `bindings/python/pyclay_module.cpp` | `automask` on the adaptive stamp, `set_automask_inputs`, `arena_stats` |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | the new sources and tests |
| `CMakeLists.txt`, `bindings/c/clay.h`, `pyproject.toml` | the version, at 0.77.0 / 77 (11.7). THREE literals — `tools/release_check.py` derives it and has no row to edit (7.5) |
| `include/clay/mesh/dynamic_bvh.h`, `src/mesh/dynamic_bvh.cpp`, `include/clay/mesh/slot_pool.h` | the buffers the adaptive walk used to own become arena blocks the caller passes in |
| `tests/unit/test_sculpt_allocation.cpp` | THE GATE — extended to every verb with automask factors on, and to the other two representations |
| `tests/unit/test_mesh_sculpt.cpp`, `tests/unit/test_dynamic_sculpt.cpp` | the workset's new identity, and the adaptive automask's regression case |
| `src/mesh/layered_sculpt.cpp` | the merge seam — the region walk addresses `items` rather than the `classes` array that no longer exists (fcb8d174, 11.7) |
| `benchmarks/bench_main.cpp` | the four automask-on/off cases 6.8 and 10.8 measure, carrying the arena counters |
| `tools/check_binding_parity.py` | the three `set_automask_inputs` exemptions, with the `std::function` reason |
| `tests/swift/smoke.swift` | the appended field and the arena statistics through SwiftPM — written, NOT compiled here (7.6) |
| `examples/run_all.py`, `docs/07-brushes-and-features.md` | example 70 — 69 was taken by `add-mesh-sculpt-layers`, which merged first (11.7) |
| `README.md`, `docs/05-claycore-library.md`, `docs/09-brush-latency-and-coverage.md`, `openspec/ROADMAP.md` | section 11 — what this change makes untrue elsewhere |

**Must not change**

`tests/unit/mesh_sculpt_goldens_linux_x64.inc`,
`tests/unit/mesh_sculpt_goldens_macos_arm64.inc`,
`tests/unit/mesh_sculpt_goldens_msvc_x64.inc`.

## Gates, and what a failure of each would mean

| Gate | Proves | A failure means |
|---|---|---|
| the three golden `.inc` files, unchanged in the diff | the fixed path did not move by a bit | the refactor re-associated a multiplication — the headers warn about this at length, and the fix is the refactor, never the table |
| `test_sculpt_allocation` with automask factors on | the automask costs the workset | `compute_automask` is still building vectors per dab, which is what it does today |
| `test_sculpt_allocation` on `DynamicSculptor` / `MultiresSculptor` | "a stamp costs what it touches" is true on all three | the adaptive path still sorts its region into fresh vectors per stamp, or `write_positions` still allocates per moved vertex |
| `test_brush_arena`'s `growths` assertion | the arena converges | scratch is growing every stamp — the failure mode an allocation count alone cannot see |
| parity P1 (identical snapshot → identical displacement) | the kernels are genuinely neutral | a sculptor is passing something the kernel reads differently — an unset `plane_normal`, a stale `average_normal` |
| parity P2 (`compose_weight` bit equality) | one weight, one order | a representation re-derived a factor instead of calling the shared composition |
| parity P3 (identical vertex set, normal-free verb, byte equality) | the runtime around the kernels agrees | the walk, the drop rule or the factor order diverged — the strongest single signal in the suite |
| parity P4 (the named differences still differ) | the differences are understood, not accidental | either an estimator was silently unified, or one representation has drifted far past the stated bound |
| the automask regression, C++ and C | the adaptive path honours the brush it was given | the divergence this change exists to close is back |
| the layered sculptor's automask regression (12.2) | the FOURTH consumer, which `main` added mid-change, reaches the same runtime | `LayeredMultiresSculptor::gather` grew a region walk of its own, and masking works for the sixteen ordinary verbs while silently failing for `erase`, `restore`, `smooth` and `stamp_detail` |
| `test_stamp_frame`'s azimuth-zero byte equality | D5's branch is present | somebody replaced it with `cos`/`sin`, and a `-0.0f` is waiting to move a golden |
| `check_layering.py` | D1 held | something in `mesh` reached into `brush`, or the leaf module got added after all |
| `check_binding_parity.py` | the ABI reaches what pyclay reaches | a Python member landed without a C counterpart |
| `examples/70` raising `SystemExit` | the claims are true of the shipped wheel and not only of the test binary | a claim in the prose stopped holding |
