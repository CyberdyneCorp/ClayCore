#pragma once

// The mesh sculpting VOCABULARY, with no implementation attached (meshing
// spec, add-shared-brush-kernels).
//
// This header exists so a sculptor can name a verb, a falloff and a brush's
// settings without including the fixed-topology one. `MeshSculptor` is one
// consumer of this vocabulary; `DynamicSculptor` and `MultiresSculptor` are
// the two that made splitting it out necessary, because including
// `mesh/sculpt.h` to reach `MeshBrush` would have dragged `Adjacency`, `Bvh`
// and the whole fixed-topology contract into representations that hold none of
// them.
//
// EVERYTHING HERE IS REPRESENTATION-NEUTRAL, and that is a rule rather than an
// observation: nothing in this file may name a `Mesh`, an `Adjacency`, a `Bvh`
// or a vertex index. `MeshBrushSettings` already qualified — it is scalars,
// vectors, enums and a borrowed alpha — which is why the extraction could take
// it whole rather than splitting it.
//
// WHY THIS DOES NOT REUSE voxel::BrushFalloff. `voxel` depends on `mesh`
// (`VoxelGrid::mesh_greedy` returns a `mesh::Mesh`), so `mesh` including
// `voxel` would be a cycle. `MeshFalloff` below is deliberately the same four
// curves with the same values and the same weights as `voxel::BrushFalloff`,
// and the duplication is the module layering rather than an oversight.

#include <cstdint>

#include "clay/field/flatten.h"  // FlattenMode
#include "clay/kernel/shim.h"
#include "clay/mesh/automask.h"

namespace clay {
namespace mesh {

// Falloff over the normalized distance from the brush centre. Same curves,
// same values and same weights as `voxel::BrushFalloff`; see the header note.
enum class MeshFalloff : std::uint8_t {
    Constant = 0,
    Linear = 1,
    Smooth = 2,  // smoothstep
    Gaussian = 3,
};

// The classical vocabulary. Each is a distinct vertex operation; the second
// group composes from the first, but each composition is ONE stamp against ONE
// snapshot rather than a sequence of calls — see `BrushRegion`.
enum class MeshBrush : std::uint8_t {
    // -- the primitives ------------------------------------------------------
    Grab = 0,     // drag the region by the stroke delta
    Draw = 1,     // displace along the REGION's averaged normal
    Inflate = 2,  // displace along EACH VERTEX's own normal, signed
    Smooth = 3,   // Laplacian average over the one-ring
    Pinch = 4,    // signed tangential gather (+) / spread (-) about the centre
    Flatten = 5,  // project toward a plane, in one of three modes
    // -- the compositions ----------------------------------------------------
    Clay = 6,        // draw's deposit CLAMPED to a plane at the stamp height
    Crease = 7,      // a tight negative draw and a pinch, in one stamp
    Scrape = 8,      // flatten cut-only and smooth, from one snapshot
    Polish = 9,      // smooth GATED by dihedral angle: noise goes, edges stay
    Snakehook = 10,  // grab re-anchored along the drag
    // -- the three the vocabulary was missing ---------------------------------
    // Slide vertices ALONG the surface to even their spacing, rather than
    // toward the Laplacian average. Smooth reshapes; this redistributes. It
    // matters more here than in a tool that can subdivide: topology is fixed,
    // so a large grab stretches the triangles it has and this is what recovers
    // them without a round trip through a retopo pass.
    Relax = 11,
    // Deposit up to a fixed height above the surface as it was when the STROKE
    // began, and no further. Every other deposit verb accumulates, so a slow
    // stroke digs deeper than a fast one over the same path; this one does not.
    // Needs the stroke's VertexDeltas to know where it started — see `stamp`.
    Layer = 12,
    // Push material along the surface in the drag direction. Grab carries the
    // region rigidly; this slides it.
    Nudge = 13,
    // -- the colour pair, and the only verbs that do not move a vertex --------
    // Blend each vertex's colour toward `MeshBrushSettings::color` by the
    // brush's own per-vertex weight, so falloff, strength, the geodesic walk,
    // the mask gate and the alpha stamp all compose with it for free.
    Paint = 14,
    // Push existing colour along `direction`, by blending each vertex toward
    // the one-ring neighbour lying most nearly OPPOSITE the drag. Topology is
    // fixed here, so the one-ring is a complete and cheap account of where the
    // colour just came from — no spatial query, and no interpolation scheme to
    // disagree with the rest of the library about.
    Smear = 15,
};

// Whether a verb writes `Mesh::colors` instead of moving vertices. The two are
// exclusive on purpose: a colour pass over a finished sculpt must not show up
// as a diff on the geometry, and a displacement verb must not disturb an
// imported model's colours.
bool writes_color(MeshBrush verb);

inline constexpr std::uint32_t kNoClass = 0xffffffffu;

// The seed token a sculptor hands out and a stamp checks a caller's seed
// against. Zero means "I did not check", which is what every caller that has
// never heard of a revision sends, and is why the default preserves the
// bounds-check-only behaviour this field was added beside rather than
// replacing it.
inline constexpr std::uint64_t kNoSeedRevision = 0;

// The most smoothing passes one stamp will run. A bound rather than a
// preference: each pass walks the whole region again, so a count arriving from
// a host's slider typo is otherwise an unbounded amount of work.
inline constexpr int kMaxSmoothIterations = 64;

struct MeshBrushSettings {
    // Where the stamp lands, in the mesh's own space, and how far it reaches.
    kernel::cfloat3 center = kernel::cf3(0, 0, 0);
    float radius = 0.25f;

