#pragma once

// Colored voxel engine (voxel-engine spec): palette-indexed dense chunks,
// editing ops (brushes, fills, mirror, flood select), build-plane queries,
// palette+RLE serialization, greedy meshing, and voxel<->SDF bridges.
//
// Voxel addressing is integer lattice cells; cell (x,y,z) occupies world
// space [x,x+1)*voxel_size. Palette index 0 means empty; indices 1..255
// reference the palette color table (editing the palette recolors every
// referencing voxel with no voxel-data touch).
//
// A grid holds a STACK OF RESOLUTION LEVELS. Level 0 is the coarsest and level
// k has half the cell size of level k-1, so every level is a uniform lattice
// with its own integer cell coordinates. That is the load-bearing property: the
// falloff dither hashes a cell coordinate, and an adaptive structure would
// change what a cell coordinate means. Levels keep the hash — and with it the
// cross-platform reproducibility the parity suite enforces — exactly as it is.
//
// One level is ACTIVE, and every method here that names cells acts on it: the
// level is grid state rather than a parameter, so a verb added later cannot
// forget to state one. A grid constructed and never given a second level has
// exactly one, and then every method, the serialised bytes and the meshed
// result are what they were before levels existed.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clay/math/geom.h"
#include "clay/field/volume.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace voxel {

inline constexpr int kChunkDim = 32;  // 32^3 voxels = 32 KiB per chunk

struct VoxelCoord {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const VoxelCoord&) const = default;
};

struct VoxelCoordHash {
    std::size_t operator()(const VoxelCoord& c) const {
        std::uint64_t h = static_cast<std::uint32_t>(c.x) * 0x9E3779B185EBCA87ull;
        h ^= static_cast<std::uint32_t>(c.y) * 0xC2B2AE3D27D4EB4Full + (h << 6);
        h ^= static_cast<std::uint32_t>(c.z) * 0x165667B19E3779F9ull + (h >> 3);
        return static_cast<std::size_t>(h);
    }
};

// What one chunk key contributed to a regional mesh: contiguous, and together
// with the other keys' ranges a PARTITION of the output — no vertex is shared
// between two keys, so a host may overwrite or drop one key's slice of a GPU
// buffer without consulting its neighbours'.
//
// That is the difference from mesh::BrickMeshRange, whose welding spans brick
// seams. A voxel face belongs to exactly one cell, which belongs to exactly
// one chunk, so there is nothing to weld and nothing to attribute across a
// seam.
struct VoxelChunkMeshRange {
    VoxelCoord key;
    std::uint32_t vertex_first = 0, vertex_count = 0;
    std::uint32_t index_first = 0, index_count = 0;
};

inline constexpr std::uint8_t kVoxMirrorX = 1;
inline constexpr std::uint8_t kVoxMirrorY = 2;
inline constexpr std::uint8_t kVoxMirrorZ = 4;

// Brush footprint. Sphere is the ball of diameter n, so it is always a subset
// of Cube for a given size, and its occupancy ratio approaches pi/6.
enum class BrushShape : std::uint8_t { Cube = 0, Sphere = 1 };

class MaskField;  // clay/voxel/mask.h

// Falloff curve over the normalized distance from the footprint centre.
// Constant is the hard-edged brush and stays the default.
enum class BrushFalloff : std::uint8_t {
    Constant = 0,
    Linear = 1,
    Smooth = 2,  // smoothstep
    Gaussian = 3,
};

// One brush stamp. Occupancy is binary, so a weight strictly between 0 and 1
// cannot be stored in a cell; it is resolved by dithering against a hash of
// the cell coordinate and `seed`. Weight 1 always applies and weight 0 never
// does, so the falloff shows up as fractional coverage across the footprint
// rather than as partial density in any one cell. The hash makes that
// reproducible: same stamp, same seed, same cells, on every platform.
struct BrushParams {
    int size = 3;
    BrushShape shape = BrushShape::Cube;
    BrushFalloff falloff = BrushFalloff::Constant;
    float strength = 1.0f;
    std::uint32_t seed = 0;
    // Optional gate. Where a mask is given, the effective weight at a cell is
    // scaled by (1 - mask), so a fully masked cell is untouched by every verb
    // below rather than by a hand-picked few. Borrowed, not owned: BrushParams
    // is a transient call-site struct.
    const MaskField* mask = nullptr;
};

