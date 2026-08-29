# Tasks: add-shared-brush-kernels

- [ ] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): first of the
      five, and a prerequisite of `add-dynamic-topology`, `add-mesh-multires`
      and `add-mesh-sculpt-layers`. Runs in parallel with nothing that touches
      `src/mesh/sculpt.cpp`

## 1. Decide before extracting

- [x] 1.1 DECIDE where the scratch arena lives — a new `memory` module with a
      `tools/check_layering.py` entry, or an addition to `parallel`. The
      precedent is `parallel` itself, moved out of the CPU backend because the
      layering rule locked the core library out of the only pool in the tree
- [x] 1.2 DECIDE whether `MeshBrushSettings` stays as the public mesh surface
      (projected onto `BrushModel` internally) or is replaced. Both bindings
      and every compiled host pass it today
- [x] 1.3 DECIDE the automask composition rule and write it once:
      `weight = falloff * alpha * (1 - gate) * automask`, with a single clamp
      at the boundary rather than per factor
- [x] 1.4 DECIDE which axes reach the C ABI in this change and which wait. A
      descriptor mirroring every axis is a large surface to get wrong once

## 2. Extract the kernels, changing nothing

- [x] 2.1 `include/clay/mesh/sculpt_common.h` — `MeshBrush`, `MeshFalloff` and
      the settings vocabulary, so a second sculptor can name a verb without
      including the fixed-topology implementation
- [x] 2.2 `include/clay/mesh/sculpt_kernels.h` + `src/mesh/sculpt_kernels.cpp`
      — snapshot in, displacement out. The interface SHALL name no `Mesh`, no
      `Adjacency` and no vertex index: a span of positions, normals and
      weights, a neighbourhood view, the stamp frame, and the plane a flatten
      family verb was given or computed
- [x] 2.3 Move all sixteen verbs' math behind it, `Paint` and `Smear` included
      — a colour write is a kernel with a different output channel, not an
      exception to the model
- [x] 2.4 `MeshSculptor` calls the shared kernels; the gather, the write-back,
      the weld-class semantics, the normal recompute and the BVH refit stay
      exactly where they are
- [x] 2.5 PARITY GATE: golden fixtures for every verb on plane, sphere, cube,
      folded sheet and two-close-sheets, compared BIT FOR BIT against main, not
      within a tolerance. A tolerance would admit a reordered accumulation,
      which is the one mistake this refactor is likely to make
- [x] 2.6 The pre-stamp snapshot rule survives the move: one stamp resolves
      against one snapshot, so a composed verb stays a single operation

## 3. The brush model

- [ ] 3.1 `include/clay/brush/model.h` — `BrushFootprint`, `BrushWeightModel`,
      `BrushFrameModel`, `BrushKernel`, accumulation, write target, post
      policy. Enums and PODs; no virtual dispatch in a per-vertex loop
- [ ] 3.2 Frames named rather than implied: region normal, vertex normal,
      stroke direction, stylus azimuth, given plane, view plane. Draw and
      Inflate become one kernel under two frames, and the existing distinction
      between them SHALL be preserved exactly by that reading
- [ ] 3.3 `include/clay/brush/runtime.h` — `BrushRuntimePlan` compiled once at
      stroke begin: what the kernel needs (normals, neighbours, snapshot,
      alpha, automask), precomputed reciprocals, and nothing the loop must
      re-derive
- [ ] 3.4 A plan is cached on preset revision, not recompiled per stamp
- [ ] 3.5 Determinism: the same samples produce the same stamps whether the
      host delivers them in one batch or five, with the transaction state
      retained. Test it as a comparison, not as an assertion about jitter
- [ ] 3.6 `apply_to_mesh` consumes `Stamp::rotation`, which today it drops.
      `resolve_stroke` puts azimuth and rotate-along-stroke there and
      `stamps_to_nodes` applies it to the node transform; the mesh consumer
      never reads it, which is exactly why a rake or chisel brush is
      inexpressible on a mesh layer. Orienting the alpha by it is the smallest
      change in this list and closes a capability rather than refining one

