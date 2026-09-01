// The stack's SHAPE OVER TIME: what a layer is, what each operation does to it,
// and the invalidation every one of them owes the composed cache
// (mesh-sculpt-layers spec, add-mesh-sculpt-layers).
//
// `sculpt_layer_eval.cpp` owns the other half — the per-block composition,
// which is measured in microseconds. Split for the reason `multires_eval.cpp`
// is split from `multires.cpp`, and it is the same reason twice: one file is
// about a structure's life and the other is a loop a stroke runs sixty times a
// second, and a file holding both is a file nobody reads twice.

#include "clay/mesh/sculpt_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clay {
namespace mesh {
namespace {

// The identity of an untouched mask. Named rather than spelled `1.0f` inline,
// because every "is this stored" test in this file is a comparison against it
// and a literal repeated eleven times is a literal one edit away from being
// two different literals.
constexpr float kMaskIdentity = 1.0f;

std::uint32_t normalize_block_size(std::uint32_t requested) {
    if (requested == 0) return DetailField::kDefaultBlockSize;
    std::uint32_t n = 4;
    while (n < requested && n < (1u << 20)) n <<= 1;
    return n;
}

void hash_u32(std::uint64_t* h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        *h ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xffu);
        *h *= 0x100000001b3ull;
    }
}

}  // namespace

// -- the mask -----------------------------------------------------------------

void SparseWeightField::reset(std::uint32_t vertex_count, std::uint32_t block_size) {
    vertex_count_ = vertex_count;
    block_size_ = normalize_block_size(block_size);
    block_slot_.assign(block_count(), kNoBlock);
    slot_block_.clear();
    storage_.clear();
}

std::uint32_t SparseWeightField::block_count() const {
    return block_size_ == 0 ? 0u : (vertex_count_ + block_size_ - 1) / block_size_;
}

float SparseWeightField::get(std::uint32_t vertex) const {
    // Outside the field entirely is ALSO 1.0, not 0.0. A composer asking about
    // a vertex a mask was never sized for must be told "this layer contributes
    // fully here" — the alternative answer erases the layer wherever a caller
    // is one level out of step, which is a silent visual failure rather than a
    // loud one.
    if (vertex >= vertex_count_) return kMaskIdentity;
    const std::uint32_t slot = block_slot_[vertex / block_size_];
    if (slot == kNoBlock) return kMaskIdentity;
    return storage_[static_cast<std::size_t>(slot) * block_size_ + (vertex % block_size_)];
}

std::size_t SparseWeightField::reserve_slot(std::uint32_t vertex) {
    const std::uint32_t block = vertex / block_size_;
    std::uint32_t slot = block_slot_[block];
    if (slot == kNoBlock) {
        slot = static_cast<std::uint32_t>(slot_block_.size());
        block_slot_[block] = slot;
        slot_block_.push_back(block);
        // Filled with the IDENTITY rather than zeroed, which is the whole
        // difference from `DetailField` and the reason this is not a template
        // over it: a freshly allocated block here means "unmasked", and a
        // zeroed one would mean the layer vanished across 1024 vertices the
        // moment one of them was masked.
        storage_.resize(storage_.size() + block_size_, kMaskIdentity);
    }
    return static_cast<std::size_t>(slot) * block_size_ + (vertex % block_size_);
}

void SparseWeightField::set(std::uint32_t vertex, float weight) {
    if (vertex >= vertex_count_) return;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    // Writing the identity into a block that does not exist stays nothing, for
    // the reason `DetailField::set` refuses to allocate for a zero.
    if (w == kMaskIdentity && block_slot_[vertex / block_size_] == kNoBlock) return;
    storage_[reserve_slot(vertex)] = w;
}

bool SparseWeightField::empty() const {
    for (float w : storage_)
        if (w != kMaskIdentity) return false;
    return true;
}

std::size_t SparseWeightField::resident_vertices() const {
    return slot_block_.size() * block_size_;
}

