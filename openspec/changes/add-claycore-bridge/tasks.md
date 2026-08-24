# Tasks: add-claycore-bridge

**Section 1 is a measurement and a decision pass.** Sections 2-4 are the part
that does NOT depend on the seam decision or on CyberRemesherAndUV, and were
built on that basis: every query in them is a field query, useful to a caller
who is not baking, and required by option B or A alike. Sections 5-6's bake
example and the "retopo export profile" question still wait on 1.5 and 1.6.

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

- [x] 2.1 `clay_raycast_bounded`, a second entry point for exactly that reason
- [x] 2.2 Tested both ways against an analytic sphere: bounded short of the
      surface is a miss, bounded past it is the same hit the unbounded cast
      reports. A `tmax` that is not greater than `tmin` is refused rather than
      silently treated as unbounded
- [x] 2.3 Batched via `clay_project_to_surface_many`; the bounded single ray
      also serves `clay_raycast_many`'s existing batch
- [x] 2.4 Respects hidden surface groups, like every other raycast path

## 3. Cage projection

- [x] 3.1 Both directions — AND BOTH SIDES, which turned out to be the harder
      half and is not what the task anticipated. Two implementations failed
      before the third worked:

      A plain sphere-march cannot start INSIDE the surface: the signed distance
      is negative and it takes no step at all, so every cage point where the
      low-poly pinches inward returns a miss. Marching |f| instead fixes the
      stepping and breaks the stopping — with no sign to watch, a hit is only
      "close enough", and the over-relaxation that makes a march fast steps
      straight past the surface and never comes back within tolerance. That one
      passed the inside test and broke both outside tests.

      What works: step by |f|, which is still the safe distance to the nearest
      surface, and detect the crossing by SIGN CHANGE between consecutive
      samples, then bisect. Unambiguous from either side, and still a march
      rather than a scan
- [x] 3.2 Signed, returned by the call that found the point
- [x] 3.3 Batched and cancellable. A cancelled chunk RETURNS NORMALLY and
      never throws: thread_pool.h's join waits on `done >= num_tasks` and
      increments only after `fn` returns, so a throw would hang it forever

## 4. Surface measures, per point

- [x] 4.1 Moved to `brush/surface_measure.h` with `mask_from_surface` becoming
      one of its callers, so there is ONE stencil. Verified by a test that
      compares the mask against the per-point form over the same surface — and
      that comparison had to be the BULK rather than the worst case, because a
      saturated measure legitimately flips inside one cell at a region
      boundary. It still catches what it is for: a second stencil would
      disagree everywhere, not only in boundary cells
- [x] 4.2 Ambient occlusion, with all four stated. The sample pattern is a
      Hammersley sequence XOR-scrambled by a hash of the QUANTISED point and
      the seed — quantised so that two points a float-epsilon apart get the
      same rotation rather than two unrelated ones, which would make
      neighbouring texels differ by sample noise and read as film grain.
      Cosine-weighted, so no per-ray cosine term is needed. The tangent basis
      is the branchless Duff construction: the usual "cross with an arbitrary
      axis" degenerates exactly where the normal IS that axis, which on an
      axis-aligned model is most of the surface.
      Tested for bit-identity across calls AND that a different seed differs,
      or the first check would be vacuous
- [x] 4.3 Thickness. Tested against a slab of known thickness, where the
      answer is arithmetic, and for saturating rather than lying when the probe
      is shorter than the material is thick
- [ ] 4.4 STILL OPEN. `ray_count` defaults to 16 and `ray_length` to 20x
      `scale`, and neither is measured — the interface says `ray_length` is a
      guess rather than pretending otherwise. Worth a measurement pass before
      a host builds a bake around them

## 5. Prove it

- [x] 5.1 The scenarios in the spec delta, in C++, in C and in pyclay
- [x] 5.2 Every projection assertion is against an analytic sphere: from 1.5
      along -X onto a 0.5 sphere is exactly 1.0
- [ ] 5.3 STILL OPEN for the CROSS-BACKEND half. Determinism across CALLS is
      tested and holds by construction — the pattern depends on the point and
      the seed and on nothing else. Whether it holds across backends is
      untested here because the measure runs on the CPU regardless of the
      document's backend today; it becomes a real question only if the walk
      moves to the GPU
- [x] 5.4 Every measure probe asserts it is ON the surface first. CAUGHT ONE:
      the thickness test probed y=0.2 on a `clay.Box(size=(1, 0.2, 1))` — but
      `size` is the FULL extent, so the face is at y=0.1 and the probe was 0.1
      outside the surface. A correct implementation gave a wrong-looking answer.
      The on-surface guard every other measure test already had is what the
      fixed version added.
      ALSO: the cavity fixture is two overlapping spheres and NOT a torus. For
      major R and minor r the mean curvature at a torus's inner ring is
      1/r - 1/(R-r), so the tube wins and the ring reads CONVEX unless R < 2r —
      a cavity demo built on a torus measures nothing and looks fine. Pinned by
      its own test
- [ ] 5.5 STILL OPEN, and deliberately: a normal/AO MAP needs a UV layout, so
      the render belongs with the bake example that waits on 1.6. The measures
      themselves are checked numerically against analytic cases, which is
      stronger than looking at them

## 6. Reach it and say it

- [x] 6.1 C ABI: `clay_measure_points`, `clay_mask_from_surface`,
      `clay_raycast_bounded`, `clay_project_to_surface(_many)`
- [x] 6.2 pyclay: `Document.measure`, `.mask_from_surface`, `.project`
- [x] 6.3 DONE, and it found the second one. A sweep of every public header
      against both bindings turned up `brush/procedural_mask.h` as the only
      genuine capability unreachable from any host — the rest of the
      "unreachable" headers are internal plumbing (the kernel dialect reached
      through the tape, byte accounting, adjacency behind the mesh brushes).
      That gap is closed here: procedural masks now have a C and a pyclay
      surface, which they never had
- [ ] 6.4 STILL OPEN — a bake example needs a UV layout, so it waits on 1.6.
      `examples/64_measuring_the_surface.py` covers the queries themselves:
      2000 points projected onto a creased surface, all six measures compared
      between the crease and the open side, determinism checked, and the mask
      compared against the per-point form
- [ ] 6.5 `docs/08-mesh-readback.md` — the far end of the round trip it already
      documents
- [ ] 6.6 `docs/sculpt_comparison.md` — the "without it you can sculpt but not
      ship" line, updated to whatever becomes true
