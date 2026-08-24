# Design: where the seam goes

The whole change is this decision. Everything downstream follows from it, and
the three answers are not subsets of one another — picking the wrong one and
correcting later means an ABI that shipped and has to be lived with.

## Option A — ClayCore bakes

It takes a UV-parameterised low-poly mesh and returns texture maps: normal,
ambient occlusion, colour, height, curvature.

**For:** one call, one answer, nothing for a host to get wrong. It is what
3DCoat and Substance present to a user, and the comparison document names the
texture pipeline as 3DCoat's moat.

**Against, and this is decisive:** it makes this library learn **UV semantics**.
Parameterisation, seams, islands, texel density, padding, dilation, mirrored
shells, overlapping UDIMs. That is precisely what CyberRemesherAndUV exists to
own, and a second implementation of it here will disagree with theirs in the
details that matter — where a seam falls, how much padding is enough. It also
puts a rasteriser in a library whose stated boundary is that it is headless.

**Scope:** large. New module, new dependency surface, image encoding, and a
texture-space rasteriser with all the sampling and filtering that implies.

## Option B — ClayCore answers queries, the baker lives outside (RECOMMENDED)

The bake loop belongs to whoever owns the UV layout. This library supplies the
things only it can: what the field is, at a point, exactly.

Concretely: bounded cage projection, and per-point AO, thickness and curvature —
batched, because a bake is millions of points and a per-point call across an ABI
is not a bake, it is a benchmark of the ABI.

**For:** every one of these is a field query, which is what this library is. It
adds no notion of a UV, no rasteriser, no image format, and no dependency. The
layering table stays honest. It is also the only option that helps a host that
is *not* baking — cage projection is what a snap tool wants, AO and curvature
are what a procedural-mask user has been asking for since
`brush/procedural_mask.h` deferred them.

**Against:** a host still writes the loop, and two hosts will write it
differently. Mitigated, not solved, by shipping the reference loop as an
example rather than as ABI — which is what `reference/host_loop.py` already
does for the sculpt session.

**Scope:** moderate, and mostly measurable work rather than design work.

## Option C — a defined interchange package

ClayCore exports a bundle — high-poly mesh, field snapshot, cage — and
CyberRemesherAndUV consumes it.

**For:** a clean contract between two repositories.

**Against:** it is a *format*, and a format is the most expensive thing to
change. It also freezes a field snapshot, which is the one thing this library is
good at not doing — the whole architecture is that the field is re-evaluable
rather than sampled. And it does not remove the need for B: whoever consumes the
package still needs the queries.

**Scope:** large, and largely wasted if B is built anyway.

## Recommendation

**B**, with the reference bake loop shipped as an example.

The tell that B is right: every query it adds is useful to someone who is not
baking, and every part of A is useful only to someone who is. A library should
grow in the first direction.

If the product decision is that ClayCore must present a one-call bake — because
a host team will not write the loop — then A is reachable **on top of B** later,
and B is not wasted. The reverse is not true.

## What B looks like, sketched

Sketched rather than specified, because it is downstream of a decision that has
not been taken.

**Bounded rays.** A second entry point rather than a parameter, since
`clay_raycast` shipped: `clay_raycast_bounded` taking `tmin`/`tmax`, and the
batch form. A miss inside the bound has to be distinguishable from a hit beyond
it — that distinction is the whole point.

**Cage projection.** Given a point and a direction, search *both ways* within a
distance and return the nearest surface. Both ways, because a low-poly cage
point can sit inside or outside the high-poly and a baker cannot know which.
Returns the signed distance travelled, which *is* the height map value.

**Surface measures, per point and batched.** AO, thickness, curvature, cavity,
convexity. Curvature/cavity/convexity are a stencil away and already implemented
inside `brush::mask_from_surface` — the work is exposing them per point rather
than as a lattice. AO and thickness are ray casts and need their parameters
stated: ray count, maximum distance, falloff, and a seed, because an unseeded
hemisphere sample is not reproducible and this library's determinism guarantee
is not negotiable.

**Cancellable, with progress.** A bake is minutes. `add-operation-cancellation`
already built the mechanism, and the third budget class is exactly this.

## Open questions, to settle before task 2

1. **Does CyberRemesherAndUV need anything ClayCore does not already emit?**
   Nobody here can answer that; it needs the sibling repository. The answer may
   be "no", in which case the "retopo-oriented export profile" half of the
   ROADMAP row closes with a documentation change and nothing else.
2. **Whose cage?** A bake cage is usually a pushed-out copy of the low-poly.
   Does ClayCore generate one, or take one? Taking one is smaller and matches
   B.
3. **AO on a field, or AO on a mesh?** Cheaper on the field and exact, but a
   host baking for a game engine may want AO that matches what the *mesh*
   occludes, which is a different number. Possibly both, and if so it is two
   entry points and not a flag.
4. **Determinism across backends.** Every existing query is bit-identical across
   backends and there is a parity fixture enforcing it. A hemisphere sample with
   a seed can hold that line; it has to be stated as a requirement up front
   rather than discovered to be broken later.
