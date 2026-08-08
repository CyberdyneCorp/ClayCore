# cut-tool — Trim Curve

Delta for `add-trim-curve`.

## ADDED Requirements

### Requirement: An open curve trims one side of the frame
The cut tool SHALL resolve an OPEN control-point curve into a shape covering one side of the frame, so that placing it with subtract removes everything on that side and with intersect keeps only it.

An open curve and a closed one are not interchangeable. Tessellating a trim stroke closed joins its endpoints and cuts a sliver between them rather than dividing the frame, so the two SHALL be separate entry points rather than a flag on one.

The stroke is closed against the frame's own bounds on the side it covers, so the result is an ordinary polygon outline and reaches the cut resolver unchanged — a trim is an ordinary extruded item like every other cut, with the same prism rule.

#### Scenario: A trim removes one side
- **WHEN** an open curve drawn across a form is resolved and placed with subtract
- **THEN** material on the side the shape covers is gone and material on the other side is untouched

#### Scenario: The two sides are complementary
- **WHEN** the same curve is resolved for one side and then the other, and each is placed with subtract
- **THEN** together they account for the whole form: what one removes the other leaves

#### Scenario: A closed lasso of the same points is a different cut
- **WHEN** the same control points are resolved as an open trim and as a closed lasso
- **THEN** the two shapes differ, and the fields they produce differ

#### Scenario: Degenerate input is refused
- **WHEN** a trim is asked for from fewer points than describe a stroke
- **THEN** it is refused rather than producing a shape with no area
