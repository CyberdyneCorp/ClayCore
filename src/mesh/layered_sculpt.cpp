#include "clay/mesh/layered_sculpt.h"

#include <algorithm>
#include <cmath>

namespace clay {
namespace mesh {
namespace {

// The mean edge length at a level, for the stamp resolution report. Sampled
// rather than summed over the whole level: a level can hold twenty million
// vertices and this answers a question about the ORDER of the spacing, which a
// few thousand edges settle as well as all of them.
constexpr std::uint32_t kSpacingSamples = 4096;

float level_spacing(MultiresSurface& surface, std::uint32_t level) {
    const std::vector<kernel::cfloat3>& positions = surface.positions_at(level);
    const Adjacency& adjacency = surface.level_adjacency(level);
    const std::uint32_t classes = static_cast<std::uint32_t>(adjacency.class_count());
    if (classes == 0 || positions.empty()) return 0.0f;
    const std::uint32_t step = classes > kSpacingSamples ? classes / kSpacingSamples : 1u;
    double total = 0.0;
    std::size_t edges = 0;
    for (std::uint32_t c = 0; c < classes; c += step) {
        std::size_t members = 0;
        const std::uint32_t v = adjacency.members(c, &members)[0];
        if (v >= positions.size()) continue;
        std::size_t ring_count = 0;
        const std::uint32_t* ring = adjacency.ring(c, &ring_count);
        for (std::size_t i = 0; i < ring_count; ++i) {
            std::size_t nm = 0;
            const std::uint32_t n = adjacency.members(ring[i], &nm)[0];
            if (n >= positions.size()) continue;
            total += kernel::clength(positions[v] - positions[n]);
            ++edges;
        }
    }
    return edges == 0 ? 0.0f : static_cast<float>(total / static_cast<double>(edges));
}

LocalDetail lerp_detail(const LocalDetail& a, const LocalDetail& b, float t) {
    LocalDetail out;
    out.tangent = a.tangent + (b.tangent - a.tangent) * t;
    out.bitangent = a.bitangent + (b.bitangent - a.bitangent) * t;
    out.normal = a.normal + (b.normal - a.normal) * t;
    return out;
}

}  // namespace

LayeredMultiresSculptor::LayeredMultiresSculptor(MultiresSurface& surface)
    : surface_(surface), sculptor_(surface) {}

LayeredMultiresSculptor::~LayeredMultiresSculptor() {
    // A transaction destroyed while open discards, rather than leaving the
    // stack holding a composition lock nobody can release. Discarding rather
    // than committing is the same choice every RAII transaction in this library
    // makes: a gesture nobody finished is a gesture nobody asked for.
    if (open_) cancel();
}

bool LayeredMultiresSculptor::begin() {
    if (open_ || !surface_.valid()) return false;

    const SculptLayerStack& stack = surface_.sculpt_layers();
    restore_active_ = stack.active();
    switch (domain_) {
        case MultiresWriteDomain::Geometry:
            target_ = kNoSculptLayer;
            break;
        case MultiresWriteDomain::Detail:
            // Refused rather than promoted to the base: a caller that said
            // "detail" and got a form edit has had the one thing it asked
            // against done to it silently.
            if (restore_active_ == kNoSculptLayer) return false;
            target_ = restore_active_;
            break;
        case MultiresWriteDomain::Automatic:
            target_ = restore_active_;
            break;
    }
    const SculptLayer* layer = stack.find(target_);
    if (layer && layer->locked) return false;

    // The target is fixed for the gesture by pinning it on the stack, so a host
    // that changes the active layer mid-stroke cannot split one gesture across
    // two channels. Metadata only, so pinning it costs no geometry.
    surface_.set_active_sculpt_layer(target_);
    layer_delta_.clear();
    layer_delta_.set_layer(target_);
    base_delta_.clear();
    stamps_ = 0;
    report_ = DetailStampReport{};
    open_ = true;
    // Held from here: a slider that moved between two stamps would author one
    // gesture against two different surfaces.
    surface_.hold_sculpt_layer_composition(true);
    sculptor_.begin_stroke();
    return true;
}

bool LayeredMultiresSculptor::commit(SculptLayerDelta* out_layer, MultiresDelta* out_base) {
    if (!open_) return false;
    open_ = false;
    surface_.hold_sculpt_layer_composition(false);
    surface_.set_active_sculpt_layer(restore_active_);
    if (out_layer && target_ != kNoSculptLayer) {
        layer_delta_.sync_after(surface_.sculpt_layers());
        *out_layer = layer_delta_;
    }
    if (out_base && target_ == kNoSculptLayer) {
        base_delta_.sync_after(surface_);
        *out_base = base_delta_;
    }
    return true;
}

void LayeredMultiresSculptor::cancel() {
    if (!open_) return;
    open_ = false;
    surface_.hold_sculpt_layer_composition(false);
    // EXACTLY, from the recorded `before` values rather than from a
    // recomputation: a layered write is `L += ΔE`, and subtracting the deltas
    // back off would leave the last bits of every touched coefficient somewhere
    // else. The record has held the pre-stroke values since the first stamp for
    // this one reason.
    if (target_ != kNoSculptLayer) {
        surface_.apply_sculpt_layer_delta(layer_delta_, false);
    } else {
        base_delta_.revert(surface_);
    }
    surface_.set_active_sculpt_layer(restore_active_);
    layer_delta_.clear();
    base_delta_.clear();
    stamps_ = 0;
}

// -- the ordinary verbs -------------------------------------------------------

std::size_t LayeredMultiresSculptor::stamp(MeshBrush verb, const MeshBrushSettings& settings,
                                           const field::MaskGate& gate) {
    if (!open_) return 0;
    // RE-PINNED PER DAB, because pinning once at `begin` is not a pin. The
    // underlying sculptor routes a stamp by reading the stack's ACTIVE layer,
    // and set-active is deliberately allowed while the composition is held (it
    // moves no vertex, so refusing it would make a hold mean "hide from the
    // UI"). A host that changes channel between two dabs would therefore split
    // one gesture across two of them, and neither half would undo as the artist
    // made it — which is exactly the failure this transaction exists to
    // prevent. Metadata, so re-asserting it costs a comparison and no geometry.
    if (surface_.sculpt_layers().active() != target_) surface_.set_active_sculpt_layer(target_);
    ++stamps_;
    return sculptor_.stamp(verb, settings, gate, &base_delta_, &layer_delta_);
}

// -- the region ---------------------------------------------------------------

bool LayeredMultiresSculptor::gather(const MeshBrushSettings& settings,
                                     const field::MaskGate& gate) {
    region_.clear();
    MeshSculptor* level = sculptor_.level_sculptor();
    if (!level) return false;

    // THE SCULPTOR'S OWN GATHER, driven by a stamp that deposits nothing.
    //
    // A second region builder is the obvious implementation and is the wrong
    // one: the region under a brush is the falloff curve, the geodesic walk,
    // the path taper at its rim, the mask gate, the alpha and the composed
    // automask factors, in one fixed multiplication order that `sculpt_kernels`
    // documents as part of the contract. A copy of that would be a second
    // answer to the same question, drifting one edit at a time — which is
    // exactly the failure `multires_sculpt.h` opens by refusing for the verbs.
    //
    // So a zero-strength Draw is taken: `Draw` needs no neighbourhood, so the
    // gather is all it costs, and a zero displacement moves nothing and records
    // nothing. What survives is `workset()`, which is the region and its
    // weights.
    MeshBrushSettings probe = settings;
    probe.strength = 0.0f;
    level->stamp(MeshBrush::Draw, probe, gate, nullptr);

    const SculptWorkset& workset = level->workset();
    if (workset.empty()) return false;
    const Adjacency& adjacency = level->adjacency();
    for (std::size_t i = 0; i < workset.size(); ++i) {
        if (workset.weights[i] <= 0.0f) continue;
        std::size_t members = 0;
        const std::uint32_t* member = adjacency.members(workset.classes[i], &members);
        // Through the class's members rather than by assuming class == vertex.
        // They usually agree, and two level vertices that coincide bit for bit
        // weld into one class — from which every id would be off by one, and
        // the hierarchy stores detail per VERTEX.
        for (std::size_t m = 0; m < members; ++m) region_.push_back({member[m], workset.weights[i]});
    }
    return !region_.empty();
}

LocalDetail LayeredMultiresSculptor::read_target(std::uint32_t level,
                                                 std::uint32_t vertex) const {
    if (target_ != kNoSculptLayer) return surface_.sculpt_layer_detail(target_, level, vertex);
    return surface_.detail_at(level).get(vertex);
}

bool LayeredMultiresSculptor::write_target(std::uint32_t level, std::uint32_t vertex,
                                           const LocalDetail& value) {
    if (target_ != kNoSculptLayer) {
        layer_delta_.note_detail(level, vertex, surface_.sculpt_layer_detail(target_, level, vertex));
        return surface_.set_sculpt_layer_detail(target_, level, vertex, value);
    }
    base_delta_.note_detail(level, vertex, surface_.detail_at(level).get(vertex));
    surface_.set_detail(level, vertex, value);
    return true;
}

// -- the stamps ---------------------------------------------------------------

std::size_t LayeredMultiresSculptor::stamp_detail(const DetailStampSettings& stamp_settings,
                                                  const MeshBrushSettings& settings,
                                                  const field::MaskGate& gate) {
    if (!open_ || !stamp_settings.valid()) return 0;
    // A scalar weight is `MeshBrushSettings::alpha` and is refused here rather
    // than served twice: two entry points for one idea is how two bilinear
    // lookups end up disagreeing.
    if (stamp_settings.mode == DetailStampMode::Weight) return 0;
    const std::uint32_t level = surface_.sculpt_level();
    if (level == 0) return 0;  // level 0 has no transported frame to read into
    if (!gather(settings, gate)) return 0;
    ++stamps_;

    report_ = detail_stamp_report(stamp_settings, level_spacing(surface_, level));

    const std::vector<kernel::cfloat3>& positions = surface_.positions_at(level);
    const std::vector<SurfaceFrame>& frames = surface_.frames_at(level);
    // The fallback direction is the level's normal at the brush centre's
    // nearest vertex, which is what the scalar alpha's fallback already is.
    kernel::cfloat3 fallback = kernel::cf3(0, 1, 0);
    if (!region_.empty() && region_[0].vertex < frames.size())
        fallback = frames[region_[0].vertex].normal;
    const AlphaFrame frame = detail_stamp_frame(stamp_settings, fallback);

    std::size_t moved = 0;
    for (const Weighted& entry : region_) {
        if (entry.vertex >= positions.size() || entry.vertex >= frames.size()) continue;
        const DetailStampSample sample = detail_stamp_sample(stamp_settings, frame,
                                                             frames[entry.vertex],
                                                             positions[entry.vertex]);
        if (!sample.inside) continue;
        const float w = entry.weight * settings.strength;
        LocalDetail value = read_target(level, entry.vertex);
        value.tangent += w * sample.offset.tangent;
        value.bitangent += w * sample.offset.bitangent;
        value.normal += w * sample.offset.normal;
        if (write_target(level, entry.vertex, value)) ++moved;
    }
    return moved;
}

// -- smoothing ----------------------------------------------------------------

std::size_t LayeredMultiresSculptor::smooth_detail(const MeshBrushSettings& settings,
                                                   const field::MaskGate& gate) {
    const std::uint32_t level = surface_.sculpt_level();
    if (!gather(settings, gate)) return 0;
    const Adjacency& adjacency = surface_.level_adjacency(level);

    // A SIMULTANEOUS average, not a sweep: every entry reads the pre-stamp
    // coefficients, so the result does not depend on the order the region
    // happens to sit in. The same rule `SculptWorkset` states for positions.
    scratch_.resize(region_.size());
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const std::uint32_t v = region_[i].vertex;
        const std::uint32_t cls = adjacency.class_of(v);
        std::size_t ring_count = 0;
        const std::uint32_t* ring = adjacency.ring(cls, &ring_count);
        LocalDetail sum;
        std::size_t count = 0;
        for (std::size_t k = 0; k < ring_count; ++k) {
            std::size_t members = 0;
            const std::uint32_t n = adjacency.members(ring[k], &members)[0];
            const LocalDetail d = read_target(level, n);
            sum.tangent += d.tangent;
            sum.bitangent += d.bitangent;
            sum.normal += d.normal;
            ++count;
        }
        if (count == 0) {
            scratch_[i] = read_target(level, v);
            continue;
        }
        const float inv = 1.0f / static_cast<float>(count);
        sum.tangent *= inv;
        sum.bitangent *= inv;
        sum.normal *= inv;
        scratch_[i] = sum;
    }

