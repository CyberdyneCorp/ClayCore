// The consolidation policy (scene-model spec, add-consolidation-policy). See
// include/clay/scene/consolidate.h for the decisions this implements: why the
// trigger is advisory, why the scope is a layer, and why a consolidated region
// is recognised by its content rather than by a stored provenance flag.

#include "clay/scene/consolidate.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/field/redistance.h"
#include "clay/kernel/exactness.h"
#include "clay/scene/tape.h"

namespace clay {
namespace scene {

namespace {

void fill_cost(const field::FieldVolume& v, ConsolidationCost& cost) {
    cost.cell_size = v.cell_size();
    cost.band = v.band();
    cost.brick_count = v.brick_count();
    cost.sample_count = v.sample_count();
    cost.bytes = v.blob_floats() * sizeof(float);
    cost.sample_lipschitz = v.sample_lipschitz();
    // What the tape compiler will declare once this volume is an item: the
    // caller is being told the marching cost it is buying, not a raw slope.
    const kernel::CFieldInfo info = kernel::cfi_volume(v.sample_lipschitz());
    cost.lipschitz = info.lipschitz;
    cost.safe_step_scale = kernel::csafe_step_scale(info);
    cost.bounds = v.bounds();
}

// compile_layer treats a hidden layer as empty, which is right for drawing and
// wrong for both questions here: a hidden layer is still a layer whose chain
// has degraded, and reporting it as clean — or refusing to bake it — would make
// hiding a layer a way to get stuck in a state nothing will tell you about.
Layer visible_view(const Layer& layer) {
    Layer view = layer;
    view.visible = true;
    return view;
}

// ...and for a BAKE, the layer in its OWN frame as well. Sampling the
// world-space field and putting the result back under the layer would apply the
// layer's transform twice; sampling locally leaves the transform where it was
// authored, so consolidating moves nothing.
Layer local_view(const Layer& layer) {
    Layer view = visible_view(layer);
    view.xform = math::Transform{};
    return view;
}

// A node carrying samples and nothing else to edit.
bool is_volume_item(const Node& n) {
    return !n.is_group && n.prim.type == PrimType::Volume && n.volume != nullptr;
}

// Descend, counting nodes and the two things that degrade a chain. Written as
// an explicit stack rather than a recursion because the only reason to recurse
// here would be to walk a tree the arena can already walk flat.
void walk(const SdfContent& content, const std::vector<NodeId>& ids, FieldReport& out) {
    std::vector<NodeId> pending(ids.rbegin(), ids.rend());
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        const Node* n = content.find(id);
        if (!n) continue;
        ++out.item_count;
        out.longest_deformer_chain =
            std::max(out.longest_deformer_chain, static_cast<int>(n->deformers.size()));
        if (n->volume)
            out.steepest_volume = std::max(out.steepest_volume, n->volume->sample_lipschitz());
        pending.insert(pending.end(), n->children.rbegin(), n->children.rend());
    }
}

// -- the bake's grid path -----------------------------------------------------
//
// The serial bake evaluated the layer tape at every lattice sample through a
// std::function, one point at a time on one core. This is the same
// evaluation as a flat batch through the CPU backend: same tape, same points,
// and the backend's batch path is documented to match its scalar reference
// bit for bit, so the volume comes out byte-identical while every core
// participates. Blocks land in slot order whatever order the pool computed
// them in (sample_blocks assembles serially), so the bytes do not depend on
// thread scheduling either.
//
// WHY NOT PER-BRICK CULLED TAPES, when the refill path over the same bricks
// lives on them: the cull contract (scene/tape.h) is BAND-CLAMPED identity,
// and the bake is the one consumer that stores raw values past the band. It
// is not just the far samples that go soft, either — a chain of smooth
// blends can carry a culled item's quadratic tail from an accumulator value
// beyond the cull horizon down INTO the band (measured up to ~7e-3 on a
// 24-dab blend chain, dilating by the band exactly as the refill does). The
// refill never sees this because it clamps every stored value to ±band and
// its scenes pay it below its half-float precision; a bake that consolidated
// a layer to slightly different bytes than the serial bake would fail the
// determinism this feature is specified against. Culling here would need a
// per-op tail-freeness analysis to be sound — the win it offered is already
// covered by the pool, so the bake spends cores, not exactness.

// One window of bricks, every sample against the full tape, distances only.
// The loop below is the same evaluation again for a build with no registered
// backends at all.
void fill_window(const Tape& tape, const field::FieldVolume::BrickGrid& grid, std::size_t first,
                 std::size_t count, float* out) {
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
    eval::PointQuery q;
    q.points_xyz = points.data();
    q.count = n;
    eval::PointResults res;
    res.distances = out;
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    if (cpu && cpu->eval_points(tape, q, res) == eval::Status::Ok) return;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = tape.eval(kernel::cf3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2])).d;
}

}  // namespace

FieldReport report_layer(const Layer& layer, float advise_below_step_scale) {
    FieldReport out;
    const Tape tape = compile_layer(visible_view(layer));
    out.lipschitz = tape.info.lipschitz;
    out.safe_step_scale = tape.safe_step_scale();
    if (layer.sdf) walk(*layer.sdf, layer.sdf->roots, out);
    out.advises_consolidation =
        advise_below_step_scale > 0.0f && out.safe_step_scale < advise_below_step_scale;
    return out;
}

