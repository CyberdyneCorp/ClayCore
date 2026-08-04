# python-bindings — sculpting verbs

Delta for `add-sculpt-brushes`.

## ADDED Requirements

### Requirement: Falloff arguments and sculpting verbs in Python
The module SHALL expose the sculpting verbs `sculpt_smooth`, `sculpt_inflate`, `sculpt_flatten` and `sculpt_pinch`, and SHALL accept `falloff`, `strength` and `seed` arguments on the brush operations. Falloff SHALL be named by string, matching the convention used for `shape` and `axes`, and an unrecognised name SHALL raise rather than silently defaulting.

#### Scenario: Soft brush from Python
- **WHEN** a script calls `set_brush(cell, 9, index, falloff="smooth", strength=0.5)`
- **THEN** fewer cells are set than with the default constant falloff, and repeating the call with the same seed sets the same cells

#### Scenario: Sculpting a surface from Python
- **WHEN** a script calls `sculpt_smooth` or `sculpt_inflate` on a grid
- **THEN** the occupied set changes according to the verb and no exception is raised

#### Scenario: Unknown falloff is rejected
- **WHEN** a script passes `falloff="wobble"`
- **THEN** the call raises a `ValueError` naming the accepted curves
