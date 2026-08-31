## ADDED Requirements

### Requirement: A chain's Lipschitz is priced where its links can reach
Composing a deformer chain's Lipschitz factors SHALL account for FINITE SUPPORT. A link that is the identity outside its own region cannot compound with one whose region it cannot reach, and the chain's bound SHALL therefore be the worst such GROUP rather than the product of every link.

Two finite-support links SHALL be treated as able to compound when their regions are closer than `r_i + r_k` plus the total distance the chain's point warps can carry a point. That is conservative in the safe direction and does not depend on where the two sit in the chain.

A link with UNBOUNDED support acts everywhere and SHALL be charged against every group. A link whose region is degenerate SHALL be treated as unbounded rather than as empty, the other way round being the unsafe direction.

The composition SHALL keep the distinction the fold already had: a point warp multiplies the slope through the chain rule, a distance offset adds its own gradient to it.

The result SHALL remain a bound. This relaxation makes the declared step scale LARGER, so it is the one place where being wrong lets a marcher step through a surface.

#### Scenario: Disjoint brushes cost what one costs
- **WHEN** a chain carries eight grabs whose regions cannot reach one another
- **THEN** the declared step scale is the one a single grab declares

#### Scenario: Brushes that can reach each other still compound
- **WHEN** a chain carries grabs piled on one spot
- **THEN** every additional one lowers the declared step scale

#### Scenario: The threshold is the reach, not the radii
- **WHEN** two grabs' regions do not overlap but the first can carry a point into the second
- **THEN** they compound, and moving them past that reach stops them compounding

#### Scenario: An unbounded link is charged everywhere
- **WHEN** a chain carries two disjoint grabs and a twist between them
- **THEN** the bound is the twist's factor times one grab's, not the larger of the two

#### Scenario: Marching by the relaxed bound is still safe
- **WHEN** a document whose items carry disjoint brushes is marched by its declared step scale
- **THEN** no step lands past the surface
