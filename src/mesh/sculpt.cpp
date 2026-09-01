#include "clay/mesh/sculpt.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>

#include "clay/kernel/deform.h"  // calpha_sample, calpha_frame
#include "clay/mesh/automask.h"

namespace clay {
namespace mesh {
namespace {

// The three geometric helpers that genuinely READ A MESH, and so are the three
// that could not move to `sculpt_kernels.cpp` with the rest. Everything else
// this file used to define — the falloff, the alpha frame, the plane offset,
// the Laplacian, polish's gate and every verb — is now shared math and lives
// there.

// Unnormalized face normal — its length is twice the triangle's area, which is
// exactly the weight an area-weighted vertex normal wants.
kernel::cfloat3 face_normal(const Mesh& m, std::uint32_t tri) {
    const kernel::cfloat3& a = m.positions[m.indices[tri * 3]];
    const kernel::cfloat3& b = m.positions[m.indices[tri * 3 + 1]];
    const kernel::cfloat3& c = m.positions[m.indices[tri * 3 + 2]];
    return kernel::ccross(b - a, c - a);
}

// The interior angle a triangle subtends at one of its corners.
float corner_angle(const Mesh& m, std::uint32_t tri, int corner) {
    const kernel::cfloat3& p = m.positions[m.indices[tri * 3 + corner]];
    const kernel::cfloat3& a = m.positions[m.indices[tri * 3 + (corner + 1) % 3]];
    const kernel::cfloat3& b = m.positions[m.indices[tri * 3 + (corner + 2) % 3]];
    const kernel::cfloat3 u = a - p, v = b - p;
    const float lu = kernel::clength(u), lv = kernel::clength(v);
    if (lu < 1e-20f || lv < 1e-20f) return 0.0f;
    return std::acos(std::clamp(kernel::cdot(u, v) / (lu * lv), -1.0f, 1.0f));
}

// A weld class's geometric normal, ANGLE-weighted over its incident triangles.
//
// GEOMETRIC rather than the mesh's stored normals on purpose: displacement is
// about where the surface is, not about how it shades, and this way a mesh
// imported without normals sculpts exactly like one that has them.
//
// Angle-weighted rather than area-weighted, which is the more usual choice and
// is wrong here. A lattice-derived mesh — anything marching cubes or surface
// nets produced, which is most of what this library hands back — has triangles
// of wildly uneven area, so an area-weighted normal leans toward whichever
// neighbour happens to be large and varies at the LATTICE's frequency rather
// than the surface's. `inflate`, which displaces each vertex along its own
// normal, turns that straight into a golf-ball dimple across the stamp. The
// angle at the corner is a property of the surface rather than of how it was
// triangulated, and the dimple goes away.
kernel::cfloat3 class_normal(const Mesh& m, const Adjacency& adj, std::uint32_t cls) {
    std::size_t n = 0;
    const std::uint32_t* tris = adj.triangles_of(cls, &n);
    kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const kernel::cfloat3 face = safe_normalize(face_normal(m, tris[i]), kernel::cf3(0, 0, 0));
        for (int corner = 0; corner < 3; ++corner)
            if (adj.class_of(m.indices[tris[i] * 3 + corner]) == cls)
                sum = sum + face * corner_angle(m, tris[i], corner);
    }
    return safe_normalize(sum, kernel::cf3(0, 1, 0));
}

}  // namespace

bool writes_color(MeshBrush verb) {
    return verb == MeshBrush::Paint || verb == MeshBrush::Smear;
}

bool default_geodesic(MeshBrush verb) {
    // Flatten and Scrape mean "everything under this disc". A surface walk
    // would refuse to flatten across a groove, which is where a flatten is
    // most wanted.
    return verb != MeshBrush::Flatten && verb != MeshBrush::Scrape;
}

// -- VertexDeltas -------------------------------------------------------------

void VertexDeltas::clear() {
    vertices_.clear();
    before_position_.clear();
    after_position_.clear();
    before_normal_.clear();
    after_normal_.clear();
    before_color_.clear();
    after_color_.clear();
    slot_.clear();
    normals_ = false;
    colors_ = false;
}

std::optional<kernel::cfloat3> VertexDeltas::origin_of(std::uint32_t v) const {
    const auto it = slot_.find(v);
    if (it == slot_.end()) return std::nullopt;
    return before_position_[it->second];
}

void VertexDeltas::note(std::uint32_t v, const Mesh& m) {
    if (slot_.find(v) != slot_.end()) return;
    const bool has_normals = m.normals.size() == m.positions.size();
    const bool has_colors = m.colors.size() == m.positions.size();
    if (vertices_.empty()) {
        normals_ = has_normals;
        colors_ = has_colors;
    }
    slot_.emplace(v, static_cast<std::uint32_t>(vertices_.size()));
    vertices_.push_back(v);
    before_position_.push_back(m.positions[v]);
    after_position_.push_back(m.positions[v]);
    if (normals_ && has_normals) {
        before_normal_.push_back(m.normals[v]);
        after_normal_.push_back(m.normals[v]);
    }
    if (colors_ && has_colors) {
        before_color_.push_back(m.colors[v]);
        after_color_.push_back(m.colors[v]);
    }
}

void VertexDeltas::sync_after(std::uint32_t v, const Mesh& m) {
    auto it = slot_.find(v);
    if (it == slot_.end()) return;
    after_position_[it->second] = m.positions[v];
    if (!after_normal_.empty() && m.normals.size() == m.positions.size())
        after_normal_[it->second] = m.normals[v];
    if (!after_color_.empty() && m.colors.size() == m.positions.size())
        after_color_[it->second] = m.colors[v];
}

bool VertexDeltas::revert(Mesh& m) const {
    if (vertices_.empty()) return true;
    const bool normals = !before_normal_.empty() && m.normals.size() == m.positions.size();
    const bool colors = !before_color_.empty() && m.colors.size() == m.positions.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        const std::uint32_t v = vertices_[i];
        if (v >= m.positions.size()) return false;
    }
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        m.positions[vertices_[i]] = before_position_[i];
        if (normals) m.normals[vertices_[i]] = before_normal_[i];
        if (colors) m.colors[vertices_[i]] = before_color_[i];
    }
    return true;
}

