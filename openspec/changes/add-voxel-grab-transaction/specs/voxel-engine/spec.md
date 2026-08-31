## ADDED Requirements

### Requirement: A voxel drag is a gesture, not a sequence of calls
A voxel grab applied repeatedly SHALL be expressible as a transaction that captures the material as it was when the gesture began and resamples from that capture, so that the result depends on the TOTAL displacement and not on how the host chose to deliver it.

A stateless grab SHALL NOT be expected to compose. Each call reads the grid and writes back, so a second call reads the first's output; and the displacement is rounded to whole cells after the falloff weights it, so a finely split drag can move nothing at all while every call succeeds. That SHALL be stated where the stateless call is documented.

An update SHALL take the total displacement from the anchor and SHALL be idempotent: repeating one changes nothing, and returning to the anchor restores the material.

A single update from a fresh transaction SHALL produce cell-for-cell what the stateless call produces for the same displacement.

The captured region MAY widen as the drag reaches further, which is sound because a grab writes only inside its own footprint: any cell the capture later takes in has not been written by the gesture.

Cancelling SHALL restore the material exactly, and abandoning a transaction without committing SHALL cancel rather than leave a partial gesture in the grid.

#### Scenario: The split does not change the result
- **WHEN** the same total drag is delivered as one, two, four and eight updates
- **THEN** every one of them leaves the grid in the same state

#### Scenario: A split drag still moves material
- **WHEN** a drag of several cells is delivered in eight updates
- **THEN** material occupies cells it did not occupy before

#### Scenario: An update is idempotent
- **WHEN** the same total displacement is passed to update more than once
- **THEN** the grid is what one update left

#### Scenario: A drag returns to where it started
- **WHEN** a drag is updated out and then back to zero
- **THEN** the material is what it was when the gesture began

#### Scenario: One update is the stateless call
- **WHEN** a fresh transaction is updated once
- **THEN** the grid matches what the stateless grab produces for that displacement

#### Scenario: An abandoned drag leaves nothing behind
- **WHEN** a transaction is destroyed without being committed
- **THEN** the material is what it was when the gesture began