std::size_t SparseWeightField::bytes() const {
    return storage_.capacity() * sizeof(float) + block_slot_.capacity() * sizeof(std::uint32_t) +
           slot_block_.capacity() * sizeof(std::uint32_t);
}

std::uint64_t SparseWeightField::checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ull;
    hash_u32(&h, vertex_count_);
    // Per NON-IDENTITY vertex, so the value depends on the content and on
    // nothing about the container — not the block size, not the order blocks
    // were allocated in. The same argument `DetailField::checksum` makes.
    for (std::uint32_t v = 0; v < vertex_count_; ++v) {
        const float w = get(v);
        if (w == kMaskIdentity) continue;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &w, 4);
        hash_u32(&h, v);
        hash_u32(&h, bits);
    }
    return h;
}

void SparseWeightField::compact() {
    std::vector<std::uint32_t> keep;
    keep.reserve(slot_block_.size());
    for (std::size_t s = 0; s < slot_block_.size(); ++s) {
        const float* block = storage_.data() + s * block_size_;
        for (std::uint32_t i = 0; i < block_size_; ++i)
            if (block[i] != kMaskIdentity) {
                keep.push_back(static_cast<std::uint32_t>(s));
                break;
            }
    }
    if (keep.size() == slot_block_.size()) return;

    std::vector<float> packed(keep.size() * block_size_, kMaskIdentity);
    std::vector<std::uint32_t> new_slot_block(keep.size());
    std::fill(block_slot_.begin(), block_slot_.end(), kNoBlock);
    for (std::size_t i = 0; i < keep.size(); ++i) {
        const std::size_t src = static_cast<std::size_t>(keep[i]) * block_size_;
        std::copy(storage_.begin() + static_cast<std::ptrdiff_t>(src),
                  storage_.begin() + static_cast<std::ptrdiff_t>(src + block_size_),
                  packed.begin() + static_cast<std::ptrdiff_t>(i * block_size_));
        new_slot_block[i] = slot_block_[keep[i]];
        block_slot_[new_slot_block[i]] = static_cast<std::uint32_t>(i);
    }
    storage_ = std::move(packed);
    slot_block_ = std::move(new_slot_block);
}

bool SparseWeightField::block_stored(std::uint32_t block) const {
    return block < block_slot_.size() && block_slot_[block] != kNoBlock;
}

std::uint32_t SparseWeightField::stored_block_count() const {
    return static_cast<std::uint32_t>(slot_block_.size());
}

std::uint32_t SparseWeightField::stored_block_at(std::uint32_t index) const {
    return index < slot_block_.size() ? slot_block_[index] : kNoBlock;
}

// -- one layer ----------------------------------------------------------------

std::size_t SculptLayer::bytes() const {
    std::size_t total = name.capacity();
    for (const DetailField& f : detail) total += f.bytes();
    for (const SparseWeightField& m : mask) total += m.bytes();
    return total;
}

std::size_t SculptLayer::coverage_vertices() const {
    std::size_t total = 0;
    for (const DetailField& f : detail) total += f.resident_vertices();
    return total;
}

bool SculptLayer::has_content() const {
    for (const DetailField& f : detail)
        if (f.stored_block_count() != 0) return true;
    return false;
}

// -- the stack: lookup --------------------------------------------------------

const SculptLayer* SculptLayerStack::at(std::size_t index) const {
    return index < layers_.size() ? &layers_[index] : nullptr;
}

std::size_t SculptLayerStack::index_of(SculptLayerId id) const {
    if (id == kNoSculptLayer) return kNoSculptLayerIndex;
    for (std::size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].id == id) return i;
    return kNoSculptLayerIndex;
}

const SculptLayer* SculptLayerStack::find(SculptLayerId id) const {
    const std::size_t i = index_of(id);
    return i == kNoSculptLayerIndex ? nullptr : &layers_[i];
}

SculptLayer* SculptLayerStack::mutable_find(SculptLayerId id) {
    const std::size_t i = index_of(id);
    return i == kNoSculptLayerIndex ? nullptr : &layers_[i];
}