    std::size_t moved = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const float t = std::clamp(region_[i].weight * settings.strength, 0.0f, 1.0f);
        if (t == 0.0f) continue;
        const LocalDetail before = read_target(level, region_[i].vertex);
        const LocalDetail after = lerp_detail(before, scratch_[i], t);
        if (after == before) continue;
        if (write_target(level, region_[i].vertex, after)) ++moved;
    }
    return moved;
}

std::size_t LayeredMultiresSculptor::smooth_form(const MeshBrushSettings& settings,
                                                 const field::MaskGate& gate) {
    const std::uint32_t level = surface_.sculpt_level();
    if (!gather(settings, gate)) return 0;

    // THE FORM IS `S(n)` — the pure subdivision — and the detail is everything
    // stored against it. Smoothing S and leaving the coefficients alone is
    // therefore exactly "smooth the form, keep the pores"; the level's own
    // detail is where the change is stored, because that is where this
    // representation records any change to the form, and every LAYER's
    // contribution rides through untouched.
    //
    // At level 0 the form is the cage itself, so the same statement is a
    // Laplacian over the cage's positions.
    const Adjacency& adjacency = surface_.level_adjacency(level);
    const std::vector<kernel::cfloat3>& form =
        level == 0 ? surface_.positions_at(level) : surface_.subdivided_at(level);

    // EVERY SHIFT IS COMPUTED BEFORE ANY IS WRITTEN. At level 0 the array being
    // read IS the array a write moves, so a fused loop would be a Gauss-Seidel
    // sweep whose result depends on the order the region sits in — the same
    // rule `SculptWorkset` states for positions, and the same reason.
    std::vector<kernel::cfloat3> shift(region_.size(), kernel::cf3(0, 0, 0));
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const std::uint32_t v = region_[i].vertex;
        if (v >= form.size()) continue;
        const std::uint32_t cls = adjacency.class_of(v);
        std::size_t ring_count = 0;
        const std::uint32_t* ring = adjacency.ring(cls, &ring_count);
        kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
        std::size_t count = 0;
        for (std::size_t k = 0; k < ring_count; ++k) {
            std::size_t members = 0;
            const std::uint32_t n = adjacency.members(ring[k], &members)[0];
            if (n >= form.size()) continue;
            sum = sum + form[n];
            ++count;
        }
        if (count == 0) continue;
        const float t = std::clamp(region_[i].weight * settings.strength, 0.0f, 1.0f);
        shift[i] = (sum / static_cast<float>(count) - form[v]) * t;
    }

    std::size_t moved = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const std::uint32_t v = region_[i].vertex;
        if (v >= form.size()) continue;
        if (shift[i].x == 0.0f && shift[i].y == 0.0f && shift[i].z == 0.0f) continue;
        if (level == 0) {
            const kernel::cfloat3 was = surface_.base_position(v);
            base_delta_.note_base(v, was);
            surface_.set_base_position(v, was + shift[i]);
            ++moved;
            continue;
        }
        // The shift is stored in the level's own coefficients, read in the
        // vertex's transported frame — the one place a change to the form at a
        // level above the cage can live.
        const std::vector<SurfaceFrame>& frames = surface_.frames_at(level);
        if (v >= frames.size()) continue;
        LocalDetail delta;
        world_to_frame(frames[v], shift[i], &delta.tangent, &delta.bitangent, &delta.normal);
        LocalDetail value = surface_.detail_at(level).get(v);
        base_delta_.note_detail(level, v, value);
        value.tangent += delta.tangent;
        value.bitangent += delta.bitangent;
        value.normal += delta.normal;
        surface_.set_detail(level, v, value);
        ++moved;
    }
    return moved;
}

