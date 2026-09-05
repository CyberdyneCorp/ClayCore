// An ephemeral field cache for a stable edit-list prefix (sdf-prefix-cache
// spec). See include/clay/session/sdf_prefix_cache.h for the far-bound rule,
// which is the whole correctness argument, and for the measurements behind it.

#include "clay/session/sdf_prefix_cache.h"

#include <cmath>

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
        // The placement is neutralised whole, per-axis scale included: a view
        // that reset the transform and kept the squash would compile a prefix
        // in a frame no document has.
        l.scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);
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
    // Two consumers, two lattices, two entries. Without this a brick refill and
    // a Smooth transaction would be served each other's prefix and one of them
    // would silently read between samples.
    digest::mix(h, policy.align_to_lattice ? 1u : 0u);
    return h;
}

void SdfPrefixCache::note_seeded(bool fell_back) {
    if (fell_back)
        ++stats_.fallback_windows;
    else
        ++stats_.seeded_windows;
}

// THE DIGEST IS THE SAFETY NET. Invalidation by command is an optimisation and
// can be forgotten; this cannot, because it is computed from what the layer
// holds right now. A shared SdfContent edited through a sibling instance moves
// it too, since the digest reads the content and not the pointer.
//
// It is also O(prefix roots) -- 13.5 ms at 50,000 -- and that is the right cost
// for a Smooth transaction, which asks once per gesture, and the wrong one for a
// brick refill, which asks per frame. `set_structure_witness` lets a caller that
// holds a monotonic witness of structural change say when it need not be
// recomputed. Without one, nothing changes.
bool SdfPrefixCache::verify(std::uint64_t key, const scene::Layer& layer,
                            std::size_t prefix_roots) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    Entry& e = it->second;
    const bool trusted = witness_ != 0 && e.verified_witness == witness_;
    if (!trusted) {
        if (layer_prefix_fingerprint(layer, prefix_roots) != e.field.fingerprint) {
            bytes_ -= e.field.bytes();
            order_.erase(e.lru);
            entries_.erase(it);
            ++stats_.invalidations;
            stats_.entries = entries_.size();
            stats_.bytes = bytes_;
            return false;
        }
        e.verified_witness = witness_;
    }
    order_.splice(order_.begin(), order_, e.lru);
    return true;
}

const SdfPrefixField* SdfPrefixCache::find(const scene::Layer& layer, std::size_t prefix_roots,
                                           const SdfPrefixPolicy& policy) {
    if (prefix_roots == 0) return nullptr;
    const std::uint64_t key = key_of(layer.id, prefix_roots, policy);
    if (!verify(key, layer, prefix_roots)) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &entries_.find(key)->second.field;
}

const SdfPrefixField* SdfPrefixCache::find_usable(const scene::Layer& layer,
                                                  std::size_t max_boundary,
                                                  const SdfPrefixPolicy& policy) {
    if (max_boundary == 0) return nullptr;
    // The best boundary the cache actually holds, which during a stroke is the
    // one built before the stroke started rather than the one the policy names
    // now. Walked over the entries rather than searched by key, because the
    // key is a hash of the boundary and there is nothing to search.
    const SdfPrefixField* best = nullptr;
    std::uint64_t best_key = 0;
    for (const auto& [key, entry] : entries_) {
        const SdfPrefixField& f = entry.field;
        if (f.layer != layer.id) continue;
        if (f.prefix_roots == 0 || f.prefix_roots >= max_boundary) continue;
        if (f.cell_size != policy.cell_size || f.band != policy.band ||
            f.padding != policy.padding || f.align_to_lattice != policy.align_to_lattice)
            continue;
        if (best && f.prefix_roots <= best->prefix_roots) continue;
        best = &f;
        best_key = key;
    }
    if (!best) {
        ++stats_.misses;
        return nullptr;
    }
    const std::size_t roots = best->prefix_roots;
    if (!verify(best_key, layer, roots)) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &entries_.find(best_key)->second.field;
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
    const kernel::cfloat3 lo = whole.bounds.min - pad;
    const math::Aabb region =
        policy.align_to_lattice
            ? math::Aabb{kernel::cf3(std::floor(lo.x / policy.cell_size) * policy.cell_size,
                                     std::floor(lo.y / policy.cell_size) * policy.cell_size,
                                     std::floor(lo.z / policy.cell_size) * policy.cell_size),
                         whole.bounds.max + pad}
            : math::Aabb{lo, whole.bounds.max + pad};

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
    field.align_to_lattice = policy.align_to_lattice;
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

void SdfSourceField::fill_span(const float* points_xyz, std::size_t count, float* out,
                               bool covered) const {
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

bool SdfSourceField::covers(const float* points_xyz, std::size_t count) const {
    if (!composed_) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (!prefix_->volume.has_samples_at(kernel::cf3(
                points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2])))
            return false;
    return true;
}

void SdfSourceField::fill_points(const float* points_xyz, std::size_t count, float* out) const {
    // THE FAR-BOUND RULE. The cached volume may seed these points only if it
    // stores every one of them; outside its stored bricks `eval` answers with a
    // conservative bound rather than the distance the history had, and a suffix
    // folded onto that is wrong by cells rather than by rounding.
    fill_span(points_xyz, count, out, covers(points_xyz, count));
}

field::FieldVolume::BrickBlockFill SdfSourceField::block_fill() const {
    return [this](const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
                  float* out) {
        const std::size_t per = static_cast<std::size_t>(field::kBrickSamples);
        const std::size_t n = count * per;
        std::vector<float> points(n * 3);
        for (std::size_t s = 0; s < count; ++s)
            for (int i = 0; i < field::kBrickSamples; ++i) {
                const kernel::cfloat3 p = grid.sample_position(first + s, i);
                const std::size_t at = (s * per + static_cast<std::size_t>(i)) * 3;
                points[at] = p.x;
                points[at + 1] = p.y;
                points[at + 2] = p.z;
            }

        // ONE CALL FOR THE WHOLE WINDOW where there is no coverage question to
        // ask. Splitting a window into per-brick evaluations costs far more
        // than the sparsity it buys: the backend batches a window across its
        // pool, and 729 points at a time is a fraction of the work per dispatch
        // that 512 bricks is. Measured before this was written: a first dab at
        // 5,000 roots took 830 ms per-brick against a 543 ms whole-layer bake
        // -- the lazy path materialized a tenth of the bricks and still lost.
        if (!composed_) {
            fill_span(points.data(), n, out, false);
            return;
        }

        // With a prefix the question IS per brick, so runs of bricks that agree
        // are evaluated together and only a change of answer ends a batch. A
        // window is usually all-covered or all-not, so this is normally still
        // one call.
        std::size_t run_start = 0;
        bool run_covered = covers(points.data(), per);
        for (std::size_t s = 1; s <= count; ++s) {
            const bool here =
                s < count && covers(points.data() + s * per * 3, per);
            if (s < count && here == run_covered) continue;
            fill_span(points.data() + run_start * per * 3, (s - run_start) * per,
                      out + run_start * per, run_covered);
            run_start = s;
            run_covered = here;
        }
    };
}

}  // namespace session
}  // namespace clay