SculptLayerId SculptLayerStack::id_at(std::size_t index) const {
    return index < layers_.size() ? layers_[index].id : kNoSculptLayer;
}

bool SculptLayerStack::set_active(SculptLayerId id) {
    // Clearing is always allowed: "write to the base detail" must be reachable
    // whatever the stack holds.
    if (id != kNoSculptLayer && index_of(id) == kNoSculptLayerIndex) return false;
    if (active_ == id) return true;
    active_ = id;
    // METADATA. Which layer a brush would write into next changes nothing
    // about the surface, so this must not invalidate one block of it.
    ++metadata_revision_;
    return true;
}

// -- the stack: sizing --------------------------------------------------------

void SculptLayerStack::size_layer(SculptLayer* layer) const {
    layer->detail.resize(level_vertices_.size());
    layer->mask.resize(level_vertices_.size());
}

void SculptLayerStack::set_level_sizes(const std::vector<std::uint32_t>& vertices_per_level) {
    // A level whose VERTEX COUNT changed is a different level: a coefficient
    // stored against vertex 7000 of a level that now has 4000 vertices does not
    // describe a vertex that moved, it describes a vertex that does not exist.
    // Dropping is the only honest answer, and it is not a loss in practice —
    // the counts change when a level is added or removed, and removing a level
    // is already the destructive operation `remove_highest_level` documents.
    const std::size_t old_levels = level_vertices_.size();
    for (std::size_t l = 0; l < vertices_per_level.size(); ++l) {
        const bool changed = l >= old_levels || level_vertices_[l] != vertices_per_level[l];
        if (!changed) continue;
        for (SculptLayer& layer : layers_) {
            if (l < layer.detail.size()) layer.detail[l] = DetailField{};
            if (l < layer.mask.size()) layer.mask[l] = SparseWeightField{};
        }
    }
    level_vertices_ = vertices_per_level;
    for (SculptLayer& layer : layers_) size_layer(&layer);

    // `LevelDirty` starts with `all` set, which is the answer every reader
    // wants after a resize; the per-block mark array is sized where it is first
    // consulted, in `clear_dirty`.
    dirty_.assign(level_vertices_.size(), LevelDirty{});
}

std::uint32_t SculptLayerStack::level_vertex_count(std::uint32_t level) const {
    return level < level_vertices_.size() ? level_vertices_[level] : 0u;
}

std::uint32_t SculptLayerStack::level_block_count(std::uint32_t level) const {
    const std::uint32_t n = level_vertex_count(level);
    return n == 0 ? 0u : (n + block_size_ - 1) / block_size_;
}

bool SculptLayerStack::reaches_level(std::uint32_t level) const {
    // DETAIL ONLY, and deliberately: a mask multiplies coefficients, so a mask
    // over a layer with nothing stored contributes nothing and must not make a
    // level allocate a composed field it would fill with the base detail. That
    // is what keeps the no-layer path reading the base field through exactly
    // the arithmetic it used before this change existed.
    for (const SculptLayer& layer : layers_) {
        if (level >= layer.detail.size()) continue;
        if (layer.detail[level].stored_block_count() != 0) return true;
    }
    return false;
}

// -- the stack: invalidation --------------------------------------------------

void SculptLayerStack::note_block(std::uint32_t level, std::uint32_t block) {
    if (level >= dirty_.size()) return;
    LevelDirty& d = dirty_[level];
    if (d.all) return;
    if (block >= d.mark.size()) return;
    if (d.mark[block]) return;
    d.mark[block] = 1;
    d.blocks.push_back(block);
}

void SculptLayerStack::note_content(std::uint32_t level, std::uint32_t block) {
    note_block(level, block);
    ++content_revision_;
    ++content_bumps_;
}

void SculptLayerStack::invalidate(std::uint32_t level, std::uint32_t block) {
    note_block(level, block);
}