std::size_t LayeredMultiresSculptor::smooth(MultiresSmoothMode mode,
                                            const MeshBrushSettings& settings,
                                            const field::MaskGate& gate) {
    if (!open_) return 0;
    switch (mode) {
        case MultiresSmoothMode::Geometry:
            return stamp(MeshBrush::Smooth, settings, gate);
        case MultiresSmoothMode::DetailOnly:
            ++stamps_;
            return smooth_detail(settings, gate);
        case MultiresSmoothMode::PreserveDetail:
            ++stamps_;
            return smooth_form(settings, gate);
    }
    return 0;
}

// -- erase and restore --------------------------------------------------------

std::size_t LayeredMultiresSculptor::fade_toward_zero(const MeshBrushSettings& settings,
                                                      const field::MaskGate& gate, bool base) {
    const std::uint32_t level = surface_.sculpt_level();
    if (!gather(settings, gate)) return 0;
    ++stamps_;
    std::size_t moved = 0;
    for (const Weighted& entry : region_) {
        const float t = std::clamp(entry.weight * settings.strength, 0.0f, 1.0f);
        if (t == 0.0f) continue;
        const LocalDetail before =
            base ? surface_.detail_at(level).get(entry.vertex) : read_target(level, entry.vertex);
        if (before.zero()) continue;
        LocalDetail after;
        after.tangent = before.tangent * (1.0f - t);
        after.bitangent = before.bitangent * (1.0f - t);
        after.normal = before.normal * (1.0f - t);
        if (base) {
            base_delta_.note_detail(level, entry.vertex, before);
            surface_.set_detail(level, entry.vertex, after);
            ++moved;
            continue;
        }
        if (write_target(level, entry.vertex, after)) ++moved;
    }
    return moved;
}

std::size_t LayeredMultiresSculptor::erase(const MeshBrushSettings& settings,
                                           const field::MaskGate& gate) {
    if (!open_) return 0;
    // With no target layer there is no pass to erase, and reaching for the base
    // instead would make the eraser a flattening brush wearing the wrong name —
    // `restore` is the verb that means that, and it says so.
    if (target_ == kNoSculptLayer) return 0;
    return fade_toward_zero(settings, gate, false);
}

std::size_t LayeredMultiresSculptor::restore(const MeshBrushSettings& settings,
                                             const field::MaskGate& gate) {
    if (!open_) return 0;
    // Level 0 has no pure subdivision to return to; see the header.
    if (surface_.sculpt_level() == 0) return 0;
    return fade_toward_zero(settings, gate, true);
}

}  // namespace mesh
}  // namespace clay
