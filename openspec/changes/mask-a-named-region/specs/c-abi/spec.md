# c-abi — a named region as a selection

Delta for `mask-a-named-region`.

## ADDED Requirements

### Requirement: A named region can be turned into a mask

The region a surface group names SHALL be paintable into a mask, so that a group
reaches every verb a mask already gates rather than only the automask that keeps
a brush inside the group it began on.

Naming a region and deciding what to do to it are two acts, and an artist
performs them in that order. An automask that keeps a stroke inside the group it
started in cannot express "flatten this panel", because it requires the stroke
to have begun there.

The group's own extent SHALL drive the fill; no region argument is taken. Two
lattices that each describe the same border are how the two come to disagree
about it.

The two lattices SHALL NOT be required to share a cell size, and the resulting
border SHALL be the GROUP's, quantised to whichever lattice is coarser. This is
the border every other group operation draws and SHALL NOT be represented as
finer than it is.

Painting with zero SHALL release the cells rather than record zeros in them,
because that is what zero means everywhere else in the mask vocabulary — so a
group can un-mask its own region.

Naming no group SHALL paint nothing and SHALL NOT be an error: "not in a group"
is not a region, and the complement of every group is a different request.

#### Scenario: A group becomes a mask and back
- **WHEN** a named region is painted into a mask and that mask is used to name a region again
- **THEN** the same cells carry the group, cell for cell

#### Scenario: The mask is where the group is
- **WHEN** a mask filled from a group is sampled at points across the model
- **THEN** it reads as painted exactly where the group answers with that id

#### Scenario: Zero un-masks the region
- **WHEN** a fully painted mask is filled from a group with zero
- **THEN** the mask's painted cell count falls by the number erased, rather than the cells remaining as zeros

#### Scenario: The lattices differ
- **WHEN** the mask's cell size differs from the group lattice's
- **THEN** the fill still agrees with the group everywhere, quantised to the coarser of the two

#### Scenario: Naming nothing
- **WHEN** the fill names "no group", or an id nothing carries
- **THEN** nothing is painted and the call succeeds