## 4. Workset and scratch

- [ ] 4.1 `include/clay/mesh/sculpt_workset.h` — vertices, triangles,
      positions, normals, weights, automask, bounds, and `clear_keep_capacity`
- [ ] 4.2 READ HALO distinguished from WRITE REGION. Smooth, Relax and Polish
      read a ring they do not write, and the dirty report SHALL cover the write
      region only or a host uploads geometry that did not change
- [ ] 4.3 Scratch arena per sculptor, reset rather than freed between stamps
- [ ] 4.4 Epoch-marked visited sets rather than a hash set per dab
- [ ] 4.5 ALLOCATION GATE: after warm-up, an ordinary local stamp on a
      fixed-topology mesh performs zero heap allocations. Instrumented in the
      benchmark build, asserted in a test, and allowed to fail loudly on the
      first stamp of a larger footprint
- [ ] 4.6 Smooth's iteration loop ping-pongs local buffers and queries the tree
      ONCE, not once per pass. `kMaxSmoothIterations` stays the bound

## 5. Automasking

- [ ] 5.1 `include/clay/mesh/automask.h` — normal-angle, topology-connected,
      boundary, cavity and surface-group gates, each producing one scalar per
      WORKSET vertex and never one per mesh vertex
- [ ] 5.2 Cavity and curvature call the SAME estimator `brush/procedural_mask`
      uses. A painted cavity mask and a cavity automask disagreeing about one
      surface is the failure this task exists to prevent
- [ ] 5.3 The surface-group gate reads `voxel::GroupField` through the caller,
      not a new per-face id — the world-lattice decision in `add-surface-groups`
      is what makes a group survive a representation bridge, and a per-face
      copy would be a second answer to the same question
- [ ] 5.4 A fully automasked vertex is bit-identical to its input position
- [ ] 5.5 Automasks compose by multiplication, and the composition is tested
      against each factor alone

## 6. Presets

- [ ] 6.1 `brush::BrushPreset` — name, `StrokePreset`, `BrushModel`, schema
      version from v1. An unknown newer version is REFUSED, not reinterpreted,
      as `StrokePreset` already does
- [ ] 6.2 No image bytes in a preset. Alpha and any future displacement image
      stay caller-owned and borrowed for the call
- [ ] 6.3 A reference preset library covering the artist families — Standard,
      Clay, Clay Buildup, Clay Strips, Inflate, Smooth, Relax, Move, Move
      Topological, Snake Hook, Pinch, Dam/Crease, Flatten, Scrape, hPolish,
      Trim, Layer, Nudge, Rake — as data, with no engine path of their own
- [ ] 6.4 Round-trip: serialize, deserialize, resolve a stroke, compare stamps

## 7. Bindings, gallery, gates

- [ ] 7.1 C ABI: preset create/serialize/deserialize and the model axes decided
      in 1.4, `struct_size` on every descriptor, bounded output fills
- [ ] 7.2 pyclay: the same surface, numpy-native where an array is natural
- [ ] 7.3 `tools/check_binding_parity.py` green — a capability reachable from
      C and not from pyclay does not count as shipped, and three capabilities
      in a row landed reachable from neither
- [ ] 7.4 A numbered example that renders the same gesture through five presets
      over one mesh and ASSERTS what separates them, raising `SystemExit` when
      the claim stops holding
- [ ] 7.5 Version lines together: `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml`, and `release_check.py`'s row
- [ ] 7.6 Four presets green (release, metal, opencl, asan-ubsan) plus
      `release_check`; `tsan` under `setarch -R`
- [ ] 7.7 `python3 tools/check_layering.py` green, including whatever 1.1
      decided
- [ ] 7.8 Docs: `docs/07-brushes-and-features.md` gains the model as the way
      the vocabulary is organised, and says which named brushes are presets
