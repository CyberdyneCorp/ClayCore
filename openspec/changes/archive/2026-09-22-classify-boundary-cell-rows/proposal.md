# Classify boundary cell rows

## Why
Issue #531 remains above the 16 ms brush budget. The mesher spends roughly 5 ms collecting boundary triangles on the sphere fixture. Ring enumeration repeatedly tests the same neighboring-brick combinations for every cell.

## What Changes
Classify the three x positions for each of nine y/z boundary categories once per owner. Emit full rows when their interior reaches a requested neighbor; otherwise emit only qualifying endpoints. Handle dimension one explicitly.

## Impact
Internal mesher enumeration only. Preserve cell order, boundary attribution, complete mesh bytes, C ABI and formats. No persistent cache or additional allocation.
