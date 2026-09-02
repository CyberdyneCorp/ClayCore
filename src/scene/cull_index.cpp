#include "clay/scene/cull_index.h"

#include "clay/scene/bounds.h"

namespace clay {
namespace scene {

CullIndex::CullIndex(const Document& doc) : doc_(&doc) {
    for (const Layer& layer : doc.layers) {
        if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf) continue;
        // cull_pad_terms walks the flat node map, exactly as the compiler did
        // per compile: it must count a feathered volume wherever it sits,
        // including under a group this build never descends into. Kept per
        // layer and unadded (see LayerPad) so an append need not walk again.
        pads_.push_back(LayerPad{&layer, ::clay::scene::cull_pad_terms(*layer.sdf, layer),
                                 layer.sdf->nodes().size()});
        build_chain(*layer.sdf, layer.sdf->roots, layer);
    }
    refresh_pad();
}

// The document's pad from the layers': a maximum of their sums, never a sum of
// their maxima. O(layers), so an append pays it whole. Each layer's blend term
// is resolved against its CURRENT effective contributor count here — the terms
// hold raw maxima (bounds.h, CullPadTerms), the count `plan_pads` maintains is
// the raw map size, and the symmetry multiplicity is read from the LIVE layer,
// so an appended index resolves exactly what a fresh build would.
//
// Reading the multiplicity live is safe because a symmetry edit can never
// reach an index between builds: SetLayerMirrorCmd/SetLayerRadialCmd are not
// AddNodeCmds, so tail_append (bindings/c/clay_c.cpp) never routes them to
// touch_appended — they take the general invalidation, which forgets the
// append log and forces the rebuild. Within an index's life the layer's
// symmetry is constant; appends only grow `nodes`, and both factors of the
// product only rise, keeping the raise-only append contract exact.
void CullIndex::refresh_pad() {
    pad_ = 0.0f;
    for (const LayerPad& p : pads_)
        pad_ = kernel::cmax(pad_,
                            p.terms.total(p.nodes * layer_symmetry_multiplicity(*p.layer)));
}

namespace {
// The box a chain's scan tests for one entry: the entry's own bound, or an
// infinite box when the entry can never be culled. `!local` and an infinite
// bound are exactly the two clauses that make an entry survive whatever region
// is asked about, so deciding them at build time leaves the scan one box
// intersection instead of three tests.
math::Aabb probe_for(const CullIndex::Entry& e) {
    return (e.local && !e.bound.is_infinite()) ? e.bound : math::Aabb::infinite();
}

// `Aabb::intersects` with both of its emptiness guards discharged: the entry's
// by `probe_for`, the region's by `plan` before the loop starts. Six
// comparisons, no branches, and it is the whole of the scan.
bool probe_hits(const math::Aabb& p, const math::Aabb& t) {
    return p.min.x <= t.max.x && p.max.x >= t.min.x && p.min.y <= t.max.y &&
           p.max.y >= t.min.y && p.min.z <= t.max.z && p.max.z >= t.min.z;
}

// The scan a plan runs over one chain, packed: `probes` is parallel to
// `entries`, so the survivors come out in chain order without a sort.
void scan_packed(const std::vector<CullIndex::Entry>& entries,
                 const std::vector<math::Aabb>& probes, const math::Aabb& test,
                 std::vector<CullIndex::Entry>* kept) {
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (probe_hits(probes[i], test)) kept->push_back(entries[i]);
}

// The same scan with the predicate as the compiler writes it, for the one
// region the packed form cannot answer (see `plan`).
void scan_exact(const std::vector<CullIndex::Entry>& entries, const math::Aabb& test,
                std::vector<CullIndex::Entry>* kept) {
    for (const CullIndex::Entry& e : entries)
        // The compiler's own test (Compiler::culled), inverted: an entry
        // survives unless it is local, finite and misses the region.
        if (!e.local || e.bound.is_infinite() || e.bound.intersects(test)) kept->push_back(e);
}

}  // namespace

// One Chain per (layer, child list), mirroring the compiler's traversal:
// every group's children are their own chain — cull_dropped, which the
// feathered-replace choice reads, is scoped to a single compile_list call,
// so prunability is decided chain by chain rather than for the whole tree.
void CullIndex::build_chain(const SdfContent& content, const std::vector<NodeId>& ids,
                            const Layer& layer) {
    Chain chain;
    chain.layer = &layer;
    chain.ids = &ids;
    chain.prunable = true;
    chain.entries.reserve(ids.size());
    chain.probes.reserve(ids.size());
    for (NodeId id : ids) {
        const Node* n = content.find(id);
        if (!n || !n->visible) continue;
        Entry e;
        e.node = n;
        e.id = id;
        if (n->is_group) {
            e.bound = node_influence_bound(content, id, layer);
            // A group is always cull-tested; its bound reports infinity for
            // a non-local subtree, which the survive test lets through.
            e.local = true;
            build_chain(content, n->children, layer);
        } else {
            e.bound = item_geometry_bound(*n, layer);
            e.local = item_influence_is_local(*n);
            // The compiler's choice between the feathered and the hard
            // replace depends on whether the cull dropped ANYTHING from this
            // chain — including items a coarse cull would hide from it — so
            // such a chain keeps the full walk and decides per brick.
            if (item_is_feathered_replace(*n)) chain.prunable = false;
        }
        chain.entries.push_back(e);
        chain.probes.push_back(probe_for(e));
    }
    chains_.push_back(std::move(chain));
}

namespace {
// The last layer a compile emits for, which is the only one an append extends
// -- the same rule compile_document_append follows, and for the same reason.
const Layer* last_visible_sdf_layer(const Document& doc) {
    const Layer* found = nullptr;
    for (const Layer& layer : doc.layers)
        if (layer.visible && layer.kind == LayerKind::Sdf && layer.sdf) found = &layer;
    return found;
}

// The one claim the caller makes that an index can check for itself, and it is
// O(appended) rather than O(document).
bool is_tail_of(const std::vector<NodeId>& roots, const std::vector<NodeId>& appended) {
    if (appended.size() > roots.size()) return false;
    const std::size_t first = roots.size() - appended.size();
    for (std::size_t i = 0; i < appended.size(); ++i)
        if (roots[first + i] != appended[i]) return false;
    return true;
}

// What one appended subtree contributes to its layer's pad, and how many nodes
// it holds, in ONE walk of the subtree.
//
// EVERY node it reaches, including the children of an invisible group: the pad
// is a fold over the flat node map, which does not care what a build descends
// into, and cull_pad_terms already zeroes an invisible node itself.
std::size_t gather_pad(const SdfContent& content, const Layer& layer, NodeId id,
                       CullPadTerms* terms) {
    const Node* n = content.find(id);
    if (!n) return 0;
    terms->raise(::clay::scene::cull_pad_terms(*n, layer));
    std::size_t count = 1;
    for (NodeId child : n->children) count += gather_pad(content, layer, child, terms);
    return count;
}
}  // namespace

std::vector<std::size_t> CullIndex::chains_over(const std::vector<NodeId>& ids) const {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < chains_.size(); ++i)
        if (chains_[i].ids == &ids) found.push_back(i);
    return found;
}

