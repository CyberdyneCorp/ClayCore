# c-abi

## ADDED Requirements

### Requirement: Every sculptable surface can be told where it is

A handle that sculpts a surface SHALL be able to declare the transform that
places that surface in world, because the lattices a brush consults — the
painted mask, the cavity field and the group field — are world-addressed while a
layer's vertices are layer-local.

#### Scenario: A declared frame places the painted mask

- **GIVEN** a multires hierarchy whose vertices are layer-local
- **AND** a session frame declared with
  `clay_multires_sculptor_set_world_frame` that translates it
- **WHEN** `clay_multires_sculptor_stamp` is called with a mask painted over the
  region the surface occupies in WORLD
- **THEN** the stamp is gated by that mask
- **AND** a mask painted over the region the surface occupies in LAYER-LOCAL
  coordinates does not gate it

#### Scenario: The five sculpt-layer stroke verbs are placed together

- **GIVEN** a sculpt-layer stroke with a declared session frame
- **WHEN** any of `_stamp`, `_stamp_detail`, `_smooth`, `_erase` or `_restore`
  is called with a mask
- **THEN** the mask is sampled at the placed point, because all five read their
  brush and their mask through one helper

#### Scenario: An adaptive surface takes a declared frame

- **GIVEN** a `clay_dynamic_surface` and a session frame declared with
  `clay_dynamic_sculptor_set_world_frame`
- **WHEN** `clay_dynamic_sculptor_stamp` is called with a mask
- **THEN** the mask is sampled at the placed point

#### Scenario: An adaptive surface has no layer transform to adopt

- **GIVEN** a `clay_dynamic_sculptor`
- **THEN** no `_use_layer_transform` is offered, because a
  `clay_dynamic_surface` is not a document layer and there is no transform to
  read
- **AND** the header SHALL state that as a fact about the ABI rather than leave
  it as an apparent omission

#### Scenario: An unset frame is the identity

- **GIVEN** a sculptor on which no frame has been declared
- **WHEN** it stamps with a mask
- **THEN** the mask is sampled at the surface's own coordinates, exactly as
  before this change, so a host that has not heard of the frame is not opted in

#### Scenario: A standalone hierarchy belongs to no layer

- **GIVEN** a hierarchy built by `clay_multires_from_mesh` rather than borrowed
  from a document layer
- **WHEN** `clay_multires_sculptor_use_layer_transform` is called
- **THEN** it SHALL answer `CLAY_ERROR_NOT_FOUND` rather than silently adopt the
  identity

#### Scenario: A layer carrying a per-axis scale is refused

- **GIVEN** a hierarchy borrowed from a layer that carries a per-axis scale
- **WHEN** `_use_layer_transform` is called
- **THEN** it SHALL answer `CLAY_ERROR_UNSUPPORTED`, on the same terms and for
  the same reason as `clay_mesh_sculptor_use_layer_transform`: a round brush in
  world is an ellipsoid on the model, so `radius` stops naming anything a
  spherical walk can honour

### Requirement: A declared frame is readable, and cannot be spelled twice

A host SHALL be able to read back what a handle declares, and SHALL be refused
when it declares the same frame two ways.

#### Scenario: Reading back a declared frame

- **WHEN** `clay_multires_sculptor_world_frame`,
  `clay_dynamic_sculptor_world_frame` or
  `clay_multires_sculpt_layer_stroke_world_frame` is called
- **THEN** `out_declared` SHALL be non-zero exactly when a frame is declared
- **AND** `out_frame`, when non-NULL, SHALL carry that transform

#### Scenario: Clearing a declared frame

- **WHEN** a `_set_world_frame` is called with NULL
- **THEN** the session returns to the identity and `out_declared` reads zero

#### Scenario: A per-call frame beside a declared one is refused

- **GIVEN** a multires sculptor with a declared session frame
- **WHEN** `clay_multires_sculptor_apply_stroke` is called with a non-NULL
  `mesh_to_world`
- **THEN** it SHALL be refused rather than resolved by precedence, because a
  host passing both means one of the two is what it believes and picking
  silently would make the other a wrong belief nothing corrects

#### Scenario: The same call with NULL is accepted

- **GIVEN** the same sculptor with a declared session frame
- **WHEN** `clay_multires_sculptor_apply_stroke` is called with a NULL
  `mesh_to_world`
- **THEN** it SHALL proceed, using the declared frame
