#include "clay/scene/armature.h"
#include "clay/scene/commands.h"

#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace clay {
namespace scene {

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

namespace {

SdfContent* content_of(Document& doc, LayerId id) {
    Layer* l = doc.find_layer(id);
    return (l && l->sdf) ? l->sdf.get() : nullptr;
}

std::optional<Command> apply_one(Document& doc, const AddNodeCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    if (!content || c.subtree.empty()) return std::nullopt;
    if (!content->reinsert(c.subtree, c.parent, c.index)) return std::nullopt;
    return Command{RemoveNodeCmd{c.layer, c.subtree.front().id}};
}

std::optional<Command> apply_one(Document& doc, const RemoveNodeCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    if (!content) return std::nullopt;
    NodeId parent = kNoNode;
    int index = -1;
    if (!content->locate(c.node, &parent, &index)) return std::nullopt;
    std::vector<Node> subtree = content->remove(c.node);
    if (subtree.empty()) return std::nullopt;
    return Command{AddNodeCmd{c.layer, parent, index, std::move(subtree)}};
}

std::optional<Command> apply_one(Document& doc, const MoveNodeCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    if (!content) return std::nullopt;
    NodeId parent = kNoNode;
    int index = -1;
    if (!content->locate(c.node, &parent, &index)) return std::nullopt;
    if (!content->move(c.node, c.new_parent, c.new_index)) return std::nullopt;
    return Command{MoveNodeCmd{c.layer, c.node, parent, index}};
}

template <typename Cmd, typename Get, typename Set>
std::optional<Command> apply_field(Document& doc, const Cmd& c, Get get, Set set) {
    SdfContent* content = content_of(doc, c.layer);
    Node* n = content ? content->find_mut(c.node) : nullptr;
    if (!n) return std::nullopt;
    Cmd inverse = c;
    get(inverse, *n);
    set(*n, c);
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetTransformCmd& c) {
    return apply_field(
        doc, c, [](SetTransformCmd& inv, const Node& n) { inv.xform = n.xform; },
        [](Node& n, const SetTransformCmd& cc) { n.xform = cc.xform; });
}

std::optional<Command> apply_one(Document& doc, const SetPrimCmd& c) {
    return apply_field(
        doc, c, [](SetPrimCmd& inv, const Node& n) { inv.prim = n.prim; },
        [](Node& n, const SetPrimCmd& cc) { n.prim = cc.prim; });
}

std::optional<Command> apply_one(Document& doc, const SetColorCmd& c) {
    return apply_field(
        doc, c, [](SetColorCmd& inv, const Node& n) { inv.color = n.color; },
        [](Node& n, const SetColorCmd& cc) { n.color = cc.color; });
}

std::optional<Command> apply_one(Document& doc, const SetOpBlendCmd& c) {
    return apply_field(
        doc, c,
        [](SetOpBlendCmd& inv, const Node& n) {
            inv.op = n.op;
            inv.blend = n.blend;
            inv.rounding = n.rounding;
        },
        [](Node& n, const SetOpBlendCmd& cc) {
            n.op = cc.op;
            n.blend = cc.blend;
            n.rounding = cc.rounding;
        });
}

std::optional<Command> apply_one(Document& doc, const AppendStrokeCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    Node* n = content ? content->find_mut(c.node) : nullptr;
    if (!n || n->prim.type != PrimType::Stroke) return std::nullopt;
    n->stroke.insert(n->stroke.end(), c.points.begin(), c.points.end());
    return Command{TrimStrokeCmd{c.layer, c.node, static_cast<std::uint32_t>(c.points.size())}};
}

std::optional<Command> apply_one(Document& doc, const TrimStrokeCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    Node* n = content ? content->find_mut(c.node) : nullptr;
    if (!n || n->stroke.size() < c.count) return std::nullopt;
    AppendStrokeCmd inverse{c.layer, c.node,
                            {n->stroke.end() - c.count, n->stroke.end()}};
    n->stroke.resize(n->stroke.size() - c.count);
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetStrokePointsCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    Node* n = content ? content->find_mut(c.node) : nullptr;
    // A swept guide is the same control-point list, so it is the same edit. It
    // was Stroke-only, which left a placed sweep's guide unreachable: the only
    // way to change it was to rebuild the item and replace the node.
    if (!n || !prim_carries_curve(n->prim.type)) return std::nullopt;
    SetStrokePointsCmd inverse{c.layer, c.node, n->stroke, n->stroke_closed, n->curve_tolerance};
    n->stroke = c.points;
    n->stroke_closed = c.closed;
    n->curve_tolerance = c.tolerance;
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetArmatureCmd& c) {
    SdfContent* content = content_of(doc, c.layer);
    Node* n = content ? content->find_mut(c.node) : nullptr;
    if (!n || !prim_is_armature(n->prim.type)) return std::nullopt;
    // A tree whose parents do not form a forest is refused rather than stored:
    // a cycle makes the field depend on the order links are walked instead of
    // on the tree, and the undo stack would then carry something unrenderable.
    if (!armature_is_valid(c.nodes, c.parents)) return std::nullopt;
    SetArmatureCmd inverse{c.layer, c.node, n->stroke, n->armature_parents, n->stroke_blend_k};
    n->stroke = c.nodes;
    n->armature_parents = c.parents;
    n->stroke_blend_k = c.blend_k;
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetDeformersCmd& c) {
    return apply_field(
        doc, c, [](SetDeformersCmd& inv, const Node& n) { inv.deformers = n.deformers; },
        [](Node& n, const SetDeformersCmd& cc) { n.deformers = cc.deformers; });
}

std::optional<Command> apply_one(Document& doc, const AddLayerCmd& c) {
    doc.insert_layer(c.layer, c.index);
    return Command{RemoveLayerCmd{c.layer.id}};
}

std::optional<Command> apply_one(Document& doc, const RemoveLayerCmd& c) {
    Layer removed;
    int index;
    if (!doc.remove_layer(c.id, &removed, &index)) return std::nullopt;
    return Command{AddLayerCmd{std::move(removed), index}};
}

std::optional<Command> apply_one(Document& doc, const SetLayerVisibleCmd& c) {
    Layer* l = doc.find_layer(c.id);
    if (!l) return std::nullopt;
    SetLayerVisibleCmd inverse{c.id, l->visible};
    l->visible = c.visible;
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetLayerProtectionCmd& c) {
    Layer* l = doc.find_layer(c.id);
    if (!l) return std::nullopt;
    SetLayerProtectionCmd inverse{c.id, l->ghost, l->locked};
    l->ghost = c.ghost;
    l->locked = c.locked;
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetLayerTransformCmd& c) {
    Layer* l = doc.find_layer(c.id);
    if (!l) return std::nullopt;
    SetLayerTransformCmd inverse{c.id, l->xform};
    l->xform = c.xform;
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetLayerNameCmd& c) {
    Layer* l = doc.find_layer(c.id);
    if (!l) return std::nullopt;
    SetLayerNameCmd inverse{c.id, l->name};
    l->name = c.name;
    return Command{inverse};
}

std::optional<Command> apply_one(Document& doc, const SetLayerMirrorCmd& c) {
    Layer* l = doc.find_layer(c.id);
    if (!l) return std::nullopt;
    SetLayerMirrorCmd inverse{c.id, l->mirror_axes, l->mirror_k};
    l->mirror_axes = c.axes;
    l->mirror_k = c.k;
    return Command{inverse};
}

}  // namespace

namespace {
// Adding a layer creates its target, and changing protection is how a
// protected layer is released — locking would otherwise be irreversible.
constexpr LayerId kNoLayer = 0;
}  // namespace

LayerId edited_layer(const Command& cmd) {
    return std::visit(
        [](const auto& c) -> LayerId {
            using C = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<C, AddLayerCmd> ||
                          std::is_same_v<C, SetLayerProtectionCmd>)
                return kNoLayer;
            else if constexpr (std::is_same_v<C, RemoveLayerCmd> ||
                               std::is_same_v<C, SetLayerVisibleCmd> ||
                               std::is_same_v<C, SetLayerTransformCmd> ||
                               std::is_same_v<C, SetLayerMirrorCmd> ||
                               std::is_same_v<C, SetLayerNameCmd>)
                return c.id;
            else
                return c.layer;
        },
        cmd);
}