bool VertexDeltas::apply(Mesh& m) const {
    if (vertices_.empty()) return true;
    const bool normals = !after_normal_.empty() && m.normals.size() == m.positions.size();
    const bool colors = !after_color_.empty() && m.colors.size() == m.positions.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i)
        if (vertices_[i] >= m.positions.size()) return false;
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        m.positions[vertices_[i]] = after_position_[i];
        if (normals) m.normals[vertices_[i]] = after_normal_[i];
        if (colors) m.colors[vertices_[i]] = after_color_[i];
    }
    return true;
}

// -- MeshSculptor -------------------------------------------------------------

namespace {

// The seed-token counter. Process-wide and monotonic, so no two live sculptors
// share a token and a token is never reused after one is destroyed — a
// hierarchy destroys and rebuilds its level sculptor on every rebind, and a
// counter that restarted would hand the new one the retired one's identity,
// which is precisely the confusion the token exists to catch.
//
// Relaxed ordering is enough: the only thing required of the value is that it
// differs between sculptors, never that it orders anything against other
// memory.
std::atomic<std::uint64_t> g_seed_revision{kNoSeedRevision};

std::uint64_t next_seed_revision() {
    return g_seed_revision.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

MeshSculptor::MeshSculptor(Mesh& m, float weld_epsilon)
    : mesh_(m), adjacency_(Adjacency::build(m, weld_epsilon)),
      seed_revision_(next_seed_revision()) {}

MeshSculptor::MeshSculptor(Mesh& m, Adjacency adjacency)
    : mesh_(m), adjacency_(std::move(adjacency)), seed_revision_(next_seed_revision()) {}

const Bvh& MeshSculptor::bvh() {
    if (!bvh_) bvh_ = std::make_unique<Bvh>(Bvh::build(mesh_));
    return *bvh_;
}

void MeshSculptor::refresh_bvh() {
    bvh_ = std::make_unique<Bvh>(Bvh::build(mesh_));
    clear_bvh_dirty();  // a rebuild covers everything, so nothing is owed
}

// Record that `cls` moved, for the next refit. Marked through `class_dirty_` so
// a stroke that stamps the same classes hundreds of times keeps ONE entry each.
void MeshSculptor::mark_bvh_dirty(std::uint32_t cls) {
    if (bvh_all_dirty_) return;  // already owed everything
    if (class_dirty_.size() != adjacency_.class_count()) {
        class_dirty_.assign(adjacency_.class_count(), 0);
        dirty_classes_.clear();
    }
    if (cls >= class_dirty_.size() || class_dirty_[cls]) return;
    class_dirty_[cls] = 1;
    dirty_classes_.push_back(cls);
}

void MeshSculptor::clear_bvh_dirty() {
    // Through the list, so the reset costs what was touched. Clearing the mark
    // array would be O(classes), which is O(mesh) on the per-stamp path this
    // whole change exists to keep proportional to the brush.
    for (std::uint32_t c : dirty_classes_)
        if (c < class_dirty_.size()) class_dirty_[c] = 0;
    dirty_classes_.clear();
    bvh_all_dirty_ = false;
}

void MeshSculptor::refit_bvh() {
    if (!bvh_) return;  // nothing built yet; the next bvh() builds a correct one

    // EVERYTHING moved since the last fit, not everything the last stamp moved.
    // `apply_stroke` runs many stamps and `region_` holds only the final one,
    // so deriving the set from it would name a SUBSET — and a subset leaves the
    // unnamed triangles' ancestors holding pre-stroke bounds, which is a query
    // that quietly answers for geometry that has moved.
    if (bvh_all_dirty_) {
        if (!bvh_->refit(mesh_)) refresh_bvh();
        clear_bvh_dirty();
        return;
    }
    if (dirty_classes_.empty()) return;

    // A triangle changes when any of its corners does, so the triangles of the
    // moved classes are exactly the changed set — not merely a superset.
    refit_tris_.clear();
    for (std::uint32_t c : dirty_classes_) {
        std::size_t n = 0;
        const std::uint32_t* tris = adjacency_.triangles_of(c, &n);
        for (std::size_t k = 0; k < n; ++k) refit_tris_.push_back(tris[k]);
    }
    if (refit_tris_.empty()) {
        clear_bvh_dirty();
        return;
    }
    if (!bvh_->refit(mesh_, refit_tris_.data(), refit_tris_.size()))
        refresh_bvh();
    else
        clear_bvh_dirty();
}

// The BVH, refitted to the mesh as it is now.
//
// The brushes needed a spatial index over the surface and this is already one,
// built over the same triangles and — since `refit` — cheap to keep current
// under a stroke. A second structure would be a second thing to keep in step
// with the vertices, and the two would disagree the first time one was missed.
//
// The first call on a large mesh pays a build, exactly as a host's first pick
// does today. Every call after that pays a refit of what moved.
const Bvh* MeshSculptor::surface_index() {
    // NEVER BUILDS ONE. Measured on a million-vertex grid: a build is 689 ms
    // and it saves 1.24 ms per stamp, so building it here would need ~550
    // stamps to break even and every shorter session would be worse off. A
    // brush is not the right caller to decide a host should own a ray tree.
    //
    // Any host that places a brush by picking already has one, because
    // `raycast` builds it on the first pick — and since `refit` it is cheap to
    // keep current. So the common case gets the index for free, and a host that
    // never picks keeps exactly the behaviour it had, by the scans below.
    if (!bvh_) return nullptr;
    refit_bvh();
    return bvh_.get();
}

// Every class with a vertex inside the ball. Exact: a class sits on a vertex,
// a vertex is a corner of every triangle that references it, and the tree
// returns every triangle reaching the ball — so no class inside it is missed.
bool MeshSculptor::classes_in_ball(kernel::cfloat3 centre, float radius,
                                   std::vector<std::uint32_t>* out) {
    const Bvh* tree = surface_index();
    if (!tree) return false;  // no index; the caller scans
    out->clear();
    tree->triangles_in_ball(centre, radius, &ball_tris_);
    const float r2 = radius * radius;
    if (ball_mark_.size() != adjacency_.class_count()) ball_mark_.assign(adjacency_.class_count(), 0);
    for (std::uint32_t t : ball_tris_) {
        const std::size_t base = static_cast<std::size_t>(t) * 3;
        if (base + 2 >= mesh_.indices.size()) continue;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = mesh_.indices[base + k];
            if (v >= mesh_.positions.size()) continue;
            const std::uint32_t c = adjacency_.class_of(v);
            if (c >= ball_mark_.size() || ball_mark_[c]) continue;
            // The tree admits a triangle that merely REACHES the ball, so the
            // corner still has to be tested. Over-admitting there is what makes
            // this exact here.
            if (kernel::cdot2(mesh_.positions[v] - centre) > r2) continue;
            ball_mark_[c] = 1;
            out->push_back(c);
        }
    }
    for (std::uint32_t c : *out) ball_mark_[c] = 0;  // retire what we set
    return true;
}

kernel::cfloat3 MeshSculptor::class_position(std::uint32_t cls) const {
    std::size_t n = 0;
    return mesh_.positions[adjacency_.members(cls, &n)[0]];
}

std::uint32_t MeshSculptor::nearest_class(kernel::cfloat3 p) {
    // Through the tree, which finds the nearest triangle CORNER — and a class
    // sits on a vertex, so the nearest corner names the nearest class. O(log N)
    // where the scan this replaced was O(classes) and, being on the seed path
    // of every unseeded stroke, was the largest single term in a dab on a big
    // mesh (measured 1.34 ms of a 1.34 ms stamp at a million vertices).
    //
    // A class NO TRIANGLE REFERENCES cannot be found this way. Such a vertex is
    // unreachable by a brush anyway — a region is a walk over triangles — so
    // the two answers differ only where the old one was useless.
    const Bvh* tree = surface_index();
    if (!tree) {
        // No ray tree to ask, so the scan this replaced is still the answer.
        const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
        std::uint32_t best = kNoClass;
        float best_d2 = 0.0f;
        for (std::uint32_t c = 0; c < classes; ++c) {
            const float d2 = kernel::cdot2(class_position(c) - p);
            if (best == kNoClass || d2 < best_d2) {
                best = c;
                best_d2 = d2;
            }
        }
        return best;
    }
    const Bvh::NearestVertex hit = tree->nearest_vertex(p);
    if (!hit.found) return kNoClass;
    const std::size_t base = static_cast<std::size_t>(hit.triangle) * 3 +
                             static_cast<std::size_t>(hit.corner);
    if (base >= mesh_.indices.size()) return kNoClass;
    const std::uint32_t v = mesh_.indices[base];
    if (v >= mesh_.positions.size()) return kNoClass;
    return adjacency_.class_of(v);
}

// The brush's OWN facing, for the normal-angle automask. Fixed for the stamp
// and never taken from the region: the region's average normal is weighted by
// the very weights the automask is shaping, so reading it here would be
// circular.
kernel::cfloat3 MeshSculptor::automask_reference(const MeshBrushSettings& settings) {
    if (!is_zero(settings.deposit_normal))
        return safe_normalize(settings.deposit_normal, kernel::cf3(0, 1, 0));
    const std::uint32_t near = automask_seed_ != kNoClass ? automask_seed_
                                                          : nearest_class(settings.center);
    return near != kNoClass ? class_normal(mesh_, adjacency_, near) : kernel::cf3(0, 1, 0);
}

std::uint32_t MeshSculptor::accepted_seed(const MeshBrushSettings& settings) {
    if (settings.seed_class >= adjacency_.class_count()) return kNoClass;
    // A caller that claims nothing gets what it has always got: the bounds
    // check above and nothing more. Silently REFUSING an unrevisioned seed
    // would have been the stricter design and was rejected — it turns every
    // shipped caller's fast path into a full scan, which is a performance
    // regression delivered as a correctness fix.
    if (settings.seed_revision == kNoSeedRevision) return settings.seed_class;
    if (settings.seed_revision == seed_revision_) return settings.seed_class;
    // In bounds, and from somebody else's numbering. Falling back to the scan
    // costs one query; honouring it costs the stamp, because a seed outside the
    // radius makes `geodesic_region` return an empty region.
    ++stale_seeds_rejected_;
    return kNoClass;
}

void MeshSculptor::gather(const MeshBrushSettings& settings, const field::MaskGate& gate) {
    BrushRegion& r = region_;
    // Retire the LAST stamp's slots before `r.classes` is overwritten, so the
    // reset costs what that stamp touched. `adjacency.h` states the rule this
    // follows and the reason: "allocating a per-class array per stamp is the
    // entire cost of the stroke". `slot` has to stay a full per-class array
    // because the verbs index it by arbitrary ring neighbours — what changes is
    // that it is sized ONCE and never cleared wholesale again.
    if (r.slot.size() != adjacency_.class_count()) {
        r.slot.assign(adjacency_.class_count(), kNoClass);
        r.classes.clear();
    } else {
        for (std::uint32_t c : r.classes)
            if (c < r.slot.size()) r.slot[c] = kNoClass;
    }
    // Resolved ONCE, before the two region shapes divide, because both of them
    // read the caller's seed and a seed rejected on one path must be rejected
    // on the other. `kNoClass` from here means "no usable seed was given",
    // which is the state each branch below already knows how to handle.
    const std::uint32_t given_seed = accepted_seed(settings);
    if (settings.geodesic) {
        // Seeded from the index when there is one. `geodesic_region` scans
        // every class for a seed when it is given none, which put an O(mesh)
        // term on the DEFAULT path — `seed_class` was the only way out and few
        // callers have one.
        //
        // Without an index the seed is left unset, so that scan still happens
        // — inside `geodesic_region`, exactly as before. Doing it here instead
        // measured 1.30 -> 1.98 ms at a million classes, and the cause was one
        // extra branch per iteration in a differently-shaped copy of the same
        // loop. Two copies of a hot scan is a defect whichever is faster.
        std::uint32_t seed = given_seed;
        if (seed >= adjacency_.class_count() && surface_index() != nullptr)
            seed = nearest_class(settings.center);
        automask_seed_ = seed < adjacency_.class_count() ? seed : kNoClass;
        geodesic_region(mesh_, adjacency_, settings.center, settings.radius, walk_, &r.classes,
                        &distance_, seed);
    } else {
        // The euclidean region IS a ball query, and used to be written as a
        // scan over every class. Flatten and Scrape are the two verbs that take
        // this path (`default_geodesic`), and they are the two that most want a
        // large radius.
        // The connectivity automask needs an anchor and a ball query has no
        // walk to have chosen one, so it is resolved here — and ONLY when a
        // factor actually wants it, because it costs a query.
        automask_seed_ = kNoClass;
        if (has_factor(settings.automask.factors, AutomaskFactor::TopologyConnected)) {
            automask_seed_ =
                given_seed < adjacency_.class_count() ? given_seed : nearest_class(settings.center);
        }
        if (classes_in_ball(settings.center, settings.radius, &r.classes)) {
            // SORTED, so the region is the same list whether it came from the
            // tree or from the scan below — and so it does not depend on the
            // tree's shape, which a rebuild changes. The verbs accumulate a
            // weighted normal and a plane over this list, and float addition is
            // not associative, so an order that varied would make a brush's
            // result depend on whether the host happened to have picked.
            std::sort(r.classes.begin(), r.classes.end());
            distance_.resize(r.classes.size());
            for (std::size_t i = 0; i < r.classes.size(); ++i)
                distance_[i] = kernel::clength(class_position(r.classes[i]) - settings.center);
        } else {
            euclidean_region(mesh_, adjacency_, settings.center, settings.radius, &r.classes,
                             &distance_);
        }
    }

    r.weights.resize(r.classes.size());
    r.positions.resize(r.classes.size());
    r.normals.resize(r.classes.size());

    // The alpha's frame, once for the whole stamp rather than per vertex. Its
    // direction defaults to the brush's own averaged normal — which is not yet
    // computed here, so an unset direction uses the mesh normal nearest the
    // centre. Good enough, and cheaper than a second pass: the stamp is a disc
    // on a surface, and its normal barely varies across one brush radius.
    AlphaFrame alpha_frame;
    if (settings.has_alpha()) {
        // The fallback is resolved LAZILY, only when the caller supplied no
        // direction: it costs a nearest-class query, and a caller that named a
        // direction must not pay for one.
        kernel::cfloat3 fallback = kernel::cf3(0, 1, 0);
        if (kernel::clength(settings.alpha_direction) < 1e-9f) {
            const std::uint32_t near = nearest_class(settings.center);
            if (near != kNoClass) fallback = class_normal(mesh_, adjacency_, near);
        }
        alpha_frame = alpha_frame_for(settings, fallback);
    }

    // Weigh, snapshot, and drop what the falloff or the mask reduced to
    // nothing — compacting in place so the region is exactly what moves.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < r.classes.size(); ++i) {
        const std::uint32_t c = r.classes[i];
        std::size_t mc = 0;
        const kernel::cfloat3 p = mesh_.positions[adjacency_.members(c, &mc)[0]];
        // THE WALK DECIDES WHAT IS REACHED; THE STRAIGHT LINE DECIDES HOW MUCH.
        //
        // Weighing by the walk's own distance was tried first and looks wrong:
        // an edge path overestimates geodesic distance by a direction-dependent
        // amount, so on an irregular triangulation the falloff picks up a
        // visible herringbone banding that a render shows and the numbers do
        // not. The straight-line distance carries no such bias, and it is
        // bounded by the walk's — a class the walk reached within the radius is
        // within the radius in space too — so weighing by it costs nothing.
        //
        // What survives is exactly the property the walk exists for: the chin
        // is not REACHED from the upper lip, so it is not in the region at all
        // and its weight never comes up. The price is a step at the region's
        // rim on a strongly folded surface, where the two distances diverge; on
        // ordinary curvature they agree to a few percent and the falloff there
        // is already near zero.
        //
        // THE FACTORS ARE COMPOSED IN ONE FIXED ORDER, in `compose_weight`,
        // and the order is the contract rather than an implementation detail:
        // these are separate multiplications, float multiplication is not
        // associative, and re-associating them moves the last bit of every
        // displacement that reads the weight.
        WeightFactors f;
        f.falloff = falloff_weight(settings.falloff,
                                   kernel::clength(p - settings.center) / settings.radius);
        // ...and fade out over the last stretch of the walk's path budget, so
        // the rim is smooth even where the two bounds disagree. See
        // kPathTaperStart. A euclidean region has no walk, so it passes 1 and
        // the multiplication is the identity.
        if (settings.geodesic)
            f.path_taper = path_taper(distance_[i] / settings.radius, kPathTaperStart,
                                      kDefaultPathBudget);
        if (gate) f.gate = gate(p);
        // The alpha multiplies the WEIGHT, which is why it needs no per-verb
        // code: every verb already scales by this, and every falloff already
        // shaped it.
        f.alpha = alpha_at(settings, alpha_frame, p);
        const float w = compose_weight(f);
        if (w <= 0.0f) continue;
        r.classes[kept] = c;
        r.weights[kept] = w;
        r.positions[kept] = p;
        r.normals[kept] = class_normal(mesh_, adjacency_, c);
        ++kept;
    }
    r.classes.resize(kept);
    r.weights.resize(kept);
    r.positions.resize(kept);
    r.normals.resize(kept);

