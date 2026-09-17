## Context

`brush::apply_to_mesh` and `brush::apply_to_multires` already agree about what a
stroke IS, through three helpers in `src/brush/stroke.cpp`:
`mesh_stamp_settings` (radius and strength from the stamp, the drag direction,
the Grab/Snakehook centre), `mesh_mask_gate` (the world-addressed mask placed
once) and `mesh_automask_inputs` (the cavity and group estimators built once).
What differs between them is where the result is stored and how the Snakehook
anchor is addressed: a weld class on the fixed mesh, a class of the bound level
on the hierarchy.

`DynamicSculptor::stamp` already takes the same `MeshBrushSettings`, composes the
same weight (falloff, taper, gate, alpha, automask), refuses `Layer` through
`dynamic_offers`, runs `default_timing(verb)` around the deformation, and
accumulates into a `TopologyDelta`. It ignores `seed_class`: its walk always
seeds at `nearest_vertex(brush.center)`.

## Goals / Non-Goals

**Goals:** one stroke meaning across three representations; the per-verb remesh
schedule on every stamp; Layer refused; the azimuth reaching the alpha on
request; the entry point reachable from C, Python and Swift.

**Non-Goals:** changing the fixed path's Grab anchoring; a topology undo handle
in the ABI; widening the flat-packed stroke calls; a device latency case.

## Decisions

### D1. A consumer in `brush`, not a stroke method on `DynamicSculptor`

`mesh` may not see `brush` (module layering), and the stroke's meaning already
lives in `brush`. A method on the sculptor would be a second copy of
`mesh_stamp_settings`, and the fixed/hierarchy pair shows what that costs: the
helpers exist because two copies drifted.

```cpp
std::size_t apply_to_dynamic(mesh::DynamicSculptor& sculptor,
                             const std::vector<Stamp>& stamps,
                             mesh::MeshBrush verb, const mesh::MeshBrushSettings& settings,
                             const mesh::DynamicTopologySettings& topology,
                             const voxel::MaskField* mask = nullptr,
                             mesh::TopologyDelta* record = nullptr,
                             const MeshStrokeOptions& options = {},
                             mesh::DynamicStampResult* summary = nullptr);
```

Returns the number of stamps that changed the surface (`DynamicStampResult::
changed()`), matching the other two's "stamps that moved a vertex" — a stamp that
only remeshed is a change a host must re-upload. `summary`, when given,
accumulates moved vertices, the remesh counts (split, collapsed, flipped,
relaxed, `hit_budget` OR-ed), the union of dirty bounds, and the final revisions.

*Alternative rejected:* returning `DynamicStampResult` by value. It breaks the
shape the other two consumers share, and a caller that wants only the count
would pay for nothing but still read a struct.

### D2. Snakehook anchors on a `VertexId`, revalidated every stamp

The fixed path anchors on a class that cannot die. Here a collapse retires ids
(measured: up to 14 deaths in a 61-stamp stroke at detail 4). Per stamp:

1. If `surface.vertex(anchor)` is null, re-find
   `sculptor.nearest_vertex(previous_stamp_position)`.
2. The stamp's centre is the anchor's CURRENT position.

