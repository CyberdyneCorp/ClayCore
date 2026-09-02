# Design: add-mesh-weld

## Decisions

### 1. A named verb, not a repair inside the conversion

`DynamicSurface::from_mesh` states its policy in the header: *"Refuse rather
than repair. A conversion that quietly drops a face is a conversion the caller
cannot reason about."* That is a good policy and this does not weaken it.

Repairing inside the conversion would fix exactly one caller. The marcher would
go on emitting two per cent zero-area triangles into every exporter, every BVH,
every decimator and every file. A named verb fixes it once for all of them, and
`from_mesh(weld(m))` reads as what it is rather than hiding a repair inside a
conversion.

### 2. Through `Adjacency`, not beside it

The spatial hash, the 27-cell neighbourhood search and the exact-bit path all
already exist in `adjacency.cpp`. Duplicating them would give the library two
answers to "are these the same vertex", and the caller most likely to notice is
the one that welds a mesh and then builds an `Adjacency` over it at the same
epsilon — which must see nothing left to weld.

### 3. Attribute splits are preserved by default

A UV seam is duplicated positions carrying different UVs. That is not a defect
to be cleaned up; it is how a flat mesh represents a seam at all. Merging across
one destroys the layout silently, which is precisely the loss
`mesh/transfer.h` spends a paragraph explaining it cannot refund.

So the default refuses to merge two vertices whose UVs or colours disagree, and
a caller who genuinely wants one flattened mesh asks for it by name. The
marching case this verb exists for is unaffected: a marcher emits no UVs.

### 4. A no-op is byte-identical, and an out-of-range index is not a no-op

Renumbering a mesh that needed nothing done to it would invalidate every index a
caller was holding for no reason, so the clean case returns untouched.

But "nothing to merge" is not the same as "nothing to do". A triangle naming a
vertex that does not exist has to go regardless, and the first version of this
took the fast path whenever no vertices merged — which meant `weld` sanitised
indices only when it happened to be doing other work. The test caught it. Now
the fast path requires both, so "every index is in range afterwards" is true
always rather than usually.

### 5. What this does not do

**It does not change the marcher.** Not emitting zero-area triangles is the root
fix, and it moves the mesher's output bit for bit — `to_field` has golden hashes
downstream and the mesh sculpt golden tables are per platform. That is a
re-baselining exercise worth doing when something other than this needs it.

**It does not change `validate`.** A marched mesh reports `clean()` while
carrying zero-area faces, because `degenerate_triangles` means repeated INDICES
and `sliver_triangles` is informational. Whether `clean()` should account for
slivers is a separate question about a widely-depended-on predicate.

## Open questions

- **Whether welding should be part of meshing's own output.** Every marcher
  could weld before returning, which would make the defect disappear at source.
  It would also change every mesher's output and make an operation that is
  currently O(cells) additionally O(vertices) with a hash. Not obviously wrong;
  not this change.
