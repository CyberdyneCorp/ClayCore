## 1. The measured diff, before anything was written

- [x] 1.1 `brush::StrokePreset` flattens to 19 scalars; `clay_stroke_preset`
      carried 15 plus `struct_size`
- [x] 1.2 Missing: `rotate_to_azimuth`, `velocity_response.size`,
      `.strength`, `.reference` — TWO controls, four scalars
- [x] 1.3 NOT missing: `pressure`, a nested struct already flattened into
      `pressure_size` / `pressure_strength` / `pressure_curve`
- [x] 1.4 NOT missing: the sample channels both controls read — azimuth,
      velocity and timestamp already cross as `clay_stroke_sample_full`
      (ABI 0.48.0)
- [x] 1.5 `clay_brush_params` checked and ruled out: it is the voxel brush
      footprint, not a stroke preset
- [x] 1.6 The consequence measured rather than argued:
      `clay_brush_preset_by_name("Rake")` returned `rotate_to_azimuth == 0`,
      so the reference library's rake was a Draw brush to every C host

## 2. The change

- [x] 2.1 Four fields appended to `clay_stroke_preset` behind `struct_size`,
      all defaulting to zero, all-zero being the behaviour that shipped
- [x] 2.2 `clay_stroke_preset_defaults` supplies `velocity_reference = 1.0`;
      the struct's own default stays zero, because a field's value may never be
      the "caller did not declare this" signal
- [x] 2.3 `read_preset` passes both rotations through without an exclusion
      check — `resolve_stroke` already decides, and the barrel wins
- [x] 2.4 A non-positive `velocity_reference` with a non-zero channel is
      refused, not run inert

## 3. The descriptor this re-laid out

- [x] 3.1 `clay_brush_preset` embeds `clay_stroke_preset` by value with fields
      after it, so `model` and `brush` moved
- [x] 3.2 Confirmed the outcome is a REFUSAL and not a misread: an old host's
      368 is below the 376 this build calls original
- [x] 3.3 Its own message, because `read_desc`'s generic one tells the caller to
      do what it already did
- [x] 3.4 Rejected the second-struct answer with its cost written down

## 4. Tests, each proven by reverting the fix

- [x] 4.1 The barrel follows the stylus, and is not where the path pointed
- [x] 4.2 Both rotations set: the barrel wins, quaternion for quaternion
- [x] 4.3 The C boundary agrees with `resolve_stroke` stamp for stamp
- [x] 4.4 The speed response is signed: 0.100 -> 0.150 and 0.100 -> 0.050
- [x] 4.5 A stroke that is not moving is unchanged by either sign
- [x] 4.6 A response with no reference speed is refused
- [x] 4.7 A caller declaring the shorter `struct_size` resolves the old stroke
      with both controls set loudly past what it declared
- [x] 4.8 A preset round trip keeps both
- [x] 4.9 `Rake` crosses as a rake, and it is the only library preset that does
- [x] 4.10 The 0.115.0 `clay_brush_preset` layout is refused with a message
      naming the change
- [x] 4.11 Proof: with the four mappings removed from `clay_c.cpp`, 6 of the
      21 cases fail on 43 assertions; restored, 21/21 pass

## 5. Landing

- [x] 5.1 Version lines moved together: `CMakeLists.txt`, `CLAY_ABI_MINOR`,
      `pyproject.toml` — 0.114.0 -> 0.116.0 (0.115.0 is taken by PR #584)
- [x] 5.2 The four `StrokePreset.*` aliases dropped from
      `tools/check_binding_parity.py`: they pointed the gate at
      `clay_stroke_preset_serialize` because no field existed, and the fields
      now satisfy it under the ordinary struct rule
- [x] 5.3 `docs/07-brushes-and-features.md` updated
- [x] 5.4 Issue #530's count corrected by comment rather than a silent edit