class VoxelGrid {
  public:
    explicit VoxelGrid(float voxel_size = 0.1f) {
        levels_.emplace_back().voxel_size = voxel_size;
        palette_.resize(1, kernel::cf3(0, 0, 0));  // index 0 = empty, unused
    }

    // Cell size of the ACTIVE level.
    float voxel_size() const { return levels_[active_].voxel_size; }

    // -- resolution levels ---------------------------------------------------
    // Level 0 is the coarsest; level k has half the cell size of level k-1.
    std::size_t level_count() const { return levels_.size(); }
    std::size_t active_level() const { return active_; }
    float level_voxel_size(std::size_t level) const;
    // Occupied cells at one level, so a caller can report what each costs.
    std::size_t level_occupied_count(std::size_t level) const;
    // One cell of ONE level, without making it active. get() answers for the
    // active level; this is what a reader spanning levels needs — the smooth
    // mesher and the field conversion both take a level rather than assuming
    // the active one. 0 for a level this grid does not have.
    std::uint8_t cell_index(std::size_t level, VoxelCoord c) const;

    // Chooses which level the verbs act on. False (and no change) for a level
    // this grid does not have.
    bool set_active_level(std::size_t level);

    // Appends a level at half the finest cell size, seeded by subdividing every
    // occupied cell into its eight children. The solid the grid represents is
    // therefore unchanged, and returns the new level's index.
    //
    // The stack is capped, and at the cap this returns the finest level it
    // already had — so level_count() is what says whether the stack grew.
    static constexpr std::size_t kMaxLevels = 16;
    std::size_t add_level();
    // Drops the finest level, along with the detail only it held. False when
    // there is only one level, since a grid always has at least one.
    bool drop_level();

    // -- palette -------------------------------------------------------------
    // Returns the palette index for a color, adding it when new (up to 255).
    std::uint8_t palette_add(kernel::cfloat3 color, float tolerance = 1e-3f);
    kernel::cfloat3 palette_color(std::uint8_t index) const;
    void palette_set(std::uint8_t index, kernel::cfloat3 color);
    std::size_t palette_size() const { return palette_.size(); }

    // -- single-voxel ops ----------------------------------------------------
    std::uint8_t get(VoxelCoord c) const;
    void set(VoxelCoord c, std::uint8_t index);            // index 0 erases
    void erase(VoxelCoord c) { set(c, 0); }
    void paint(VoxelCoord c, std::uint8_t index);          // occupied cells only

    // -- brushes and fills ---------------------------------------------------
    // Footprint of size n centered on c, as a solid cube or as the sphere of
    // the same diameter. Size n spans exactly n cells per axis for every n:
    // the footprint runs -((n-1)/2) ..= n/2, symmetric for odd n and biased
    // half a cell toward +XYZ for even n. The sphere admits cells whose
    // centre is within radius n/2 of the footprint centre.
    void set_brush(VoxelCoord c, int n, std::uint8_t index,
                   BrushShape shape = BrushShape::Cube);
    void erase_brush(VoxelCoord c, int n, BrushShape shape = BrushShape::Cube) {
        set_brush(c, n, 0, shape);
    }
    void paint_brush(VoxelCoord c, int n, std::uint8_t index,
                     BrushShape shape = BrushShape::Cube);
    void fill_box(VoxelCoord a, VoxelCoord b, std::uint8_t index);  // inclusive corners
    void fill_line(VoxelCoord a, VoxelCoord b, std::uint8_t index); // 3D DDA

