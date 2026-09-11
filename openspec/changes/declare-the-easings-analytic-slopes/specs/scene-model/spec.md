## ADDED Requirements

### Requirement: An easing curve's declared slope is its own, not a blanket margin

The steepest slope declared for an easing curve SHALL be that curve's own
derivative supremum where it is known in closed form, rather than a sampled
maximum carrying a fixed margin.

The slope multiplies a deformer link's Lipschitz factor, and a chain multiplies
those factors, so an excess declared per link is raised to the power of the
chain's length. A margin that is negligible on one link is not negligible on
forty.

**A sampled maximum SHALL keep its margin.** Sampling a curve at finitely many
points yields a LOWER bound on its supremum, and a declared slope below the true
one does not make a march slow — it makes it cross the surface. So a curve whose
supremum is not established in closed form SHALL continue to declare a sampled
value with a margin, and the two kinds SHALL be distinguishable in the source
rather than merged into one number.

A curve whose declared slope is known to be below its true supremum SHALL be
recorded as such rather than silently corrected by a margin that does not bound
it.

#### Scenario: A curve with a known supremum declares it
- **WHEN** the steepest slope is asked of a curve whose derivative supremum is known in closed form
- **THEN** that value is returned, and a dense sample of the curve does not exceed it

#### Scenario: A curve without one keeps its margin
- **WHEN** the steepest slope is asked of a curve whose supremum is not established in closed form
- **THEN** a sampled maximum carrying a margin is returned

#### Scenario: The declared slope bounds the curve
- **WHEN** every easing curve is sampled densely and its difference quotient compared against its declared slope
- **THEN** no curve is observed steeper than it declares, except those recorded as known exceptions