void SculptLayerStack::note_layer_coverage(const SculptLayer& layer) {
    for (std::uint32_t l = 0; l < static_cast<std::uint32_t>(layer.detail.size()); ++l) {
        const DetailField& f = layer.detail[l];
        const std::uint32_t stored = f.stored_block_count();
        for (std::uint32_t i = 0; i < stored; ++i) note_block(l, f.stored_block_at(i));
        // A masked block with no coefficients contributes nothing, but a mask
        // CHANGE over a block the layer does reach still matters — and the two
        // sets are usually the same one. Walking the mask's blocks as well
        // costs the mask's blocks and closes the case where they are not.
        if (l >= layer.mask.size()) continue;
        const SparseWeightField& m = layer.mask[l];
        const std::uint32_t masked = m.stored_block_count();
        for (std::uint32_t i = 0; i < masked; ++i)
            if (f.block_stored(m.stored_block_at(i))) note_block(l, m.stored_block_at(i));
    }
}

bool SculptLayerStack::level_all_dirty(std::uint32_t level) const {
    return level >= dirty_.size() || dirty_[level].all;
}

const std::vector<std::uint32_t>& SculptLayerStack::dirty_blocks(std::uint32_t level) const {
    static const std::vector<std::uint32_t> kEmpty;
    return level < dirty_.size() ? dirty_[level].blocks : kEmpty;
}

void SculptLayerStack::clear_dirty(std::uint32_t level) {
    if (level >= dirty_.size()) return;
    LevelDirty& d = dirty_[level];
    // Reset through the LIST rather than clearing the mark array, so the cost
    // is what was actually dirty and not what the level holds — the same
    // discipline `MultiresSurface::clear_dirty` uses for its patches.
    for (std::uint32_t b : d.blocks)
        if (b < d.mark.size()) d.mark[b] = 0;
    d.blocks.clear();
    // WHERE THE MARK ARRAY IS SIZED, and the only place it can be. It exists so
    // a second `note_block` on one block does not list it twice, and
    // `note_block` reads it only when `all` is false — which starts being true
    // one line below and nowhere else. Sizing it when a LEVEL is declared
    // instead let a forty-eight-byte stack chunk reserve three gigabytes of
    // index for a hierarchy carrying no layers at all.
    const std::size_t blocks = level_block_count(level);
    if (d.mark.size() != blocks) d.mark.assign(blocks, 0);
    d.all = false;
}

bool SculptLayerStack::any_dirty() const {
    for (std::size_t l = 0; l < dirty_.size(); ++l) {
        if (dirty_[l].all) {
            // "Everything is stale" is only work when there is something to
            // recompose; on a level no layer reaches it is the resting state.
            if (reaches_level(static_cast<std::uint32_t>(l))) return true;
            continue;
        }
        if (!dirty_[l].blocks.empty()) return true;
    }
    return false;
}

void SculptLayerStack::dirty_all() {
    for (LevelDirty& d : dirty_) {
        d.blocks.clear();
        std::fill(d.mark.begin(), d.mark.end(), 0);
        d.all = true;
    }
}

// -- the stack: lifecycle -----------------------------------------------------

SculptLayerId SculptLayerStack::add(std::string name) {
    if (composition_held_) return kNoSculptLayer;
    if (layers_.size() >= kMaxLayers) return kNoSculptLayer;
    SculptLayer layer;
    layer.id = next_id_++;
    layer.name = std::move(name);
    size_layer(&layer);
    layers_.push_back(std::move(layer));
    active_ = layers_.back().id;
    // COMPOSITION, even though an empty layer contributes nothing: the count a
    // host reads changed, and a revision that only moved when the geometry did
    // would make "the stack changed" unobservable until the first stamp.
    ++composition_revision_;
    ++composition_bumps_;
    ++metadata_revision_;
    return layers_.back().id;
}

bool SculptLayerStack::insert(std::size_t index, SculptLayer layer) {
    if (composition_held_) return false;
    if (layer.id == kNoSculptLayer || index_of(layer.id) != kNoSculptLayerIndex) return false;
    if (layers_.size() >= kMaxLayers) return false;
    if (index > layers_.size()) index = layers_.size();
    size_layer(&layer);
    // The id counter only ever moves FORWARD. Reinserting a layer whose id came
    // from a previous session — an undo of a remove, a decode — must not let a
    // later `add` mint the same id again.
    if (layer.id >= next_id_) next_id_ = layer.id + 1;
    note_layer_coverage(layer);
    layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    ++composition_revision_;
    ++composition_bumps_;
    return true;
}