    // Signed for every verb that has a sign: Inflate and Draw deposit or dig,
    // Pinch gathers (+) or spreads (-), Crease cuts (+) or raises a ridge (-).
    // Scaled into world units by `radius`, so a brush behaves the same at any
    // size and a strength of 1 moves the surface by about one radius.
    float strength = 0.5f;

    MeshFalloff falloff = MeshFalloff::Smooth;

    // Grab and Snakehook only: the motion THIS stamp applies, in world units.
    // The other verbs ignore it.
    kernel::cfloat3 direction = kernel::cf3(0, 0, 0);

    // Draw, Clay and Crease: an explicit deposit direction. Zero — the default
    // — means "the region's averaged normal", which is what makes Draw a
    // rounded organic swell instead of a balloon.
    kernel::cfloat3 deposit_normal = kernel::cf3(0, 0, 0);

    // Measure the falloff ALONG THE SURFACE rather than in a straight line.
    // The Move Topological rule: a brush on the upper lip must not drag the
    // chin through the closed mouth. Off for the verbs whose meaning is
    // "everything under this disc" — see `default_geodesic`.
    bool geodesic = true;

    // The geodesic walk's seed, when the caller knows it — any class of the
    // triangle the pick hit. Without one the seed is found by a linear scan
    // over the classes, which is fine for a test and wrong for a stroke on a
    // million-vertex mesh.
    std::uint32_t seed_class = kNoClass;

    // WHICH CLASS SPACE `seed_class` WAS PICKED IN. A seed is an index, and an
    // index outlives the numbering it was taken from: a hierarchy rebinds its
    // level sculptor whenever the sculpt level or the cache generation moves
    // (`MultiresSculptor::bind`), and every rebind is a new set of classes. A
    // seed picked at level 3 and spent at level 4 is still comfortably IN
    // BOUNDS, so the bounds check above cannot see it, and what it buys is not
    // a slightly wrong region — `geodesic_region` returns EMPTY when the seed
    // is farther than the radius from the centre, so the dab silently does
    // nothing and the host has no way to tell that from a masked stroke.
    //
    // Carrying the token the picker was handed makes that case a rejected seed
    // and a scan rather than a lost stamp. Zero (`kNoSeedRevision`) means the
    // caller is not claiming anything, and is what keeps every shipped caller
    // — the C ABI, pyclay's `pick`, every test — behaving exactly as before.
    //
    // The rejected alternative was validating the seed GEOMETRICALLY, by
    // checking that the class it names sits within the radius. That accepts a
    // stale seed whenever the two levels happen to overlap there, which is the
    // common case for a hierarchy of the same model, so it would have passed
    // the obvious test and failed in the field.
    std::uint64_t seed_revision = kNoSeedRevision;

    // Flatten and Scrape. CutOnly is the hard-surface family — Trim Dynamic,
    // hPolish, the Planar brushes — where cutting WITHOUT filling is the whole
    // brush.
    field::FlattenMode flatten_mode = field::FlattenMode::TwoSided;

