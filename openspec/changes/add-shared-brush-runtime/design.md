# Design: the shared brush runtime

## Context

This change is the residual of `add-shared-brush-kernels`. That change is
merged and its claims hold — verified against the tree rather than taken from
the roadmap:

| Claimed landed | Verified |
|---|---|
| `mesh/sculpt_kernels.{h,cpp}`, snapshot in / displacement out | yes — 243 + 456 lines, names no `Mesh`, no `Adjacency`, no vertex index |
| `mesh/brush_model.h` — `BrushFootprint`, `BrushFrame`, `BrushKernelId`, `BrushWriteTarget`, `BrushPostPolicy`, `model_of`, `BrushRuntimePlan`, `compile_plan` | yes |
| `mesh/sculpt_workset.h` — read halo vs write region, capacity-preserving clear | yes |
| `mesh/automask.{h,cpp}` | yes |
| `brush/preset.{h,cpp}` — versioned, `reference_presets()` | yes |
| `compose_weight()`'s one fixed factor order | yes — `falloff · taper · (1-gate) · alpha · automask` |
| fixed-mesh bit-parity goldens, three toolchains | yes — `tests/unit/mesh_sculpt_goldens_{linux_x64,macos_arm64,msvc_x64}.inc` |

So the deformation math is genuinely shared. What is not shared is the runtime
around it, and the four gaps the proposal lists were each read out of the
source rather than inferred:

- `src/mesh/dynamic_sculpt.cpp:344-408` composes `WeightFactors{falloff,
  path_taper, gate, alpha}` and never reads `brush.automask`. There is no
  `set_automask_inputs` on `DynamicSculptor` and no call to
  `compute_automask` anywhere outside `src/mesh/sculpt.cpp:501`.
- `src/mesh/automask.cpp:83-141` allocates five `std::vector`s per stamp when
  the boundary or connectivity factor is set, and
  `tests/unit/test_sculpt_allocation.cpp` never sets one, so the gate is green
  over a path it does not cover.
- `src/mesh/dynamic_sculpt.cpp` allocates in `euclidean_region` (four vectors),
  `geodesic_region` (three) and `write_positions` (one per moved vertex), and
  `DynamicSurface::refresh_normals` allocates one more per call. Nothing gates
  any of it.
- `AlphaFrame` is the only orthonormal stamp basis in the tree and it exists
  only when `settings.has_alpha()`.

## Goals / Non-Goals

**Goals.** A workset that three representations fill and one automask reads. An
arena that makes "a stamp costs what it touches" true on the representation
where it currently is not. A stamp frame that the directional brush family can
be written against. Parity gates that say what must agree AND what must
differ. One preset gesture demonstrated across all three.

**Non-Goals.** No new verb, and no new kernel. No change to what the fixed
sculptor produces — the goldens are the gate, not a baseline. No dynamic
topology policy change, no hierarchy change. No new named brush families beyond
what `reference_presets()` already carries; if a directional family needs one
after `StampFrame` exists, it is a preset in a later change and not a code path
here.

## Decisions

### D1 — The neutral runtime stays in `mesh`. There is no new module

**The question.** The implementation guide's section 3.3 asks for
`include/clay/brush/runtime.h`, `workset.h`, `frame.h`, `kernels.h` and
`scratch_arena.h`. `tools/check_layering.py` records `brush -> mesh` because
`brush::apply_to_mesh` is the stroke engine's fourth consumer, so `mesh` may
not include `brush`, and the loops that read these types are
`MeshSculptor::stamp`, `DynamicSculptor::stamp` and `MultiresSculptor::stamp`
— all in `mesh`. Writing the guide's layout is a cycle on the first include.
The two legitimate options are (a) keep the neutral runtime in `mesh` and let
"representation-neutral" be a property of the TYPES, or (b) add a leaf module
below both — `clay/brushrt/` — depending only on `{parallel, kernel, math}`,
with its own `ALLOWED` entry, the way `parallel` was added when the layering
rule locked the core library out of the only thread pool in the tree.

**The choice: (a).** The neutral runtime is
`mesh/work_item.h`, `mesh/sculpt_workset.h`, `mesh/brush_arena.h` and
`mesh/stamp_frame.h`, and `tools/check_layering.py` is not edited.

