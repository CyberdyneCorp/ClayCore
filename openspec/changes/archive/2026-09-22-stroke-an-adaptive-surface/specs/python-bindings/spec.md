## ADDED Requirements

### Requirement: An adaptive stroke is reachable from Python
`pyclay` SHALL expose `DynamicSculptor.apply_stroke` and `DynamicSculptor.apply_preset`, mirroring `MeshSculptor`'s stroke calls and mapping to `clay_dynamic_sculptor_apply_stroke` and `clay_dynamic_sculptor_apply_preset`, so the binding-parity gate passes without an alias or an exemption.

Samples SHALL keep the `(N, K)` convention with `K` from 3 to 8, so a sixth column carries the azimuth. A Layer stroke SHALL raise with the reason the adaptive stamp already gives, rather than returning zero.

#### Scenario: Parity holds against a built module
- **WHEN** `tools/check_binding_parity.py` runs with an imported `pyclay` after this change
- **THEN** both adaptive stroke calls are matched to their C counterparts, and the gate reports that it imported the module rather than parsing its source

#### Scenario: A Layer stroke raises
- **WHEN** a script applies a Layer stroke to an adaptive surface
- **THEN** the call raises and the surface is unchanged
