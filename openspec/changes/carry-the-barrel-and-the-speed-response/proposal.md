## Why

Issue #530 claims "9 of 12 stroke controls never reach a host". Measured against
the code, the claim is stale in both directions and the engine-side gap is
**two**, not nine.

### What the diff actually says

`brush::StrokePreset` (`include/clay/brush/stroke.h`) flattens to 19 scalars.
`clay_stroke_preset` (`bindings/c/clay.h`) carried 15 plus its `struct_size`.
Four scalars, two controls, were missing:

| C++ name | crossed as | verdict |
|---|---|---|
| `radius`, `spacing`, `strength` | same names | crossed |
| `pressure.size/.strength/.curve` | `pressure_size` / `pressure_strength` / `pressure_curve` | crossed, flattened |
| `jitter_position/_size/_rotation`, `seed` | same names | crossed |
| `rotate_along_stroke` | same name | crossed |
| `taper_start`, `taper_end`, `steady` | same names | crossed |
| `accumulation` | same name | crossed |
| **`rotate_to_azimuth`** | — | **MISSING** |
| **`velocity_response.size/.strength/.reference`** | — | **MISSING** |

`pressure` is not a missing control: it is a nested struct already flattened
into the three `pressure_*` fields, so it was reachable the whole time. The
sample channels the two missing controls read — azimuth, velocity, timestamp —
already cross as `clay_stroke_sample_full` (ABI 0.48.0). Only the preset was
short.

### The measurable consequence

`clay_brush_preset_by_name("Rake")` handed back a preset with
`stroke.rotate_to_azimuth` dropped. Rake is `Draw` plus a tighter spacing plus
that one flag (`src/brush/preset.cpp`), so the reference library's rake was a
Draw brush to every C host, and the C preset round trip turned a schema-v2
preset back into a v1 one in all but its version byte.

## What lands

Four fields appended to `clay_stroke_preset` behind its `struct_size`:
`rotate_to_azimuth`, `velocity_size`, `velocity_strength`, `velocity_reference`.
All four default to zero, and all-zero is the behaviour that shipped: the engine
reads a non-positive reference as "speed changes nothing".

`rotate_to_azimuth` wins over `rotate_along_stroke` where both are set. That is
already how `resolve_stroke` behaves; the C boundary passes both through rather
than refusing the pair, because refusing would make a brush library unloadable
the moment a preset in it set both.

A speed response with a non-positive `velocity_reference` and a non-zero channel
is **refused**, on the same footing as a radius of zero. The engine would have
run it with both channels silently inert, and a control that does not act is
worse than one that is missing.

## What building it found, and what it cost

**`clay_brush_preset` had to be re-laid out, and it is the only descriptor in
this ABI that could be.** It embeds `clay_stroke_preset` BY VALUE with `model`
and `brush` after it, so appending to the stroke preset moved both. `struct_size`
negotiates a TAIL, not a shift.

What it can still do is NOTICE, and it does, for free: `kBrushPresetOriginal` is
derived from `offsetof(clay_brush_preset, brush)`, so it grew with the struct and
a 0.115.0 host's declared 368 now falls below the 376 this build calls original.
Every entry point taking one returns `CLAY_ERROR_INVALID_ARGUMENT`. The refusal
was given its own message because `read_desc`'s generic one — "set it to the
sizeof of the struct you compiled against" — is exactly what such a caller did.

**A second struct was considered and rejected**, which is the answer
`clay_stroke_sample_full` took for the sample packing. That case had no choice: an
array element carries no `struct_size`, so the corruption could not be detected.
Here it can. The cost of the alternative is a parallel `clay_brush_preset_full`
plus a parallel entry point for each of the four calls that take one, forever, so
that a stroke preset could never again be appended to. A loud break at a 0.x
minor is cheaper than a permanent fork of the preset surface.

## What this does NOT fix, stated so it is not discovered later

Both new controls read channels only `clay_stroke_sample_full` carries. The
`count*5` float packing every other stroke entry point takes —
`clay_stroke_resolve`, `clay_voxel_apply_stroke`, `clay_mask_apply_stroke`,
`clay_layer_apply_stroke`, `clay_mesh_sculptor_apply_stroke`,
`clay_multires_sculptor_apply_stroke` — reports no azimuth and no velocity, so on
that packing a barrel-following stamp faces a constant +x and the speed response
is off at every sample. `clay_stroke_resolve_full` plus `clay_layer_place_stamps`
is the path that carries them today. Widening the consumers is its own change
with its own six entry points; it is documented in the header rather than half
done here.

## What changes for a caller

- A `clay_stroke_preset` caller compiled against 0.115.0 or earlier is
  unaffected: it declares the shorter size, never sets the four fields, and
  resolves exactly the strokes it resolved before.
- A `clay_brush_preset` caller compiled against 0.115.0 or earlier **must
  recompile**. Until it does, every call taking one is refused with a message
  naming this change.
- ABI 0.115.0 -> 0.116.0.