std::optional<Command> apply(Document& doc, const Command& cmd) {
    // Refused, not silently dropped and not silently applied: a host that
    // greys a protected layer out wants to know its edit was rejected, and one
    // that does not must not quietly discard the artist's work. nullopt is
    // this vocabulary's "no", and it already means the document is unchanged.
    LayerId target = edited_layer(cmd);
    if (target != kNoLayer) {
        const Layer* l = doc.find_layer(target);
        if (l && l->protected_from_edits()) return std::nullopt;
    }
    return std::visit([&](const auto& c) { return apply_one(doc, c); }, cmd);
}

// ---------------------------------------------------------------------------
// serialization
// ---------------------------------------------------------------------------

namespace {

struct Writer {
    std::vector<std::uint8_t> out;
    // The layout version being written. Mirrors Reader::minor so the two
    // gates sit side by side and cannot drift apart.
    std::uint16_t minor = kSceneMinor;
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <typename T>
    void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof(T));
    }
    void u32(std::uint32_t v) { pod(v); }
};

struct Reader {
    const std::uint8_t* p;
    std::size_t remaining;
    bool ok = true;
    bool bytes(void* dst, std::size_t n) {
        if (!ok || remaining < n) return ok = false;
        std::memcpy(dst, p, n);
        p += n;
        remaining -= n;
        return true;
    }
    template <typename T>
    T pod() {
        T v{};
        bytes(&v, sizeof(T));
        return v;
    }
    std::uint32_t u32() { return pod<std::uint32_t>(); }