    // Already cleared at the top of this call, for exactly the entries the last
    // stamp set; only the new region's entries are written here.
    for (std::size_t i = 0; i < r.classes.size(); ++i)
        r.slot[r.classes[i]] = static_cast<std::uint32_t>(i);

    // THE AUTOMASK IS A SECOND PASS, and it has to be: two of its five factors
    // — the boundary fade and the connectivity walk — spread over the workset's
    // own neighbourhood, so they cannot be answered one vertex at a time while
    // the workset is still being built. It multiplies into the weight LAST,
    // which is what keeps a stamp with no automask on exactly the bits it had
    // before automasking existed.
    r.automask.clear();
    if (settings.automask.any()) {
        r.automask.resize(kept);
        compute_automask(mesh_, adjacency_, r, settings.automask, automask_inputs_,
                         automask_reference(settings), automask_seed_, r.automask.data());
        std::size_t survived = 0;
        for (std::size_t i = 0; i < kept; ++i) {
            const float w = r.weights[i] * r.automask[i];
            if (w <= 0.0f) {
                // A fully automasked vertex leaves the workset entirely, which
                // is what makes it BIT-IDENTICAL to its input rather than
                // merely close: nothing writes it at all.
                r.slot[r.classes[i]] = kNoClass;
                continue;
            }
            r.classes[survived] = r.classes[i];
            r.weights[survived] = w;
            r.positions[survived] = r.positions[i];
            r.normals[survived] = r.normals[i];
            r.automask[survived] = r.automask[i];
            ++survived;
        }
        if (survived != kept) {
            r.classes.resize(survived);
            r.weights.resize(survived);
            r.positions.resize(survived);
            r.normals.resize(survived);
            r.automask.resize(survived);
            for (std::size_t i = 0; i < survived; ++i)
                r.slot[r.classes[i]] = static_cast<std::uint32_t>(i);
            kept = survived;
        }
    }

