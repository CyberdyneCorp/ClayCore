# Proposal: taper, twist and bend on a mesh layer

## Why

`Deformer` has twenty-one entries and every one of them applies to an SDF item.
A mesh layer takes a lattice cage and nothing else, so ZBrush's Deformation
palette — Taper, Twist, Bend, the transforms an artist reaches for to shape a
blockout — is unreachable on the representation an artist is holding after a
retopo pass or an import.

The workaround today is to convert the mesh into an SDF item, deform it, and
mesh it back. That is a workflow break and it is lossy in both directions: it
resamples the surface, and it discards the vertex colours and UVs that are the
reason to hold a mesh layer rather than a field.

### The part that inverts the intuition

Mesh deformers look like the expensive option and are the cheap one. Our own
kernel says why, in `deform.h`:

> The cage's control-point offsets **ARE THE INVERSE WARP**, which is the whole
> design decision. Forward FFD has no closed-form inverse, and a claycore
> deformer must run backwards: it answers "where did the material at p come
> from". […] What that costs, stated rather than implied: this is NOT the exact
> inverse of forward FFD. […] the difference is under 1.5% of the drag.
>
> Cost per sample is nx * ny * nz multiply-adds, which is why divisions are
> capped low here **and not on the mesh lattice: that one evaluates once per
> vertex**.

An SDF deformer must run BACKWARDS, which is the hard direction — and we
already pay for it: the SDF lattice is capped at 4³ control points and carries
a ~1.5% error against the forward cage. A mesh deformer runs FORWARDS, once per
vertex. `taper`'s inverse is `p.x / s`; forward is `p.x * s`. `twist` and
`pose` rotate by `-angle * w`; forward is `+`.

So this is not a port of the SDF deformers to a harder setting. It is the same
math in the **easier** direction, and it does not inherit the approximation the
SDF side accepts.

## What changes

`MeshSculptor::apply_deformer` — one forward-map walk over weld classes,
modelled directly on `apply_lattice`, which already does exactly this for the
cage. **Two deformers**, both of which have an EXACT forward map:

- **taper** — scale across a span, along the frame's axis
- **twist** — rotate about the axis proportionally to distance along it,
  including the ranged form that holds beyond a span

### Why bend is not in this change

Bend was in the first draft of this proposal and the precondition check
removed it, which is what the precondition was for.

`taper` and `twist` compute their scale and angle from the coordinate ALONG the
axis, and neither map moves that coordinate — so the forward map is the same
function with the scale reciprocated or the angle negated, and a point round
trips to within float epsilon (measured: 1.2e-07 and 2.4e-07).

`cbend_point` computes its angle from `p.x` and then MOVES `p.x`. Negating `k`
is not its inverse — measured worst error **1.73**, not 1e-7. Worse, the map is
not injective: swept along `y = 0`, rest points at `x = -1.74` and `x = +1.75`
land **0.0101 apart** at `k = 0.9`, and `x = ±0.63` land 0.0053 apart at
`k = 2.5`. **The bend folds over, so no forward map exists** past gentle angles,
and a fixed-point inversion diverges exactly where the fold begins (converges
at k = 0.3, diverges from k = 0.9).

Where the inverse does exist, the closest cheap candidate — `cbend_point(q, -k)`
— is still 0.05 off at k = 0.2 and 0.11 at k = 0.3.

So a mesh bend has three possible shapes, and choosing between them is a
decision about the SDF bend's convention rather than about this change:
iterate and refuse past the fold (a verb that stops working at 50° is a bad
verb), use the standard DCC bend and accept that a bent mesh and a bent field
differ, or change the SDF bend to be authored forwards. Recorded in the roadmap
with these numbers; not decided here.

### Five decisions the implementation has to make

**Point warps only.** `ctape_deform_point` and `ctape_deform_offset` are
already two different functions, and the split is exactly the right scope
boundary: `noise`, `blob`, `alpha` and `displace` modify a DISTANCE, and a mesh
has no distance to modify. Their mesh equivalent is displacement along the
vertex normal, which `Inflate` and the alpha-modulated brushes already are.
`elongate` sits in both and only its point warp carries over.

**Frame-relative, like a gizmo.** The kernel's canonical `twist` and `taper`
are Y-axis maps; an SDF item gets other axes from its own transform. A mesh
layer has no item transform to borrow, so the deformer carries a frame and the
warp happens in its local space. That is ZBrush's gizmo model and it is the
only way "taper along this limb" is expressible.

**The whole mesh, gated by the mask — not a brush region.** ZBrush's
Deformation palette applies to the SubTool and respects masking, and that is
the right model: a taper is a statement about the form, not a dab. The mask
scales the warp, so a masked region interpolates back toward its rest position
by the rule every other verb already follows.

**By weld class, never by raw vertex.** `apply_lattice` states the reason and
it applies unchanged: position-coincident vertices holding a hard edge must
stay coincident, and evaluating each copy separately agrees only up to float
rounding — a seam that opens by an ULP is a visible crack.

**Topology is untouched**, as it is for every mesh verb: `indices` and `quads`
come out byte for byte, and `VertexDeltas` records the gesture so it reverts
bit-identically.

## What this deliberately does not do

**No remeshing, and no DynaMesh pipeline.** ZBrush needs DynaMesh because its
deformers wreck topology and it has voxel remeshing to repair it. The roadmap
already decided against that — "Topology-CHANGING mesh sculpting — dyntopo,
multires, remeshing, subdivision… not this engine's fight" — and its
2026-08-14 amendment draws precisely this line: *"Moving the vertices that
already exist is a different claim… tessellating new ones stays out."* A mesh
deformer moves vertices that already exist, so it is on the near side of that
line; a remesher is not.

The honest cost of that, stated rather than discovered later: a heavy deform
stretches the triangles it has and there is no re-tessellation to recover.

**And `Relax` does not rescue it**, which the first draft of this proposal
assumed it would. Measured in `examples/57`: after a taper to 0.18, six relax
passes move edge-length variation from 0.2929 to 0.3050 — slightly *worse*. The
reason is that a taper leaves the top ring with the same vertex count around a
smaller circumference, so the damage is ANISOTROPY rather than uneven spacing,
and relax slides vertices without changing how many a ring has. Relax is the
recovery for a large `grab`, where the damage really is uneven spacing. There
is no recovery for this one short of re-tessellation.

`grab`, `pose`, `pose_line` and `magnify` are excluded: each already has a mesh
brush that is the same gesture with a brush's falloff (`Grab`, `Pinch`), and
two spellings of one operation is what the brush vocabulary has avoided
throughout. `wrap_around` and `elongate` are deferred rather than rejected —
they are the same loop, and they can follow once the frame convention has been
used in anger.

## Impact

Additive. New entry points on the mesh sculptor in both bindings; no signature
changes and no descriptor growth. ABI 0.38.0.
