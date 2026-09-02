# Proposal: add mesh welding

## Why

**The default mesher emits zero-area triangles.** Measured on a plain analytic
sphere at a 0.02 lattice: 1458 of 70,140 triangles — two per cent — have two
corners at BIT-IDENTICAL positions. `mesh_tape`, the ordinary document meshing
path, does the same. So does `voxel_remesh`, less often.

Nothing had noticed, and the reasons are worth stating because they are why this
went unfound for the life of the mesher:

- Everything downstream tolerates them. An exporter writes them, a BVH holds
  them, the decimator removes them on its own account.
- `mesh::validate` counts them as SLIVERS, not as degenerates — its
  `degenerate_triangles` means *repeated indices*, and these have distinct
  indices at identical positions. A marched mesh therefore reports `clean()`
  while carrying them.

`mesh::DynamicSurface` cannot tolerate them. A half-edge surface has no way to
express a face with two of the same vertex, and `from_mesh` refuses rather than
silently dropping it — correctly, and with a consequence nobody had hit: **no
mesh this library marches could be converted to an adaptive surface at all.**
`add-voxel-remesher` documented a `to_mesh` → remesh → `from_mesh` round trip in
three places, and it did not work.

The reason it went unhit is that no caller had tried. The dynamic-topology
example builds its input analytically, so the two subsystems had never met.

## What changes

- **`mesh::weld`** — merge coincident vertices, remove the triangles that
  collapses, compact, and report what happened. Reuses `Adjacency`'s existing
  class assignment rather than adding a second answer to "are these the same
  vertex".
- The C ABI and pyclay surfaces, and the layer geometry revision bumped when a
  weld actually changes something (a weld rewrites the triangles, so it is as
  invalidating as a rebuild — and a weld that changed nothing must not
  invalidate a live sculptor).
- **A correction** to `remesh-through-the-document`'s design note, which
  recorded the fix as "pass `weld_epsilon = 0`". That was wrong: the
  bit-identical corners are refused at any epsilon including zero. It appeared
  to work only because the voxel remesh emits none of them.

## Approach

The fix belongs in a named verb rather than inside `DynamicSurface::from_mesh`.
That conversion states its policy explicitly — *"Refuse rather than repair. A
conversion that quietly drops a face is a conversion the caller cannot reason
about"* — and that policy is right. Repairing there would fix one caller and
leave the marcher emitting degenerate faces into every other one; a named verb
fixes it once, keeps the policy, and makes the cleanup visible at the call site.

`from_mesh(weld(m))` reads as what it is.

## Non-goals

- Changing the marcher. Not emitting these in the first place is the root fix
  and it moves the mesher's output bit for bit, which the golden hashes
  downstream of `to_field` and the sculpt golden tables would all have to be
  re-baselined for. Worth doing when something other than this needs it.
- Changing `DynamicSurface::from_mesh`'s refusal, its default epsilon, or
  `validate`'s sliver-versus-degenerate distinction.
- Decimation. `weld` removes what has no area; it does not simplify what does.

## Impact

`meshing` gains the verb. `c-abi` and `python-bindings` gain the surface.
Nothing existing changes behaviour: a mesh with nothing to merge comes back
byte-identical, and no existing caller welds.