    // The plane and the shared direction, taken from the snapshot and never
    // from what the stamp is about to deposit.
    kernel::cfloat3 nsum = kernel::cf3(0, 0, 0), psum = kernel::cf3(0, 0, 0);
    float wsum = 0.0f;
    for (std::size_t i = 0; i < kept; ++i) {
        nsum = nsum + r.normals[i] * r.weights[i];
        psum = psum + r.positions[i] * r.weights[i];
        wsum += r.weights[i];
    }
    r.average_normal = safe_normalize(nsum, kernel::cf3(0, 1, 0));
    r.centroid = wsum > 0.0f ? psum / wsum : settings.center;
    if (settings.use_given_plane) {
        r.plane_point = settings.plane_point;
        r.plane_normal = safe_normalize(settings.plane_normal, r.average_normal);
    } else {
        // The weighted centroid with the weighted average normal: the plane
        // ZBrush and Blender both flatten onto, and the one an artist means by
        // "flatten what is under the brush". A least-squares fit would be a
        // better plane for a flat noisy patch and a worse one for a curved
        // patch, which is the case that matters.
        r.plane_point = r.centroid;
        r.plane_normal = r.average_normal;
    }

    // The workset's peak, published once the region is FINAL — after the
    // falloff and the automask have dropped what they drop. Reporting the
    // pre-compaction count would make the number a property of the brush radius
    // rather than of the memory a stamp actually stands on, which is what a
    // host tunes a profile against.
    if (telemetry_ != nullptr) telemetry_->observe_workset(r.size());
}