    // The layout version this stream was written at. Carried on the reader
    // rather than threaded through read_layer/read_content/read_node, which
    // would put a parameter on five signatures to be forwarded unchanged.
    std::uint16_t minor = kSceneMinor;
};

// StrokePoint carries a uint8 among its floats, so it goes field-wise for the
// same reason Prim and Blend do: struct-wise writes put indeterminate padding
// on the wire, which bit a portability check on GCC once already.
constexpr std::size_t kStrokePointBytes =
    sizeof(kernel::cfloat3) * 3 + sizeof(float) + sizeof(StrokePointType);

// Prim and Blend contain padding (uint8 + float) — serialize field-wise so
// indeterminate padding bytes never reach the stream.
void write_point(Writer& w, const StrokePoint& p) {
    w.pod(p.pos);
    w.pod(p.radius);
    w.pod(p.type);
    w.pod(p.in_handle);
    w.pod(p.out_handle);
}

StrokePoint read_point(Reader& r) {
    StrokePoint p;
    p.pos = r.pod<kernel::cfloat3>();
    p.radius = r.pod<float>();
    p.type = r.pod<StrokePointType>();
    p.in_handle = r.pod<kernel::cfloat3>();
    p.out_handle = r.pod<kernel::cfloat3>();
    return p;
}

// A node's sampled volume, minor 4 and above. Its own function for the same
// reason read_point and read_prim are: read_node is a long flat walk over the
// record, and a nested multi-branch block in the middle of it is where that
// walk stops being readable.
void read_volume(Reader& r, Node& n) {
    std::uint32_t bytes = r.u32();
    if (bytes > r.remaining) {
        r.ok = false;
        return;
    }
    if (bytes == 0) return;  // a node without one wrote a zero length
    std::vector<std::uint8_t> raw(bytes);
    if (!r.bytes(raw.data(), raw.size())) return;
    std::optional<field::FieldVolume> v = field::FieldVolume::deserialize(raw.data(), raw.size());
    // A malformed volume fails the read rather than loading an item that would
    // silently contribute nothing — a document that had quietly lost a shape is
    // the harder thing to notice.
    if (!v) {
        r.ok = false;
        return;
    }
    n.volume = std::make_shared<field::FieldVolume>(std::move(*v));
}

void write_prim(Writer& w, const Prim& p) {
    w.pod(p.type);
    for (float f : p.params) w.pod(f);
}

Prim read_prim(Reader& r) {
    Prim p;
    p.type = r.pod<PrimType>();
    for (float& f : p.params) f = r.pod<float>();
    return p;
}

void write_blend(Writer& w, const Blend& b) {
    w.pod(b.profile);
    w.pod(b.k);
}

Blend read_blend(Reader& r) {
    Blend b;
    b.profile = r.pod<BlendProfile>();
    b.k = r.pod<float>();
    return b;
}

// Node has std::vectors — serialize field-wise.
// One codec for a deformer chain, used by a node and by the command that
// replaces one. Two copies of this would be two things to keep in step.
void write_deformers(Writer& w, const std::vector<Deformer>& deformers) {
    w.u32(static_cast<std::uint32_t>(deformers.size()));
    for (const Deformer& d : deformers) {
        w.pod(d.type);
        w.pod(d.k);
        w.pod(d.a);
        w.pod(d.b);
        w.pod(d.c);
        w.pod(d.ease);
        // The type is already on the wire, so the reader knows how many
        // extension floats follow. Old files carry only old types and decode
        // exactly as before.
        for (int e = 0; e < Deformer::ext_count(d.type); ++e) w.pod(d.ext[e]);
    }
}

