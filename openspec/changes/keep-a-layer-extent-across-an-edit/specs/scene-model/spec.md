## ADDED Requirements

### Requirement: A layer's extent can be kept across an edit

An intersect is bounded by its layer's extent, and computing that extent walks
every visible item in the layer. A consumer that edits a layer repeatedly SHALL
be able to keep that extent across edits rather than recompute it per edit,
without the kept extent ever differing from a freshly computed one.

The kept form SHALL be exact rather than conservative. In particular the extent
SHALL shrink when the edit shrinks it: a form that can only be widened is not
the extent, and an extent that is too LARGE is merely slow while one built by
widening a stale union is too SMALL — which is under-invalidation, and renders
as stale geometry rather than as an error.

It SHALL remain cheap for the item being edited repeatedly even when that item
determines how far the extent reaches, because the item a host drags across a
form is typically a boolean operand large enough to extend past it. A form that
is cheap only for items lying strictly inside the extent does not satisfy this,
having no effect on the case it exists for.

Being told that an item changed SHALL NOT itself compute anything. A layer
holding no intersect never has its extent asked for, and such layers are the
common case; work done at the moment of the edit is therefore paid by layers
that never benefit from it.

The kept extent SHALL be abandoned for any change it cannot account for,
including adding, removing or reparenting an item, any change made to the layer
itself, and any mutation reaching the layer outside the command vocabulary.

Whether a given command is confined to a single item SHALL be decided in one
place, so that a component keeping an extent and a test checking one cannot
disagree about a command, and a command added later is classified rather than
assumed harmless.

#### Scenario: A drag walks the layer once
- **WHEN** one item is edited over many consecutive frames
- **THEN** the layer is walked on the first of them and not on the others, and the extent equals a freshly computed one on every frame

#### Scenario: The dragged item sticks out of the form
- **GIVEN** the edited item extends past the rest of the layer on some face
- **WHEN** it is dragged over many frames
- **THEN** the layer is still walked only once

#### Scenario: The edit makes the extent smaller
- **WHEN** the edited item is moved back inside the others, or made smaller
- **THEN** the extent shrinks to match a freshly computed one

#### Scenario: A layer nobody asks about is not walked
- **WHEN** a layer holding no intersect is edited over many frames
- **THEN** its extent is never computed

#### Scenario: A change the kept form cannot account for
- **WHEN** an item is added or removed, or the layer's own transform, mirror or radial setting changes
- **THEN** the next extent is computed by walking rather than kept
