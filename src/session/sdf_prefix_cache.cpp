// An ephemeral field cache for a stable edit-list prefix (sdf-prefix-cache
// spec). See include/clay/session/sdf_prefix_cache.h for the far-bound rule,
// which is the whole correctness argument, and for the measurements behind it.

#include "clay/session/sdf_prefix_cache.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "clay/eval/backend.h"

#include "layer_digest.h"

namespace clay {
namespace session {

namespace {

// The layer as `bake_layer` samples it: visible whatever the artist hid, and
// its own transform identity so the frame is the layer's own. Every other layer
// is hidden, because `compile_layer_prefix` and `compile_layer_suffix` work on
// the document's LAST VISIBLE SDF layer and this is how a caller says which one
// it means.
//
// Shallow: `Layer::sdf` is a shared_ptr, so this shares every node and copies
// only the layer records. Node ids are therefore unchanged, which is what lets
// the suffix name the roots the caller knows about.
scene::Document local_view_document(const scene::Document& doc, scene::LayerId layer) {
    scene::Document view = doc;
    for (scene::Layer& l : view.layers) {
        if (l.id != layer) {
            l.visible = false;
            continue;
        }
        l.visible = true;
        l.xform = math::Transform{};
    }
    return view;
}

// The checkpoint `compile_layer_suffix` reads: the layer, and whether an
// accumulator is on the stack. STATED rather than derived, exactly as
// clay_c.cpp's plan_resume states it — the byte lengths on a TapeCheckpoint are
// for `compile_document_append`, which copies a prefix, and this does not.
//
// `doc_have_acc` is false because the view above leaves exactly one visible SDF
// layer, so nothing is underneath for the suffix to union onto.
scene::TapeCheckpoint checkpoint_for(scene::LayerId layer) {
    scene::TapeCheckpoint cp;
    cp.valid = true;
    cp.layer = layer;
    cp.layer_have_acc = true;
    cp.doc_have_acc = false;
    return cp;
}

}  // namespace

std::uint64_t layer_prefix_fingerprint(const scene::Layer& layer, std::size_t root_count) {
    std::uint64_t h = digest::kFnvOffset;
    digest::mix_layer_head(h, layer);
    digest::mix_roots(h, layer, root_count);
    return h;
}

std::size_t prefix_boundary_for(const scene::Layer& layer, const SdfPrefixPolicy& policy) {
    if (policy.max_bytes == 0) return 0;          // the cache is off
    if (!(policy.cell_size > 0.0f)) return 0;     // no resolution was chosen
    if (!layer.sdf) return 0;
    const std::size_t roots = layer.sdf->roots.size();
    if (roots < policy.min_history_roots) return 0;  // the walk is already cheap
    if (roots <= policy.keep_live_suffix_roots) return 0;
    const std::size_t boundary = roots - policy.keep_live_suffix_roots;
    // An empty prefix is not a prefix, and a boundary at the very end leaves no
    // suffix for the seed to be folded onto — `compile_layer_suffix` refuses
    // that, so decline here rather than build a volume nothing can use.
    if (boundary == 0 || boundary >= roots) return 0;
    return boundary;
}

std::uint64_t SdfPrefixCache::key_of(scene::LayerId layer, std::size_t roots,
                                     const SdfPrefixPolicy& policy) {
    // Resolution is part of the key, not just the fingerprint: the same prefix
    // sampled at two cell sizes is two different caches and neither is wrong.
    std::uint64_t h = digest::kFnvOffset;
    digest::mix(h, layer);
    digest::mix(h, roots);
    digest::mix(h, policy.cell_size);
    digest::mix(h, policy.band);
    digest::mix(h, policy.padding);
    return h;
}

void SdfPrefixCache::note_seeded(bool fell_back) {
    if (fell_back)
        ++stats_.fallback_windows;
    else
        ++stats_.seeded_windows;
}

const SdfPrefixField* SdfPrefixCache::find(const scene::Layer& layer, std::size_t prefix_roots,
                                           const SdfPrefixPolicy& policy) {
    if (prefix_roots == 0) return nullptr;
    const std::uint64_t key = key_of(layer.id, prefix_roots, policy);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    // THE DIGEST IS THE SAFETY NET. Invalidation by command is an optimisation
    // and can be forgotten; this cannot, because it is computed from what the
    // layer holds right now. A shared SdfContent edited through a sibling
    // instance moves it too, since the digest reads the content and not the
    // pointer.
    if (layer_prefix_fingerprint(layer, prefix_roots) != it->second.field.fingerprint) {
        bytes_ -= it->second.field.bytes();
        order_.erase(it->second.lru);
        entries_.erase(it);
        ++stats_.invalidations;
        ++stats_.misses;
        stats_.entries = entries_.size();
        stats_.bytes = bytes_;
        return nullptr;
    }
    order_.splice(order_.begin(), order_, it->second.lru);
    ++stats_.hits;
    return &it->second.field;
}

const SdfPrefixField* SdfPrefixCache::build(const scene::Document& doc, scene::LayerId layer_id,
                                            const SdfPrefixPolicy& policy,
                                            const scene::BakePointEval& point_eval,
                                            parallel::CancelToken* token) {
    const scene::Layer* layer = doc.find_layer(layer_id);
    if (!layer || layer->kind != scene::LayerKind::Sdf || !layer->sdf) return nullptr;
    const std::size_t boundary = prefix_boundary_for(*layer, policy);
    if (boundary == 0) return nullptr;

    if (const SdfPrefixField* had = find(*layer, boundary, policy)) return had;

    const scene::Document view = local_view_document(doc, layer_id);
    scene::Tape prefix_tape;
    if (!scene::compile_layer_prefix(view, boundary, &prefix_tape)) return nullptr;

    // THE LATTICE IS THE WHOLE LAYER'S, NOT THE PREFIX'S. `sample_blocks` takes
    // its origin straight from `region.min`, so two volumes share a lattice
    // exactly when they share a region and a cell size — and a seed read off a
    // lattice it shares with its consumer IS the stored sample, where one read
    // off a lattice half a cell away is an interpolation of two.
    //
    // Measured: with the prefix baked over its own padded bounds, a bake
    // through the cached source differed from the full walk by 0.0074 on the
    // consumer's lattice; over the layer's region it is float rounding.
    //
    // It is also the right region on its own terms. The prefix must answer
    // wherever the SUFFIX might need it, and a suffix grows the surface into
    // places the prefix never reached — the prefix's own bounds are exactly the
    // region that does not cover that.
    const scene::Layer* view_layer = view.find_layer(layer_id);
    if (!view_layer) return nullptr;
    const scene::Tape whole = scene::compile_layer(*view_layer);
    if (whole.empty() || whole.bounds.empty() || whole.bounds.is_infinite()) return nullptr;
    const float band = policy.band > 0.0f ? policy.band : policy.cell_size * 3.0f;
    const float padding = policy.padding > 0.0f ? policy.padding : band;
    const kernel::cfloat3 pad = kernel::cf3(padding, padding, padding);
    const math::Aabb region{whole.bounds.min - pad, whole.bounds.max + pad};

    scene::ConsolidationParams params;
    params.cell_size = policy.cell_size;
    params.band = policy.band;
    params.padding = policy.padding;
    params.region = region;
    // NO REDISTANCE, AND THEREFORE NO COMPACT. This is the one place a bake's
    // defaults are wrong: redistancing replaces every sample with the distance
    // to the surface those samples imply, which is exactly right for a volume
    // that is about to BE the layer and exactly wrong for one that has to
    // reproduce an ACCUMULATOR. The suffix folds onto the number the prefix
    // produced; hand it a redistanced approximation of that number and the
    // composition stops being the walk.
    //
    // Measured: with redistance on, a bake through the cached source differed
    // from the full walk by 0.063 ON ITS OWN LATTICE -- two cells -- where the
    // raw samples differ by float rounding. Compact rides along because it only
    // runs after a successful redistance, and dropping bricks would also shrink
    // the region the far-bound rule calls covered.
    params.skip_redistance = true;
    // The colour question is asked of the NODES the prefix covers, not of the
    // tape and not of the whole layer: a prefix that is all one colour must not
    // grow a channel because a suffix item is red.
    bool prefix_colours_vary = false;
    {
        scene::Layer prefix_only = *layer;
        prefix_only.sdf = std::make_shared<scene::SdfContent>(*layer->sdf);
        prefix_only.sdf->roots.resize(boundary);
        prefix_colours_vary = scene::layer_colors_vary(prefix_only);
    }
    std::optional<field::FieldVolume> volume =
        scene::bake_tape(prefix_tape, params, prefix_colours_vary, nullptr, point_eval, token);
    if (!volume) return nullptr;
    if (parallel::cancelled(token)) return nullptr;  // a cancelled build caches nothing

    SdfPrefixField field;
    field.layer = layer_id;
    field.prefix_roots = boundary;
    field.fingerprint = layer_prefix_fingerprint(*layer, boundary);
    field.cell_size = policy.cell_size;
    field.band = policy.band;
    field.padding = policy.padding;
    field.volume = std::move(*volume);

    const std::uint64_t key = key_of(layer_id, boundary, policy);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        bytes_ -= it->second.field.bytes();
        order_.erase(it->second.lru);
        entries_.erase(it);
    }
    Entry entry;
    entry.field = std::move(field);
    bytes_ += entry.field.bytes();
    order_.push_front(key);
    entry.lru = order_.begin();
    const SdfPrefixField* stored = &entries_.emplace(key, std::move(entry)).first->second.field;
    ++stats_.builds;
    max_bytes_ = max_bytes_ ? max_bytes_ : policy.max_bytes;
    evict_to_budget();
    stats_.entries = entries_.size();
    stats_.bytes = bytes_;
    // Evicting to budget may have dropped the entry just built, which is a
    // budget too small to hold one prefix rather than an error. Ask again
    // rather than hand back a pointer into a freed node.
    auto again = entries_.find(key);
    return again == entries_.end() ? nullptr : stored;
}

void SdfPrefixCache::evict_to_budget() {
    if (max_bytes_ == 0) return;
    while (bytes_ > max_bytes_ && !order_.empty()) {
        const std::uint64_t victim = order_.back();
        auto it = entries_.find(victim);
        if (it == entries_.end()) {
            order_.pop_back();
            continue;
        }
        bytes_ -= it->second.field.bytes();
        entries_.erase(it);
        order_.pop_back();
        ++stats_.evictions;
    }
}

void SdfPrefixCache::set_max_bytes(std::size_t bytes) {
    max_bytes_ = bytes;
    evict_to_budget();
    stats_.entries = entries_.size();
    stats_.bytes = bytes_;
}

void SdfPrefixCache::invalidate_layer(scene::LayerId layer) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.field.layer != layer) {
            ++it;
            continue;
        }
        bytes_ -= it->second.field.bytes();
        order_.erase(it->second.lru);
        it = entries_.erase(it);
        ++stats_.invalidations;
    }
    stats_.entries = entries_.size();
    stats_.bytes = bytes_;
}

