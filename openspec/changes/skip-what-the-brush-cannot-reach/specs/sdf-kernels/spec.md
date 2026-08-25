# sdf-kernels — stop paying for the samples a brush cannot reach

Delta for `skip-what-the-brush-cannot-reach`.

## ADDED Requirements

### Requirement: A brush pays for what it reaches, not for the box around it
An operator confined to a region SHALL be able to declare that region as the shape it actually acts over, and the traversal SHALL reject whole bricks that shape cannot reach. A brush is a ball and the bricks selected for it are a box around one, which holds nearly twice the volume: measured on a dab, most of the bricks selected could not hold a sample the brush could reach, and most of the samples visited were weighed and handed back unchanged.

Rejecting them is sound for the reason confining the traversal is sound at all: the operator is the identity outside its region, so a brick the region cannot reach holds nothing the pass may change. It follows that the traversal and anything preserving its input MUST narrow by the same shape, or what was preserved stops covering what is written.

Deciding that a sample lies outside the region SHALL NOT cost more than the arithmetic that decides it. A weight that falls to nothing outside a radius and interpolates only across a taper SHALL answer both of the uninterpolated cases without a square root, since they are the overwhelming majority of what a brush is handed.

An operator SHALL NOT read a sample it has already been given. A traversal that hands its callback the value held at a sample has handed it the value a lookup of that sample would return, and paying for the lookup as well costs once per sample.

#### Scenario: A ball rewrites what the box around it would
- **GIVEN** an operator that is the identity outside a ball
- **WHEN** it is applied over that ball, over the box around it, and over the whole field
- **THEN** the three produce the same volume, sample for sample

#### Scenario: What was preserved still covers what was written
- **GIVEN** an input preserved over the same ball the traversal is narrowed to
- **WHEN** the ball is rewritten and the preserved input is read
- **THEN** every sample reads as the field held it before the rewrite began

#### Scenario: A dab's cost follows the brush and not the box
- **GIVEN** the same brush applied at a radius small against the bricks its box spans
- **WHEN** a stroke of dabs is applied
- **THEN** what it costs tracks the samples the brush can reach rather than the samples its box contains
