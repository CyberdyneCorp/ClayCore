# c-abi

## ADDED Requirements

### Requirement: A ray can be bounded
The library SHALL provide a raycast that takes a maximum distance, and SHALL distinguish a miss within that distance from a hit beyond it.

An unbounded raycast cannot express "search this far along this normal", which is the query a cage projection and a snap tool both are. Conflating a miss with a distant hit is what places incorrect samples in a baked texture's seams, so the two SHALL be separately reportable rather than both returning "no useful hit".

The bounded form SHALL be a separate entry point rather than a change to the existing one, since the existing signature has shipped.

#### Scenario: A surface beyond the bound is not reported
- **WHEN** a ray is cast with a maximum distance shorter than the distance to the nearest surface
- **THEN** no hit is reported, and the result is distinguishable from a ray that found nothing at any distance

#### Scenario: A surface within the bound is reported
- **WHEN** a ray is cast with a maximum distance longer than the distance to the nearest surface
- **THEN** the same hit is reported as an unbounded cast of the same ray

### Requirement: A point can be projected onto the surface within a cage
The library SHALL project a point onto the document's surface along a given direction, searching both directions within a maximum distance, and SHALL report the signed distance travelled.

Both directions, because a cage point produced from a low-polygon mesh may lie inside or outside the high-polygon surface and the caller cannot know which. The signed distance SHALL be reported by the same call that finds the point, since it is the height value a bake needs and computing it separately admits disagreement.

The operation SHALL be available in batched form and SHALL be cancellable with progress, since a bake is a minutes-long operation of millions of points.

#### Scenario: A point outside the surface projects inward
- **WHEN** a point outside the surface is projected along its inward normal within a sufficient distance
- **THEN** the surface point is returned with a signed distance whose sign indicates the direction travelled

#### Scenario: A point with no surface within the cage reports no projection
- **WHEN** a point is projected and no surface lies within the maximum distance in either direction
- **THEN** no projection is reported, rather than a distant or arbitrary surface point

### Requirement: Surface measures are available per point
The library SHALL report curvature, cavity, convexity, ambient occlusion and thickness at given points, in batched form.

These measures already exist inside the procedural-mask implementation, which can only return a lattice. A caller sampling a texture needs a value per point, and the two SHALL share one implementation so that a mask and a baked map cannot disagree about the same surface.

Ambient occlusion and thickness SHALL state their parameters explicitly — ray count, maximum distance, falloff, and a seed. A seed SHALL be required rather than implicit, because the library's results are reproducible across runs and backends and a hemisphere sample without a stated seed would break that guarantee silently.

#### Scenario: Curvature agrees with the mask that measures it
- **WHEN** curvature is sampled at a point and a procedural mask is built from the same measure over a region containing it
- **THEN** the two agree about that point

#### Scenario: An occluded point reports more occlusion than an exposed one
- **WHEN** ambient occlusion is sampled inside a crevice and on an open convex surface of the same model
- **THEN** the crevice reports the greater occlusion

#### Scenario: Ambient occlusion is reproducible
- **WHEN** ambient occlusion is sampled twice at the same points with the same parameters and seed
- **THEN** the results are identical