**Why.** `parallel` is the cited precedent and it is the opposite case. It
moved because callers in *many* modules — every mesher, every voxel verb,
redistance, the per-brick cull — could not legally reach it. Here the consumer
list is closed and it is three classes in one directory. A module created for
one module's callers is a directory with a longer name, not a boundary, and
this repository has already decided exactly this question once: D1 of
`add-shared-brush-kernels` put the scratch in `mesh` on this reasoning, and
nothing has changed since except that two more consumers appeared — both in
`mesh`. Reversing a decision requires new evidence, and the new evidence points
the same way.

**What (b) would have cost, concretely.** `sculpt_common.h` is the vocabulary
the neutral types are written in, and it includes `clay/field/flatten.h` for
`FlattenMode` — which `kernel_flatten` and `plane_offset` both take. A
`brushrt` module allowed only `{parallel, kernel, math}` could not include it,
so the move would have forced one of three things: a fourth `ALLOWED` edge
(`brushrt -> field`, legal, since `field` is a leaf on the same three, but an
edge added to make a directory work rather than to express a dependency); a
THIRD copy of the flatten modes, in a tree that already carries `MeshFalloff`
as a deliberate duplicate of `voxel::BrushFalloff` and says so at length; or
hoisting `field/flatten.h` below `field`, which is a change to an unrelated
module to accommodate a directory. It would also have split `automask.h` in
two, because `compute_automask`'s fixed adapter names `Mesh` and `Adjacency`
and cannot follow the neutral core down. And it would have rewritten the
include line in every file that reaches `sculpt_common.h` — 40-odd sources,
both bindings and the C ABI — for a rename.

The reversal remains cheap if a fourth consumer ever appears outside `mesh`:
these headers name nothing above `math` except `field::FlattenMode`, so moving
them is a `git mv`, an `ALLOWED` entry and an include sweep. Creating the
module now and finding nothing else wants it is the move that cannot be undone
quietly.

### D2 — `WorkItemId` is 64 bits, and the width is chosen by the adaptive surface

**The question.** `SculptWorkset::classes` is `std::vector<std::uint32_t>` — a
weld class, the fixed mesh's identity. The adaptive surface's `VertexId` is a
slot plus a generation; the hierarchy addresses a vertex as (level, vertex).

**The choice.** A 64-bit opaque `WorkItemId` in `mesh/work_item.h`, with named
constructors and readbacks — `WorkItemId::weld_class(u32)`,
`WorkItemId::surface_vertex(VertexId)`, `WorkItemId::level_vertex(u32, u32)` —
and `SculptWorkset::items` replacing `classes`. `MeshSculptor::write_region()`
widens to `const std::vector<WorkItemId>&`.

**Why 64 and not a template.** The width is not arbitrary and it is not a
guess at the future: `VertexId` needs its generation carried or a stale handle
in a write region is indistinguishable from a live one, and (level, vertex) is
two 32-bit numbers. Sixty-four is what the two existing non-fixed
representations already are. A template on the identity type was the
alternative and it is worse in the exact place this change exists to improve:
`compute_automask`, the write-region report and the weight composition would
each be instantiated three times, which is three copies of the thing whose
single-copy-ness is the point. The cost of the choice is four bytes per workset
entry — a workset is footprint-sized, a few thousand entries, so single-digit
kilobytes — against a per-representation copy of the automask.

**Why `write_region()` widens rather than projects.** It has two callers:
`MultiresSculptor` (which reads level weld classes) and one assertion in
`test_mesh_sculpt.cpp`. Keeping the `std::uint32_t` spelling would mean a
second array holding the same information in a narrower type, which is a second
answer to "what did this stamp write" — the exact failure `sculpt_workset.h`
was created to prevent. The C ABI and both bindings do not touch it, so the
widening is source-visible to two call sites and nothing else.

**Where the reverse map goes.** `SculptWorkset::slot` — item identity to
workset index — CANNOT be neutral: it is a dense array keyed by weld class,
vertex slot or level vertex, and its size is the representation's, not the
workset's. It stays, typed `std::vector<std::uint32_t>` and OWNED BY THE
ADAPTER, sized and reset by whoever filled the workset. The neutral code never
READS a neighbour through it; it asks the topology adapter (D6) instead.

