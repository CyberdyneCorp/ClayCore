# python-bindings — Trim Curve

Delta for `add-trim-curve`.

## ADDED Requirements

### Requirement: Trimming from Python
The module SHALL expose building a trim shape from an open curve and the side it covers, alongside the closed-lasso constructor, and SHALL say which is which so a caller does not reach for the lasso when it means a trim.

#### Scenario: A script trims a form in half
- **WHEN** a script resolves an open curve as a trim and places it with subtract
- **THEN** one side of the form is removed and the other is untouched