void write_node(Writer& w, const Node& n) {
    w.pod(n.id);
    w.pod(n.is_group);
    w.pod(n.visible);
    w.pod(n.op);
    write_blend(w, n.blend);
    write_prim(w, n.prim);
    w.pod(n.xform);
    w.pod(n.rounding);
    w.pod(n.color);
    w.pod(n.mirror);
    w.pod(n.stroke_blend_k);
    w.u32(static_cast<std::uint32_t>(n.stroke.size()));
    // Field-wise: StrokePoint gained a uint8 among its floats, so a struct-wise
    // write would put indeterminate padding on the wire.
    for (const StrokePoint& sp : n.stroke)
        if (w.minor >= 2) {
            write_point(w, sp);
        } else {
            w.pod(sp.pos);
            w.pod(sp.radius);
        }
    if (w.minor >= 2) {
        w.pod(n.stroke_closed);
        w.pod(n.curve_tolerance);
    }
    // An armature's topology: one parent index per stroke point. Written only
    // from minor 7, so serializing AT an older minor still produces exactly the
    // bytes that minor always did — which is how a document is made readable by
    // a build that predates armatures.
    //
    // Note that the count word goes out for every node at minor 7, armature or
    // not: a no-armature document at 7 is four bytes per node longer than the
    // same document at 6, not identical to it. Node records are not individually
    // framed, so a build that predates 7 reading a 7 document desynchronises —
    // it fails the reader's bounds and count checks rather than misreading, but
    // it does not open the file. The format notes in io/clayspace.h say so.
    if (w.minor >= 7) {
        w.u32(static_cast<std::uint32_t>(n.armature_parents.size()));
        for (std::uint32_t parent : n.armature_parents) w.u32(parent);
    }
    // Deformer carries uint8 + floats: field-wise, so padding never reaches
    // the stream (the portability trap struct-wise writes hit on GCC).
    w.pod(n.repeat.type);
    w.pod(n.repeat.spacing);
    w.pod(n.repeat.counts);
    w.pod(n.profile.type);
    for (float f : n.profile.params) w.pod(f);
    w.u32(static_cast<std::uint32_t>(n.profile_points.size()));
    for (const kernel::cfloat2& v : n.profile_points) w.pod(v);
    // Loft's profile list, written and read only at minor 3 and above. Where
    // it sits in the record does not matter precisely because both sides are
    // told the version — which is what versioning the scene chunk bought.
    if (w.minor >= 3) {
        w.u32(static_cast<std::uint32_t>(n.profiles.size()));
        for (std::size_t i = 0; i < n.profiles.size(); ++i) {
            w.pod(n.profiles[i].type);
            for (float f : n.profiles[i].params) w.pod(f);
            const std::vector<kernel::cfloat2>& pts =
                i < n.profile_polygons.size() ? n.profile_polygons[i]
                                              : std::vector<kernel::cfloat2>{};
            w.u32(static_cast<std::uint32_t>(pts.size()));
            for (const kernel::cfloat2& v : pts) w.pod(v);
        }
    }
    // A sampled volume, minor 4 and above. It goes on the wire as the same
    // flat float block the tape's blob carries, so there is one layout to keep
    // right rather than two. A node without one writes a zero length.
    if (w.minor >= 4) {
        std::vector<std::uint8_t> volume_bytes;
        if (n.volume && !n.volume->empty()) volume_bytes = n.volume->serialize();
        w.u32(static_cast<std::uint32_t>(volume_bytes.size()));
        w.bytes(volume_bytes.data(), volume_bytes.size());
    }
    w.pod(n.transition.a);
    w.pod(n.transition.b);
    w.pod(n.transition.r0);
    w.pod(n.transition.r1);
    w.pod(n.transition.ease);
    write_deformers(w, n.deformers);
    w.u32(static_cast<std::uint32_t>(n.children.size()));
    for (NodeId c : n.children) w.pod(c);
}

std::vector<Deformer> read_deformers(Reader& r) {
    std::vector<Deformer> out;
    std::uint32_t dc = r.u32();
    if (dc > r.remaining / 6) {  // smallest possible encoded deformer
        r.ok = false;
        return out;
    }
    for (std::uint32_t i = 0; i < dc && r.ok; ++i) {
        Deformer d;
        d.type = r.pod<std::uint8_t>();
        d.k = r.pod<float>();
        d.a = r.pod<float>();
        d.b = r.pod<float>();
        d.c = r.pod<float>();
        d.ease = r.pod<std::uint8_t>();
        for (int e = 0; e < Deformer::ext_count(d.type); ++e) d.ext[e] = r.pod<float>();
        out.push_back(d);
    }
    return out;
}

