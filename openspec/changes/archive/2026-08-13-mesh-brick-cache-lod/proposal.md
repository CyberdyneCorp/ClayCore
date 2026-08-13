# Proposal: the LOD a meshing host can build but cannot use

## Why

Issue #93, from ClaySpaceDesktop. The brick cache can BUILD a mip
(`clay_brick_cache_build_mip`), READ one (`clay_brick_cache_read_bricks` takes
an `int32_t lod`) and REPORT one (`clay_brick_cache_current_lod`). Nothing
meshes one: `clay_brick_cache_mesh` takes a key list and no level.

So the half of LOD that shipped is the half a meshing host cannot use on its
own. `docs/06` recommends meshing the cache for most hosts; such a host can
build a coarse level and then has nowhere to send it. Its only route to coarse
triangles is to march the fp16 samples by hand, which means reimplementing the
mesher — the same lattice, the same band clamping, the same straddler
attribution fixed in #66 — and that is precisely what a host adopts the brick
cache to avoid. LOD is therefore reachable only by a host that RAYMARCHES the
bricks. That is a legitimate architecture and it is the one `docs/06` leans
toward, but it leaves the meshing host with no coarse path at all rather than a
slower one.

## What the investigation found

**A thin plumbing change, not work in the mesher.** The mip is not a separate
representation: `BrickCache::build_mip` keeps every second lattice point of the
2x2x2 block it covers, so a coarse brick is the cache's own `dim³` lattice, its
coarse lattice coordinate `g` sitting at world `g * 2 * voxel_size` — the same
lattice, anchored at the same origin, at twice the spacing. Marching consumes
it directly. A level therefore changes exactly two things in `mesh_bricks`: the
spacing doubles, and the keys are the COARSE keys `build_mip` and
`current_lod` already take. The march, the seam welding, the ranges and the
straddler attribution are the ones level 0 uses, unchanged.

The two things that were missing were both enumeration: `BrickCache` could
sample and enumerate level 0 only. `sample_lod` (which `sample` now is at level
0) and `surface_bricks_lod` supply the level-aware halves, and `find_lod`
already existed for the readback path.

## What changes

- `mesh::mesh_bricks` gains a trailing `int lod = 0`. A default argument rather
  than an overload: every existing call site compiles unchanged, and the
  parameter belongs in the same appended-optional group as `keys`,
  `out_ranges` and `cull_index`, each of which arrived the same way.
- `clay_brick_cache_mesh_lod`, `clay_brick_cache_mesh` plus an `int32_t lod`
  placed immediately before the key list it reinterprets — the position `lod`
  holds in `clay_brick_cache_read_bricks`.
- `BrickCache::sample_lod`, `BrickCache::surface_bricks_lod` and
  `BrickCache::mip_count`.

## A sibling, not another arity change

`close-webgpu-host-abi-gaps` changed `clay_brick_cache_mesh`'s arity rather
than adding a `_subset` sibling, on the grounds that "two entry points
differing by one nullable argument would be two ways to say one thing." That
reasoning does not carry here, and this change adds a sibling instead:

- **A level is not a nullable argument, it is a mode**, and it changes what
  another argument MEANS: at lod 0 `keys_xyz` names fine brick keys, at lod 1
  it names coarse block keys. Two entry points whose key list means different
  things are not two ways to say one thing.
- **The refusals differ.** At lod 1 a named key with no lattice is an error
  ("not built"), where at lod 0 it is an ordinary uniform brick; and colours
  and gradient normals are refused. A single signature would carry a set of
  rules that only applies to one of its values.
- **Releases since 0.24.0 have been additive**, and the house rule is that an
  existing exported call keeps its signature. The 0.22 break was defensible
  because there was no behaviour to preserve — `clay_mesh_load` gained an
  optional budget. Here `clay_brick_cache_mesh(...)` is exactly
  `clay_brick_cache_mesh_lod(..., 0, ...)`, so keeping it costs one forwarding
  line and buys every 0.29 consumer a recompile they do not need.

Both entry points share one implementation, so the older one is the newer one
at lod 0 by construction rather than by two bodies agreeing.

## Two rules the level brings, both mirrored rather than invented

**An unbuilt level is `CLAY_ERROR_NOT_FOUND`, not an empty mesh.** An empty
mesh already means "no surface bricks", which the header calls an ordinary
state of a session. If a level that was never built answered the same way, a
host could not tell a missing mip from a missing surface — and the fix for one
is `build_mip` while the fix for the other is sculpting. So a named coarse key
with no valid mip is refused, and a whole-level request is refused when the
cache holds surface bricks but not one mip. A cache with nothing in it at all
still meshes EMPTY at every valid level, because there is nothing to be
mistaken about.

**Colours and gradient normals are level 0 only**, refused rather than
downgraded. `close-webgpu-host-abi-gaps` already decided that a mip carries no
colour: averaging RGBA8 over a 2x2x2 block is a filtering policy, and
`read_bricks` reports rather than picks one. Meshing adds a second reason that
covers gradient normals too. Both attributes ride per-brick CULLED tapes, and
their exactness argument is that a vertex sits on the FIELD's surface, deep
inside the band where a culled tape and the whole document's agree bit for bit.
A coarse vertex sits on the MIP's surface, up to most of a coarse cell away,
where the two tapes are only both-out-of-band rather than equal. The numbers
would be silently approximate, which is the failure the lod > 1 rejection and
the apron ceiling both exist to prevent. `CLAY_NORMAL_FACE` comes from the
triangles, needs no field, and works at every level.

`lod > 1` is rejected rather than clamped, the rule
`close-webgpu-host-abi-gaps/design.md` states for `read_bricks`.

## What this change does not do

- **No second mip level.** There is one, `build_mip` builds one, and this
  change meshes the one that exists. A level pyramid is a separate decision
  about storage and invalidation, not about meshing.
- **No mip building policy.** Which coarse keys to build and when stays the
  host's, as it already is: the cache publishes `build_mip` and
  `current_lod` and owns no scheduler.
- **No change to lod 0.** Proven, not asserted — see the regression suite and
  the fingerprint diff in the tasks.
- **No persistence.** The brick cache is not serialized; there is no format
  change and nothing to round-trip through save/load.

## Impact

`c-abi` gains the level-meshing requirement, `meshing` the statement that the
mesher marches a level, `brick-cache` the level-aware sampling and enumeration.
Docs: `docs/08-mesh-readback.md` (the route table and the attribute table) and
`docs/RELEASE.md`. pyclay is unaffected — it does not reach the brick cache,
which `tools/check_binding_parity.py` already records with its reason.