*Refined during implementation.* The composition has to PUBLISH into that map —
twice, since the automask's topology reads a ring neighbour's membership through
it and a second drop invalidates it — and splitting `compose_workset` in two so
that a caller could publish between the halves would have made "the one step all
three walks end in" two steps with a representation-specific one wedged between
them. It publishes directly instead, keyed by `WorkItemId::key()`: the weld
class, the vertex slot and the level vertex are each the LOW 32 bits of the id,
which is why the encoding puts the generation and the level in the high half.
One spelling, `slot[item.key()] = i`, is correct on all three. Sizing and
resetting stay with the adapter, which is the half that actually needs to know
how big the array is.

### D3 — The arena is a bump allocator for trivially destructible scratch, one per sculptor, and nothing else

**The choice.** `BrushScratchArena`: one owned byte block, `allocate<T>(count)`
returning a `T*` from a bump pointer, `reset()` returning the bump pointer to
zero and keeping the block, plus `capacity_bytes()`, `high_water_bytes()` and
`growths()`. `static_assert(std::is_trivially_destructible_v<T>)` in
`allocate`. A member of each sculptor. Never a process-global.

**Why a static_assert rather than destructor tracking.** `reset()` runs no
destructors — that is what makes it a pointer store instead of a walk — so a
type that owns memory would leak silently, once per stamp, at pointer rates.
Tracking destructors would make the arena a general allocator, which is a
different and much larger thing than the one this needs to be. Refusing the
type at compile time costs nothing and cannot be got wrong at runtime. The
scratch this serves is `float`, `std::uint32_t`, `kernel::cfloat3`,
`WorkItemId`, `FaceId`, `VertexId` and `std::pair<float, std::uint32_t>` —
every one of them trivially destructible.

**Why one per sculptor and never global.** Three sculptors can be live at once
— `MultiresSculptor` OWNS a `MeshSculptor` — and a document can hold several
mesh layers. A shared mutable arena would make two stamps on two layers alias
each other's scratch, and would make the sculptor's cost depend on what else
the host is doing. It would also be a data race the moment a host stamps two
layers on two threads, which nothing in the current design forbids.

**What the arena does NOT replace.** The sculptors' persistent buffers —
`region_positions_`, `nb_offsets_`, `SculptScratch::smoothed`, `displacement_`
— stay `std::vector` members. They already never allocate after warm-up, they
carry their contents ACROSS the arena's reset boundary in the multi-pass
kernels, and moving them into the arena would buy nothing and cost the one
property the goldens depend on: that nothing about where a float lives changed.
The arena is for what is transient WITHIN one stamp and is a `std::vector`
local today.

**What it must serve**, from the requirement and from the measured allocation
sites: the automask's `depth`/`frontier`/`next`/`reached`/`stack`; the adaptive
region's sort permutation and its two sorted copies; the ball query's face
list; `write_positions`' incident faces; `refresh_normals`' touched-vertex
list; the neighbour normals a polish stamp needs; and the topology candidates
the remesher hands back.

### D4 — `mesh::BrushFrame` keeps its name. The new basis is `StampFrame`

**The collision.** `mesh::BrushFrame` is an ENUM naming the direction a kernel
displaces along — `RegionNormal`, `VertexNormal`, `StrokeDirection`,
`RegionPlane`, `None`. The guide's `BrushFrame` is an orthonormal basis. The
tree also already has `mesh::SurfaceFrame` in `surface_frame.h`, a
tangent/bitangent/normal triple, which is the frame a multires detail
coefficient is measured in.

**The choice.** Keep the enum's name. The new type is `mesh::StampFrame`
(`origin`, `normal`, `tangent`, `bitangent`, `rotation`), built by
`make_stamp_frame(origin, normal, azimuth, explicit_rotation)` and sampled by
`stamp_uv(frame, p, extent)`, in a new `mesh/stamp_frame.h`. Each of the three
headers gains a paragraph naming the other two, so a reader who meets one
"frame" knows there are three and what separates them.

**Why not rename the enum.** Its value is SERIALIZED: `BrushPreset::serialize`
writes `model.frame` as a byte at version 1, and `src/brush/preset.cpp:171`
reads it back with `static_cast<mesh::BrushFrame>`. It is mirrored in the
public C ABI as `clay_brush_frame` with five `CLAY_BRUSH_FRAME_*` enumerators
and the `frame` field of `clay_brush_model_desc`. Renaming the C++ half alone
produces the worst outcome available — two names for one axis, differing across
the ABI boundary. Renaming both halves breaks source compatibility for every
host that spells `CLAY_BRUSH_FRAME_REGION_NORMAL`, in exchange for a spelling.
The enum is also not badly named for what it is: it selects which frame the
displacement is taken in, and `BrushModel::frame` reads correctly.