void SdfPrefixCache::clear() {
    entries_.clear();
    order_.clear();
    bytes_ = 0;
    stats_.entries = 0;
    stats_.bytes = 0;
}

// -- the source field ---------------------------------------------------------

std::optional<SdfSourceField> SdfSourceField::open(const scene::Document& doc,
                                                   scene::LayerId layer_id,
                                                   SdfPrefixCache* cache,
                                                   const SdfPrefixPolicy& policy,
                                                   const scene::BakePointEval& point_eval,
                                                   parallel::CancelToken* token) {
    const scene::Layer* layer = doc.find_layer(layer_id);
    if (!layer || layer->kind != scene::LayerKind::Sdf || !layer->sdf) return std::nullopt;

    SdfSourceField out;
    out.view_ = local_view_document(doc, layer_id);
    out.layer_ = layer_id;
    out.cache_ = cache;
    const scene::Layer* view_layer = out.view_.find_layer(layer_id);
    out.full_ = scene::compile_layer(*view_layer);
    out.bounds_ = out.full_.bounds;

    // The accelerated half is entirely optional: everything below can fail and
    // the object still answers every point correctly through `full_`.
    if (!cache) return out;
    const std::size_t boundary = prefix_boundary_for(*view_layer, policy);
    if (boundary == 0) return out;
    // FIND, never build. Opening a source must not pay for a bake: this is the
    // call a Smooth transaction makes at pointer-down, and building a prefix
    // there would put back the whole-layer cost the lazy path exists to
    // remove. A host schedules `build` when it has somewhere to put the work;
    // until then every window is the full walk, which is correct and slower.
    (void)point_eval;
    (void)token;
    const SdfPrefixField* prefix = cache->find(*view_layer, boundary, policy);
    if (!prefix) return out;

    const std::vector<scene::NodeId>& roots = view_layer->sdf->roots;
    std::vector<scene::NodeId> appended(roots.begin() + static_cast<std::ptrdiff_t>(boundary),
                                        roots.end());
    scene::Tape suffix;
    if (!scene::compile_layer_suffix(checkpoint_for(layer_id), out.view_, appended, &suffix,
                                     nullptr))
        return out;  // refused: the full walk is still correct
    // The exact seed, for the windows the volume cannot cover. Compiled once
    // here rather than per window, because a window that falls back is already
    // paying the walk and should not pay a compile as well.
    if (!scene::compile_layer_prefix(out.view_, boundary, &out.prefix_tape_)) return out;

    out.prefix_ = prefix;
    out.suffix_ = std::move(suffix);
    out.suffix_roots_ = appended.size();
    out.composed_ = true;
    return out;
}