// THE PLAN IS COMPILED ONCE PER STROKE, not per stamp.
//
// Nothing in it depends on where the stamp landed or how hard: the verb decides
// the kernel, the kernel decides what the gather owes it, and only
// `smooth_iterations` and the geodesic override come from the settings. So the
// key is those three, and a stroke of two hundred stamps with an unchanged
// brush compiles exactly once.
const BrushRuntimePlan& MeshSculptor::plan_for(MeshBrush verb,
                                               const MeshBrushSettings& settings) {
    if (!plan_valid_ || plan_verb_ != verb || plan_iterations_ != settings.smooth_iterations ||
        plan_geodesic_ != settings.geodesic) {
        plan_ = compile_plan(model_of(verb), settings);
        plan_verb_ = verb;
        plan_iterations_ = settings.smooth_iterations;
        plan_geodesic_ = settings.geodesic;
        plan_valid_ = true;
        ++plan_compilations_;
    }
    return plan_;
}

std::size_t MeshSculptor::stamp(MeshBrush verb, const MeshBrushSettings& settings,
                                const field::MaskGate& gate, VertexDeltas* record) {
    if (!valid() || settings.radius <= 0.0f || mesh_.positions.empty()) return 0;
    const BrushRuntimePlan& plan = plan_for(verb, settings);
    gather(settings, gate);
    if (region_.empty()) return 0;

    if (plan.needs_neighbors)
        build_neighbors(plan.needs_neighbor_normals, plan.needs_neighbor_colors);

    const SculptSnapshot snapshot = snapshot_of();
    const SculptNeighbors neighbors = plan.needs_neighbors ? neighbors_of() : SculptNeighbors{};

    // The colour verbs take a different path end to end: they fill a colour
    // target rather than a displacement, and they write through write_colors.
    // Sharing `displacement_` would have made "moved nothing" and "painted
    // nothing" the same number.
    if (plan.model.target == BrushWriteTarget::Color)
        return stamp_color(verb, settings, snapshot, neighbors, record);

    displacement_.assign(region_.size(), kernel::cf3(0, 0, 0));
    kernel::cfloat3* out = displacement_.data();

    switch (verb) {
        case MeshBrush::Grab:
        case MeshBrush::Snakehook:
            // One stamp of snakehook IS a grab. What makes it a snakehook is
            // the RE-ANCHORING between stamps, which brush::apply_to_mesh does
            // by walking the brush centre along the drag.
            kernel_grab(snapshot, settings, out);
            break;
        case MeshBrush::Draw:
            kernel_draw(snapshot, settings, out);
            break;
        case MeshBrush::Inflate:
            kernel_inflate(snapshot, settings, out);
            break;
        case MeshBrush::Pinch:
            kernel_pinch(snapshot, settings, out);
            break;
        case MeshBrush::Flatten:
            kernel_flatten(snapshot, settings, out);
            break;
        case MeshBrush::Clay:
            kernel_clay(snapshot, settings, out);
            break;
        case MeshBrush::Crease:
            kernel_crease(snapshot, settings, out);
            break;
        case MeshBrush::Smooth:
        case MeshBrush::Polish:
        case MeshBrush::Scrape:
            kernel_smooth_family(verb, snapshot, neighbors, settings, scratch_, out);
            break;
        case MeshBrush::Nudge:
            kernel_nudge(snapshot, settings, out);
            break;
        case MeshBrush::Relax:
            kernel_relax(snapshot, neighbors, settings, scratch_, out);
            break;
        case MeshBrush::Layer:
            // Without a record there is no stroke to measure from, and every
            // stamp would clamp against the CURRENT surface — which is draw
            // wearing layer's name. A verb that silently becomes a different
            // verb is worse than one that refuses. The plan is what says this
            // kernel needs the gesture's origin.
            if (plan.needs_stroke_origin && !record) return 0;
            gather_stroke_origin(*record);
            kernel_layer(snapshot, settings, origin_.data(), out);
            break;
        case MeshBrush::Paint:
        case MeshBrush::Smear:
            // Returned above, through write_colors. Named here rather than
            // left to a default so that adding a verb still fails the switch.
            break;
    }
    return write(record);
}

// The colour half of `stamp`, lifted out so the displacement switch reads as
// one thing. It shares the gather, the snapshot and the neighbours with the
// verbs above and diverges only in where it writes.
std::size_t MeshSculptor::stamp_color(MeshBrush verb, const MeshBrushSettings& settings,
                                      const SculptSnapshot& snapshot,
                                      const SculptNeighbors& neighbors, VertexDeltas* record) {
    if (!has_colors()) return 0;  // an explicit ensure_colors is the fix
    color_target_.resize(region_.size());
    for (std::size_t i = 0; i < region_.size(); ++i) {
        std::size_t mc = 0;
        const std::uint32_t v = adjacency_.members(region_.classes[i], &mc)[0];
        color_target_[i] = mesh_.colors[v];
    }
    // A copy, because both kernels read every entry's PRE-STAMP colour while
    // writing the same array: a smear that read what it had already written
    // would be a sweep whose result depends on the order the entries sit in.
    //
    // A MEMBER rather than a local, which is the allocation gate rather than a
    // style choice: a local vector here allocated and freed on every dab of
    // every colour stroke, which is exactly the "an ordinary local stamp
    // performs no heap allocation" rule this change is asserting.
    color_current_.assign(color_target_.begin(), color_target_.end());
    if (verb == MeshBrush::Paint)
        kernel_paint(snapshot, settings, color_current_.data(), color_target_.data());
    else
        kernel_smear(snapshot, neighbors, settings, color_current_.data(), color_target_.data());
    return write_colors(record);
}

