#pragma once

// Making a sampled field a DISTANCE field again (sdf-kernels spec,
// add-consolidation-policy).
//
// WHY THIS EXISTS. Baking does not bound a field's Lipschitz. That is the
// measurement the consolidation policy was written against and it is worth
// stating plainly, because the opposite is the intuitive guess: resampling a
// steep field onto a lattice reproduces the steepness, it does not remove it.
// Two polish passes over a sphere produce a field whose stored samples vary at
// 14x the cell size; sampling THAT into a fresh volume gives samples that vary
// at 14x the cell size, and a finer cell makes it worse rather than better
// because there are then more cells across the same steep shell.
//
// Steepness is a property of the FIELD, and the only thing that removes it is
// replacing the field with the distance to its own zero set. The zero set is
// what an artist sees; everything else about the samples is bookkeeping the
// marcher pays for. So consolidation is a bake AND this, and without this half
// a consolidated chain declares the same Lipschitz the chain did.
//
// WHAT IS PRESERVED AND WHAT IS NOT. The surface — the zero set — is preserved
// to the accuracy of the sampling, because the interface is located by linear
// interpolation between the samples that bracket it, which is the same
// approximation the mesher already makes. The values AWAY from the surface are
// deliberately replaced: they are the whole cost being paid off.
//
// WHY UNSIGNED-THEN-RESIGN rather than a signed solve. The sign of a sample is
// exactly the information the input still has right — a steep field crosses
// zero in the same place a gentle one does. Solving the unsigned eikonal from
// the interface and re-applying the input's sign keeps that, and it means the
// solver never has to reason about the sample-free bricks, which carry a lower
// bound rather than a distance and would otherwise seed the sweep with a lie.

#include "clay/field/volume.h"

namespace clay {
namespace field {

// Rewrite `v`'s stored samples as the distance to their own zero set, keeping
// each sample's sign, and re-declare its sample Lipschitz from what the result
// actually measures.
//
// `rounds` is how many times the eight-direction sweep is run. Zhao's fast
// sweeping needs one round of 2^3 sweeps to resolve every characteristic
// direction of a fixed source, so one is the guaranteed default; a second
// round only tightens the corners where two fronts met, and is offered because
// it costs a pass rather than a rewrite when a caller wants it.
//
// A volume with no stored samples, or one whose samples never change sign, has
// no zero set to measure from and is left exactly as it was — reporting a
// distance to a surface that is not there would be worse than reporting the
// bound the bake already produced.
//
// COST. One float and one flag per lattice sample, plus eight passes over
// them. The lattice is denser than the sparse storage on purpose: a sweep
// needs a total order across brick boundaries, and the alternative — a
// priority queue over a hash of stored coordinates — is slower per sample and
// much harder to show correct. The scratch is freed on return, so the peak is
// the volume plus about one and a half times its samples, once.
bool redistance(FieldVolume& v, int rounds = 1);

}  // namespace field
}  // namespace clay
