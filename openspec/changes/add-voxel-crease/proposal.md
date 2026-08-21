# Proposal: DamStandard on a voxel layer

## Why

DamStandard is one of the two or three brushes a ZBrush sculptor reaches for
most: the sharp V-groove that cuts wrinkles, lip lines, scars, panel seams and
muscle separations, and — held inverted — raises the crisp ridge instead.

It exists here on two representations out of three:

| | | |
|---|---|---|
| **SDF** | `Op::Incise` | Labelled `(Crease, DamStandard)` in the kernel; relief's exact inverse, a smoothstepped valley whose sharpness is the region width |
| **Mesh** | `MeshBrush::Crease` | The real thing: a cut along the normal PLUS a tangential pull toward the stroke centre, in one stamp, signed so a negative strength raises a ridge |
| **Voxel** | — | **Missing.** Ten verbs and no crease |

The voxel layer is where free-form clay-pushing happens — it is the side the
docs recommend for exactly the organic work a crease is for — and it is the one
that cannot cut a wrinkle.

**Composing it by hand is not the same operation**, and this codebase has
already written down why, twice. `sculpt_scrape`:

> Calling the two verbs in sequence is not the same thing: the flatten's output
> would feed the smooth's neighbourhood, which is exactly what the snapshot
> discipline exists to prevent.

and `verb_crease` on the mesh side:

> Sequenced separately they leave a rounded ditch: the pinch would gather
> vertices the draw had already pushed down, instead of closing the fold as it
> forms.

A carve followed by a pinch is a rounded ditch. A crease is a V. The difference
is that both decisions come from one snapshot of the surface as it was.

## What changes

`VoxelGrid::sculpt_crease(cell, params, normal, depth)` — one snapshot, and a
single `decide` pass that cuts along the normal and steps surface cells toward
the stamp centre, from the same `before` state.

The verb is small because `brush_pass` already carries everything a voxel verb
shares: the footprint shape, the falloff curve, the strength, **the mask gate**
(`weight *= 1 - mask_at(...)`), **the cell-coordinate dither** that makes a
fractional strength a reproducible subset of cells, the parallel split above a
span threshold, and the write ordering through the sink. A new verb writes the
decision, not the machinery.

### Three decisions, recorded rather than left to the implementation

**The normal is explicit, as it is for flatten and scrape.** A voxel grid has
no surface normal to average — occupancy is a yes/no per cell — so the cut
direction has to come from the caller, which gets it from
`clay_voxel_raycast`. That is not a wart; it is the same signature the two
existing directional verbs already take.

**Depth is a separate signed parameter, NOT a negative strength.** `p.strength`
is the dither weight inside `brush_pass`, so a negative one would not mean
"the other way", it would mean "no cells pass". `sculpt_inflate(c, p, amount)`
already settled this — *"amount > 0 dilates, < 0 erodes"* — and a crease follows
it: **`depth > 0` carves, `depth < 0` raises the ridge.** That is the Alt
behaviour, spelled the way this module already spells signed verbs.

**The pinch pulls toward the STAMP centre, radially.** DamStandard's groove is a
line, but the line comes from the stroke engine walking stamps along a path,
not from a single stamp knowing about one. That is exactly how the mesh
`Crease` works (`toward = settings.center - positions[i]`), and it keeps a
single stamp meaningful on its own.

## What it will not do, stated up front

**A valley narrower than a cell cannot exist.** Occupancy is binary, and
DamStandard's whole value is *tight* crevices — so on a coarse grid this
staircases, and no amount of falloff tuning fixes that. The answer is the
resolution level stack: `add_level` to refine where the detail goes rather than
paying for a fine grid everywhere. The pro-tip that DamStandard "requires high
polygon counts" has an exact voxel analogue, and the docs should say so rather
than let a user discover it on a size-8 brush.

## Impact

Additive: one engine verb plus its bindings. Nothing existing changes
behaviour. ABI 0.39.0.