void SdfSourceField::fill_points(const float* points_xyz, std::size_t count, float* out) const {
    if (count == 0) return;
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    const eval::PointQuery q{points_xyz, count, 1e-4f};

    if (!composed_) {
        // No cache, or it declined. The whole layer, which is the oracle every
        // accelerated answer is checked against.
        if (cpu) {
            cpu->eval_points(full_, q, eval::PointResults{out, nullptr, nullptr});
            return;
        }
        for (std::size_t i = 0; i < count; ++i)
            out[i] = full_.eval(kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1],
                                            points_xyz[i * 3 + 2]))
                         .d;
        return;
    }

    // THE FAR-BOUND RULE. The cached volume may seed this window only if it
    // stores every sample of it; outside its stored bricks `eval` answers with
    // a conservative bound rather than the distance the history had, and a
    // suffix folded onto that is wrong by cells rather than by rounding.
    //
    // Decided for the WINDOW, not per point, so the fast path stays a straight
    // loop and the slow one is a whole extra tape evaluation rather than a
    // branch inside the inner loop.
    bool covered = true;
    for (std::size_t i = 0; i < count && covered; ++i)
        covered = prefix_->volume.has_samples_at(
            kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]));

    std::vector<float> seed(count);
    if (covered) {
        for (std::size_t i = 0; i < count; ++i)
            seed[i] = prefix_->volume.eval(kernel::cf3(
                points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]));
    } else if (cpu) {
        cpu->eval_points(prefix_tape_, q, eval::PointResults{seed.data(), nullptr, nullptr});
    } else {
        for (std::size_t i = 0; i < count; ++i)
            seed[i] = prefix_tape_
                          .eval(kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1],
                                            points_xyz[i * 3 + 2]))
                          .d;
    }
    if (cache_) cache_->note_seeded(!covered);
    eval::eval_points_seeded(suffix_, q, seed.data(), nullptr,
                             eval::PointResults{out, nullptr, nullptr});
}

field::FieldVolume::BrickBlockFill SdfSourceField::block_fill() const {
    return [this](const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
                  float* out) {
        const std::size_t n = count * field::kBrickSamples;
        std::vector<float> points(n * 3);
        for (std::size_t s = 0; s < count; ++s)
            for (int i = 0; i < field::kBrickSamples; ++i) {
                const kernel::cfloat3 p = grid.sample_position(first + s, i);
                const std::size_t at = (s * field::kBrickSamples + static_cast<std::size_t>(i)) * 3;
                points[at] = p.x;
                points[at + 1] = p.y;
                points[at + 2] = p.z;
            }
        // Per BRICK rather than per window: one brick that the prefix does not
        // cover would otherwise drag a whole 512-brick window onto the slow
        // path, and the coverage question is a brick's to answer.
        for (std::size_t s = 0; s < count; ++s)
            fill_points(points.data() + s * field::kBrickSamples * 3, field::kBrickSamples,
                        out + s * field::kBrickSamples);
    };
}

}  // namespace session
}  // namespace clay
