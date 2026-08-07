# python-bindings — document grab

Delta for `add-document-grab`.

## ADDED Requirements

### Requirement: Moving a surface from Python
The module SHALL expose resolving a world drag into a plan of edits and applying it, with the centre, radius, displacement, easing and front-only gate under the caller's control.

The binding SHALL state that this is the difference between the `grab` deformer and a Move brush: the deformer drags one item's own field in that item's local frame, and this drags the surface.

#### Scenario: A script drags a multi-item form
- **WHEN** a script resolves a world drag over a form built from several items and applies it
- **THEN** the surface moves as one, and the document's undo restores it

#### Scenario: Resolving is separate from applying
- **WHEN** a script resolves a drag without applying it
- **THEN** the document is unchanged and the plan describes which items would be edited