Node read_node(Reader& r) {
    Node n;
    n.id = r.pod<NodeId>();
    n.is_group = r.pod<bool>();
    n.visible = r.pod<bool>();
    n.op = r.pod<Op>();
    n.blend = read_blend(r);
    n.prim = read_prim(r);
    n.xform = r.pod<math::Transform>();
    n.rounding = r.pod<float>();
    n.color = r.pod<kernel::cfloat3>();
    n.mirror = r.pod<bool>();
    n.stroke_blend_k = r.pod<float>();
    std::uint32_t sc = r.u32();
    // Minor 0 and 1 wrote position + radius only. Reading them as hard corners
    // with no handles is not a fallback: it is what those points already
    // meant, so an older document's field is unchanged.
    const std::size_t point_bytes =
        r.minor >= 2 ? kStrokePointBytes : (sizeof(kernel::cfloat3) + sizeof(float));
    if (sc > r.remaining / point_bytes) {
        r.ok = false;
        return n;
    }
    n.stroke.reserve(sc);
    for (std::uint32_t i = 0; i < sc && r.ok; ++i) {
        if (r.minor >= 2) {
            n.stroke.push_back(read_point(r));
        } else {
            StrokePoint sp;
            sp.pos = r.pod<kernel::cfloat3>();
            sp.radius = r.pod<float>();
            n.stroke.push_back(sp);
        }
    }
    if (r.minor >= 2) {
        n.stroke_closed = r.pod<bool>();
        n.curve_tolerance = r.pod<float>();
    }
    if (r.minor >= 7) {
        std::uint32_t ac = r.u32();
        if (ac > r.remaining / sizeof(std::uint32_t)) {
            r.ok = false;
            return n;
        }
        n.armature_parents.reserve(ac);
        for (std::uint32_t i = 0; i < ac && r.ok; ++i) n.armature_parents.push_back(r.u32());
    }
    n.repeat.type = r.pod<std::uint8_t>();
    n.repeat.spacing = r.pod<kernel::cfloat3>();
    n.repeat.counts = r.pod<kernel::cfloat3>();
    n.profile.type = r.pod<std::uint8_t>();
    for (float& f : n.profile.params) f = r.pod<float>();
    std::uint32_t pc = r.u32();
    if (pc > r.remaining / sizeof(kernel::cfloat2)) {
        r.ok = false;
        return n;
    }
    for (std::uint32_t i = 0; i < pc && r.ok; ++i)
        n.profile_points.push_back(r.pod<kernel::cfloat2>());
    if (r.minor >= 3) {
        std::uint32_t profile_count = r.u32();
        if (profile_count > r.remaining / (sizeof(std::uint8_t) + 4 * sizeof(float))) {
            r.ok = false;
            return n;
        }
        for (std::uint32_t i = 0; i < profile_count && r.ok; ++i) {
            Profile profile;
            profile.type = r.pod<std::uint8_t>();
            for (float& f : profile.params) f = r.pod<float>();
            n.profiles.push_back(profile);
            std::uint32_t vc = r.u32();
            if (vc > r.remaining / sizeof(kernel::cfloat2)) {
                r.ok = false;
                return n;
            }
            std::vector<kernel::cfloat2> pts;
            pts.reserve(vc);
            for (std::uint32_t v = 0; v < vc && r.ok; ++v)
                pts.push_back(r.pod<kernel::cfloat2>());
            n.profile_polygons.push_back(std::move(pts));
        }
    }
    if (r.minor >= 4) read_volume(r, n);
    n.transition.a = r.pod<kernel::cfloat3>();
    n.transition.b = r.pod<kernel::cfloat3>();
    n.transition.r0 = r.pod<float>();
    n.transition.r1 = r.pod<float>();
    n.transition.ease = r.pod<std::uint8_t>();
    n.deformers = read_deformers(r);
    std::uint32_t cc = r.u32();
    if (cc > r.remaining / sizeof(NodeId)) {
        r.ok = false;
        return n;
    }
    for (std::uint32_t i = 0; i < cc && r.ok; ++i) n.children.push_back(r.pod<NodeId>());
    return n;
}

void write_content(Writer& w, const SdfContent& content) {
    w.u32(static_cast<std::uint32_t>(content.roots.size()));
    for (NodeId id : content.roots) w.pod(id);
    // deterministic order: preorder from roots
    std::vector<Node> flat;
    for (NodeId id : content.roots) {
        std::vector<NodeId> stack{id};
        while (!stack.empty()) {
            NodeId cur = stack.back();
            stack.pop_back();
            const Node* n = content.find(cur);
            if (!n) continue;
            flat.push_back(*n);
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(*it);
        }
    }
    w.u32(static_cast<std::uint32_t>(flat.size()));
    for (const Node& n : flat) write_node(w, n);
}

std::shared_ptr<SdfContent> read_content(Reader& r) {
    auto content = std::make_shared<SdfContent>();
    std::uint32_t rc = r.u32();
    std::vector<NodeId> roots;
    for (std::uint32_t i = 0; i < rc && r.ok; ++i) roots.push_back(r.pod<NodeId>());
    std::uint32_t nc = r.u32();
    std::vector<Node> flat;
    for (std::uint32_t i = 0; i < nc && r.ok; ++i) flat.push_back(read_node(r));
    if (!r.ok) return content;
    if (!flat.empty()) content->reinsert(flat, kNoNode, -1);
    content->roots = roots;
    // The wholesale assignment above bypasses the location index that
    // insert/remove/move maintain; rebuild it so the first undo after a load
    // does not pay a full-arena walk per root.
    content->reindex_roots();
    return content;
}

void write_layer(Writer& w, const Layer& l) {
    w.pod(l.id);
    w.u32(static_cast<std::uint32_t>(l.name.size()));
    w.bytes(l.name.data(), l.name.size());
    w.pod(l.kind);
    w.pod(l.xform);
    // Packed into the byte the visibility flag already occupied, rather than
    // appended: an older document has 0 or 1 there, so it loads with both
    // protection flags off with no version handling at all. The cost is the
    // other direction — a pre-0.14 build reading a new file sees no
    // protection, and a layer that is both hidden AND ghosted reads as
    // visible there. That is why kClaySpaceMinor moves with this.
    w.pod(static_cast<std::uint8_t>((l.visible ? 1u : 0u) | (l.ghost ? 2u : 0u) |
                                    (l.locked ? 4u : 0u)));
    w.pod(l.resolution);
    w.pod(l.mirror_axes);
    w.pod(l.mirror_k);
    bool has_sdf = l.sdf != nullptr;
    w.pod(has_sdf);
    if (has_sdf) write_content(w, *l.sdf);
}

