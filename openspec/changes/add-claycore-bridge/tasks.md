# Tasks: add-claycore-bridge

**Section 1 is a measurement and a decision pass. Nothing below section 1 is
started until 1.6 is answered**, because the three options imply different ABIs
and are not subsets of one another.

## 1. Establish what is missing, and decide the seam

- [x] 1.1 VERIFIED against the tree, not assumed: the ROADMAP row's
      "field-evaluation callback so a baker can sample exact normals" is
      ALREADY SHIPPED. `clay_eval_points` gives batched distances and colour,
      `clay_eval_gradients` gives unit-length normals by the tetrahedron trick,
      both on any backend and both with per-layer forms. The row is stale and
      the proposal starts from that rather than building it twice
- [x] 1.2 VERIFIED shipped: quad export at a target density, re-import with
      byte-identical `indices`, fixed-topology mesh sculpting, and attribute
      transfer with a fall-back report. The round trip exists; what is missing
      is what happens at the far end of it
- [x] 1.3 VERIFIED absent: no bounded ray (`clay_raycast` exposes no tmax, so
      "search 5 mm along this normal" cannot be said, and a miss is
      indistinguishable from a hit on the far side of the model); no AO; no
      thickness; no per-point curvature; no bake entry point; no notion of a UV
      layout anywhere in the library
- [x] 1.4 CONFIRMED that AO and thickness were deferred deliberately rather
      than overlooked — `brush/procedural_mask.h` names them and says why:
      "both need rays cast from the surface, which is a different cost class
      and a different set of parameters (ray count, length, falloff), so they
      are their own change rather than two more enumerators here pretending to
      be as cheap as the rest." This is that change
- [ ] 1.5 ASK CyberRemesherAndUV what it needs that ClayCore does not emit.
      Nobody in this repository can answer it. If the answer is "nothing", the
      "retopo-oriented export profile" half of the ROADMAP row closes with a
      documentation change and no code
- [ ] 1.6 **DECIDE the seam: A, B or C** (see design.md). Recommendation is B —
      every query B adds is useful to someone who is NOT baking, and every part
      of A is useful only to someone who is. A is reachable on top of B later;
      the reverse is not true. NOTHING BELOW STARTS UNTIL THIS IS ANSWERED
- [ ] 1.7 DECIDE the three open questions design.md lists: whose cage, AO on the
      field or on the mesh, and whether determinism across backends is a
      requirement for a seeded hemisphere sample. The last one is cheap to
      promise now and expensive to retrofit

## 2. Bounded rays (assumes B)

- [ ] 2.1 A SECOND entry point, not a parameter: `clay_raycast` shipped and a
      token cannot be appended to it — the same rule
      `add-operation-cancellation` followed
- [ ] 2.2 A miss inside the bound MUST be distinguishable from a hit beyond it.
      That distinction is the entire point, and conflating them is what puts
      garbage in bake seams
- [ ] 2.3 The batch form, because a bake is millions of rays
- [ ] 2.4 Respects hidden surface groups, like every other raycast path

## 3. Cage projection

- [ ] 3.1 Search BOTH directions within a distance and return the nearest
      surface: a low-poly cage point can sit inside or outside the high-poly
      and a baker cannot know which
- [ ] 3.2 Return the SIGNED distance travelled — that is the height map value,
      and computing it separately would be a second chance to disagree
- [ ] 3.3 Batched, cancellable, with progress. A bake is minutes, which is the
      third budget class `add-operation-cancellation` exists for

## 4. Surface measures, per point

- [ ] 4.1 Curvature, cavity and convexity, exposed PER POINT. Already computed
      inside `brush::mask_from_surface`; the work is returning a value rather
      than a lattice, and sharing the stencil so the two cannot drift
- [ ] 4.2 AO. Parameters stated rather than assumed: ray count, maximum
      distance, falloff, and a SEED — an unseeded hemisphere sample is not
      reproducible, and this library's determinism guarantee is not negotiable
- [ ] 4.3 Thickness, which is AO's inward twin and shares its machinery
- [ ] 4.4 MEASURE the cost per point before choosing defaults. A default ray
      count picked without a measurement is a guess with a unit attached — the
      same mistake `add-history-budget` task 1.3 was written to prevent

## 5. Prove it

- [ ] 5.1 The scenarios in the spec delta
- [ ] 5.2 Cage projection against a known analytic case, where the right answer
      is arithmetic rather than a rendering
- [ ] 5.3 AO determinism ACROSS BACKENDS, against the parity fixture. If a
      seeded hemisphere cannot hold that line, say so before shipping it rather
      than after
- [ ] 5.4 CHECK EVERY FIXTURE IS NON-DEGENERATE: a cage projection tested where
      the cage already coincides with the surface measures nothing, and an AO
      probe in open space returns 1.0 whatever the implementation does
- [ ] 5.5 A RENDER, not only a test. Bake a normal map and an AO map on the
      reference model and look at them — `add-surface-groups` shipped picking
      wired on two of four paths with every test green, and the render is what
      caught it

## 6. Reach it and say it

- [ ] 6.1 C ABI
- [ ] 6.2 pyclay
- [ ] 6.3 CHECK THE CAPABILITY IS REACHABLE, not merely implemented. Two Tier 2
      rows — `add-surface-groups` and procedural masks — were reported landed
      while reachable from no host at all. The check is `grep` for the entry
      point in `clay.h` and in `pyclay_module.cpp`, not a passing test suite
- [ ] 6.4 An example that bakes a normal and an AO map off a retopologized mesh,
      because the value is the workflow
- [ ] 6.5 `docs/08-mesh-readback.md` — the far end of the round trip it already
      documents
- [ ] 6.6 `docs/sculpt_comparison.md` — the "without it you can sculpt but not
      ship" line, updated to whatever becomes true