**Why not reuse `SurfaceFrame`.** Same shape, different contract, and the
contract is the whole of that type: a detail frame is TRANSPORTED and never
rebuilt, because rebuilding it rotates the detail stored in it. A stamp frame
is rebuilt every stamp by definition. Sharing the struct would put two opposite
rules on one type, and `surface_frame.h` pulls in `subdivide.h`, which would
drag the hierarchy into the fixed sculptor's include graph.

**`AlphaFrame` becomes a projection.** `alpha_frame_for(settings, fallback)` is
reimplemented as `make_stamp_frame` plus the extent, and the result is
bit-identical by construction rather than by measurement: both paths call
`kernel::calpha_frame(dir, settings.alpha_tangent, &n, &t, &b)` exactly once
with the same two arguments, and `alpha_at`'s `u`/`v` arithmetic is unchanged.
`AlphaFrame` itself is kept — it is the cached record `alpha_at` reads, and
`extent` is a property of the stamp rather than of the basis.

### D5 — A zero azimuth takes no rotation, and that is a correctness rule

`make_stamp_frame`'s azimuth rotates `tangent` and `bitangent` in the plane.
The default is zero and the overwhelming majority of stamps will pass zero.

**The implementation SHALL branch on `azimuth == 0.0f` and return the
unrotated basis**, rather than evaluating `t·cos θ + b·sin θ` with `cos 0 == 1`
and `sin 0 == 0`.

This looks like a micro-optimisation and it is not. `1.0f * x == x` and
`0.0f * y == 0` hold, but `x + 0.0f` is NOT the identity when `x` is `-0.0f`:
it produces `+0.0f`. A sign bit flips, `alpha_at`'s dot product can land on a
different bilinear sample at a texel boundary, and a golden moves. The same
class of argument the weight composition already makes about multiplying an
identical 1.0 in last, taken one step further because addition is involved.
`explicit_rotation`, when supplied, replaces the azimuth rather than composing
with it, so there is one path and not a product of two.

### D6 — The automask splits into a neutral core and a topology adapter

**The problem.** `compute_automask` takes `const Mesh&` and `const Adjacency&`.
Three of its five factors do not need them: NormalAngle reads
`workset.normals`, Cavity and SurfaceGroup call caller-supplied functions on
`workset.positions`. Two do: Boundary spreads over the ring and asks
`is_boundary_class`, TopologyConnected floods the ring from the seed.

**The choice.** A `WorkItemTopology` interface with exactly two queries, both
answered in WORKSET SLOTS rather than in representation identities:

```
struct WorkItemTopology {
    // The workset slots of item `slot`'s ring that are themselves in the
    // workset. Neighbours outside it are not reported: the automask's two
    // topological factors spread over the workset alone, by construction.
    virtual void ring_slots(std::uint32_t slot, ScratchVector<std::uint32_t>* out) const = 0;
    virtual bool on_open_border(std::uint32_t slot) const = 0;
};
```

`compute_automask(const WorkItemTopology&, const SculptWorkset&, ...)` is the
neutral core. The existing `compute_automask(const Mesh&, const Adjacency&,
...)` overload stays as the fixed adapter, so no caller changes and the C ABI
sees nothing.

**Why a virtual here, when `sculpt_kernels.h` refused one.** That refusal was
specific and its reason does not apply: a neighbour callback would have been a
virtual call in the innermost loop of a smoothing pass, per neighbour, per
pass, at pointer rates on a million-vertex surface. This is one virtual per
workset entry, on two of five factors, in a pass that already calls `acos` per
entry and a caller-supplied `std::function` per entry for cavity. The
alternative — a template on the topology — reinstates the three instantiations
D2 rejected.