    // Falloff-weighted forms of the same three brushes.
    void set_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index);
    void erase_brush(VoxelCoord c, const BrushParams& p) { set_brush(c, p, 0); }
    void paint_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index);

    // -- sculpting verbs -----------------------------------------------------
    // These reshape existing material instead of stamping a footprint. Each
    // reads a snapshot of the region first, so a cell's outcome never depends
    // on a neighbour the same call already changed.

    // Majority filter over the 26-neighbourhood: spurs dissolve, notches fill.
    void sculpt_smooth(VoxelCoord c, const BrushParams& p);
    // amount > 0 dilates, < 0 erodes, |amount| times.
    void sculpt_inflate(VoxelCoord c, const BrushParams& p, int amount = 1);
    // Pull the surface onto the plane through the brush centre (offset in
    // cells along `normal`): material on the + side goes, hollows on the -
    // side that touch material are filled.
    void sculpt_flatten(VoxelCoord c, const BrushParams& p, kernel::cfloat3 normal,
                        float offset_cells = 0.0f);
    // Move surface cells one step toward the brush centre.
    void sculpt_pinch(VoxelCoord c, const BrushParams& p);
    // ...and one step away from it: the inverse, sharing pinch's walk so the
    // two cannot drift apart. The SDF side spells the pair as one signed
    // strength for the same reason.
    void sculpt_magnify(VoxelCoord c, const BrushParams& p);

    // Fill pockets inside the footprint: an empty cell with at least four of
    // its six face neighbours occupied is inside a cavity rather than beside a
    // surface, and `passes` iterations reach that many cells deep. A
    // through-hole and an open face have too few occupied neighbours to
    // qualify, which is what makes this "fill the pockets" rather than "fill
    // everything". This is the same rule repair_close_holes applies over a
    // whole grid.
    void sculpt_fill_cavities(VoxelCoord c, const BrushParams& p, int passes = 1);

    // Flatten onto the plane AND smooth, from ONE snapshot. Calling the two
    // verbs in sequence is not the same thing: the flatten's output would feed
    // the smooth's neighbourhood, which is exactly what the snapshot
    // discipline exists to prevent.
    void sculpt_scrape(VoxelCoord c, const BrushParams& p, kernel::cfloat3 normal,
                       float offset_cells = 0.0f);

    // Drag SURFACE material along a direction, leaving the interior where it
    // was. That is what separates it from grab, which translates every cell in
    // the region: grab moves a lump, smudge smears a skin.
    void sculpt_smudge(VoxelCoord c, const BrushParams& p, kernel::cfloat3 displacement);

    // A caller-supplied scalar stamp modulating the footprint's per-cell
    // strength. `alpha` is width*height samples in [0,1], row-major, sampled
    // by projecting each cell onto the plane perpendicular to `direction`.
    // The engine decodes no images: a host that has an alpha has already
    // loaded a PNG, and handing over the samples costs it nothing.
    //
    // index 0 carves (the usual use); a non-zero index deposits instead.
    // Returns false for a malformed stamp — mismatched dimensions or a zero
    // extent — rather than stamping something the caller did not describe.
    bool sculpt_carve_alpha(VoxelCoord c, const BrushParams& p, const float* alpha,
                            int alpha_width, int alpha_height, kernel::cfloat3 direction,
                            std::uint8_t index = 0);

    // -- repair --------------------------------------------------------------
    // What a pre-bake check wants to know, without performing the fix: a
    // destructive operation whose input is somebody's sculpt should be
    // askable before it is answerable.
    struct RepairReport {
        std::size_t enclosed_voids = 0;  // empty regions the outside cannot reach
        std::size_t void_cells = 0;      // their total size
        std::size_t largest_void = 0;
        bool airtight = false;           // no enclosed voids at all
    };
    RepairReport repair_report() const;

    // Seal perforations by the same pocket rule, over the whole grid: a
    // pierced wall's hole has four occupied face neighbours and fills, while a
    // wide opening has one or two and does not. Only ever adds cells, so no
    // material is lost.
    void repair_close_holes(int passes = 1, const MaskField* mask = nullptr);

    // Fill every empty cell the outside cannot reach, colouring from the shell
    // that encloses it. Reachability is over EMPTY cells by face adjacency,
    // seeded outside the occupied bounds — enclosure is decided rather than
    // guessed at from a local neighbourhood.
    void repair_fill_voids(const MaskField* mask = nullptr);

    // Grab: translate occupancy through the same inverse map the SDF grab
    // deformer uses, so both representations mean the same thing. Occupancy is
    // binary, so this resamples nearest-cell — a displacement larger than a
    // cell moves material in whole cells rather than flowing.
    void sculpt_grab(VoxelCoord c, const BrushParams& p, kernel::cfloat3 displacement,
                     bool front_only = false);

    // -- mirror --------------------------------------------------------------
    // Mirror plane passes through lattice coordinate 0 on each active axis:
    // cell x reflects to -1-x. Ops with `axes` apply the edit and every
    // mirror combination in one call.
    static VoxelCoord mirrored(VoxelCoord c, std::uint8_t axes);
    void set_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes);
    void paint_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes);

    // -- queries -------------------------------------------------------------
    std::size_t occupied_count() const;

    // Cell writes that actually changed a cell, since this grid was
    // constructed. Monotone; only the DIFFERENCE between two reads means
    // anything. Every verb writes through set(), so this is how a caller tells
    // "the edit did nothing" from "the edit did something" without diffing the
    // grid — which for a verb that conserves material is the only other way to
    // ask, since occupied_count() cannot see a lump move.
    std::uint64_t change_count() const { return change_count_; }
    // inclusive cell-index bounds of occupied voxels (nullopt when empty)
    std::optional<VoxelCoord> bounds_min() const;
    std::optional<VoxelCoord> bounds_max() const;

    // Cell a world-space ray enters at build-plane height y = plane_cell
    // (the cell whose bottom face lies on that lattice plane).
    std::optional<VoxelCoord> build_plane_pick(const math::Ray& ray, std::int32_t plane_cell) const;

    // 6-connected flood select over same palette index (same_color=true) or
    // any occupied voxel. Bounded by max_count.
    std::vector<VoxelCoord> flood_select(VoxelCoord seed, bool same_color,
                                         std::size_t max_count = 1 << 20) const;

    // -- serialization (palette + RLE, deterministic) ------------------------
    // The stream opens with the COARSEST level in exactly the layout it always
    // had, and any further level follows as a tail of per-cell offsets. A grid
    // with one level therefore serialises to the same bytes it always did, and
    // a reader that predates the tail stops after the coarsest level and opens
    // the grid there rather than failing.
    std::vector<std::uint8_t> serialize() const;
    static std::optional<VoxelGrid> deserialize(const std::uint8_t* data, std::size_t size);

    // -- greedy meshing ------------------------------------------------------
    // Merged quads per axis slice, palette color per face, no color bleed
    // across merged faces; emits two triangles per quad with face normals.
    // Meshes the ACTIVE level; the overload names one explicitly, and returns
    // an empty mesh for a level this grid does not have.
    mesh::Mesh mesh_greedy() const { return mesh_greedy(active_); }
    mesh::Mesh mesh_greedy(std::size_t level) const;

    // -- smooth meshing ------------------------------------------------------
    // The same occupancy as a rounded form rather than as boxes (#108).
    //
    // Surface nets over occupancy sampled at voxel CENTRES: one vertex per
    // surface cell, at the centroid of that cell's edge crossings. The
    // centroid is what smooths — a corner cell's vertex is pulled toward the
    // average of its crossings, so the corner rounds — and it is why nothing
    // has to be filtered first and why nothing can VANISH: a lone occupied
    // voxel has a sign change on each of its six edges and still gets a
    // surface.
    //
    // `blur` is optional extra smoothing, in passes of a 3x3x3 box over
    // occupancy, and is 0 by default deliberately. A blur is what erases thin
    // features: one pass puts an isolated voxel near 0.3 occupancy, below the
    // isolevel, and it is gone. A caller passing more than 0 is asking for
    // that trade.
    //
    // A PREVIEW mesh, not an export one. Per mesh/surface_nets.h a cell the
    // surface crosses twice gets one vertex and the sheets pinch, so this is
    // neither manifold nor watertight. mesh_greedy remains the export path and
    // is unchanged by this existing.
    struct SmoothOptions {
        int blur;
    };
    static constexpr SmoothOptions kSmoothDefault{0};
    mesh::Mesh mesh_smooth(SmoothOptions options = kSmoothDefault) const {
        return mesh_smooth(active_, options);
    }
    mesh::Mesh mesh_smooth(std::size_t level, SmoothOptions options = kSmoothDefault) const;

    // -- back into the document ----------------------------------------------
    // The sculpt as a FIELD, so it can be an operand again (#90).
    //
    // The bridge ran one way: SDF to voxel is rasterize_tape, and voxel back
    // was a detour through the mesher and mesh::to_field — two resamplings, a
    // BVH to do them, and the palette dropped. This is direct. Occupancy is
    // read by trilinear interpolation between cell CENTRES, which is what puts
    // the isosurface somewhere real rather than on a cell boundary, and then
    // field::redistance measures the distance to that surface so the result
    // carries a Lipschitz bound a marcher and a blend can trust.
    //
    // `index` restricts the conversion to ONE palette entry, which is how
    // colour survives a trip that a single field has nowhere to put it: a
    // caller converts once per index in use and places each result with that
    // entry's colour. The union of the parts is the whole solid, and the
    // interface between two colours is interior to that union.
    //
    // Lossy, and in both directions. Going to voxels quantised to the lattice
    // and no care here recovers it; coming back turns occupancy into a
    // distance, and the band decides how much of the field means anything.
    // What is preserved is the surface within about a cell, and the colour.
    // What is not is exactness and the procedural history.
    //
    // nullopt when the level does not exist, holds nothing, holds nothing of
    // `index`, or spans a box too large to sample.
    struct FieldOptions {
        int blur;            // extra occupancy smoothing; 0 keeps thin features
        float band;          // 0 = three cells
        std::uint8_t index;  // 0 = every occupied cell, whatever its colour
    };
    static constexpr FieldOptions kFieldDefault{0, 0.0f, 0};
    std::optional<field::FieldVolume> to_field(FieldOptions options = kFieldDefault) const {
        return to_field(active_, options);
    }
    std::optional<field::FieldVolume> to_field(std::size_t level,
                                               FieldOptions options = kFieldDefault) const;

    // -- incremental meshing -------------------------------------------------
    // The chunks holding material, so a caller can mesh a grid a chunk at a
    // time without draining the dirty set. Order is the map's own and is not
    // stable across a mutation, as BrickCache::surface_bricks is not.
    std::vector<VoxelCoord> occupied_chunk_keys() const {
        return occupied_chunk_keys(active_);
    }
    std::vector<VoxelCoord> occupied_chunk_keys(std::size_t level) const;

    // Mesh ONLY the named chunks, reporting per key what it contributed.
    //
    // The exposure test still reads the neighbour cell wherever it lives —
    // including in a chunk that was not named and in one that does not exist —
    // so the SURFACE is exactly the one mesh_greedy describes over the same
    // chunks. What changes is the merge: greedy quads are axis-aligned and
    // exact, and a face belongs to exactly one cell in exactly one chunk, so
    // clamping the merge to a chunk boundary emits MORE, SMALLER quads over
    // the identical surface. Never a crack, and no straddler to attribute the
    // way mesh_bricks must (#66).
    //
    // A key holding no chunk contributes nothing and is not an error: a
    // drained dirty set routinely names a chunk that has since been emptied,
    // and that is precisely the key whose geometry a host has to drop. Its
    // range is empty and sits where its geometry would have begun.
    //
    // `out_ranges` (may be null) receives one entry per key, in the order
    // given. A key repeated in the list is meshed once per occurrence.
    mesh::Mesh mesh_greedy_chunks(const std::vector<VoxelCoord>& keys,
                                  std::vector<VoxelChunkMeshRange>* out_ranges = nullptr) const {
        return mesh_greedy_chunks(active_, keys, out_ranges);
    }
    mesh::Mesh mesh_greedy_chunks(std::size_t level, const std::vector<VoxelCoord>& keys,
                                  std::vector<VoxelChunkMeshRange>* out_ranges = nullptr) const;

    // -- dirty chunks --------------------------------------------------------
    // Which chunks a mutation could have changed the meshed surface of, so a
    // host re-meshes the dab instead of the model. The same shape the brick
    // cache uses on the SDF side: mark (implicitly, on every write) -> drain
    // -> mesh the drained keys.
    //
    // Every public verb funnels its writes through one place, so the set is
    // fed there and a verb added later reports without being told to. A write
    // that does not change the cell dirties nothing — nothing about the
    // surface moved — which is the distinction change_count() already draws.
    //
    // A write on a cell lying on a chunk FACE also dirties the chunk across
    // that face, because that chunk's exposure test reads this cell. Missing
    // that is a hole in the surface visible only at chunk boundaries. A
    // neighbour that does not exist is left alone: an absent chunk holds no
    // material and emits no quads.
    //
    // A chunk that reaches zero occupancy is ERASED from the grid, and is
    // still reported dirty — otherwise the quads it contributed are never
    // removed.
    //
    // The set is per LEVEL, and propagation across levels dirties the levels
    // it writes, so a level's set describes that level whether or not it was
    // active when the edit landed.
    //
    // The drain returns the set and empties it, in a deterministic order
    // (lexicographic by x, then y, then z), so a host that skipped a frame
    // coalesces rather than replaying. A write that lands after a drain
    // appears in the NEXT drain, never in the one already returned. An
    // un-drained set costs nothing but memory: mesh_greedy neither reads it
    // nor clears it, so "mesh everything once, then track" and "track from the
    // start" reach the same state.
    //
    // A grid that was just deserialized, rasterized or given a level reports
    // every chunk it wrote, which is what a host that has drawn nothing needs.
    std::vector<VoxelCoord> take_dirty_chunks() { return take_dirty_chunks(active_); }
    std::vector<VoxelCoord> take_dirty_chunks(std::size_t level);
    // How many are queued, without draining.
    std::size_t dirty_chunk_count() const { return dirty_chunk_count(active_); }
    std::size_t dirty_chunk_count(std::size_t level) const;

    // -- voxel <-> SDF bridges -----------------------------------------------
    // Step-function field: -voxel_size/2 inside occupied cells, +voxel_size/2
    // elsewhere (a bound, not a distance — classify accordingly).
    float sample_step_field(kernel::cfloat3 world_p) const;

    // Rasterize an SDF tape into the grid over a world region: cells whose
    // center evaluates inside are set, colored from the tape's color field
    // via nearest palette entry (added as needed).
    //
    // What this preserves, stated because the return trip (to_field) is only
    // as trustworthy as the half nobody wrote down. These are properties of
    // sampling a continuous field onto a lattice, not defects:
    //
    //  - THE SURFACE MOVES by up to half a cell. Membership is decided at the
    //    cell centre, so a surface crossing a cell takes or leaves the whole
    //    cell. This is the quantisation the return trip cannot undo, and the
    //    reason its tolerance is a cell rather than zero.
    //  - A FEATURE THINNER THAN A CELL MAY VANISH. Nothing samples between
    //    centres, so a rib or a gap narrower than the lattice can fall between
    //    them and leave no cells at all. Rasterize finer; nothing downstream
    //    can invent what was never stored.
    //  - A SHARP EDGE BECOMES A STAIRCASE at the cell size, in the lattice's
    //    axes, and comes back from a conversion rounded. The information is
    //    lost HERE rather than on the way back.
    //  - COLOUR IS QUANTISED to the palette by nearest entry. Two colours
    //    closer than the palette tolerance become one.
    //  - THE REGION BOUNDS THE WORK. Content outside it is not rasterized and
    //    is not reported as missing.
    void rasterize_tape(const scene::Tape& tape, const math::Aabb& world_region);

  private:
    struct Chunk {
        std::vector<std::uint8_t> data;  // kChunkDim^3
        int occupied = 0;
    };
    using ChunkMap = std::unordered_map<VoxelCoord, Chunk, VoxelCoordHash>;

    // One resolution level. `detail` indexes exactly the cells whose value
    // differs from the cell above them in the coarser level — the offsets that
    // make a level change non-destructive. A coarse edit rewrites this level's
    // cells from its new parent EXCEPT where a detail entry overrides it, so a
    // broad stroke and the fine detail already sculpted both survive. Empty for
    // level 0, which has nothing coarser to differ from.
    struct Level {
        float voxel_size = 0.1f;
        ChunkMap chunks;
        std::unordered_map<VoxelCoord, std::uint8_t, VoxelCoordHash> detail;
        // Chunks whose meshed surface a write could have changed, since the
        // last drain. `dirty_memo` is the last key marked: writes walk a
        // footprint, so consecutive cells nearly always share a chunk, and an
        // interior cell of a chunk already marked needs no set probe at all.
        // A rasterize writes a million cells into eighty chunks, and the memo
        // is what keeps that a million compares rather than a million hashes.
        std::unordered_set<VoxelCoord, VoxelCoordHash> dirty;
        VoxelCoord dirty_memo{};
        bool dirty_memo_valid = false;
    };

    static VoxelCoord chunk_key(VoxelCoord c);
    static std::size_t chunk_offset(VoxelCoord c);
    void emit_quad(mesh::Mesh& out, int axis, int sign, int a, int u, int v, int w, int h,
                   std::uint8_t idx, float cell_size) const;
    // One face direction's sweep over one chunk-aligned (u, v) window of one
    // chunk slab: build each slice's exposure mask, greedy-merge it, emit.
    // The whole-grid mesh hands it a window spanning a slab's chunks, which is
    // why its merge crosses chunk boundaries; the regional mesh hands it a
    // window of exactly one chunk, which is what clamps the merge.
    void sweep_window(std::size_t level, int axis, int sign, int slab_index, int u0, int v0,
                      int nu, int nv, std::vector<std::uint8_t>& mask, mesh::Mesh& out) const;
    // The chunk this write changed, plus the neighbour across any chunk face
    // it sits on. Called only when the cell actually changed.
    void mark_chunk_dirty(std::size_t level, VoxelCoord c, VoxelCoord key);
    // Every occupied chunk at every level: what a grid built from a file owes a
    // host that has drawn nothing yet.
    void mark_every_chunk_dirty();

    // Level-addressed storage. Everything public funnels through these, so the
    // active level is read in one place rather than at every call site.
    std::uint8_t cell_at(std::size_t level, VoxelCoord c) const;
    bool write_cell(std::size_t level, VoxelCoord c, std::uint8_t index);

    // Propagation around an edited cell. Down averages into the coarser levels,
    // up replays the finer ones from their parent and their stored detail.
    void record_detail(std::size_t level, VoxelCoord c);
    void refresh_detail(std::size_t level, VoxelCoord parent);
    std::uint8_t downsample_cell(std::size_t fine, VoxelCoord coarse) const;
    void propagate_down(std::size_t from, VoxelCoord c);
    void propagate_up(std::size_t from, VoxelCoord c);
    void subdivide_into(std::size_t fine);
    // The serialised level tail, read into a grid that already holds its
    // coarsest level. Advances `pos`; false leaves the grid unusable and the
    // caller refuses the stream.
    bool read_level_tail(const std::uint8_t* data, std::size_t size, std::size_t* pos,
                         std::uint32_t palette_count);

    std::uint64_t change_count_ = 0;
    std::vector<kernel::cfloat3> palette_;
    std::vector<Level> levels_;
    std::size_t active_ = 0;
};

}  // namespace voxel
}  // namespace clay