// The pad half of an append, gathered before the entry half writes anything.
// Refuses -- which costs the caller a rebuild -- when a layer has no pad slot,
// or when its node map did not grow by exactly the appended subtree: the terms
// are raised from that subtree alone, so a map that gained something else would
// leave them BELOW what a fresh build reports, and a pad that is too small
// plans against too small a region.
bool CullIndex::plan_pads(const std::vector<std::size_t>& targets,
                          const std::vector<NodeId>& appended,
                          std::vector<PadGain>* gains) const {
    for (std::size_t at : targets) {
        const Layer& layer = *chains_[at].layer;
        const SdfContent& content = *layer.sdf;
        PadGain gain{pads_.size(), CullPadTerms{}, 0};
        for (std::size_t i = 0; i < pads_.size(); ++i)
            if (pads_[i].layer == &layer) gain.slot = i;
        if (gain.slot == pads_.size()) return false;
        std::size_t added = 0;
        for (NodeId id : appended) added += gather_pad(content, layer, id, &gain.terms);
        if (pads_[gain.slot].nodes + added != content.nodes().size()) return false;
        gain.nodes = content.nodes().size();
        gains->push_back(gain);
    }
    return true;
}

bool CullIndex::append(const std::vector<NodeId>& appended) {
    if (!doc_ || appended.empty()) return false;
    const Layer* last = last_visible_sdf_layer(*doc_);
    if (!last) return false;
    const std::vector<NodeId>& roots = last->sdf->roots;
    if (!is_tail_of(roots, appended)) return false;

    // EVERY chain over that root list, not just the last layer's. An INSTANCED
    // layer shares its SdfContent with the layer it instances, so one roots
    // vector is compiled once per layer that names it -- and each of those is
    // its own chain, with its own bounds, because `item_geometry_bound` reads
    // the layer's transform and mirror. Appending to only one of them leaves
    // the others describing a document that no longer exists, which is a
    // silently smaller tape rather than a crash: the corpus in
    // test_cull_index.cpp has such a layer and caught exactly that.
    const std::vector<std::size_t> targets = chains_over(roots);
    if (targets.empty()) return false;

    // Nothing is written until every check has passed, so a refusal leaves the
    // index exactly as it was -- which is why the pad is gathered here and
    // applied below rather than raised as the walk goes.
    std::vector<PadGain> gains;
    if (!plan_pads(targets, appended, &gains)) return false;

    for (std::size_t at : targets) {
        const Layer& layer = *chains_[at].layer;
        const SdfContent& content = *layer.sdf;
        for (NodeId id : appended) {
            const Node* n = content.find(id);
            if (!n || !n->visible) continue;
            Entry e;
            e.node = n;
            e.id = id;
            bool forbids_pruning = false;
            if (n->is_group) {
                e.bound = node_influence_bound(content, id, layer);
                // A group is always cull-tested; its bound reports infinity for
                // a non-local subtree, which the survive test lets through.
                e.local = true;
                // Grows chains_, so nothing may hold a Chain* across this.
                build_chain(content, n->children, layer);
            } else {
                e.bound = item_geometry_bound(*n, layer);
                e.local = item_influence_is_local(*n);
                forbids_pruning = item_is_feathered_replace(*n);
            }
            chains_[at].entries.push_back(e);
            chains_[at].probes.push_back(probe_for(e));
            if (forbids_pruning) chains_[at].prunable = false;
        }
    }
    // Both terms of a layer's pad are maxima over its visible nodes, so an
    // append can only raise them, and only for the layers it touched. They used
    // to be recomputed by re-walking the layer's node map -- a cheap walk
    // against the 2.45 ms rebuild it avoided, and all but 0.0003 ms of the
    // 0.054 ms an append cost once that rebuild was gone (#347). Raising them
    // from the appended subtree instead is exact because they are kept PER
    // LAYER, where a maximum of sums and a sum of maxima are the same number;
    // see LayerPad for why one global pair would not be.
    for (const PadGain& gain : gains) {
        pads_[gain.slot].terms.raise(gain.terms);
        pads_[gain.slot].nodes = gain.nodes;
    }
    refresh_pad();
    return true;
}