**Why bit-parity survives it.** The factor arithmetic is untouched: the same
`fade`, the same smoothstep, the same multiplication order, the same
`out[i] = 0.0f` for a masked-out entry. Only neighbour ENUMERATION moves behind
the interface. Both topological factors are set-valued — BFS depth is assigned
per level and a flood's `reached` set is order-independent — so even a
different ring ORDER could not move a value. The adapter preserves the order
anyway, because a property that holds for a reason is worth not relying on
twice.

### D7 — The workset builders live beside their representations; only the composition is neutral

The guide asks for `build_fixed_mesh_workset`, `build_dynamic_surface_workset`
and `build_multires_workset`. They are added under those names, and each is
declared in the header of the representation it serves — the fixed one in
`sculpt.h`, the adaptive one in `dynamic_sculpt.h`, the hierarchy's in
`multires_sculpt.h`.

**They are NOT declared in `sculpt_workset.h`.** That header states a rule
about itself — "nothing here may name a `Mesh`, an `Adjacency`, a `Bvh` or a
vertex index" — and `build_dynamic_surface_workset` names a `DynamicSurface`.
A neutral header holding three signatures that are each specific to one
representation is neutral in the directory listing only.

What IS neutral, and what goes in `sculpt_workset.h`, is the step all three
end with: `compose_workset(...)` — take the walked candidates with their
straight-line and path distances, compose the five weight factors in the one
fixed order, drop the entries that reach zero, run the automask through the
supplied topology, drop again, and resolve `average_normal` / `centroid` /
`plane_point` / `plane_normal`. That is the code whose duplication is the
defect, and its signature names a `SculptWorkset`, a `WorkItemTopology`, a
`MeshBrushSettings`, a `MaskGate` and an arena — no representation.

**Why not extract the whole gather.** Because the walk IS the representation.
The fixed one walks weld classes through `Adjacency`; the adaptive one walks
half-edges through a mutable pool whose slots retire under it; the hierarchy
does not walk at all, it delegates to the bound level's `MeshSculptor`.
Unifying those would be a rewrite of three working walks against a golden that
must not move, for no shared line at the end of it.

`build_multires_workset` is therefore thin and is still worth having: it takes
the bound level sculptor's workset and re-tags its items as
`WorkItemId::level_vertex(level, v)`, which is what makes a multires write
region reportable at the hierarchy's own addressing instead of at the level
mesh's.

### D8 — What "parity across representations" can mean, and what it cannot

This is the decision the parity gates stand on, and getting it wrong produces
either a test that cannot pass or a test that passes vacuously.

**It cannot mean byte-equal stamps in general.** The three representations
compute vertex normals by different estimators and neither is wrong:
`class_normal` (`src/mesh/sculpt.cpp:55`) is angle-weighted, summing face
normals scaled by `corner_angle`, which reaches `acos`;
`DynamicSurface::compute_vertex_normal` averages the incident face normals the
surface already caches. Any verb that reads a normal therefore differs between
them at the last bits and often further, before a single line of this change is
written. Their geodesic walks also differ — weld-class ring versus half-edge
ring — so path distances differ, so the taper differs.

**So parity is asserted at four levels, and the fourth is a DIFFERENCE.**

- **P1 — the kernels.** Given an identical `SculptSnapshot` and
  `SculptNeighbors`, every kernel produces byte-identical displacements
  whoever built them. Already gated by `test_sculpt_kernels.cpp`; the parity
  files extend it to a snapshot built by each of the three adapters.
- **P2 — the weight.** For an identical position, distance, path distance,
  gate value and alpha, `compose_weight` returns the same bits on every
  representation, factor for factor and in the one order. Asserted directly on
  the composition, not inferred from a stamp.
- **P3 — an identical vertex set, an identical stamp.** On a plane grid
  converted with `DynamicSurface::from_mesh` (topology disabled) and used as a
  multires cage at level 0, the three hold the same vertices at the same
  positions. A Grab or Nudge stamp with an explicit `direction`, a Euclidean
  footprint and no automask reads NO normal, so all three must write
  byte-identical positions. This is the strong gate and it is the one that
  would catch a re-associated weight in the shared composition.
- **P4 — the named differences.** Draw on the same fixture MUST differ between
  fixed and adaptive, because the normals differ; the parity test asserts that
  it differs and that it differs by less than a stated bound. A gate that only
  checked agreement would be satisfied by two representations that had become
  equally wrong, and a divergence that suddenly vanished would mean an
  estimator had been silently unified — which is a real change to what a brush
  does and must not land unnoticed.