Layer read_layer(Reader& r) {
    Layer l;
    l.id = r.pod<LayerId>();
    std::uint32_t ns = r.u32();
    if (ns > r.remaining) {
        r.ok = false;
        return l;
    }
    l.name.resize(ns);
    r.bytes(l.name.data(), ns);
    l.kind = r.pod<LayerKind>();
    l.xform = r.pod<math::Transform>();
    std::uint8_t flags = r.pod<std::uint8_t>();
    l.visible = (flags & 1u) != 0;
    l.ghost = (flags & 2u) != 0;
    l.locked = (flags & 4u) != 0;
    l.resolution = r.pod<int>();
    l.mirror_axes = r.pod<std::uint8_t>();
    l.mirror_k = r.pod<float>();
    if (r.pod<bool>()) l.sdf = read_content(r);
    return l;
}

enum class Tag : std::uint8_t {
    AddNode = 1,
    RemoveNode,
    MoveNode,
    SetTransform,
    SetPrim,
    SetColor,
    SetOpBlend,
    AppendStroke,
    TrimStroke,
    AddLayer,
    RemoveLayer,
    SetLayerVisible,
    SetLayerTransform,
    SetLayerProtection,
    SetStrokePoints,
    SetDeformers,
    SetLayerMirror,
    SetArmature,
    SetLayerName,
};

struct SerializeVisitor {
    Writer& w;
    void operator()(const AddNodeCmd& c) {
        w.pod(Tag::AddNode);
        w.pod(c.layer);
        w.pod(c.parent);
        w.pod(c.index);
        w.u32(static_cast<std::uint32_t>(c.subtree.size()));
        for (const Node& n : c.subtree) write_node(w, n);
    }
    void operator()(const RemoveNodeCmd& c) {
        w.pod(Tag::RemoveNode);
        w.pod(c.layer);
        w.pod(c.node);
    }
    void operator()(const MoveNodeCmd& c) {
        w.pod(Tag::MoveNode);
        w.pod(c.layer);
        w.pod(c.node);
        w.pod(c.new_parent);
        w.pod(c.new_index);
    }
    void operator()(const SetTransformCmd& c) {
        w.pod(Tag::SetTransform);
        w.pod(c.layer);
        w.pod(c.node);
        w.pod(c.xform);
    }
    void operator()(const SetPrimCmd& c) {
        w.pod(Tag::SetPrim);
        w.pod(c.layer);
        w.pod(c.node);
        write_prim(w, c.prim);
    }
    void operator()(const SetColorCmd& c) {
        w.pod(Tag::SetColor);
        w.pod(c.layer);
        w.pod(c.node);
        w.pod(c.color);
    }
    void operator()(const SetOpBlendCmd& c) {
        w.pod(Tag::SetOpBlend);
        w.pod(c.layer);
        w.pod(c.node);
        w.pod(c.op);
        write_blend(w, c.blend);
        w.pod(c.rounding);
    }
    void operator()(const AppendStrokeCmd& c) {
        w.pod(Tag::AppendStroke);
        w.pod(c.layer);
        w.pod(c.node);
        w.u32(static_cast<std::uint32_t>(c.points.size()));
        for (const StrokePoint& p : c.points) write_point(w, p);
    }
    void operator()(const SetStrokePointsCmd& c) {
        w.pod(Tag::SetStrokePoints);
        w.pod(c.layer);
        w.pod(c.node);
        w.u32(static_cast<std::uint32_t>(c.points.size()));
        for (const StrokePoint& p : c.points) write_point(w, p);
        w.pod(c.closed);
        w.pod(c.tolerance);
    }
    void operator()(const SetArmatureCmd& c) {
        w.pod(Tag::SetArmature);
        w.pod(c.layer);
        w.pod(c.node);
        w.u32(static_cast<std::uint32_t>(c.nodes.size()));
        for (const StrokePoint& p : c.nodes) write_point(w, p);
        w.u32(static_cast<std::uint32_t>(c.parents.size()));
        for (std::uint32_t parent : c.parents) w.u32(parent);
        w.pod(c.blend_k);
    }
    void operator()(const SetDeformersCmd& c) {
        w.pod(Tag::SetDeformers);
        w.pod(c.layer);
        w.pod(c.node);
        write_deformers(w, c.deformers);
    }
    void operator()(const TrimStrokeCmd& c) {
        w.pod(Tag::TrimStroke);
        w.pod(c.layer);
        w.pod(c.node);
        w.pod(c.count);
    }
    void operator()(const AddLayerCmd& c) {
        w.pod(Tag::AddLayer);
        w.pod(c.index);
        write_layer(w, c.layer);
    }
    void operator()(const RemoveLayerCmd& c) {
        w.pod(Tag::RemoveLayer);
        w.pod(c.id);
    }
    void operator()(const SetLayerVisibleCmd& c) {
        w.pod(Tag::SetLayerVisible);
        w.pod(c.id);
        w.pod(c.visible);
    }
    void operator()(const SetLayerProtectionCmd& c) {
        w.pod(Tag::SetLayerProtection);
        w.pod(c.id);
        w.pod(c.ghost);
        w.pod(c.locked);
    }
    void operator()(const SetLayerTransformCmd& c) {
        w.pod(Tag::SetLayerTransform);
        w.pod(c.id);
        w.pod(c.xform);
    }
    void operator()(const SetLayerMirrorCmd& c) {
        w.pod(Tag::SetLayerMirror);
        w.pod(c.id);
        w.pod(c.axes);
        w.pod(c.k);
    }
    void operator()(const SetLayerNameCmd& c) {
        w.pod(Tag::SetLayerName);
        w.pod(c.id);
        w.u32(static_cast<std::uint32_t>(c.name.size()));
        w.bytes(c.name.data(), c.name.size());
    }
};

}  // namespace