Measured alternatives, all deterministic: never re-finding (freeze the dead
anchor's last position) collapses reach to 15–18% where the anchor dies 6–14
times, and still costs 10–42 points (41–88%) where it dies 1–3 times; re-finding at the dead
anchor's last position gave 57–96%; re-finding at the previous stamp position
gave 81–98% and was never worse in the sixteen detail-4/8 rows. Adopted.

The initial anchor is `nearest_vertex(stamps.front().position)`. `seed_class` is
not consulted: the adaptive walk ignores it on every path today, and a class
index does not name a vertex of a surface whose slots move.

*Needs `nearest_vertex` public.* It is the estimator the walk seeds with; a copy
in `brush` would be a second estimator for one question — the defect the
nearest-vertex comment in `dynamic_sculpt.cpp` records having removed once.

### D3. Grab is the fixed path's Grab

Centre on the first stamp, direction = motion since the previous stamp.
Measured to agree with `apply_to_mesh` to 1e-3 on a pull-out (1.248 vs 1.247).
Its 41% reach is `main`'s behaviour on the fixed mesh as well, so changing it
here alone would make the representations disagree; it is left for a follow-up
that changes both with their goldens.

The AFTER remesh therefore runs at the first stamp's centre, not at the stretched
tip. That is `DynamicSculptor::stamp`'s rule for a stamp and is kept; the tip is
refined by the stamps whose balls still reach it. Named as a risk below.

### D4. Every stamp goes through `DynamicSculptor::stamp`

Not a stroke-level remesh pass. That is the only way the timing policy stays one
function (`default_timing`) with one reason per verb, and the only way "a stroke
equals its stamps" can be tested bit-exact (determinism measured). A stroke-level
schedule — remesh once per N stamps, say — is a latency optimisation this change
does not need (1.001x shows the stamp dominates) and would need its own
measurements.

### D5. Refusals

- `MeshBrush::Layer`: `apply_to_dynamic` returns 0 before touching the surface,
  the record, the automask inputs or the telemetry. C: `CLAY_ERROR_INVALID_ARGUMENT`
  with the `clay_dynamic_sculptor_stamp` wording. Python: `ValueError`
  (`std::invalid_argument`) with the `DynamicSculptor.stamp` wording.
- `options.defer_normals == true`: returns 0 and applies nothing. The adaptive
  sculptor refreshes normals locally per stamp and has no deferral. Not exposed
  in either binding, so only a C++ caller can reach the refusal.
- Empty stamps: 0, nothing touched.

### D6. The C ABI shape

```c
clay_result clay_dynamic_sculptor_apply_stroke(
    clay_dynamic_sculptor* sculptor,
    const clay_stroke_sample_full* samples, size_t sample_count,
    const clay_stroke_preset* preset,
    const clay_mesh_brush_desc* brush,
    const clay_dynamic_topology_desc* topology,   /* NULL = defaults */
    const clay_mask* mask,                        /* NULL = none */
    int32_t orient_alpha_by_stamp,
    size_t* out_applied,
    clay_dynamic_stamp_report* out_report);       /* accumulated; may be NULL */

clay_result clay_dynamic_sculptor_apply_preset(
    clay_dynamic_sculptor* sculptor,
    const clay_stroke_sample_full* samples, size_t sample_count,
    const clay_brush_preset* preset,
    const float* alpha, int32_t alpha_width, int32_t alpha_height,
    const clay_dynamic_topology_desc* topology,
    const clay_mask* mask,
    int32_t orient_alpha_by_stamp,
    size_t* out_applied,
    clay_dynamic_stamp_report* out_report);
```

How the other two are shaped, and why this one differs:

| | mesh / multires stroke | adaptive stroke | why |
|---|---|---|---|
| samples | `float*` count*5 | `clay_stroke_sample_full*` | the goal requires the azimuth; the flat packing carries none, and a NEW entry point has no legacy stride to protect |
| frame | per-call `mesh_to_world` + session frame, both = refused | session frame only | the dynamic handle already declares one (`clay_dynamic_sculptor_set_world_frame`); adding a second spelling to a new call would create the conflict the others must refuse |
| `defer_normals` | yes | no | D5 |
| undo record | `clay_mesh_deltas*` / none | none | no ABI handle for `TopologyDelta`; `_stamp` passes none either |
| report | `out_applied` (+ multires report) | `out_applied` + accumulated `clay_dynamic_stamp_report` | the report already exists with `struct_size`, and remesh counts + dirty bounds are what a host needs to drain chunks |

The topology descriptor decode in `clay_dynamic_sculptor_stamp` is extracted into
one helper both calls use, so the stroke and the stamp cannot read the same
descriptor two ways. The session frame is applied as `_stamp` applies it:
`brush_settings_to_local`, `stroke_to_local`, and `options.mesh_to_world` = the
declared frame, so the mask and the estimators are placed once.

`hit_budget` accumulates as an OR; dirty bounds as a union; revisions are read
once at the end.

### D7. pyclay

`DynamicSculptor.apply_stroke(samples, preset, verb, topology=..., falloff,
strength, geodesic, smooth_iterations, mask, automask, stamp_azimuth,
orient_alpha_by_stamp=False)` and `DynamicSculptor.apply_preset(samples, preset,
topology=..., alpha=None, alpha_extent=0.0, mask=None,
orient_alpha_by_stamp=False)`, mirroring `MeshSculptor`'s. Samples keep the
(N, 3..8) numpy convention, so column 6 is the azimuth. Both return a dict with
`applied` plus the stamp report's keys, since `stamp` already returns that
dict. The gate maps both through the existing `clay_dynamic_sculptor_` prefix; no
alias or exemption is needed. The gate must be read from a BUILT module
(`imported <path>`), not the parsed-source fallback.

## Risks / Trade-offs

- **[Grab's AFTER remesh runs at the first stamp's centre]** → Tip refinement
  depends on later balls reaching it. Mitigation: a test asserts a pull-out Grab
  stroke validates and its splits are non-zero; the anchoring question is the
  named follow-up.
- **[Re-finding at the previous stamp position picks a vertex on a different
  sheet when two surfaces are within a radius]** → the same risk
  `nearest_vertex` already carries for every stamp's seed. Not new; not
  mitigated here.
- **[ABI minor collision]** → another branch in flight may take 0.118.0 first;
  this one rebases and takes the next number, per `claycore-change`.
- **[Cognitive complexity]** → `apply_to_mesh` is already a long loop. The
  anchor revalidation goes in its own function
  (`dynamic_snakehook_centre`) so `apply_to_dynamic` stays within 15; the
  implementation PR states the measured scores.

## Migration Plan

Additive. No existing call changes behaviour, no descriptor is re-laid out, no
format moves. ABI 0.117.0 -> 0.118.0 in the implementing PR.

## Open Questions

- Should the fixed-mesh Grab capture its region at the first stamp instead of
  re-gathering around it? Out of scope; measured here so the follow-up starts
  from numbers (41% vs 66% reach on a 0.6 pull-out, `cube_sphere(24)`).