// The snapshot the kernels read: the region, in the neutral shape. No copy —
// the region already holds these arrays contiguously, which is why it was
// worth keeping them parallel rather than as a vector of structs.
SculptSnapshot MeshSculptor::snapshot_of() const {
    SculptSnapshot s;
    s.positions = region_.positions.data();
    s.normals = region_.normals.data();
    s.weights = region_.weights.data();
    s.count = region_.size();
    s.average_normal = region_.average_normal;
    s.centroid = region_.centroid;
    s.plane_point = region_.plane_point;
    s.plane_normal = region_.plane_normal;
    return s;
}

SculptNeighbors MeshSculptor::neighbors_of() const {
    SculptNeighbors n;
    n.offsets = nb_offsets_.data();
    n.slots = nb_slots_.data();
    n.positions = nb_positions_.data();
    n.normals = nb_normals_.empty() ? nullptr : nb_normals_.data();
    n.colors = nb_colors_.empty() ? nullptr : nb_colors_.data();
    return n;
}

// Flatten the region's one-rings into the CSR the kernels read.
//
// The walk is the one the verbs used to do inline, done once instead of once
// per verb and once per smoothing pass. `r.slot` already answers "is this
// neighbour under the brush", and `kNoClass` and `kOutsideRegion` are the same
// value on purpose — the static_assert below is what keeps them that way, since
// a region slot is copied straight into the neighbour list.
void MeshSculptor::build_neighbors(bool want_normals, bool want_colors) {
    static_assert(kNoClass == kOutsideRegion,
                  "a region slot is copied into the neighbour list unchanged");
    const BrushRegion& r = region_;
    // Cleared keeping capacity: a stroke of similar stamps allocates on its
    // first one and never again.
    nb_offsets_.clear();
    nb_slots_.clear();
    nb_positions_.clear();
    nb_normals_.clear();
    nb_colors_.clear();
    nb_offsets_.push_back(0);
    const bool colors = want_colors && has_colors();
    for (std::size_t i = 0; i < r.size(); ++i) {
        std::size_t n = 0;
        const std::uint32_t* ring = adjacency_.ring(r.classes[i], &n);
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t nc = ring[k];
            nb_slots_.push_back(nc < r.slot.size() ? r.slot[nc] : kOutsideRegion);
            std::size_t mc = 0;
            const std::uint32_t nv = adjacency_.members(nc, &mc)[0];
            nb_positions_.push_back(mesh_.positions[nv]);
            if (colors) nb_colors_.push_back(mesh_.colors[nv]);
            if (want_normals) nb_normals_.push_back(class_normal(mesh_, adjacency_, nc));
        }
        nb_offsets_.push_back(static_cast<std::uint32_t>(nb_slots_.size()));
    }
}
void MeshSculptor::gather_stroke_origin(const VertexDeltas& record) {
    origin_.resize(region_.size());
    for (std::size_t i = 0; i < region_.size(); ++i) {
        std::size_t members = 0;
        const std::uint32_t v = adjacency_.members(region_.classes[i], &members)[0];
        const std::optional<kernel::cfloat3> seen = record.origin_of(v);
        // Not yet touched by this stroke: it starts here.
        origin_[i] = seen ? *seen : region_.positions[i];
    }
}

bool MeshSculptor::has_colors() const {
    return mesh_.colors.size() == mesh_.positions.size() && !mesh_.positions.empty();
}

bool MeshSculptor::ensure_colors(kernel::cfloat3 fill) {
    if (has_colors()) return false;
    mesh_.colors.assign(mesh_.positions.size(), fill);
    return true;
}

// The colour counterpart of `write`. No normals to recompute and no ring to
// mark: a colour is a per-vertex value that changes nothing about the surface,
// which is why this is twenty lines and `write` is fifty.
std::size_t MeshSculptor::write_colors(VertexDeltas* record) {
    region_.write_region.clear();
    region_.write_bounds = math::Aabb{};
    std::size_t painted = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const std::uint32_t c = region_.classes[i];
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        // Unchanged means unwritten, so a zero-weight rim costs nothing and a
        // record does not grow entries whose before and after are equal.
        if (is_zero(color_target_[i] - mesh_.colors[members[0]])) continue;
        ++painted;
        region_.write_region.push_back(c);
        region_.write_bounds.expand(region_.positions[i]);
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.colors[members[k]] = color_target_[i];
            if (record) record->sync_after(members[k], mesh_);
        }
    }
    return painted;
}

std::size_t MeshSculptor::write(VertexDeltas* record) {
    // Retire the marks the LAST write set, which `pending_normals_` names
    // exactly, rather than clearing an array the size of the mesh. Same rule as
    // `gather`'s slot reset above.
    if (normal_mark_.size() != adjacency_.class_count()) {
        normal_mark_.assign(adjacency_.class_count(), 0);
    } else {
        for (std::uint32_t c : pending_normals_)
            if (c < normal_mark_.size()) normal_mark_[c] = 0;
    }
    pending_normals_.clear();
    // THE WRITE REGION, not the workset. The rim of a falloff, a fully masked
    // vertex and a verb that declined all leave entries in `classes` that never
    // move, and a host told about those would upload geometry that did not
    // change. Smooth, Relax and Polish additionally READ a ring they do not
    // write, and that ring is not here either.
    region_.write_region.clear();
    region_.write_bounds = math::Aabb{};
    std::size_t moved = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        if (is_zero(displacement_[i])) continue;
        ++moved;
        const std::uint32_t c = region_.classes[i];
        mark_bvh_dirty(c);  // for the next refit_bvh, which drains the whole set
        const kernel::cfloat3 target = region_.positions[i] + displacement_[i];
        region_.write_region.push_back(c);
        region_.write_bounds.expand(region_.positions[i]);
        region_.write_bounds.expand(target);
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.positions[members[k]] = target;
            if (record) record->sync_after(members[k], mesh_);
        }
        // A triangle's normal changes when ANY of its corners moves, so the
        // moved class's whole ring needs recomputing, not just the class.
        if (!normal_mark_[c]) {
            normal_mark_[c] = 1;
            pending_normals_.push_back(c);
        }
        std::size_t rc = 0;
        const std::uint32_t* ring = adjacency_.ring(c, &rc);
        for (std::size_t k = 0; k < rc; ++k)
            if (!normal_mark_[ring[k]]) {
                normal_mark_[ring[k]] = 1;
                pending_normals_.push_back(ring[k]);
            }
    }
    if (moved == 0) return 0;
    if (defer_normals_) {
        deferred_normals_.insert(deferred_normals_.end(), pending_normals_.begin(),
                                 pending_normals_.end());
    } else {
        recompute_normals(pending_normals_, record);
    }
    return moved;
}