The automask gets its own row under P3: with `NormalAngle` off and
`Boundary | TopologyConnected` on, the factor is topological and set-valued, so
it must agree EXACTLY across representations on the identical fixture. That is
the assertion that proves the automask actually reaches the adaptive path,
rather than being present and returning ones.

### D9 — The C ABI takes the arena statistics and the azimuth, and nothing else

`clay_mesh_brush_desc` gains one appended field, `stamp_azimuth` (radians, 0 =
no rotation, which is the identity path D5 requires), guarded by the existing
`struct_size` rule so a host compiled against minor 74 is unaffected.

Three new functions report the arena: `clay_mesh_sculptor_arena_stats`,
`clay_dynamic_sculptor_arena_stats`, `clay_multires_sculptor_arena_stats`, all
filling one `clay_brush_arena_stats { capacity_bytes, high_water_bytes,
growths }`.

**Why the statistics cross at all.** The library's stated target kills an app
for memory rather than warning it twice — the reasoning `add-mesh-multires`
gives for its preflight — and a per-stroke scratch high-water is a number such
a host budgets against. It is also what lets `examples/70` assert the arena's
claim from Python without a C++ allocation counter, which matters because the
allocation gate is a test-binary technique (it replaces `operator new`) and
cannot be shipped to a host.

**What deliberately does not cross.** No arena tuning knob — no reserve, no
cap, no growth factor. Every one of those is a number a host would tune against
its own device and then be wrong about after a footprint change, and the arena
already sizes itself from the largest recent footprint, which is the
measurement the knob would be guessing at. `WorkItemId` does not cross either:
the C write-region transport is per representation today and each one already
speaks its own identity.

**No new automask surface in C.** The four automask fields are already in
`clay_mesh_brush_desc` and `clay_dynamic_sculptor_stamp` already takes that
descriptor. The fix is that the adaptive path stops ignoring them, which needs
no new function — which is exactly why the gap was invisible.

## Risks / Trade-offs

- **The goldens are the whole gate and they are silent when they pass.**
  Mitigated by ordering: the arena and `WorkItemId` land before the automask
  work, each in its own commit, each with the parity suite run — so a moved
  golden is attributable to one change rather than to five.
- **The neutral automask's virtual is a real call in a real loop.** Bounded by
  measurement rather than by argument: the allocation gate says nothing about
  time, so a benchmark case comparing a boundary-automasked stamp before and
  after is part of the work, and the number goes in the task.
- **The arena can hide a leak of the opposite kind** — scratch that grows every
  stamp and is never reset, which allocates nothing after warm-up and consumes
  memory without bound. `high_water_bytes()` and `growths()` exist so a test
  can assert the arena STOPS growing, not merely that a stamp stopped
  allocating.
- **Widening `write_region()` is a source break** for anyone outside this
  repository who reads it. Two call sites here; the C ABI and both bindings do
  not touch it. Taken rather than carrying a parallel narrow array.
- **`DynamicSculptor` gaining an automask changes its output** where a host was
  already setting factors it had no effect from. That is the fix, and it is
  stated in the change rather than shipped as a silent improvement.

## Migration Plan

Additive at the C ABI (one appended field, three new functions, minor 74 → 75).
Source-visible in C++ at exactly two places: `write_region()`'s element type
and `SculptWorkset::classes` becoming `items`. `BrushRegion` stays as the alias
it already is. `DynamicSculptor::last_region()` returns `VertexId`s today and
keeps doing so, projected out of the workset, because a caller asking the
adaptive sculptor what it reached wants the adaptive surface's own handles.

## Open Questions

- Whether `compose_workset` should take the arena or borrow the caller's
  scratch. Taking it is simpler and makes the neutral half own its own
  transients; borrowing keeps the arena entirely inside the sculptors.
- Whether `ring_slots` should hand back a span into the arena or fill a
  caller-owned buffer. The span reads better and makes the arena's lifetime
  part of the interface, which may be more coupling than the two callers need.
- Whether the multires parity fixture should sculpt at level 0 only. Level 1
  and above are `Subdivide(parent) + Detail`, so an identical vertex set with
  the fixed mesh exists only at level 0 — P3 is a level-0 claim, and whether
  anything above it is assertable beyond P1 and P2 is unresolved.