std::vector<std::uint8_t> serialize(const Command& cmd) {
    Writer w;
    std::visit(SerializeVisitor{w}, cmd);
    return std::move(w.out);
}

std::optional<Command> deserialize(const std::uint8_t* data, std::size_t size) {
    Reader r{data, size};
    Tag tag = r.pod<Tag>();
    Command cmd;
    switch (tag) {
        case Tag::AddNode: {
            AddNodeCmd c;
            c.layer = r.pod<LayerId>();
            c.parent = r.pod<NodeId>();
            c.index = r.pod<int>();
            std::uint32_t n = r.u32();
            for (std::uint32_t i = 0; i < n && r.ok; ++i) c.subtree.push_back(read_node(r));
            cmd = std::move(c);
            break;
        }
        case Tag::RemoveNode:
            cmd = RemoveNodeCmd{r.pod<LayerId>(), r.pod<NodeId>()};
            break;
        case Tag::MoveNode: {
            MoveNodeCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.new_parent = r.pod<NodeId>();
            c.new_index = r.pod<int>();
            cmd = c;
            break;
        }
        case Tag::SetTransform: {
            SetTransformCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.xform = r.pod<math::Transform>();
            cmd = c;
            break;
        }
        case Tag::SetPrim: {
            SetPrimCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.prim = read_prim(r);
            cmd = c;
            break;
        }
        case Tag::SetColor: {
            SetColorCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.color = r.pod<kernel::cfloat3>();
            cmd = c;
            break;
        }
        case Tag::SetOpBlend: {
            SetOpBlendCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.op = r.pod<Op>();
            c.blend = read_blend(r);
            c.rounding = r.pod<float>();
            cmd = c;
            break;
        }
        case Tag::AppendStroke: {
            AppendStrokeCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            std::uint32_t n = r.u32();
            if (n > r.remaining / kStrokePointBytes) return std::nullopt;
            for (std::uint32_t i = 0; i < n && r.ok; ++i) c.points.push_back(read_point(r));
            cmd = std::move(c);
            break;
        }
        case Tag::SetArmature: {
            SetArmatureCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            std::uint32_t n = r.u32();
            if (n > r.remaining / kStrokePointBytes) return std::nullopt;
            for (std::uint32_t i = 0; i < n && r.ok; ++i) c.nodes.push_back(read_point(r));
            std::uint32_t pn = r.u32();
            if (pn > r.remaining / sizeof(std::uint32_t)) return std::nullopt;
            for (std::uint32_t i = 0; i < pn && r.ok; ++i) c.parents.push_back(r.u32());
            c.blend_k = r.pod<float>();
            cmd = std::move(c);
            break;
        }
        case Tag::SetStrokePoints: {
            SetStrokePointsCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            std::uint32_t n = r.u32();
            if (n > r.remaining / kStrokePointBytes) return std::nullopt;
            for (std::uint32_t i = 0; i < n && r.ok; ++i) c.points.push_back(read_point(r));
            c.closed = r.pod<bool>();
            c.tolerance = r.pod<float>();
            cmd = std::move(c);
            break;
        }
        case Tag::SetDeformers: {
            SetDeformersCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.deformers = read_deformers(r);
            cmd = std::move(c);
            break;
        }
        case Tag::TrimStroke: {
            TrimStrokeCmd c;
            c.layer = r.pod<LayerId>();
            c.node = r.pod<NodeId>();
            c.count = r.pod<std::uint32_t>();
            cmd = c;
            break;
        }
        case Tag::AddLayer: {
            AddLayerCmd c;
            c.index = r.pod<int>();
            c.layer = read_layer(r);
            cmd = std::move(c);
            break;
        }
        case Tag::RemoveLayer:
            cmd = RemoveLayerCmd{r.pod<LayerId>()};
            break;
        case Tag::SetLayerVisible: {
            SetLayerVisibleCmd c;
            c.id = r.pod<LayerId>();
            c.visible = r.pod<bool>();
            cmd = c;
            break;
        }
        case Tag::SetLayerProtection: {
            SetLayerProtectionCmd c;
            c.id = r.pod<LayerId>();
            c.ghost = r.pod<bool>();
            c.locked = r.pod<bool>();
            cmd = c;
            break;
        }
        case Tag::SetLayerTransform: {
            SetLayerTransformCmd c;
            c.id = r.pod<LayerId>();
            c.xform = r.pod<math::Transform>();
            cmd = c;
            break;
        }
        case Tag::SetLayerMirror: {
            SetLayerMirrorCmd c;
            c.id = r.pod<LayerId>();
            c.axes = r.pod<std::uint8_t>();
            c.k = r.pod<float>();
            cmd = c;
            break;
        }
        case Tag::SetLayerName: {
            SetLayerNameCmd c;
            c.id = r.pod<LayerId>();
            std::uint32_t n = r.u32();
            // The length guard read_layer makes, for the same reason: a
            // truncated stream must not ask for a gigabyte-long name.
            if (!r.ok || n > r.remaining) return std::nullopt;
            c.name.resize(n);
            r.bytes(c.name.data(), n);
            cmd = std::move(c);
            break;
        }
        default:
            return std::nullopt;
    }
    if (!r.ok) return std::nullopt;
    return cmd;
}

