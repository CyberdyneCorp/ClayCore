# Design

Each dimension-d brick owns d³ cells. The current marcher fetches eight corners for each cell, though only (d+1)³ distinct lattice points are needed. At d=8 this reduces cache lookups from 4,096 to 729.

A private bounded sample block stores those values in z/y/x order. It receives the original sampler and global low corner; the original sampler still determines every value, including neighboring, missing and uniform bricks. Access maps the unchanged global coordinates to a local index. March-cell and march-cells helpers accept a typed callable so the local array access can inline. Existing lattice callers retain their original sampling behavior.

Only the optimized ordinary-brick branch uses the block. The general/reference branch and boundary-cell collection remain independent. There is no storage shared across workers or calls. The block is stack-local and bounded at 19,652 sample bytes; it is not an unbounded allocation proportional to a model.

Tests must verify every stored float bit, coordinate mapping, exact callback count, dimensions 1–16, negative origins, independent blocks, and complete mesh equality with the reference. Existing subset, LOD, boundary and thread-count comparisons must continue to pass. Large/invalid dimensions retain the existing fallback. Measure production and live timing before claiming application benefit.