std::optional<field::FieldVolume> bake_layer(const Layer& layer,
                                             const ConsolidationParams& params,
                                             ConsolidationCost* out_cost) {
    if (!(params.cell_size > 0.0f)) return std::nullopt;
    const float band = params.band > 0.0f ? params.band : params.cell_size * 3.0f;
    const float padding = params.padding > 0.0f ? params.padding : band;

    const Layer view = local_view(layer);
    const Tape tape = compile_layer(view);
    if (tape.empty() || tape.bounds.empty() || tape.bounds.is_infinite()) return std::nullopt;

    math::Aabb region = params.region;
    if (region.empty()) {
        // Padded: sampling exactly to the bounds would clip the band at the
        // surface, which is where it is needed most.
        const kernel::cfloat3 pad = kernel::cf3(padding, padding, padding);
        region = math::Aabb{tape.bounds.min - pad, tape.bounds.max + pad};
    }

    field::FieldVolume volume = field::FieldVolume::sample_blocks(
        [&tape](const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
                float* out) { fill_window(tape, grid, first, count, out); },
        region, params.cell_size, band);
    // brick_count rather than empty(): a volume covering only empty space
    // still has a full brick index, it just stores no samples, and handing one
    // back from a bake would replace the layer with something that silently
    // contributes nothing.
    if (volume.brick_count() == 0) return std::nullopt;

    // compact() only AFTER a successful redistance, never on its own: it
    // drops bricks on the strength of their samples all being past the band,
    // which a steep field does not entitle anyone to conclude. Redistancing is
    // what earns it, and it is what stops a repeatedly consolidated chain from
    // growing a brick of stored shell per bake.
    if (!params.skip_redistance && field::redistance(volume)) volume.compact();
    // Re-measured because redistance and compact both changed the samples
    // since sample() measured them. Declaring anything smaller than this would
    // be the overstep the bound exists to prevent, so it is measured rather
    // than reasoned about at every step that could have moved it.
    volume.set_sample_lipschitz(volume.measure_sample_lipschitz());

    if (out_cost) fill_cost(volume, *out_cost);
    return volume;
}

bool consolidate_layer(Document& doc, LayerId layer_id, const ConsolidationParams& params,
                       UndoStack* undo, ConsolidationCost* out_cost) {
    const Layer* layer = doc.find_layer(layer_id);
    if (!layer || layer->kind != LayerKind::Sdf || !layer->sdf) return false;
    // Checked before the bake, not after: a locked layer should not cost a
    // full resampling to say no. apply() refuses it again on the way through,
    // which is what keeps protection a property of the command rather than of
    // this entry point.
    if (layer->protected_from_edits()) return false;

    // Hidden roots are left alone. They contribute nothing to the field, so
    // absorbing them would discard their parameters in exchange for nothing —
    // and the artist hid them precisely to come back to them.
    std::vector<NodeId> absorb;
    for (NodeId id : layer->sdf->roots) {
        const Node* n = layer->sdf->find(id);
        if (n && n->visible) absorb.push_back(id);
    }
    if (absorb.empty()) return false;

    std::optional<field::FieldVolume> volume = bake_layer(*layer, params, out_cost);
    if (!volume) return false;

    Node baked;
    baked.id = layer->sdf->reserve_id();
    baked.prim = Prim::volume();
    // The bake sampled the layer's field, mirror copies included, so the
    // volume already holds both sides. Re-mirroring it is idempotent for the
    // union but doubles what every later evaluation pays.
    baked.mirror = false;
    baked.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
    // One colour for what may have been many. A volume carries a single
    // colour, so the first absorbed item's is the one that survives — stated
    // here because it is a loss, not a detail.
    if (const Node* first = layer->sdf->find(absorb.front())) baked.color = first->color;

    int index = -1;
    NodeId parent = kNoNode;
    layer->sdf->locate(absorb.front(), &parent, &index);

    auto run = [&doc, undo](const Command& cmd) {
        if (undo) return undo->perform(doc, cmd);
        return scene::apply(doc, cmd).has_value();
    };

    if (undo) undo->begin_group();
    // Removed last-first so that the recorded inverses, replayed in reverse on
    // undo, reinsert at ascending indices — which is what puts the edit list
    // back in its original order rather than reversed.
    for (auto it = absorb.rbegin(); it != absorb.rend(); ++it)
        run(Command{RemoveNodeCmd{layer_id, *it}});
    const bool added =
        run(Command{AddNodeCmd{layer_id, parent, index, std::vector<Node>{std::move(baked)}}});
    if (undo) undo->end_group();
    return added;
}

bool consolidation_state(const Layer& layer, ConsolidationCost* out_cost) {
    if (!layer.sdf || layer.sdf->roots.size() != 1) return false;
    const Node* n = layer.sdf->find(layer.sdf->roots.front());
    if (!n || !is_volume_item(*n)) return false;
    if (out_cost) fill_cost(*n->volume, *out_cost);
    return true;
}

}  // namespace scene
}  // namespace clay
