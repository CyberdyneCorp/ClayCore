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
- [ ] 4.5 REGRESSION TEST for the divergence this change exists to close: a
      stamp on an adaptive surface with `AutomaskFactor::NormalAngle` set must
      move fewer vertices than the same stamp without it, and the difference
      must be on the side facing away from the brush. It fails on main
- [ ] 4.6 REGRESSION TEST at the C ABI, where the contract is written down:
      `clay_dynamic_sculptor_stamp` with `automask_factors` set must produce a
      different report from the same call with 0, because
      `clay_dynamic_sculptor_stamp`'s own header says the descriptor is "the
      same descriptor the fixed path takes". It fails on main
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
- [ ] 5.2 The zero-azimuth branch, with the `-0.0f` reasoning in the comment —
      D5. A test asserts the unrotated basis is byte-identical to the basis
      built with `azimuth = 0`.
      HALF DONE: the branch and its reasoning are in `src/mesh/stamp_frame.cpp`.
      The assertion is 6.6's and is not written yet, so this stays open
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

- [ ] 6.1 `tests/unit/test_brush_arena.cpp` — bump order, alignment, `reset`
      keeps capacity, `high_water_bytes` is a maximum and not a current, and
      `growths` stops growing over a stroke of similar stamps
- [ ] 6.2 `tests/unit/test_sculpt_allocation.cpp` EXTENDED: every verb with
      `Boundary | TopologyConnected | NormalAngle` set, which is the coverage
      hole that let `compute_automask`'s five vectors through. It fails on main
- [ ] 6.3 `tests/unit/test_sculpt_allocation.cpp` EXTENDED to
      `DynamicSculptor` (topology disabled, so the surface is stable) and to
      `MultiresSculptor`. A warm stamp allocates nothing on all three
      representations, which is the requirement's actual wording
- [ ] 6.4 `tests/unit/test_dynamic_shared_brush_parity.cpp` — P1, P2, P3 and
      P4 for the adaptive surface, on a plane grid converted with
      `DynamicSurface::from_mesh` and topology disabled
- [ ] 6.5 `tests/unit/test_multires_shared_brush_parity.cpp` — the same, at
      level 0 where an identical vertex set exists; P1 and P2 above it
- [ ] 6.6 `tests/unit/test_stamp_frame.cpp` — the basis is orthonormal and
      right-handed, `stamp_uv` inverts it, the azimuth rotates in-plane, and
      an explicit rotation replaces the azimuth rather than composing
- [ ] 6.7 THE ACCEPTANCE GATE, run after every commit in this branch:
      `mesh_sculpt_goldens_linux_x64.inc`, `..._macos_arm64.inc` and
      `..._msvc_x64.inc` are UNCHANGED in the diff. A moved golden is a failed
      refactor and never a new baseline
- [ ] 6.8 A benchmark case for the neutral automask's virtual — a
      boundary-automasked stamp, before and after, on the same fixture. The
      measured number goes in the commit message; a ceiling goes in
      `tools/check_bench.py` only if it can be set from the runner rather than
      from a development machine

## 7. The ABI and the bindings

- [ ] 7.1 `bindings/c/clay.h`: `CLAY_ABI_MINOR 75`; `stamp_azimuth` appended to
      `clay_mesh_brush_desc` under the `struct_size` rule;
      `clay_brush_arena_stats` and the three `*_arena_stats` calls — D9
- [ ] 7.2 `bindings/c/clay_c.cpp`: `read_mesh_brush` carries the azimuth,
      `write_mesh_brush` reports it, the three stat calls are implemented
- [ ] 7.3 `bindings/python/pyclay_module.cpp`: `DynamicSculptor.stamp` gains an
      `automask` argument (it has none today, so the divergence is unreachable
      from Python even after 4.4); `set_automask_inputs` on `DynamicSculptor`;
      `arena_stats` on all three sculptors
- [ ] 7.4 `python3 tools/check_binding_parity.py` — every new pyclay member has
      a C counterpart or an exemption with a reason. `arena_stats` maps to
      `clay_*_sculptor_arena_stats` under the existing `CLASS_PREFIX` rule
- [ ] 7.5 The four version lines, in lockstep, at 0.75.0 / 75

## 8. The demonstration

- [ ] 8.1 `examples/69_shared_brush_runtime.py` — ONE
      `brush.reference_preset(...)` gesture, resolved once, replayed over a
      fixed mesh, an adaptive surface and a multires hierarchy built from the
      same source model
- [ ] 8.2 It ASSERTS what must match: the normal-free verb writes identical
      positions on all three (P3); the topological automask reaches the same
      vertex set; the arena's `growths` stops climbing over the stroke
- [ ] 8.3 It ASSERTS what legitimately differs, and by how much: Draw diverges
      between fixed and adaptive because the normal estimators differ (D8/P4),
      and the example says so in prose rather than hiding it
- [ ] 8.4 `raise SystemExit` when a claim stops holding, which is this
      repository's example convention
- [ ] 8.5 Registered: `EXAMPLES` in `examples/run_all.py`, the render committed
      under `examples/output/`, and a line in
      `docs/07-brushes-and-features.md`. No new `CAPABILITY_EXAMPLES` key —
      this change adds requirements to `meshing` and `brush-engine`, which
      already have examples
- [ ] 8.6 `python3 tools/check_gallery.py`

## 9. Ship

- [ ] 9.1 `cmake --build --preset cpu-only -j 8` clean;
      `ctest --preset cpu-only --output-on-failure` green
- [ ] 9.2 `asan-ubsan` green; `setarch -R ctest --preset tsan` green — the
      arena is per-sculptor and the claim that two sculptors never alias it is
      a threading claim
- [ ] 9.3 `check_layering.py`, `check_binding_parity.py`, `check_c_abi.py`,
      `check_gallery.py`, `release_check.py`. The four known-failing
      `release_check` rows in this environment (`bindings`, `abi`, `tests`
      from the anaconda GLIBCXX_3.4.31 mismatch, `device` from the hardware
      gate) are pre-existing; anything else is verified against main before it
      is called that
- [ ] 9.4 Every new `src/**.cpp` in `CMakeLists.txt`, every new
      `tests/unit/test_*.cpp` in `tests/CMakeLists.txt`
- [ ] 9.5 `openspec validate add-shared-brush-runtime --strict`

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
| `examples/69_shared_brush_runtime.py` | one preset gesture over three representations, asserted and rendered |

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
| `include/clay/mesh/brush_model.h`, `include/clay/mesh/surface_frame.h` | each names the other two frames — D4 |
| `bindings/c/clay.h`, `bindings/c/clay_c.cpp` | minor 75, `stamp_azimuth`, the arena statistics |
| `bindings/python/pyclay_module.cpp` | `automask` on the adaptive stamp, `set_automask_inputs`, `arena_stats` |
| `CMakeLists.txt`, `tests/CMakeLists.txt`, `pyproject.toml`, `tools/release_check.py` | the new sources and tests, and the four version lines at 0.75.0 / 75 |
| `examples/run_all.py`, `docs/07-brushes-and-features.md` | example 69 |

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
| `test_stamp_frame`'s azimuth-zero byte equality | D5's branch is present | somebody replaced it with `cos`/`sin`, and a `-0.0f` is waiting to move a golden |
| `check_layering.py` | D1 held | something in `mesh` reached into `brush`, or the leaf module got added after all |
| `check_binding_parity.py` | the ABI reaches what pyclay reaches | a Python member landed without a C counterpart |
| `examples/69` raising `SystemExit` | the claims are true of the shipped wheel and not only of the test binary | a claim in the prose stopped holding |