std::vector<std::uint8_t> serialize_document(const Document& doc, std::uint16_t minor) {
    Writer w;
    w.minor = minor;
    w.u32(static_cast<std::uint32_t>(doc.layers.size()));
    for (const Layer& l : doc.layers) write_layer(w, l);
    return std::move(w.out);
}

std::optional<Document> deserialize_document(const std::uint8_t* data, std::size_t size,
                                             std::uint16_t minor) {
    Reader r{data, size};
    r.minor = minor;
    std::uint32_t count = r.u32();
    if (!r.ok || count > 100000) return std::nullopt;
    Document doc;
    for (std::uint32_t i = 0; i < count && r.ok; ++i) {
        Layer l = read_layer(r);
        if (r.ok) doc.insert_layer(std::move(l), -1);
    }
    if (!r.ok) return std::nullopt;
    return doc;
}

// ---------------------------------------------------------------------------
// undo stack
// ---------------------------------------------------------------------------

bool UndoStack::try_coalesce(Entry& top, const Command& cmd, const Command& inverse) {
    // AppendStroke + AppendStroke on the same node -> merge the Trim counts.
    const auto* append = std::get_if<AppendStrokeCmd>(&cmd);
    if (!append || top.inverses.size() != 1) return false;
    auto* trim = std::get_if<TrimStrokeCmd>(&top.inverses.back());
    const auto* new_trim = std::get_if<TrimStrokeCmd>(&inverse);
    if (!trim || !new_trim || trim->layer != append->layer || trim->node != append->node)
        return false;
    trim->count += new_trim->count;
    return true;
}

bool UndoStack::perform(Document& doc, const Command& cmd) {
    std::optional<Command> inverse = scene::apply(doc, cmd);
    if (!inverse) return false;
    redo_.clear();
    if (grouping_ && !undo_.empty()) {
        undo_.back().inverses.push_back(std::move(*inverse));
        return true;
    }
    if (!undo_.empty() && try_coalesce(undo_.back(), cmd, *inverse)) return true;
    undo_.push_back(Entry{{std::move(*inverse)}});
    return true;
}

bool UndoStack::undo(Document& doc) {
    if (undo_.empty()) return false;
    Entry entry = std::move(undo_.back());
    undo_.pop_back();
    Entry redo_entry;
    for (auto it = entry.inverses.rbegin(); it != entry.inverses.rend(); ++it) {
        std::optional<Command> original = scene::apply(doc, *it);
        if (original) redo_entry.inverses.push_back(std::move(*original));
    }
    redo_.push_back(std::move(redo_entry));
    return true;
}

bool UndoStack::redo(Document& doc) {
    if (redo_.empty()) return false;
    Entry entry = std::move(redo_.back());
    redo_.pop_back();
    Entry undo_entry;
    for (auto it = entry.inverses.rbegin(); it != entry.inverses.rend(); ++it) {
        std::optional<Command> inverse = scene::apply(doc, *it);
        if (inverse) undo_entry.inverses.push_back(std::move(*inverse));
    }
    undo_.push_back(std::move(undo_entry));
    return true;
}

void UndoStack::begin_group() {
    grouping_ = true;
    undo_.push_back(Entry{});
}

void UndoStack::end_group() {
    grouping_ = false;
    if (!undo_.empty() && undo_.back().inverses.empty()) undo_.pop_back();
}

}  // namespace scene
}  // namespace clay