    // An explicit plane for Flatten and Scrape. Without one the plane is the
    // region's weighted centroid with the region's weighted average normal —
    // the plane ZBrush and Blender both use, and the one an artist means by
    // "flatten what is under the brush".
    bool use_given_plane = false;
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    // Polish: how far the surface around a vertex may bend before the
    // smoothing fades out, in radians. Full strength up to this angle, zero at
    // twice it — so an edge survives a pass that removes the noise beside it.
    //
    // The default is deliberately TIGHT. Measured on a dented box (see
    // examples/46), 0.2 removes marginally more roughness than a plain smooth
    // while moving the box's edges a third as far; by 0.45 the gate is open
    // almost everywhere and polish is a plain smooth wearing a different name,
    // which is not a useful default for a brush called polish.
    float polish_angle = 0.20f;

    // Smooth, Polish, Scrape and Relax: passes per stamp. Bounded by
    // kMaxSmoothIterations.
    int smooth_iterations = 1;

    // Layer only: how far above the stroke's STARTING surface the deposit may
    // reach, in WORLD units.
    //
    // World rather than radius-relative, unlike `strength`, and that is the
    // point rather than an inconsistency: a ceiling that moved when the brush
    // resized would not be a ceiling. Negative digs to a floor instead.
    float layer_height = 0.05f;

    // -- alpha ---------------------------------------------------------------
    // A caller-supplied scalar stamp scaling this brush's per-vertex weight, so
    // detail work on a mesh layer is alpha-driven as it already is on voxels
    // (sculpt_carve_alpha) and on SDF items (Deformer::alpha).
    //
    // THE ENGINE DECODES NO IMAGES: `alpha` is `alpha_width * alpha_height`
    // samples in [0,1], row-major with u fastest, BORROWED for the duration of
    // the call. Null — the default — leaves every verb exactly as it was.
    //
    // Sampled by the kernel's `calpha_sample`, the same function the SDF alpha
    // uses, so one stamp reads identically on a mesh and on a field rather than
    // through two bilinear lookups that could drift apart.
    //
    // It multiplies the WEIGHT, so it composes with every verb and every
    // falloff without a line of per-verb code.
    const float* alpha = nullptr;
    int alpha_width = 0;
    int alpha_height = 0;
    // The square the stamp covers, in the plane through `center` whose normal
    // is `alpha_direction`; `alpha_tangent` orients it there and any rough "up"
    // works, since it is re-orthogonalised. Zero extent means the brush's own
    // diameter, which is what a host stamping under the cursor wants.
    kernel::cfloat3 alpha_direction = kernel::cf3(0, 0, 0);  // 0 = region normal
    kernel::cfloat3 alpha_tangent = kernel::cf3(0, 0, 0);    // 0 = derived
    float alpha_extent = 0.0f;                               // 0 = 2 * radius

    // -- the stamp's grain ----------------------------------------------------
    // How far the stamp's in-plane axes are turned about its own facing, in
    // radians. Zero — the default — is no rotation AT ALL rather than a
    // rotation by zero: see `make_stamp_frame`, where the difference is a sign
    // bit and a moved golden.
    //
    // On the brush's own frame rather than on the alpha, although the alpha is
    // its first consumer, because it orients everything the stamp does
    // directionally — a rotated alpha and a raked grain are the same angle, and
    // two fields for one angle is two chances for a host to set the wrong one.
    float stamp_azimuth = 0.0f;

    // -- colour ---------------------------------------------------------------
    // Paint's target. Whatever space the caller keeps `Mesh::colors` in: this
    // is blended toward componentwise and never converted, so a linear buffer
    // stays linear and an sRGB one stays sRGB.
    //
    // Smear ignores it — its colour comes from the surface it is dragging
    // across, which is the whole difference between the two verbs.
    kernel::cfloat3 color = kernel::cf3(1, 1, 1);

    // -- automasking ----------------------------------------------------------
    // The gates the brush applies to ITSELF, composed into the per-vertex
    // weight rather than branched into each verb. Off by default, and a stamp
    // with none is bit-identical to one taken before automasking existed — the
    // factor is applied last, so it multiplies by an exact 1.0.
    //
    // The two factors this struct cannot carry — the cavity estimator and the
    // group field — are set on the SCULPTOR for the stroke, because they hold
    // `std::function`s and copying those per stamp is an allocation per dab.
    AutomaskSettings automask;

    bool has_alpha() const { return alpha != nullptr && alpha_width >= 2 && alpha_height >= 2; }
};

// Whether a verb measures its falloff along the surface by default. Flatten
// and Scrape do not: they mean "everything under this disc", and a surface
// walk would refuse to flatten across a groove — which is the one place a
// flatten is most wanted.
bool default_geodesic(MeshBrush verb);
}  // namespace mesh
}  // namespace clay