void MeshSculptor::flush_normals(VertexDeltas* record) {
    if (deferred_normals_.empty()) return;
    std::sort(deferred_normals_.begin(), deferred_normals_.end());
    deferred_normals_.erase(std::unique(deferred_normals_.begin(), deferred_normals_.end()),
                            deferred_normals_.end());
    recompute_normals(deferred_normals_, record);
    deferred_normals_.clear();
}

std::size_t MeshSculptor::apply_lattice(const Lattice& cage, VertexDeltas* record) {
    // An untouched cage displaces nothing anywhere, so the walk is skipped
    // rather than run to write every vertex back to itself — which would also
    // fill an undo record with entries that changed nothing.
    if (cage.is_identity()) return 0;
    // A whole-mesh operation has no small dirty set to name, so the next refit
    // refits everything. Cheaper than a rebuild and correct, where a refit of
    // the last stamp's region would be neither.
    bvh_all_dirty_ = true;

    std::size_t moved = 0;
    // By WELD CLASS rather than by raw vertex, so that position-coincident
    // vertices — the split ones holding a hard edge — stay coincident. Walking
    // `positions` directly would give each copy its own evaluation; they agree
    // mathematically, but only up to float rounding, and a seam that opens by
    // an ulp is a visible crack.
    const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
    std::vector<std::uint32_t> touched;
    touched.reserve(classes);
    for (std::uint32_t c = 0; c < classes; ++c) {
        std::size_t count = 0;
        const std::uint32_t* members = adjacency_.members(c, &count);
        if (count == 0) continue;
        const kernel::cfloat3 d = cage.displacement(mesh_.positions[members[0]]);
        if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) continue;
        for (std::size_t k = 0; k < count; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.positions[members[k]] = mesh_.positions[members[k]] + d;
            ++moved;
        }
        touched.push_back(c);
    }
    if (touched.empty()) return 0;

    // The cage moved vertices, so their normals are stale — and their
    // neighbours' are too, since a face normal is shared. Recomputing over the
    // touched classes is what every stamp does; deferring follows the same
    // switch, so a host draining a gesture pays once.
    if (defer_normals_) {
        deferred_normals_.insert(deferred_normals_.end(), touched.begin(), touched.end());
    } else {
        recompute_normals(touched, record);
    }
    if (record)
        for (std::uint32_t c : touched) {
            std::size_t count = 0;
            const std::uint32_t* members = adjacency_.members(c, &count);
            for (std::size_t k = 0; k < count; ++k) record->sync_after(members[k], mesh_);
        }
    return moved;
}

std::size_t MeshSculptor::apply_deformer(const MeshDeformSettings& settings,
                                        const field::MaskGate& gate, VertexDeltas* record) {
    // An identity deformer displaces nothing anywhere, so the walk is skipped
    // rather than run to write every vertex back to itself — which would also
    // fill an undo record with entries that changed nothing. Same rule as an
    // untouched lattice cage.
    if (settings.is_identity()) return 0;

    std::size_t moved = 0;
    // BY WELD CLASS, for apply_lattice's reason and with the same consequence:
    // position-coincident vertices holding a hard edge or a UV seam must stay
    // coincident, and evaluating each copy separately agrees only up to float
    // rounding — a seam that opens by an ulp is a visible crack.
    bvh_all_dirty_ = true;  // whole-mesh, as apply_lattice
    const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
    std::vector<std::uint32_t> touched;
    touched.reserve(classes);
    for (std::uint32_t c = 0; c < classes; ++c) {
        std::size_t count = 0;
        const std::uint32_t* members = adjacency_.members(c, &count);
        if (count == 0) continue;
        const kernel::cfloat3 rest = mesh_.positions[members[0]];
        kernel::cfloat3 target = deform_point(settings, rest);

        // The gate holds part of the form still. It scales the DISPLACEMENT
        // rather than the parameters, so a half-gated vertex travels half way
        // — the rule every other verb follows — and a fully gated one is
        // bit-identical to where it started rather than a lerp that lands one
        // ulp away.
        if (gate) {
            const float g = std::clamp(gate(rest), 0.0f, 1.0f);
            if (g >= 1.0f) continue;
            if (g > 0.0f) target = rest + (target - rest) * (1.0f - g);
        }
        if (is_zero(target - rest)) continue;

        for (std::size_t k = 0; k < count; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.positions[members[k]] = target;
            ++moved;
        }
        touched.push_back(c);
    }
    if (touched.empty()) return 0;

    if (defer_normals_) {
        deferred_normals_.insert(deferred_normals_.end(), touched.begin(), touched.end());
    } else {
        recompute_normals(touched, record);
    }
    if (record)
        for (std::uint32_t c : touched) {
            std::size_t count = 0;
            const std::uint32_t* members = adjacency_.members(c, &count);
            for (std::size_t k = 0; k < count; ++k) record->sync_after(members[k], mesh_);
        }
    return moved;
}