bool SculptLayerStack::remove(SculptLayerId id, SculptLayer* out) {
    if (composition_held_) return false;
    const std::size_t i = index_of(id);
    if (i == kNoSculptLayerIndex) return false;
    // The coverage is noted BEFORE the layer goes, because it is the removed
    // layer's own blocks that have to be recomposed — and after the erase there
    // is nothing left to ask.
    note_layer_coverage(layers_[i]);
    if (out) *out = std::move(layers_[i]);
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(i));
    if (active_ == id) active_ = kNoSculptLayer;
    ++composition_revision_;
    ++composition_bumps_;
    ++metadata_revision_;
    return true;
}

bool SculptLayerStack::move_to(SculptLayerId id, std::size_t index) {
    if (composition_held_) return false;
    const std::size_t from = index_of(id);
    if (from == kNoSculptLayerIndex) return false;
    if (index >= layers_.size()) index = layers_.size() - 1;
    if (from == index) return true;
    SculptLayer moved = std::move(layers_[from]);
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(from));
    layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
    // NO BLOCK IS INVALIDATED, and that is not an oversight. Addition commutes,
    // so a reorder cannot move a vertex; what it changes is the order a host
    // draws the list in. The revision still moves, because a host watching the
    // stack has to redraw.
    //
    // That is only true because composition sums in ID order rather than in
    // stack order — see `gather_contributors`. Summing in stack order would
    // make this line a bug rather than an optimisation: float addition does not
    // associate, so the blocks a later stroke happened to recompose would carry
    // one order and the blocks still cached would carry the other.
    ++composition_revision_;
    ++composition_bumps_;
    return true;
}

// -- the stack: properties ----------------------------------------------------

bool SculptLayerStack::rename(SculptLayerId id, std::string name) {
    SculptLayer* layer = mutable_find(id);
    if (!layer) return false;
    if (name.size() > kMaxNameBytes) return false;
    layer->name = std::move(name);
    // METADATA, and this is the case task 5.2 names: a rename must not
    // invalidate one block of geometry. Allowed while a stroke holds the
    // composition, because it moves nothing.
    ++metadata_revision_;
    return true;
}

bool SculptLayerStack::set_strength(SculptLayerId id, float strength) {
    if (composition_held_) return false;
    SculptLayer* layer = mutable_find(id);
    if (!layer) return false;
    // Clamped rather than refused. A strength is a proportion of what was
    // recorded, and a host slider that overshoots by a rounding step should
    // reach full contribution rather than fail.
    const float s = std::clamp(strength, 0.0f, 1.0f);
    if (s == layer->strength) return true;
    layer->strength = s;
    note_layer_coverage(*layer);
    ++composition_revision_;
    ++composition_bumps_;
    return true;
}

bool SculptLayerStack::set_visible(SculptLayerId id, bool visible) {
    if (composition_held_) return false;
    SculptLayer* layer = mutable_find(id);
    if (!layer) return false;
    if (layer->visible == visible) return true;
    layer->visible = visible;
    note_layer_coverage(*layer);
    ++composition_revision_;
    ++composition_bumps_;
    return true;
}

bool SculptLayerStack::set_locked(SculptLayerId id, bool locked) {
    SculptLayer* layer = mutable_find(id);
    if (!layer) return false;
    layer->locked = locked;
    // A lock is a PERMISSION and not a contribution: it changes what the next
    // stamp is allowed to do and nothing about the surface, so it is metadata
    // and it is allowed mid-stroke.
    ++metadata_revision_;
    return true;
}

// -- the stack: content -------------------------------------------------------

