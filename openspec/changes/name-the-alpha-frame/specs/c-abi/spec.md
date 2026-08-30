# c-abi — alpha refusals

Delta for `name-the-alpha-frame`.

## ADDED Requirements

### Requirement: The alpha entry point refuses a degenerate stamp
`clay_item_add_alpha` SHALL refuse a direction with no length and a non-positive radius, returning an invalid-argument result and leaving the item unchanged, and its documentation SHALL name the space its coordinates are in.

Both inputs were previously accepted with a success result and appended a deformer that did nothing — the case the header's own note calls harder to notice than a failure.

#### Scenario: The two new refusals leave the item alone
- **WHEN** an alpha is added with a zero-length direction, and again with a non-positive radius
- **THEN** both calls return an invalid-argument result, and adding the item afterwards yields the same document as one to which the alpha was never offered