void MeshSculptor::recompute_normals(const std::vector<std::uint32_t>& classes,
                                     VertexDeltas* record) {
    if (mesh_.normals.size() != mesh_.positions.size() || mesh_.normals.empty()) return;
    for (std::uint32_t c : classes) {
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.normals[members[k]] = kernel::cf3(0, 0, 0);
        }
    }
    for (std::uint32_t c : classes) {
        std::size_t tc = 0;
        const std::uint32_t* tris = adjacency_.triangles_of(c, &tc);
        for (std::size_t k = 0; k < tc; ++k) {
            // Angle-weighted, for the reason class_normal gives: an
            // area-weighted normal on a lattice-derived mesh tracks the
            // lattice rather than the surface.
            const kernel::cfloat3 fn =
                safe_normalize(face_normal(mesh_, tris[k]), kernel::cf3(0, 0, 0));
            for (int corner = 0; corner < 3; ++corner) {
                const std::uint32_t v = mesh_.indices[tris[k] * 3 + corner];
                // Only this class's own corners: a split vertex keeps its own
                // faces, which is what preserves a hard edge's shading.
                if (adjacency_.class_of(v) == c)
                    mesh_.normals[v] = mesh_.normals[v] + fn * corner_angle(mesh_, tris[k], corner);
            }
        }
    }
    for (std::uint32_t c : classes) {
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        for (std::size_t k = 0; k < mc; ++k) {
            mesh_.normals[members[k]] =
                safe_normalize(mesh_.normals[members[k]], kernel::cf3(0, 1, 0));
            if (record) record->sync_after(members[k], mesh_);
        }
    }
}

// -- VertexDeltas encoding (survive-a-crash) ---------------------------------
//
// Layout, little-endian throughout:
//
//   u32 magic 'CVDL'   u16 version   u8 has_normals   u8 has_colors
//   u32 count
//   u32 vertices[count]
//   f32 before_position[count][3]   f32 after_position[count][3]
//   f32 before_normal[count][3]     f32 after_normal[count][3]     (if has_normals)
//   f32 before_color[count][3]      f32 after_color[count][3]      (if has_colors)
//
// Fixed-width and positional rather than tagged: this is a crash artifact
// paired with one snapshot, not a document, so it needs to be cheap to write on
// every step rather than forgiving to read years later. The version is there so
// a build that does not understand it REFUSES, which is the whole point — a
// recovery that silently drops what it could not read is the failure the
// feature exists to prevent.
namespace {

constexpr std::uint32_t kDeltaMagic = 0x4C445643u;  // 'CVDL'
constexpr std::uint16_t kDeltaVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

void put_vec3(std::vector<std::uint8_t>& out, const std::vector<kernel::cfloat3>& v) {
    for (const kernel::cfloat3& p : v) {
        put_f32(out, p.x);
        put_f32(out, p.y);
        put_f32(out, p.z);
    }
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;

    std::uint32_t u32() {
        if (at + 4 > size) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(data[at]) |
                                (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return v;
    }
    std::uint8_t u8() {
        if (at + 1 > size) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    float f32() {
        const std::uint32_t bits = u32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    void vec3(std::vector<kernel::cfloat3>& out, std::size_t count) {
        out.resize(count);
        for (std::size_t i = 0; i < count && ok; ++i) {
            const float x = f32();
            const float y = f32();
            const float z = f32();
            out[i] = kernel::cf3(x, y, z);
        }
    }
};

}  // namespace

std::size_t VertexDeltas::bytes() const {
    std::size_t n = sizeof(VertexDeltas);
    n += vertices_.capacity() * sizeof(std::uint32_t);
    const std::size_t vec3 = sizeof(kernel::cfloat3);
    n += (before_position_.capacity() + after_position_.capacity()) * vec3;
    n += (before_normal_.capacity() + after_normal_.capacity()) * vec3;
    n += (before_color_.capacity() + after_color_.capacity()) * vec3;
    // The slot index is rebuildable but it is really allocated, so a budget
    // that ignored it would under-report a record following many vertices.
    n += slot_.size() * (sizeof(std::uint32_t) * 2 + sizeof(void*));
    return n;
}

std::vector<std::uint8_t> VertexDeltas::encode() const {
    std::vector<std::uint8_t> out;
    const std::size_t n = vertices_.size();
    out.reserve(16 + n * (4 + 24 + (normals_ ? 24u : 0u) + (colors_ ? 24u : 0u)));
    put_u32(out, kDeltaMagic);
    out.push_back(static_cast<std::uint8_t>(kDeltaVersion));
    out.push_back(static_cast<std::uint8_t>(kDeltaVersion >> 8));
    out.push_back(normals_ ? 1 : 0);
    out.push_back(colors_ ? 1 : 0);
    put_u32(out, static_cast<std::uint32_t>(n));
    for (std::uint32_t v : vertices_) put_u32(out, v);
    put_vec3(out, before_position_);
    put_vec3(out, after_position_);
    if (normals_) {
        put_vec3(out, before_normal_);
        put_vec3(out, after_normal_);
    }
    if (colors_) {
        put_vec3(out, before_color_);
        put_vec3(out, after_color_);
    }
    return out;
}

bool VertexDeltas::decode(const std::uint8_t* data, std::size_t size, VertexDeltas* out) {
    if (!data || !out) return false;
    Reader r{data, size};
    if (r.u32() != kDeltaMagic) return false;
    const std::uint16_t version =
        static_cast<std::uint16_t>(r.u8() | (static_cast<std::uint16_t>(r.u8()) << 8));
    // Refused rather than reinterpreted. A newer layout read as this one would
    // revert a mesh to values that were never in it.
    if (!r.ok || version != kDeltaVersion) return false;
    const bool has_normals = r.u8() != 0;
    const bool has_colors = r.u8() != 0;
    const std::uint32_t count = r.u32();
    if (!r.ok) return false;
    // A count larger than the buffer could hold is a malformed or hostile
    // record, and sizing from it is how a reader gets asked to allocate a
    // gigabyte. The smallest a vertex can be is its id plus two positions.
    const std::size_t least_per_vertex = 4 + 24;
    if (static_cast<std::size_t>(count) > (size - r.at) / least_per_vertex + 1) return false;

    VertexDeltas built;
    built.normals_ = has_normals;
    built.colors_ = has_colors;
    built.vertices_.resize(count);
    for (std::uint32_t i = 0; i < count && r.ok; ++i) built.vertices_[i] = r.u32();
    r.vec3(built.before_position_, count);
    r.vec3(built.after_position_, count);
    if (has_normals) {
        r.vec3(built.before_normal_, count);
        r.vec3(built.after_normal_, count);
    }
    if (has_colors) {
        r.vec3(built.before_color_, count);
        r.vec3(built.after_color_, count);
    }
    if (!r.ok) return false;
    // The slot index is rebuilt rather than stored: it is derivable, and a
    // stored hash map would be a second source of truth for the same thing.
    built.slot_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) built.slot_.emplace(built.vertices_[i], i);
    *out = std::move(built);
    return true;
}

}  // namespace mesh
}  // namespace clay