namespace {

// Size a layer's field to the level the first time something writes into it, so
// a layer over a twelve-level hierarchy costs the levels it REACHED. The block
// table alone is four bytes a block, which over 128 layers and every level of a
// deep hierarchy is real memory spent to say "nothing here".
template <typename Field>
Field* sized_field(std::vector<Field>& fields, std::uint32_t level, std::uint32_t vertex_count,
                   std::uint32_t block_size) {
    if (level >= fields.size() || vertex_count == 0) return nullptr;
    Field& f = fields[level];
    if (f.vertex_count() != vertex_count) f.reset(vertex_count, block_size);
    return &f;
}

}  // namespace

DetailField* SculptLayerStack::detail_mutable(SculptLayerId id, std::uint32_t level) {
    SculptLayer* layer = mutable_find(id);
    if (!layer) return nullptr;
    return sized_field(layer->detail, level, level_vertex_count(level), block_size_);
}

const DetailField* SculptLayerStack::detail_at(SculptLayerId id, std::uint32_t level) const {
    const SculptLayer* layer = find(id);
    if (!layer || level >= layer->detail.size()) return nullptr;
    return &layer->detail[level];
}

SparseWeightField* SculptLayerStack::mask_mutable(SculptLayerId id, std::uint32_t level) {
    SculptLayer* layer = mutable_find(id);
    if (!layer) return nullptr;
    return sized_field(layer->mask, level, level_vertex_count(level), block_size_);
}

const SparseWeightField* SculptLayerStack::mask_at(SculptLayerId id, std::uint32_t level) const {
    const SculptLayer* layer = find(id);
    if (!layer || level >= layer->mask.size()) return nullptr;
    return &layer->mask[level];
}

bool SculptLayerStack::add_detail(SculptLayerId id, std::uint32_t level, std::uint32_t vertex,
                                  const LocalDetail& delta) {
    if (delta.zero()) return true;
    DetailField* field = detail_mutable(id, level);
    if (!field || vertex >= field->vertex_count()) return false;
    LocalDetail value = field->get(vertex);
    value.tangent += delta.tangent;
    value.bitangent += delta.bitangent;
    value.normal += delta.normal;
    field->set(vertex, value);
    note_content(level, vertex / block_size_);
    return true;
}

bool SculptLayerStack::set_detail(SculptLayerId id, std::uint32_t level, std::uint32_t vertex,
                                  const LocalDetail& value) {
    DetailField* field = detail_mutable(id, level);
    if (!field || vertex >= field->vertex_count()) return false;
    field->set(vertex, value);
    note_content(level, vertex / block_size_);
    return true;
}

bool SculptLayerStack::set_mask(SculptLayerId id, std::uint32_t level, std::uint32_t vertex,
                                float weight) {
    SparseWeightField* field = mask_mutable(id, level);
    if (!field || vertex >= field->vertex_count()) return false;
    field->set(vertex, weight);
    // A mask edit is COMPOSITION and not content: it changes how much of what
    // is stored reaches the surface, which is what the strength slider does by
    // another route.
    note_block(level, vertex / block_size_);
    ++composition_revision_;
    ++composition_bumps_;
    return true;
}

// The union of the two layers' coverages, and nothing outside it: everywhere
// else both terms are zero and the answer is the zero already there. Gathered
// rather than walked, because a level's blocks are numbered and the two layers
// each store an ascending list of the ones they reach.
void SculptLayerStack::merge_blocks(std::size_t ui, std::size_t li, std::uint32_t level,
                                    std::vector<std::uint32_t>* blocks) const {
    const DetailField& du = layers_[ui].detail[level];
    const DetailField& dl = layers_[li].detail[level];
    blocks->clear();
    for (std::uint32_t i = 0; i < du.stored_block_count(); ++i)
        blocks->push_back(du.stored_block_at(i));
    for (std::uint32_t i = 0; i < dl.stored_block_count(); ++i)
        blocks->push_back(dl.stored_block_at(i));
    std::sort(blocks->begin(), blocks->end());
    blocks->erase(std::unique(blocks->begin(), blocks->end()), blocks->end());
}

