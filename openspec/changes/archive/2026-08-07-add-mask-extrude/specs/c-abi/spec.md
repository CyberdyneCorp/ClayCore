# c-abi — mask extrude

Delta for `add-mask-extrude`.

## ADDED Requirements

### Requirement: Mask extrude through the C ABI
The ABI SHALL expose the mask-to-distance conversion and both extrudes — into a volume item and into a voxel grid — with a versioned parameter block carrying the thickness, side, threshold, rim rounding, border smoothing and sampling, as the relax and flatten blocks already do.

A refused extrude SHALL report a typed error rather than handing back an empty item, so a host distinguishes "the mask missed the surface" from "the surface is small".

The result SHALL be a handle the CALLER owns, on the same lifetime rule the rest of the voxel and volume surface follows.

#### Scenario: A host extracts a plate
- **WHEN** a host paints a mask on a layer and extrudes it
- **THEN** it receives a new item or grid holding the plate, and the source layer is untouched

#### Scenario: A refusal is typed
- **WHEN** an extrude is asked for with an empty mask
- **THEN** the call returns a typed error and produces no handle
