## ADDED Requirements

### Requirement: A culled compile drops a warp its region cannot reach

A tape compiled against a cull region SHALL NOT carry a domain warp whose
support that region cannot reach. Such a warp is the IDENTITY over the region,
so carrying it is work done for samples it cannot affect — measured at 3.20x the
cost of the same samples with no warps at all, for twelve grabs none of which
reached them.

The dropping SHALL be sound along the CHAIN and not merely per warp. Warps apply
in authoring order, so a warp is the identity over the region only if every warp
before it was: a warp that is kept may move a point, and the region each
subsequent warp is tested against SHALL therefore be widened by the most that
warp can move one.

Only a warp with FINITE SUPPORT may be dropped. A warp whose weight merely
decays, or clamps, reaches everywhere and SHALL always be carried.

A compile with no cull region SHALL carry every warp: there is no region to test
against, and that compile is the one a whole-document evaluation and a host
upload use.

This SHALL NOT change the field. Inside the region the culled tape SHALL return
exactly what the whole document's tape returns, because a dropped warp was the
identity there.

#### Scenario: A region no warp reaches carries none
- **WHEN** a tape is compiled against a region outside every warp's support
- **THEN** it carries no warps, and evaluates to exactly what the whole document's tape does over that region

#### Scenario: A region a warp reaches keeps it
- **WHEN** the region is within a warp's support
- **THEN** that warp is carried, and the field is unchanged

#### Scenario: A warp is judged after the warps before it have moved the point
- **WHEN** a warp that the region reaches displaces points toward a second warp the region alone does not reach
- **THEN** the second warp is carried too, and the field is unchanged