// One level of a merge: `L_l' = s_u·m_u·L_u + s_l·m_l·L_l`, stored DIRECTLY.
// The naive concatenation would solve for a coefficient carrying the ratio of
// the two strengths and be undefined at `s_l = 0` — a state one slider reaches.
// The target's composition is set to the identity by the caller, which is what
// makes the evaluated surface unchanged by construction rather than by luck.
void SculptLayerStack::merge_level(std::size_t ui, std::size_t li, std::uint32_t level, float fu,
                                   float fl) {
    const std::uint32_t vertices = level_vertex_count(level);
    if (vertices == 0) return;
    if (layers_[ui].detail[level].stored_block_count() == 0 &&
        layers_[li].detail[level].stored_block_count() == 0)
        return;

    std::vector<std::uint32_t> blocks;
    merge_blocks(ui, li, level, &blocks);

    const DetailField& du = layers_[ui].detail[level];
    const DetailField& dl = layers_[li].detail[level];
    const SparseWeightField& mu = layers_[ui].mask[level];
    const SparseWeightField& ml = layers_[li].mask[level];
    std::vector<LocalDetail> merged;
    std::vector<std::uint32_t> vertex_ids;
    for (std::uint32_t b : blocks) {
        const std::uint32_t begin = b * block_size_;
        const std::uint32_t end = std::min(begin + block_size_, vertices);
        for (std::uint32_t v = begin; v < end; ++v) {
            const LocalDetail a = du.get(v), c = dl.get(v);
            const float wu = fu * mu.get(v), wl = fl * ml.get(v);
            LocalDetail out;
            out.tangent = wu * a.tangent + wl * c.tangent;
            out.bitangent = wu * a.bitangent + wl * c.bitangent;
            out.normal = wu * a.normal + wl * c.normal;
            merged.push_back(out);
            vertex_ids.push_back(v);
        }
    }

    DetailField* target = sized_field(layers_[li].detail, level, vertices, block_size_);
    if (!target) return;
    for (std::size_t i = 0; i < merged.size(); ++i) target->set(vertex_ids[i], merged[i]);
    // The mask is CLEARED rather than combined. It has already been folded into
    // the coefficients above, and leaving it would apply it twice.
    layers_[li].mask[level] = SparseWeightField{};
}

bool SculptLayerStack::merge_down(SculptLayerId upper) {
    if (composition_held_) return false;
    const std::size_t ui = index_of(upper);
    if (ui == kNoSculptLayerIndex || ui == 0) return false;
    const std::size_t li = ui - 1;
    // Merging WRITES the target, so a locked target refuses — the same rule a
    // sculpt write follows, applied to the other operation that changes a
    // layer's coefficients.
    if (layers_[li].locked) return false;

    const float fu = layers_[ui].composition_factor();
    const float fl = layers_[li].composition_factor();
    note_layer_coverage(layers_[ui]);
    note_layer_coverage(layers_[li]);

    for (std::uint32_t l = 0; l < static_cast<std::uint32_t>(level_vertices_.size()); ++l)
        merge_level(ui, li, l, fu, fl);

    // The identity the target now needs for the evaluated surface to be
    // unchanged. Visible as well as full strength: merging into a hidden layer
    // must not hide the pass that was just folded into it.
    layers_[li].strength = 1.0f;
    layers_[li].visible = true;

    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(ui));
    if (active_ == upper) active_ = layers_[li].id;
    ++composition_revision_;
    ++composition_bumps_;
    ++content_revision_;
    ++content_bumps_;
    ++metadata_revision_;
    return true;
}

// -- accounting ---------------------------------------------------------------

void SculptLayerStack::compact() {
    for (SculptLayer& layer : layers_) {
        for (DetailField& f : layer.detail) f.compact();
        for (SparseWeightField& m : layer.mask) m.compact();
    }
}

SculptLayerMemory SculptLayerStack::memory() const {
    SculptLayerMemory m;
    m.layers = layers_.size();
    for (const SculptLayer& layer : layers_) {
        m.content += layer.name.capacity();
        for (const DetailField& f : layer.detail) m.content += f.bytes();
        for (const SparseWeightField& mask : layer.mask) m.masks += mask.bytes();
    }
    m.total = m.content + m.masks;
    return m;
}

