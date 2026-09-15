# Skip welding exclusive brick edges

## Why

After local edge deduplication, ordinary interior edges belong to only one requested brick. Global welding still hashes and stores every such edge. In issue #531's fixtures, avoiding those unnecessary lookups reduces full-sphere meshing from 19.54 to 13.36 ms without attributes while preserving complete output.

## What changes

Prove eligibility once for unique, bounded requested keys with dimensions 1–16 and no straddlers. Emit locally deduplicated interior edges directly; retain global welding for every boundary edge and every unsupported request. Keep vertex order, interpolation, attributes, triangle indices and brick ranges unchanged.

## Scope

Private CPU meshing implementation and regression coverage. Public APIs and data formats stay unchanged. This is another increment toward the still-open all-brush 16 ms target.