CullPlan CullIndex::plan(const math::Aabb& region) const {
    CullPlan plan;
    plan.pruned_.reserve(chains_.size());
    // Dilated by the feather pad exactly as the per-brick test is
    // (Compiler::begin_cull), so coarse survival stays a superset of
    // per-brick survival.
    const math::Aabb test = pad_ > 0.0f ? region.dilated(pad_) : region;
    // The packed scan folds `Aabb::intersects`' two emptiness guards away, which
    // is exact only for a region that discharges them itself. An INFINITE one
    // does not: it makes six bare comparisons true for EVERY probe, including
    // the probe of an entry whose own bound is empty -- a stroke or armature
    // with no points, a volume with no payload -- which the predicate drops.
    // So that region alone takes the predicate as written; it is a plan over
    // the whole document, where the scan is not what costs anything anyway.
    //
    // An EMPTY region needs no such fallback, and the asymmetry is worth
    // stating because it looks like an oversight. Against `min = +FLT_MAX,
    // max = -FLT_MAX` the bare test passes only a probe that is infinite on
    // all three axes, and `probe_for` gives exactly the always-survive entries
    // exactly that box -- which is what the predicate returns there too, every
    // `intersects` against an empty region being false. Equal by construction,
    // and `test_cull_index.cpp` holds both regions against the predicate.
    //
    // `is_infinite` reads only the x axis, and that is enough: an empty probe
    // survives only where BOTH x bounds are extreme, which is a subset of what
    // it reports, so it can send an answerable region down the slow path but
    // never a diverging one down the fast one.
    const bool packed = !test.is_infinite();
    for (const Chain& chain : chains_) {
        if (!chain.prunable) continue;
        std::vector<Entry> kept;
        if (packed)
            scan_packed(chain.entries, chain.probes, test, &kept);
        else
            scan_exact(chain.entries, test, &kept);
        // Stored even when nothing was dropped: the survivors carry the
        // cached bounds, so a planned chain never recomputes one per brick.
        plan.pruned_.emplace(Key{chain.layer, chain.ids}, std::move(kept));
    }
    return plan;
}

// See the header for why the in-place branch is safe and why this decision
// lives here rather than in the cache that makes it.
bool append_cached(std::shared_ptr<CullIndex>& slot, const std::vector<NodeId>& appended) {
    if (!slot) return false;
    if (slot.use_count() == 1) return slot->append(appended);
    // Shared: copying the entries is the memcpy the rebuild this replaces
    // would have done anyway, and the holder keeps the index it planned with.
    auto grown = std::make_shared<CullIndex>(*slot);
    if (!grown->append(appended)) return false;
    slot = std::move(grown);
    return true;
}

}  // namespace scene
}  // namespace clay