// -- the delta ----------------------------------------------------------------

void SculptLayerDelta::clear() {
    detail_.clear();
    mask_.clear();
    detail_slot_.clear();
    mask_slot_.clear();
    layer_ = kNoSculptLayer;
}

std::size_t SculptLayerDelta::bytes() const {
    return detail_.capacity() * sizeof(DetailEntry) + mask_.capacity() * sizeof(MaskEntry) +
           (detail_slot_.size() + mask_slot_.size()) *
               (sizeof(std::uint64_t) + sizeof(std::uint32_t));
}

std::vector<std::uint32_t> SculptLayerDelta::levels() const {
    std::vector<std::uint32_t> out;
    for (const DetailEntry& e : detail_) out.push_back(e.level);
    for (const MaskEntry& e : mask_) out.push_back(e.level);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void SculptLayerDelta::note_detail(std::uint32_t level, std::uint32_t vertex,
                                   const LocalDetail& before) {
    // The FIRST sighting wins, which is what makes a hundred stamps over one
    // vertex one entry whose `before` is where the STROKE found it.
    const std::uint64_t key = key_of(level, vertex);
    if (detail_slot_.find(key) != detail_slot_.end()) return;
    detail_slot_.emplace(key, static_cast<std::uint32_t>(detail_.size()));
    DetailEntry e;
    e.level = level;
    e.vertex = vertex;
    e.before = before;
    e.after = before;
    detail_.push_back(e);
}

void SculptLayerDelta::note_mask(std::uint32_t level, std::uint32_t vertex, float before) {
    const std::uint64_t key = key_of(level, vertex);
    if (mask_slot_.find(key) != mask_slot_.end()) return;
    mask_slot_.emplace(key, static_cast<std::uint32_t>(mask_.size()));
    MaskEntry e;
    e.level = level;
    e.vertex = vertex;
    e.before = before;
    e.after = before;
    mask_.push_back(e);
}

void SculptLayerDelta::sync_after(const SculptLayerStack& stack) {
    for (DetailEntry& e : detail_) {
        const DetailField* f = stack.detail_at(layer_, e.level);
        if (f) e.after = f->get(e.vertex);
    }
    for (MaskEntry& e : mask_) {
        const SparseWeightField* m = stack.mask_at(layer_, e.level);
        if (m) e.after = m->get(e.vertex);
    }
}

bool SculptLayerDelta::write(SculptLayerStack& stack, bool forward) const {
    if (layer_ == kNoSculptLayer || !stack.find(layer_)) return false;
    // EVERY entry is checked against the stack BEFORE one of them is written,
    // so a record paired with the wrong surface changes nothing rather than
    // half of something.
    for (const DetailEntry& e : detail_)
        if (e.vertex >= stack.level_vertex_count(e.level)) return false;
    for (const MaskEntry& e : mask_)
        if (e.vertex >= stack.level_vertex_count(e.level)) return false;
    for (const DetailEntry& e : detail_)
        stack.set_detail(layer_, e.level, e.vertex, forward ? e.after : e.before);
    for (const MaskEntry& e : mask_)
        stack.set_mask(layer_, e.level, e.vertex, forward ? e.after : e.before);
    return true;
}

bool SculptLayerDelta::revert(SculptLayerStack& stack) const { return write(stack, false); }
bool SculptLayerDelta::apply(SculptLayerStack& stack) const { return write(stack, true); }

std::size_t SculptLayerProperty::bytes() const {
    return sizeof(SculptLayerProperty) + name_before.capacity() + name_after.capacity() +
           stack_before.capacity() + stack_after.capacity() +
           base_detail.capacity() * sizeof(DetailEntry) +
           base_vertices.capacity() * sizeof(std::uint32_t) +
           (base_before.capacity() + base_after.capacity()) * sizeof(kernel::cfloat3);
}

}  // namespace mesh
}  // namespace clay
