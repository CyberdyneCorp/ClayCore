/* clay.h — the flat, stable, versioned C ABI of claycore (c-abi spec).
 *
 * Rules of the boundary:
 *  - opaque handles, integer error codes, caller-owned buffers
 *  - buffers are either caller-allocated via the size-query pattern or
 *    returned behind an owner handle with an explicit clay_*_destroy
 *  - no C++ types, no exceptions, fixed-size integer types, UTF-8 strings
 *  - no variadics, no bitfields (FFI-general: Swift, C#, Rust, ctypes)
 *  - every descriptor struct starts with a uint32_t struct_size the caller
 *    sets and the library reads only up to (see descriptor struct versioning)
 *
 * Error details: every failing call records a thread-local UTF-8 message
 * retrievable with clay_last_error().
 */

#ifndef CLAY_H
#define CLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAY_ABI_MAJOR 0
#define CLAY_ABI_MINOR 82
#define CLAY_ABI_PATCH 0

/* Upper bound on the element count of any batch call: points, rays, cells,
 * selected node ids, stroke points, polygon vertices. A count above it is
 * rejected with CLAY_ERROR_INVALID_ARGUMENT rather than used to size a
 * buffer. A count is the one argument this ABI cannot check against the
 * caller's memory, and several calls allocate from one, so a byte length
 * passed where an element count belongs — or a negative signed value widened
 * to size_t, the usual Swift Int mistake — would otherwise allocate until the
 * process dies. Split larger work into batches. */
#define CLAY_MAX_BATCH 16777216 /* 1 << 24 */

typedef enum clay_result {
    CLAY_OK = 0,
    CLAY_ERROR_INVALID_ARGUMENT = 1,
    CLAY_ERROR_NOT_FOUND = 2,
    CLAY_ERROR_BUFFER_TOO_SMALL = 3,
    CLAY_ERROR_IO = 4,
    CLAY_ERROR_FORWARD_VERSION = 5,
    CLAY_ERROR_BUDGET_EXCEEDED = 6,
    CLAY_ERROR_UNSUPPORTED = 7,
    CLAY_ERROR_BACKEND = 8,
    /* The user stopped it. An ordinary outcome of an interactive session, not a
     * failure: distinct from CLAY_ERROR_BUDGET_EXCEEDED, which means a limit
     * the HOST declared before the call, and distinct from every fault code.
     * A cancelled operation leaves everything it was given exactly as it found
     * it — see clay_cancel_token. */
    CLAY_ERROR_CANCELLED = 9
} clay_result;

/* Library/ABI version (see c-abi spec). Compare majors at init — and while
 * the major is 0, the minor too: under SemVer's 0.x rule the ABI may still
 * break on a minor bump, 0.2.0 did break (see below), and every minor below
 * 1.0 adds entry points a host cannot dlopen its way out of missing. */
void clay_version(int32_t* major, int32_t* minor, int32_t* patch);

/* Thread-local detail message for the last failing call on this thread. */
const char* clay_last_error(void);

/* -- enums (values are ABI-stable) ---------------------------------------- */

/* Every primitive the scene model supports. The values are the engine's tape
 * opcodes, so there is no translation table to drift; a static assertion per
 * entry pins them (bindings/c/clay_c.cpp). params is the raw parameter block:
 * sizes are half-extents and angles are given resolved to their sine and
 * cosine, a pair the library checks is one. A plane's normal is normalized
 * for you, as it is in the Python bindings; a zero-length one is rejected. */
typedef enum clay_prim {
    CLAY_PRIM_SPHERE = 0,             /* params: r */
    CLAY_PRIM_BOX = 1,                /* params: bx by bz */
    CLAY_PRIM_ROUND_BOX = 2,          /* params: bx by bz r */
    CLAY_PRIM_BOX_FRAME = 3,          /* params: bx by bz e */
    CLAY_PRIM_TORUS = 4,              /* params: R r */
    CLAY_PRIM_CAPSULE = 5,            /* params: ax ay az bx by bz r */
    CLAY_PRIM_CAPPED_CYLINDER = 6,    /* params: r h */
    CLAY_PRIM_ROUNDED_CYLINDER = 7,   /* params: ra rb h */
    CLAY_PRIM_CAPPED_CONE = 8,        /* params: h r1 r2 */
    CLAY_PRIM_ROUND_CONE = 9,         /* params: r1 r2 h */
    CLAY_PRIM_ELLIPSOID = 10,         /* params: rx ry rz (bound) */
    CLAY_PRIM_OCTAHEDRON = 11,        /* params: s */
    CLAY_PRIM_HEX_PRISM = 12,         /* params: hx hy */
    CLAY_PRIM_PYRAMID = 13,           /* params: h */
    /* Out-of-line payload: the point chain / the 2D profile lives on the
     * item, not in params, so these need the item builder below rather than
     * clay_item_desc. */
    CLAY_PRIM_STROKE = 14,            /* points (x y z r) carried by the item */
    CLAY_PRIM_EXTRUDE = 15,           /* profile + half-depth (along Z) */
    CLAY_PRIM_REVOLVE = 16,           /* profile + axis offset (about Y) */
    CLAY_PRIM_CAPPED_TORUS = 17,      /* params: sin(aperture) cos(aperture) ra rb */
    CLAY_PRIM_LINK = 18,              /* params: le r1 r2 */
    CLAY_PRIM_CYLINDER_INFINITE = 19, /* params: cx cz r (unbounded) */
    CLAY_PRIM_CONE = 20,              /* exact cone; params: sin(a) cos(a) h, a = half-angle */
    CLAY_PRIM_PLANE = 21,             /* params: nx ny nz h, n normalized in (unbounded) */
    CLAY_PRIM_CUT_SPHERE = 22,        /* params: r h */
    CLAY_PRIM_CUT_HOLLOW_SPHERE = 23, /* params: r h t */
    CLAY_PRIM_SOLID_ANGLE = 24,       /* params: sin(angle) cos(angle) ra */
    CLAY_PRIM_TETRAHEDRON = 25,       /* params: r */
    CLAY_PRIM_DODECAHEDRON = 26,      /* params: r */
    CLAY_PRIM_ICOSAHEDRON = 27,       /* params: r */
    CLAY_PRIM_TRI_PRISM = 28,         /* params: hx hy (bound) */
    CLAY_PRIM_OCTAHEDRON_CHEAP = 29,  /* params: s (bound) */
    CLAY_PRIM_LNORM_SPHERE = 30,      /* params: r n, n >= 2 (bound) */
    /* Loft: params are the half-depth and the easing curve. The PROFILES are
     * not parameters — a variable number of them cannot fit in a fixed block —
     * so they are added with clay_item_add_loft_profile, two or more of them,
     * and interpolated evenly along Z. (bound) */
    CLAY_PRIM_LOFT = 31,
    /* Swept: the same profiles as a loft, carried along a GUIDE curve rather
     * than the Z axis. params is the easing curve. The guide is the item's
     * curve point list (clay_item_set_curve_points), and the profiles are
     * added with clay_item_add_loft_profile — a guide is not a new kind of
     * curve and a swept profile is not a new kind of profile. (bound) */
    CLAY_PRIM_SWEPT = 32,
    /* A sampled narrow-band volume. Not built with clay_item_create, which
     * has no way to supply the samples: the producers are
     * clay_item_volume_from_mesh and clay_item_volume_from_document. (bound) */
    CLAY_PRIM_VOLUME = 33,
    /* A TREE of spheres, skinned by one sphere-swept cone per node-parent pair
     * — ZBrush's ZSpheres. The nodes are a stroke's points; the topology is the
     * parent array beside them. An armature whose parents form a line IS a
     * stroke, and evaluates identically to one. */
    CLAY_PRIM_ARMATURE = 34
} clay_prim;

/* Combine ops. For the extended modes (groove..replace) blend_k is the
 * mode's radius or depth and the blend profile is ignored; groove and tongue
 * additionally read the item's rounding as the channel half-width. The two
 * transitions morph between the accumulated field and the item over a
 * world-space span; their parameters do not fit clay_item_desc, so they are
 * authored with clay_item_set_transition_linear / _radial. */
typedef enum clay_op {
    CLAY_OP_ADD = 0,
    CLAY_OP_SUBTRACT = 1,
    CLAY_OP_INTERSECT = 2,
    CLAY_OP_PAINT = 3, /* color only */
    CLAY_OP_GROOVE = 4,
    CLAY_OP_TONGUE = 5,
    CLAY_OP_PIPE = 6,
    CLAY_OP_ENGRAVE = 7, /* deboss */
    CLAY_OP_EMBOSS = 8,
    CLAY_OP_INSET = 9,
    CLAY_OP_SHELL = 10,
    CLAY_OP_REPLACE = 11,
    CLAY_OP_TRANSITION_LINEAR = 12, /* morph along a segment */
    CLAY_OP_TRANSITION_RADIAL = 13, /* morph over an XZ radius */
    /* Surface relief: the item is a REGION, and blend_k is the amplitude by
     * which the surface accumulated BEFORE it moves along its own normal. The
     * item's rounding is the falloff width, the same convention groove and
     * tongue use. Support is finite, so influence bounds and culling are
     * unaffected.
     *
     * blend_k saturates at the item's extent. A surface point moves only
     * while it stays inside the region (the item inflated by its rounding),
     * so the displacement equals blend_k only until blend_k reaches how far
     * that region extends past the surface along the normal — for a sphere
     * stamp centered on the surface, its radius plus the rounding. Past that
     * the falloff gives sharply diminishing returns, and radius + 2*rounding
     * is never reached; a host's depth slider mapped to blend_k should top
     * out near the extent rather than run past it.
     *
     * rounding = 0 therefore costs amplitude as well as edge softness: the
     * ceiling drops to the item's bare radius, and the rim becomes a step
     * whose declared steepness — amplitude over falloff width, the marcher's
     * cost model — is effectively unbounded. For a standard clay brush set
     * blend_k = rounding = the item's radius: the stamp then raises the
     * surface by exactly blend_k with a soft rim, and reaches no further
     * than 2*rounding outside the item.
     *
     * A pair rather than one signed amplitude, because blend_k is required
     * non-negative — and because add/subtract and engrave/emboss are pairs. */
    CLAY_OP_RELIEF = 14, /* build up: ZBrush Standard, ClayBuildup */
    CLAY_OP_INCISE = 15, /* cut in:   Crease, DamStandard */
    /* GROUPS ONLY (clay_layer_add_group): the group's children apply inline to
     * the chain outside it, exactly as if they had been added there. Every
     * other op makes the group a sub-expression that combines as a unit. An
     * item carrying it is refused. */
    CLAY_OP_INLINE = 255
} clay_op;

typedef enum clay_blend {
    CLAY_BLEND_HARD = 0,
    CLAY_BLEND_QUADRATIC = 1,
    CLAY_BLEND_CUBIC = 2,
    CLAY_BLEND_CIRCULAR = 3,
    CLAY_BLEND_CHAMFER = 4
} clay_blend;

/* Domain warps an item can carry, applied to its local point (item builder).
 * The parameters each kind takes, in order:
 *   TWIST     k             radians per unit of height, about Y
 *   BEND      k             radians per unit along X
 *   TAPER     y0 y1 s0 s1   cross-section scale s0 at y0 to s1 at y1
 *   DISPLACE  amplitude frequency   sine displacement of the field
 *   WRAP      x0 x1         bend the X interval around a cylinder about Z
 *   ELONGATE  hx hy hz      insert flat sections of half-extent h per axis
 *   BEND_LINEAR ax ay az bx by bz vx vy vz   displace by v, eased along a->b
 *   BEND_RADIAL r0 r1 dz  displace along Y by dz, eased across r0->r1
 *   TWIST_RANGE k y0 y1   twist at k rad/unit, ramped across y0->y1 and HELD
 *                         beyond it — ZBrush's Gizmo twist acts inside its box,
 *                         where CLAY_DEFORM_TWIST winds the whole item. With a
 *                         linear ease and a range covering the content the two
 *                         agree exactly, which is asserted rather than implied.
 *   BEND_RANGE  k x0 x1   the same for bend, across x0->x1
 *   ELONGATE_AXIS hx hy hz  per-axis stretch; a bound for any primitive
 *   GRAB      cx cy cz r dx dy dz front   pull a region; identity past r
 *   POSE      cx cy cz r ax ay az angle   rotate a region about its centre
 *   POSE_LINE ax ay az bx by bz nx ny nz angle  ramp a rotation along a -> b
 *   BEND_CURVE            a drawn guide; see clay_item_add_bend_curve
 *   BLOB      cx cy cz r amp freq octaves gain seed   noise with finite support */
typedef enum clay_deform {
    CLAY_DEFORM_TWIST = 0,
    CLAY_DEFORM_BEND = 1,
    CLAY_DEFORM_TAPER = 2,
    CLAY_DEFORM_DISPLACE = 3,
    CLAY_DEFORM_WRAP_AROUND = 4,
    CLAY_DEFORM_ELONGATE = 5,
    CLAY_DEFORM_BEND_LINEAR = 6,
    CLAY_DEFORM_BEND_RADIAL = 7,
    CLAY_DEFORM_ELONGATE_AXIS = 8,
    CLAY_DEFORM_GRAB = 9,
    CLAY_DEFORM_POSE = 10,
    CLAY_DEFORM_POSE_LINE = 11,
    /* Radial scale about a centre: centre(3), radius, strength. ONE signed
     * strength covers both directions — positive magnifies, negative pinches —
     * because they are the same deformation. */
    CLAY_DEFORM_MAGNIFY = 12,
    /* Fractal gradient noise as a distance offset: amplitude, frequency,
     * octaves, gain, seed. The irregular sibling of CLAY_DEFORM_DISPLACE,
     * whose sine is regular by construction. The seed is an ordinary parameter,
     * not global state, so the same seed always gives the same field. */
    CLAY_DEFORM_NOISE = 13,
    CLAY_DEFORM_TWIST_RANGE = 14,
    CLAY_DEFORM_BEND_RANGE = 15,
    /* Bend along a DRAWN guide instead of at a constant rate — ZBrush's Gizmo
     * Bend Curve. The item's local X span is laid onto the guide's arc length
     * and the material rides the guide's frames, so this is the inverse of the
     * swept primitive rather than a second kind of bend.
     *
     * Its guide does not fit a flat float array, so it is the one kind
     * clay_item_add_deformer does NOT take: use clay_item_add_bend_curve. */
    CLAY_DEFORM_BEND_CURVE = 16,
    /* A LATTICE (free-form deformation) cage — ZBrush's Gizmo Lattice, on an
     * SDF item.
     *
     * The cage's control-point OFFSETS are the INVERSE warp, and that is the
     * design decision rather than an implementation detail: forward FFD has no
     * closed-form inverse, and a claycore deformer runs backwards. It is not
     * the EXACT inverse: it differs from forward FFD by a term proportional to
     * how the basis varies along the displacement, so it over-travels a drag
     * toward rising weight and under-travels one pointing away. Measured
     * against the forward cage, the difference is under 1.5% of the drag.
     * (CLAY_DEFORM_GRAB always under-travels, because its weight always falls
     * off along the drag; a lattice does not inherit that sign.)
     *
     * For FORWARD FFD with no approximation, use the mesh-layer lattice
     * (clay_mesh_lattice_*), which is what ZBrush and Blender actually do —
     * a mesh knows where its vertices are, so nothing has to be inverted.
     *
     * Its cage does not fit a flat float array, so like CLAY_DEFORM_BEND_CURVE
     * it has its own entry point: clay_item_add_lattice. */
    CLAY_DEFORM_LATTICE = 17,
    /* Blob (ZBrush's): an irregular swelling under the brush rather than the
     * smooth one draw gives. Parameters: centre(3), radius, amplitude,
     * frequency, octaves, gain, seed.
     *
     * It is CLAY_DEFORM_NOISE with the finite support CLAY_DEFORM_GRAB and
     * CLAY_DEFORM_MAGNIFY have — outside the radius the field is untouched,
     * which is what makes it a brush rather than a modifier. The amplitude is
     * signed and so is the noise, so one dab both swells and eats in. */
    CLAY_DEFORM_BLOB = 19,
    /* An ALPHA stamp: a caller-supplied scalar image as a distance offset,
     * under the same radial falloff blob, grab and magnify use. Pores, fabric,
     * scales, stitching — the technique every competing sculptor details with,
     * and until now this engine had it on voxels
     * (clay_voxel_sculpt_carve_alpha) and not on fields.
     *
     * A DEFORMER, NOT A PRIMITIVE, and that is the design rather than an
     * implementation detail. An item shaped like the stamp would ADD material
     * in the stamp's shape; an alpha modulates a surface already there — pores
     * in existing skin. So it offsets the distance, exactly as noise and blob
     * do, and the surface moves along its own normal.
     *
     * THE ENGINE DECODES NO IMAGES. A host with an alpha has already loaded a
     * PNG; it hands over the samples. That rule is what keeps an image decoder
     * out of a library that compiles to five backends.
     *
     * Its samples do not fit a flat float array of fixed size, so like
     * CLAY_DEFORM_BEND_CURVE and CLAY_DEFORM_LATTICE it has its own entry
     * point: clay_item_add_alpha. */
    CLAY_DEFORM_ALPHA = 20
} clay_deform;

/* Easing curves are given by index; 0 is linear. Only the taper deformer and
 * the transition ops read one. */
#define CLAY_EASE_LINEAR 0
#define CLAY_EASE_COUNT 33 /* valid indices are [0, CLAY_EASE_COUNT) */

/* Closed 2D profiles a lift primitive (extrude, revolve) can carry. Open
 * curves are unsigned distances rather than regions, so they are not
 * profiles: flatten them to a polygon host-side. */
typedef enum clay_profile {
    CLAY_PROFILE_CIRCLE = 0,    /* params: r */
    CLAY_PROFILE_BOX = 1,       /* params: hx hy */
    CLAY_PROFILE_HEXAGON = 2,   /* params: r (face radius) */
    CLAY_PROFILE_TRIANGLE = 3,  /* params: r */
    CLAY_PROFILE_TRAPEZOID = 4, /* params: bottom top half_height */
    CLAY_PROFILE_VESICA = 5,    /* params: r d */
    CLAY_PROFILE_POLYGON = 6    /* vertices: clay_item_set_profile_polygon */
} clay_profile;

/* -- descriptor struct versioning ------------------------------------------ */

/* Every descriptor struct crossing this ABI starts with a uint32_t
 * struct_size that the caller sets to the sizeof of the struct it compiled
 * against. The library reads only that prefix, so fields may be appended
 * without a major bump; anything the caller did not declare takes its
 * documented default, which is the zero value for every field so far.
 *
 * Setting it is mandatory. The original layout — the field set a struct had
 * when the convention shipped in ABI 0.2.0 — is the shortest value accepted;
 * anything below it, zero included, is rejected with
 * CLAY_ERROR_INVALID_ARGUMENT. A value larger than this header knows about is
 * clamped, so a newer caller's unknown tail is ignored rather than misread,
 * and a value too large to be any descriptor is rejected outright.
 *
 * clay_mesh_params is the first struct to have used this: mesher and
 * experimental were appended to it in 0.3.0, and a caller compiled against
 * the 0.2.0 layout still meshes, with both fields taking their zero default.
 *
 * ABI 0.2.0 BREAKS BINARY COMPATIBILITY WITH 0.1.0 for the two descriptors
 * that predate the convention, clay_item_desc and clay_mesh_params: the new
 * leading field shifts every other one. This is deliberate and allowed by
 * SemVer's 0.x rule; there is no way to tell a 0.1.0 descriptor from a
 * zeroed 0.2.0 one by content, which is why zero is not a sentinel for the
 * original layout. A 0.1.0 binary passing either struct puts a prim value or
 * a voxel size where struct_size now is, so the call is rejected rather than
 * misread. Recompile against this header, set struct_size, and the fields
 * mean exactly what they meant in 0.1.0. */

/* -- document -------------------------------------------------------------- */

typedef struct clay_document clay_document; /* opaque */
typedef uint32_t clay_layer_id;
typedef uint32_t clay_node_id;

clay_document* clay_document_create(void);
void clay_document_destroy(clay_document* doc);

clay_result clay_document_save(const clay_document* doc, const char* path);
clay_result clay_document_load(const char* path, clay_document** out_doc);

/* -- what the history costs, and bounding it (add-history-budget) -----------
 *
 * The history had no cap of any kind: no depth limit, no byte accounting, no
 * eviction, no query. The only lever was clay_document_enable_undo, which is
 * not a lever, it is a light switch. Survivable while the history held SDF
 * edits alone; it now holds four step kinds and a journal, on a platform that
 * reclaims memory by killing processes and does not warn twice.
 *
 * WHAT IS EXPENSIVE IS NOT WHAT YOU EXPECT. The command stack stores INVERSES,
 * so REMOVING an item records a whole node — 440 bytes plus its deformer chain
 * and stroke points — while ADDING one records an id: a session of deletes and
 * a session of adds cost very differently, and nothing told you which you were
 * in. A voxel or mask step is proportional to the cells it CHANGED, so one big
 * fill can outweigh a thousand dabs. And the journal keeps its own copy, so a
 * session with crash recovery on holds roughly twice what one without does. */
typedef struct clay_history_bytes {
    uint32_t struct_size; /* = sizeof(clay_history_bytes); required */
    uint64_t undo;        /* the step list and the command stack under it */
    uint64_t redo;
    uint64_t journal;     /* the crash-recovery log; see the budget note below */
    uint64_t total;
    uint64_t undo_steps;
    uint64_t redo_steps;
    uint64_t journal_events;
    /* Steps evicted to stay inside the budget, ever. Show a horizon from this
     * rather than letting a user hunt for a step that is gone. */
    uint64_t dropped_steps;
} clay_history_bytes;

clay_result clay_document_history_bytes(const clay_document* doc,
                                        clay_history_bytes* out_bytes);

/* Zero means UNBOUNDED, which is what a host that never calls this gets — so
 * this cannot change behaviour under a host that ignores it.
 *
 * The budget bounds UNDO AND REDO ONLY, and deliberately does not evict from
 * the journal: those bytes are your crash recovery, and dropping them silently
 * would lose exactly what that feature exists to keep. The journal is reported
 * instead, and you trim it with clay_document_journal_trim once its bytes are
 * durable.
 *
 * Redo is spent before undo, because redo is transient — the next edit
 * discards it anyway — and the newest undo step is NEVER dropped: a budget
 * that could make the next undo fail would be worse than no budget, because a
 * host cannot tell that from a bug. */
clay_result clay_document_set_history_budget(clay_document* doc, uint64_t bytes);

/* Drop the oldest steps until the history fits, for a platform that has just
 * reported memory pressure and expects an immediate response rather than
 * waiting for the next edit. Does not set a budget. */
clay_result clay_document_trim_history(clay_document* doc, uint64_t bytes);

/* -- what the whole document costs (ABI 0.49.0) -----------------------------
 *
 * iOS asks this, not the user, and it does not ask politely:
 * didReceiveMemoryWarning arrives with no argument and expects an answer within
 * a frame or two. To decide what to release you must know what you are holding,
 * and until this version the library could not tell you.
 *
 * Every subsystem accounted for itself and NOTHING ROLLED UP.
 * clay_document_history_bytes reported the history, clay_voxel_sculpt_layers_-
 * bytes reported part of one layer, clay_brick_cache_stats reported a cache the
 * document does not own — and the edit list, the voxel chunk storage those
 * sculpt layers sit beside, masks, mesh layers and the passthrough blobs
 * reported nothing at all. The rest is where the memory is: a rasterized voxel
 * layer is the largest thing most documents hold and it was invisible.
 *
 * THE BREAKDOWN IS THE FEATURE, and a total is not. Under pressure you do not
 * need to know how big the document is, you need to know WHICH PART, because
 * that decides what you are allowed to release:
 *
 *   trimming history        -> costs undo depth  (set_history_budget is the lever)
 *   dropping the brick cache-> costs a stall     (not counted here; not owned here)
 *   dropping voxel or mesh  -> destroys the user's work. Never.
 *
 * A FLOOR, NOT AN EQUALITY. These are container walks: allocator block headers,
 * size-class rounding and arena fragmentation are invisible from here, as are
 * the library's own code and static data. Expect the OS to charge the process
 * MORE than this, and do not read the gap as a leak.
 *
 * It is also LARGER than the same document's file, often several times over: a
 * .clayspace is RLE- and palette-compressed and a live voxel chunk is a flat
 * array whether one cell is set or all of them.
 *
 * WHICH MEANS voxel_content FOLLOWS CHUNKS, NOT CELLS. A chunk is 32^3 cells
 * allocated whole: ONE voxel costs 32 KiB, and 32 768 voxels filling that same
 * chunk cost the same 32 KiB. Two layers whose occupancy differs by three
 * orders of magnitude report an IDENTICAL figure when they touch the same
 * chunks. Present it beside clay_voxel_occupied_count if you like, but expect
 * the two to move independently: what grows this number is the REGION an artist
 * has worked in, not how solidly they filled it. */
typedef struct clay_memory_report {
    uint32_t struct_size; /* = sizeof(clay_memory_report); required */

    /* -- the model. None of this may be released; it IS the user's work. */
    uint64_t edit_list;      /* nodes, strokes, deformer chains, sampled volumes */
    uint64_t voxel_content;  /* chunk storage across every level */
    uint64_t mesh_layers;    /* imported geometry, unrecoverable */
    uint64_t masks;          /* authoring state; small, but not nothing */

    /* -- droppable, in the order to reach for it. */
    /* Undo for voxel layers. Held INSIDE the grids, beside voxel_content, and
     * separated from it for exactly that reason: this is the only voxel figure
     * you may act on. */
    uint64_t voxel_sculpt_layers;
    /* The undo history and its journal. clay_document_set_history_budget is the
     * lever, and this is the only part of a document the engine evicts itself. */
    uint64_t history;
    /* A thumbnail and camera bookmarks carried without being interpreted.
     * Regenerable, and usually trivial — reported so the fields sum. */
    uint64_t passthrough;

    /* -- transient: memory held only while an operation is in flight.
     *
     * A mask copies its chunks on the first touch inside a recorded step, so a
     * mask costs roughly DOUBLE for the duration of that step. Reported apart
     * from `masks` so that a figure about to halve on its own is not mistaken
     * for one that will still be there afterwards.
     *
     * THROUGH THIS ABI IT IS ALWAYS ZERO, and that is a statement about the
     * ABI rather than about the mechanism. Every mask entry point here opens
     * its step and closes it before returning, and calls on one document must
     * be serialized, so there is no moment at which you could hold a handle,
     * a step could be open, and you could call this. A C++ embedder driving
     * clay::session::History directly CAN hold one open across several edits,
     * and sees a non-zero figure.
     *
     * It is exposed anyway so that `total` stays the sum of the fields above
     * if an entry point that spans a step is ever added — the alternative is a
     * total that silently gains bytes belonging to no reported field. Do not
     * build a memory-pressure response around it today: it will read zero. */
    uint64_t transient;

    /* The sum of every field above. */
    uint64_t total;

    /* What is here, for presenting the figure. */
    uint64_t voxel_layers;
    uint64_t mesh_layer_count;
    uint64_t mask_count;

    /* -- the surface tier (ABI 0.78.0, add-extreme-poly-runtime) -------------
     *
     * ZERO THROUGH clay_document_memory, AND THAT IS OWNERSHIP RATHER THAN AN
     * OMISSION. A clay_multires and a clay_dynamic_sculptor are opaque and
     * OWNING: a host holds one beside its document, never inside it, so a
     * document cannot walk them and reporting a guess would be worse than
     * reporting nothing. clay_document_memory_with_surfaces is the seam — the
     * host fills a clay_memory_ledger from the surfaces it holds and hands it
     * in, and these lines are what that ledger becomes.
     *
     * APPENDED after mask_count, not inserted beside the model lines they
     * belong with, because every field above is at an offset a shipped caller
     * compiled in. `total` includes them either way: the totals stay true
     * rather than the breakdown staying constant, which is the direction a
     * memory report has to err in. */
    uint64_t surface_content;  /* adaptive surfaces: geometry and connectivity */
    uint64_t multires_detail;  /* the coefficients: the wrinkles themselves */
    uint64_t sculpt_layers;    /* a mesh layer stack's content */
    uint64_t surface_caches;   /* chunk indices, evaluated levels, runtime caches */
    uint64_t surface_scratch;  /* per-stamp working sets and preview staging */
    uint64_t surface_undo;     /* vertex deltas and detail undo */

    /* -- the three a host under pressure can act on --------------------------
     *
     * WHAT A MEMORY WARNING ACTUALLY ASKS. A single total is not the answer:
     * under pressure a host does not need to know how big the document is, it
     * needs to know WHICH PART, because that is what decides what it is allowed
     * to release. Derived from the fields above rather than counted separately,
     * so a line added without being classified cannot make these disagree with
     * `total`. */
    uint64_t essential;    /* the user's work; never released */
    uint64_t rebuildable;  /* reconstructs to an identical surface */
    uint64_t undoable;     /* undo depth, and the host's own policy */
} clay_memory_report;

/* -- what a host will spend, and what it may take back ------------------------
 *
 * (sculpt-runtime and c-abi specs, add-extreme-poly-runtime.)
 *
 * The report above answers "what does this document cost". A device under
 * pressure asks two more questions it cannot answer — "what am I allowed to
 * spend" and "what can I release right now" — and these are them.
 *
 * FILLED BY THE HOST, WITH NO DEVICE DETECTION ANYWHERE. Not one platform call,
 * not one model-name comparison. A host knows what its operating system is
 * telling it; an engine guessing from a model string is both wrong and — worse
 * — untestable, because the tests run on a desktop. A constrained profile is a
 * struct a desktop test fills in three lines, which is the whole point.
 *
 * EVERY FIELD IS A HINT AND THE TYPE IS WHAT SAYS SO. Each one names something
 * that can be recomputed EXACTLY from what was committed: normals during a
 * drag, index quality, cache residency, the rate a preview drains. There is
 * deliberately no field for anything that IS the committed result — the
 * deformation, split and collapse thresholds, remesh targets, detail
 * coefficients, layer content, masks, brush strength and falloff. A deferred
 * split would make the committed mesh a function of machine speed, and this
 * library spends real effort on determinism that a budget-dependent topology
 * would throw away. So "a memory-saving mode changed my sculpt" is
 * unrepresentable here rather than merely forbidden. */

typedef enum clay_memory_class {
    /* No budget. Every byte field is advisory and the runtime keeps what it
     * builds. What a desktop host and every existing caller get. */
    CLAY_MEMORY_CLASS_FULL = 0,
    /* Budgets are real, inactive levels hold compact detail only, and
     * maintenance runs between interactions. */
    CLAY_MEMORY_CLASS_CONSTRAINED = 1,
    /* What a host sets when the operating system has already warned it once. */
    CLAY_MEMORY_CLASS_MINIMAL = 2
} clay_memory_class;

/* How hard a host is asking. Passed to a trim; never inferred by the engine. */
typedef enum clay_pressure {
    CLAY_PRESSURE_NONE = 0,     /* give back what is free anyway */
    CLAY_PRESSURE_WARNING = 1,
    CLAY_PRESSURE_URGENT = 2,
    /* The last stop before the operating system kills the process: everything
     * rebuildable goes, and the next edit pays to rebuild what it needs. */
    CLAY_PRESSURE_CRITICAL = 3
} clay_pressure;

/* The vocabulary every subsystem answers in. The split that matters is not
 * "big versus small" but WHAT IT COSTS TO LET IT GO: the first group is the
 * user's work and is never released, the second reconstructs bit-identically,
 * the third is undo depth and belongs to the host's own policy. */
typedef enum clay_memory_category {
    /* authoritative: releasing any of this destroys work */
    CLAY_MEMORY_BASE_GEOMETRY = 0,       /* cages, fixed meshes, surface elements */
    CLAY_MEMORY_TOPOLOGY = 1,            /* per-level face lists, half-edge connectivity */
    CLAY_MEMORY_MULTIRES_DETAIL = 2,     /* the coefficients: the wrinkles themselves */
    CLAY_MEMORY_SCULPT_LAYERS = 3,       /* a layer stack's content */
    CLAY_MEMORY_MASKS = 4,               /* authoring state */
    /* rebuildable: reconstructs to an identical surface */
    CLAY_MEMORY_CHUNK_INDEX = 5,         /* the chunk table, its arena and the trees over it */
    CLAY_MEMORY_EVALUATED_CACHE = 6,     /* subdivided positions, frames, normals */
    CLAY_MEMORY_LEVEL_RUNTIME_CACHE = 7, /* per-level meshes, adjacency, connectivity */
    CLAY_MEMORY_LAYER_EVAL_CACHE = 8,    /* an evaluated layer stack */
    CLAY_MEMORY_DERIVED_POSITIONS = 9,   /* positions of levels nobody is looking at */
    CLAY_MEMORY_SCRATCH = 10,            /* the per-stamp working set */
    CLAY_MEMORY_PREVIEW_STAGING = 11,    /* what is queued for the host's next upload */
    /* undoable: the host's policy, never the engine's */
    CLAY_MEMORY_UNDO_HISTORY = 12
} clay_memory_category;

/* How many of them this build knows. The ledger and the trim report below carry
 * an array of exactly this length, LAST in each struct, so a build that adds a
 * category grows the array without moving anything a shipped caller reads — the
 * same appended-field rule the report above follows, applied to an array. Both
 * structs also report the count they were filled with, so a caller reading a
 * shorter array knows it is reading a prefix rather than a total. */
#define CLAY_MEMORY_CATEGORY_COUNT 13

/* Never NULL, for any value, including one this build does not know. */
const char* clay_memory_class_text(int32_t memory_class);
const char* clay_pressure_text(int32_t pressure);
const char* clay_memory_category_text(int32_t category);

typedef struct clay_sculpt_memory_profile {
    uint32_t struct_size; /* = sizeof(clay_sculpt_memory_profile); required */
    int32_t memory_class; /* clay_memory_class */

    /* -- byte budgets. Zero means "no budget for this", which is what
     * CLAY_MEMORY_CLASS_FULL means field by field. */
    uint64_t cache_budget;   /* chunk indices, runtime caches, derived positions */
    uint64_t undo_budget;    /* the engine never trims this on its own */
    /* The per-stamp working set, and a HARD bound: a footprint larger than it is
     * processed in blocks rather than allocated, which is what stops a
     * 500k-vertex footprint on a constrained profile from becoming the peak
     * that kills the app. */
    uint64_t scratch_budget;
    uint64_t preview_budget; /* what a host may hold for the next frame's upload */

    /* -- residency and deferral. */
    /* How many multires levels keep their rebuildable caches; 0 means no limit.
     * On a constrained profile the sculpt level and the display level stay
     * resident and the rest hold compact detail only. */
    uint32_t max_resident_levels;
    /* Recompute exact normals at stroke end rather than per stamp. The final
     * state is exact either way; this only decides when the work happens. */
    int32_t defer_normals_in_stroke;
    /* Whether a spatial index may be REBUILT — never whether it may be
     * refitted, which is correctness. Defaults to 1 and stays advisory: a
     * rebuild helped one of five measured deformations and hurt two. */
    int32_t allow_index_rebuild;
    /* How many dirty chunks a host expects to drain per frame; 0 means "as many
     * as there are". Lossless at any value, because the transport acknowledges
     * per chunk. */
    uint32_t preview_chunks_per_frame;
} clay_sculpt_memory_profile;

clay_result clay_sculpt_memory_profile_defaults(clay_sculpt_memory_profile* out_profile);

/* Bytes by category, plus the three roll-ups. What a host holding an adaptive
 * surface, a hierarchy and a document gets ONE of, rather than three reports it
 * has to reconcile. */
typedef struct clay_memory_ledger {
    uint32_t struct_size;   /* = sizeof(clay_memory_ledger); required */
    /* How many entries of `bytes` this library filled. A caller from a later
     * header reads this rather than assuming its own CLAY_MEMORY_CATEGORY_COUNT
     * was reached. */
    uint32_t category_count;
    uint64_t essential;
    uint64_t rebuildable;
    uint64_t undoable;
    uint64_t total;
    /* Indexed by clay_memory_category. LAST, deliberately: see the count. */
    uint64_t bytes[CLAY_MEMORY_CATEGORY_COUNT];
} clay_memory_ledger;

/* What a trim actually did, per category — reported rather than returned as one
 * number, because a host that asked for 40 MB and got it out of preview staging
 * made a different decision from one that got it out of the evaluated caches it
 * is about to need again. */
typedef struct clay_trim_report {
    uint32_t struct_size; /* = sizeof(clay_trim_report); required */
    int32_t pressure;     /* clay_pressure, echoed */
    uint64_t total_released;
    /* Non-zero when a pin was held: NOTHING was released and the figures are
     * what the call WOULD have released. A host that receives a memory warning
     * while a save is running gets an honest answer instead of a document
     * mutating under the writer. */
    int32_t pinned;
    uint32_t category_count;
    uint64_t released[CLAY_MEMORY_CATEGORY_COUNT]; /* LAST, as above */
} clay_trim_report;

/* -- the pin -----------------------------------------------------------------
 *
 * What a serializer or a readback holds so a trim arriving mid-save is honest
 * rather than destructive. Consulted by every trim entry point; a NULL pin is
 * "nothing is held", which is what every caller that has not adopted it passes.
 *
 * REENTRANT, because a readback inside a save must not un-pin the save when it
 * returns: acquire and release are a counter, and the pin is held while the
 * count is non-zero. In C++ this is an RAII scope and in pyclay a `with` block;
 * in C a caller brackets the region itself, which is the same discipline every
 * other paired call in this header asks for. */
typedef struct clay_memory_pin clay_memory_pin;

clay_result clay_memory_pin_create(clay_memory_pin** out_pin);
void clay_memory_pin_destroy(clay_memory_pin* pin);
clay_result clay_memory_pin_acquire(clay_memory_pin* pin);
/* Releasing a pin nobody acquired is CLAY_OK and does nothing: an unbalanced
 * release is a caller's bug, and leaving the count at zero is the harmless
 * reading of it. Underflowing to "pinned forever" is not. */
clay_result clay_memory_pin_release(clay_memory_pin* pin);
int32_t clay_memory_pin_held(const clay_memory_pin* pin);

clay_result clay_document_memory(const clay_document* doc, clay_memory_report* out_report);

/* The same report, with the surfaces the host holds BESIDE the document folded
 * into the surface-tier lines. `surfaces` is a ledger the caller filled from
 * clay_multires_memory_ledger, clay_dynamic_sculptor_memory_ledger and
 * clay_mesh_sculptor_memory_ledger — merged by the caller, since only the
 * caller knows which surfaces belong to this document.
 *
 * A NULL ledger is exactly clay_document_memory, which is why that call stays
 * rather than gaining an argument: a host with no surfaces should not have to
 * pass a zeroed struct to say so. */
clay_result clay_document_memory_with_surfaces(const clay_document* doc,
                                               const clay_memory_ledger* surfaces,
                                               clay_memory_report* out_report);

/* The same breakdown for ONE layer, so a large document can be attributed to
 * the layer responsible rather than merely reported as large.
 *
 * `history` and `passthrough` are document-wide and are therefore ALWAYS ZERO
 * here — documented rather than removed, so one struct serves both views.
 *
 * THE CONTENT LINES SUM EXACTLY ACROSS LAYERS AND THE EDIT LIST DOES NOT. The
 * tempting reading — "the layers add up to the document" — is false and you
 * would hit it. Content sums because every voxel chunk, mask cell and triangle
 * belongs to exactly one layer id. The edit list does not, for two deliberate
 * reasons: the document-wide figure includes overhead owned by no layer, and
 * INSTANCE layers share one edit list, which the document counts ONCE (ten
 * instances of one blockout are one allocation, and saying otherwise would
 * invite you to free memory that does not exist) while each instance reports it
 * in full (displaying an instance costs an evaluation like any other layer, and
 * reporting zero would call it free). A layer's edit_list is a CEILING on its
 * contribution, not a partition of it.
 *
 * The "counted ONCE" half of that survives a SAVE, since 0.58.0: a document
 * writes a shared edit list once and names it from the other instances, so
 * reading this report back after a save and reload gives the figure it gave
 * before. Through 0.57.0 it did not — every layer's content went out inline
 * and came back as its own allocation, so a document of ten instances reloaded
 * ten times heavier and the layers were quietly no longer linked.
 *
 * An unknown layer id is CLAY_ERROR_NOT_FOUND, not a zeroed report: a zeroed
 * report reads as an empty layer and shows a wrong answer confidently. */
clay_result clay_layer_memory(const clay_document* doc, clay_layer_id layer,
                              clay_memory_report* out_report);

/* -- cancelling a long operation -------------------------------------------
 *
 * This library has three budget classes and the third had no exit. From the
 * device gate: mask_extrude measures 4403 ms and consolidate 661 ms on the
 * reference iPad, and every one of those was a synchronous call a host entered
 * and could not leave. The threading contract closed the obvious workaround —
 * calls on one handle must be serialized, const readers included — so a host
 * could not even read the document from another thread to drive a progress bar.
 *
 * A TOKEN, NOT A CALLBACK. This header contains no function pointers, and a
 * progress callback would be the first one every FFI consumer has to marshal.
 * It would also fire on a worker thread, so the header would need a rule about
 * what it may touch and the honest answer is "almost nothing". Here the engine
 * writes and the host reads, both sides plain atomics. */
typedef struct clay_cancel_token clay_cancel_token; /* opaque */

clay_cancel_token* clay_cancel_token_create(void);
void clay_cancel_token_destroy(clay_cancel_token* token);

/* Cancelling is the ONE call in this ABI that is safe to make from a thread
 * other than the one running the operation. Everything else requires the host
 * to serialize. Safe before an operation starts, during one, and after one has
 * returned. */
void clay_cancel_token_cancel(clay_cancel_token* token);
int32_t clay_cancel_token_cancelled(const clay_cancel_token* token);

/* Reusable: clears the cancelled flag and the progress. A host holding one
 * token per document must not pay an allocation per cancel. */
void clay_cancel_token_reset(clay_cancel_token* token);

/* What the operation is doing. NO TIME ESTIMATE, deliberately: a multi-phase
 * operation's phases differ in per-unit cost by more than an order, so a figure
 * derived from a fraction would be wrong in the direction that annoys users
 * most, and the host has the wall clock.
 *
 * Safe to read from another thread, and safe when nothing is running — in which
 * case `running` is 0 and the rest is zeroed rather than left stale. */
typedef struct clay_progress {
    uint32_t struct_size; /* = sizeof(clay_progress); required */
    uint32_t phase;       /* 0-based */
    uint32_t phase_count;
    int32_t running;
    float fraction; /* within the current phase, monotonic */
    uint64_t done;  /* honest units where the operation has any */
    uint64_t total; /* 0 when it does not */
} clay_progress;

clay_result clay_cancel_token_progress(const clay_cancel_token* token,
                                       clay_progress* out_progress);

/* -- serializing without a file -------------------------------------------- */

/* Bytes the library owns, for a host that has nowhere to put a path.
 *
 * Every format here has always been implemented against buffers — the *_file
 * entry points are wrappers over them — and only the wrappers crossed. That is
 * a problem where this library is pointed: documents on iPadOS arrive from a
 * document provider or a share sheet behind a security-scoped URL whose
 * lifetime the host does not own, a host syncing to a server needs bytes to
 * send, a host keeping projects in its own container needs bytes to store, and
 * a WASM build has no filesystem at all. All of them were paying for a
 * temporary file: a second copy of the document, a path to create and clean up
 * (twice, if the process died), and a full disk as a failure mode for an
 * operation that never asked to touch a disk.
 *
 * An OWNER HANDLE rather than the size-query pattern, because answering "how
 * big" would mean serializing the whole document to find out and serializing
 * it again to fill the buffer. Same shape as clay_mesh and clay_tape: take it,
 * borrow from it, release it. */
typedef struct clay_blob clay_blob; /* opaque */

/* Borrowed, valid until clay_blob_destroy, and unaffected by any later edit to
 * whatever produced it — the bytes were serialized when the handle was made,
 * so a host may hand them to an asynchronous writer without copying first.
 * clay_blob_data may return NULL only for a zero-length blob. */
const uint8_t* clay_blob_data(const clay_blob* blob);
size_t clay_blob_size(const clay_blob* blob);
void clay_blob_destroy(clay_blob* blob);

/* The same bytes clay_document_save writes, byte for byte. Free the result
 * with clay_blob_destroy. */
clay_result clay_document_save_memory(const clay_document* doc, clay_blob** out_blob);

/* And back. Accepts exactly what clay_document_load accepts, and refuses a
 * truncated or corrupt buffer without reading past `size`.
 *
 * NO BUDGET, deliberately. The only thing clay_import_budget bounds on the
 * path loader is max_file_bytes, which is a ceiling on the bytes a loader will
 * read into memory BEFORE it sizes a buffer — and a caller holding a buffer
 * has already done that read. The engine's own load_clayspace takes no budget
 * for the same reason. A parameter that cannot act is worse than an absent
 * one, so this says why instead of accepting one and ignoring it. */
clay_result clay_document_load_memory(const uint8_t* data, size_t size,
                                      clay_document** out_doc);

/* -- surviving a crash ------------------------------------------------------
 *
 * A recovery is a SNAPSHOT plus the steps since it. The snapshot is
 * clay_document_save_memory above; this is the steps.
 *
 * Why not just autosave. clay_document_save is whole-document and synchronous,
 * and the device gate prices that class of work at hundreds of milliseconds to
 * seconds. A host autosaving on a timer therefore stalls its UI for however
 * long the whole document takes, and the cost grows with the sculpt — so the
 * safer it tries to be, the worse the stall gets. A journal is proportional to
 * what changed.
 *
 * WHAT THE LIBRARY OWNS: producing the bytes and replaying them.
 * WHAT THE HOST OWNS: where the file lives, when it is flushed, how often to
 * re-snapshot, and what to do with a leftover recovery file on the next launch.
 * Those differ between iOS, a desktop filesystem and a database, and a library
 * that decided them would be wrong on two of the three. fsync and atomic
 * rename are yours.
 *
 * Requires undo to be enabled: the journal IS the history's record. */

/* Everything recorded since `from`, as bytes to append wherever you keep them.
 * `out_now_at` receives the index to pass next time.
 *
 * PEEK, not drain: the log is untouched, so a failed write is retried by
 * asking again. Indices are ABSOLUTE for the life of the session and do not
 * shift when you trim, so a host that asks below the floor is told it is gone
 * (an empty journal) rather than handed the wrong events — compare
 * clay_document_journal_range. Free the blob with clay_blob_destroy. */
clay_result clay_document_journal_since(const clay_document* doc, size_t from,
                                        clay_blob** out_blob, size_t* out_now_at);

/* The window the log still holds: [*out_first, *out_next). A host that trimmed
 * and later asks for something below *out_first gets nothing, and this is how
 * it finds out rather than by replaying a short history. */
clay_result clay_document_journal_range(const clay_document* doc, size_t* out_first,
                                        size_t* out_next);

/* Drop events below `upto`, once those bytes are durable. Indices do not
 * shift. */
clay_result clay_document_journal_trim(clay_document* doc, size_t upto);

/* Replay a journal onto a document that IS the snapshot it was taken against.
 *
 * `out_applied` receives how many events were applied. `out_stopped_at_barrier`
 * is set when replay reached an operation nothing can reproduce — every MASK
 * edit is one today, because a mask is a fourth representation with no history
 * mechanism. Replay STOPS there and returns CLAY_OK with the flag set, rather
 * than continuing and handing back a document quietly missing that operation's
 * effect: a recovery that silently skips is worse than one that refuses,
 * because the user cannot see what is missing. A host that sees the flag needs
 * a fresher snapshot, not a longer journal.
 *
 * A journal this build does not understand, or a truncated one, is REFUSED. The
 * events applied before the bad one stand — replay is not a transaction — so a
 * host that wants all-or-nothing replays onto a copy and keeps it on success. */
clay_result clay_document_replay_journal(clay_document* doc, const uint8_t* data, size_t size,
                                         size_t* out_applied,
                                         int32_t* out_stopped_at_barrier);

clay_result clay_add_sdf_layer(clay_document* doc, const char* name,
                               clay_layer_id* out_layer);

typedef struct clay_item_desc {
    uint32_t struct_size; /* = sizeof(clay_item_desc); required, see above */
    int32_t prim;        /* clay_prim */
    float params[7];     /* primitive parameters, see clay_prim comments */
    float position[3];
    float rotation[4];   /* quaternion x y z w (identity: 0 0 0 1) */
    float scale;         /* uniform; 0 means 1 */
    int32_t op;          /* clay_op */
    int32_t blend;       /* clay_blend */
    float blend_k;
    float rounding;
    float color[3];
    int32_t mirror;      /* layer-mirror participation: 0 and 1 follow the
                          * layer's mirror (the default — a zeroed field
                          * mirrors), -1 excludes this item from it. See
                          * clay_set_layer_mirror. */
} clay_item_desc;

/* Adds one edit from the flat descriptor — sugar over the item builder for
 * edits that need no modifier chain and no out-of-line payload. Primitives
 * that carry one (CLAY_PRIM_STROKE and the two lifts) and the transition ops
 * need parameters this descriptor cannot express; they are rejected with
 * CLAY_ERROR_INVALID_ARGUMENT, as is any unknown prim, op or blend value.
 * Use clay_item_create for those. */
clay_result clay_add_item(clay_document* doc, clay_layer_id layer,
                          const clay_item_desc* item, clay_node_id* out_node);
clay_result clay_remove_node(clay_document* doc, clay_layer_id layer, clay_node_id node);

/* -- undo -----------------------------------------------------------------
 * Opt-in per document: a document that never enables it behaves exactly as it
 * did before, and edits made before enabling are not undoable. Once enabled
 * every editing entry point records its own inverse, so no reachable edit
 * escapes undo. Nothing to undo is reported through *out_undone, not returned
 * as a failure, so a UI can drive the buttons without tracking state. */
/* Opt-in history. Unchanged in shape, and since ABI 0.43.0 it spans the SDF
 * edit list, VOXEL grids and MESH layers rather than the edit list alone.
 *
 * That is a behaviour change and a fix. Before it, a host that sculpted a voxel
 * layer and called clay_document_undo reversed an unrelated SDF edit, or was
 * told there was nothing to undo — because voxel edits and mesh displacements
 * are recorded by mechanisms the command stack never saw. One undo now takes
 * off the most recent edit whatever made it.
 *
 * CREATING A LAYER IS A STEP, of every kind: clay_add_sdf_layer,
 * clay_document_add_voxel_layer and clay_document_add_mesh_layer each record
 * one command. Through 0.55.0 the voxel one did not, so a conversion — make a
 * voxel layer, rasterize into it — recorded its FILL and not its layer, and one
 * undo emptied the new layer and left it standing. Undoing a voxel or mesh
 * layer's creation removes the layer and KEEPS its payload, which is what lets
 * a redo bring the content back rather than an empty layer; the payload is not
 * reachable while the layer is absent, and is not saved.
 *
 * WHAT IS STILL NOT A STEP, because nothing records it: creating a MASK — mask
 * EDITS record, since 0.47.0, but the mask's existence does not — and the
 * operations that destroy history itself (dropping a resolution level, removing
 * a sculpt layer, merging one down). Consolidate IS undoable and is worth
 * naming because it is the one most often assumed otherwise.
 *
 * The depths reported by clay_document_undo_state count steps that will
 * actually reverse something, so a host greying a menu item from one never
 * offers an undo that does nothing. */
clay_result clay_document_enable_undo(clay_document* doc);
clay_result clay_document_undo(clay_document* doc, int32_t* out_undone);
clay_result clay_document_redo(clay_document* doc, int32_t* out_redone);
/* The same two calls, plus the world-space INFLUENCE bound of what they
 * applied — the region to hand clay_brick_cache_mark_dirty.
 *
 * A host keeping a brick cache cannot work this out, and every way it might
 * try is worse than not knowing. The narrowest bound it can name from the
 * outside is clay_brick_cache_mark_dirty_layer's, so undoing one dab refills
 * the model; diffing the layer's nodes across the call (clay_layer_node_count
 * / clay_layer_node_at) catches adds and removes but NOT an in-place change —
 * an undone move, resize or colour edit keeps its node id — and under-dirtying
 * is the failure that leaves visibly stale bricks at a blend seam. The engine
 * holds the list of commands it applied; nothing outside it does.
 *
 * The bound is the union, over every command in the step, of what that command
 * targets before it is applied and after — which is what covers a move (two
 * ends), a removal (the node that is gone) and an add (the node that was not
 * there) without the caller knowing which it was. It may be LOOSER than what
 * changed and never tighter: a node inside a group reports its root ancestor's
 * bound, because the group's blend reaches past the child's own box, and an
 * edit to content shared by instanced layers reports the union over every
 * layer sharing it. A step that cannot change the field — a rename — reports
 * no bounds rather than the layer.
 *
 * The three states are clay_layer_node_influence_bound's, and they line up
 * with what mark_dirty takes:
 *   *out_has_bounds 0            nothing to dirty; out_min/out_max untouched
 *   1, *out_infinite 0           the finite box, ready for mark_dirty
 *   1, *out_infinite 1           unbounded — mark_dirty with both regions NULL
 * Any of the four bound out-pointers may be NULL. Nothing to undo reports
 * *out_undone 0 and no bounds, and is still CLAY_OK. */
clay_result clay_document_undo_bound(clay_document* doc, int32_t* out_undone, float out_min[3],
                                     float out_max[3], int32_t* out_has_bounds,
                                     int32_t* out_infinite);
clay_result clay_document_redo_bound(clay_document* doc, int32_t* out_redone, float out_min[3],
                                     float out_max[3], int32_t* out_has_bounds,
                                     int32_t* out_infinite);
/* One query for everything a UI needs to label its buttons. Any out pointer
 * may be NULL. The depths read 0 when undo is not enabled, which *out_enabled
 * distinguishes from an enabled-but-empty history. */
clay_result clay_document_undo_state(const clay_document* doc, int32_t* out_enabled,
                                     size_t* out_undo_depth, size_t* out_redo_depth);
/* Bracket a burst of edits so they undo as one step — ACROSS REPRESENTATIONS.
 *
 * The bracket spans the SDF edit list, voxel grids, masks and mesh layers, so
 * one gesture a host bracketed is one undo however many of them it touched.
 * Through 0.55.0 it bracketed the edit list alone and every other kind stayed
 * its own step, which is what made a crossing two undos: the layer went first
 * and the fill it contained second.
 *
 * A bracket that produced a single step is unchanged by the grouping, and an
 * operation nothing records stays its own step rather than being folded in —
 * so an undo is never offered across the horizon such an operation draws. */
clay_result clay_document_begin_undo_group(clay_document* doc);
clay_result clay_document_end_undo_group(clay_document* doc);

/* -- editing a placed node -------------------------------------------------
 * Everything below addresses an existing node by the id clay_layer_add_item
 * returned, and applies one command from the engine's vocabulary — the same
 * one the document format records — so a binding edit means exactly what a
 * saved document means. An id the document does not hold gives
 * CLAY_ERROR_NOT_FOUND and leaves the document byte-identical.
 *
 * Unlike the Python bindings, which take partial updates, these take the
 * whole value: C has no idiomatic "leave this one alone" argument. Read the
 * current state, change what you want, pass all of it back. */

/* Retransform a node. Takes a transform on the same terms as every other
 * transform in this ABI (see clay_mesh_transform): `position` and
 * `rotation_axis` are REQUIRED and the axis must be NON-ZERO, `scale` must
 * be > 0.
 *
 * A NULL axis is therefore CLAY_ERROR_INVALID_ARGUMENT and not "no rotation"
 * (#327). It reads like "no rotation", but the signature already says that —
 * any axis with rotation_angle 0, e.g. {0, 1, 0}, which is what
 * clay_layer_node_transform reads an unrotated node back as. Accepting NULL as
 * a second way to say it would mean a caller who only wanted to MOVE a node,
 * and passed NULL for the rotation they did not have, got a call that
 * succeeded and discarded the position too.
 *
 * A GROUP is refused: the engine composes layer * item and nothing else, so a
 * group's transform never reaches its children — recording one would be an
 * undoable, saved edit that changes nothing. Transform the children. */
clay_result clay_layer_set_transform(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float position[3], const float rotation_axis[3],
                                     float rotation_angle, float scale);
/* The same edit with a PER-AXIS scale (ABI 0.54.0, issue #320) — see
 * clay_item_set_scale_nonuniform for what it means and what it costs. The scale
 * is applied INNERMOST, in the node's own local frame.
 *
 * These two calls are ONE command and one undo step, and each writes the WHOLE
 * transform, which settles what the uniform one does to a node that carries a
 * per-axis scale: it COLLAPSES IT. clay_layer_set_transform means "this node's
 * scale is uniform s", because a call that took the whole transform and quietly
 * left one component of it alone would be the partial update this ABI does not
 * do. Read, change, write back — with the reader that matches. */
clay_result clay_layer_set_transform_nonuniform(clay_document* doc, clay_layer_id layer,
                                                clay_node_id node, const float position[3],
                                                const float rotation_axis[3], float rotation_angle,
                                                const float scale[3]);
/* Replace a node's primitive. Its deformers, repetition, profile and stroke
 * belong to the node, not to the primitive, and survive the edit. */
clay_result clay_layer_set_prim(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                int32_t prim, const float* params, size_t param_count);
clay_result clay_layer_set_color(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                 const float rgb[3]);
/* op is clay_op, blend is clay_blend; rounding must be >= 0. A GROUP takes the
 * same values clay_layer_add_group takes, CLAY_OP_INLINE included; an item
 * takes anything but that one. */
clay_result clay_layer_set_op_blend(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t op, int32_t blend, float blend_k, float rounding);
/* Reparent or reorder. new_parent 0 moves the node to the layer root, and
 * index < 0 appends. Moving a node into its own subtree is refused: it would
 * close a cycle, and the subtree would leave the root list and be dropped on
 * the next save. */
clay_result clay_layer_move(clay_document* doc, clay_layer_id layer, clay_node_id node,
                            clay_node_id new_parent, int32_t index);
/* Append (x, y, z, radius) quadruples to a placed stroke: points_xyzr holds
 * count * 4 floats. */
clay_result clay_layer_append_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float* points_xyzr, size_t count);
/* Remove the last count points from a placed stroke. */
clay_result clay_layer_trim_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   uint32_t count);
/* A GROUP is a node too, and every call above applies to one. Creating and
 * filling one is further down, with the item builder whose handle it takes:
 * see clay_layer_add_group. */

/* -- editing layers -------------------------------------------------------- */

/* Add a layer SHARING `source`'s edit list — a ZBrush-style duplicate subtool,
 * and the constructor the memory and dirty-bounds contracts above have been
 * describing since before anything could make one.
 *
 * Nothing is copied but the layer record, so the call costs the same on a
 * blockout of one item and on one of fifty thousand, and ten instances of a
 * blockout are ONE edit list in memory and ONE in the file. That is the whole
 * reason it exists: the alternative a host has without it is to replay or
 * deep-copy the source's edit list per copy, paying memory and time
 * proportional to everything the artist has already sculpted, ten times.
 *
 * WHAT IS SHARED is the edit list, and only that. An edit through EITHER layer
 * is an edit to the shared content and appears through both — that is what a
 * shared edit list means, and it is why the dirty bounds of such an edit report
 * the union over every layer sharing the content rather than the one layer you
 * named (see clay_layer_node_influence_bound). A host that dirties by what it
 * was told refills every instance; a host that dirties the named layer alone
 * leaves the other nine holding stale geometry with nothing to say so.
 *
 * WHAT IS NOT SHARED is everything else the layer carries: its transform, its
 * name, its visibility, its protection, its mirror and its radial mode. Those
 * are COPIED from the source at creation — an instance starts out looking like
 * what it was made from — and diverge from there. Placing the instance
 * somewhere else is what turns one edit list into two bolts; hiding it, ghosting
 * it or mirroring it touches the source not at all.
 *
 * ONE UNDO STEP, like every other layer creation: the add goes through the
 * command vocabulary, so a single clay_document_undo removes the instance and
 * leaves the source exactly as it was, and a redo brings it back still sharing.
 *
 * INSTANCING AN INSTANCE shares the same edit list rather than chaining. There
 * is one allocation and the layers over it are peers; a chain would invent a
 * parent layer whose later removal would have to mean something, and it means
 * nothing — see clay_document_remove_layer below.
 *
 * REMOVING THE SOURCE is legal while instances remain. The content is held by
 * every layer sharing it, so removing the layer that happened to be instanced
 * removes a placement and nothing else: the survivors still evaluate, still
 * save and still reload with their content. There is no "original" to lose.
 *
 * CONSOLIDATING AN INSTANCE severs it first — see clay_layer_consolidate.
 *
 * Refusals. `source` must exist (CLAY_ERROR_NOT_FOUND) and must be an SDF
 * layer: a voxel grid and a mesh are held beside the document by layer id
 * rather than behind the shared pointer an instance is, so a voxel or mesh
 * source is CLAY_ERROR_INVALID_ARGUMENT rather than a second kind of sharing
 * that would need its own memory contract. `name` follows
 * clay_document_set_layer_name: NULL and the empty string are refused, and
 * names are not unique — an instance may carry the source's own name. */
clay_result clay_document_instance_layer(clay_document* doc, clay_layer_id source,
                                         const char* name, clay_layer_id* out_layer);

/* Removing a layer that others INSTANCE removes that placement and nothing
 * else: the shared edit list is held by every layer sharing it, so the
 * survivors are untouched. See clay_document_instance_layer. */
clay_result clay_document_remove_layer(clay_document* doc, clay_layer_id layer);
/* Reorder a layer. The command vocabulary expresses this as remove-then-add,
 * so it is the one edit that is a pair rather than a single command. The add
 * NAMES the layer it shares its edit list with when there is one, so a
 * reordered instance survives a journal replay still sharing rather than
 * coming back as a deep copy — see clay_document_instance_layer. */
clay_result clay_document_move_layer(clay_document* doc, clay_layer_id layer, int32_t index);
/* A hidden layer contributes nothing to the field; showing it again restores
 * the original field exactly. */
clay_result clay_document_set_layer_visible(clay_document* doc, clay_layer_id layer,
                                            int32_t visible);
/* Rename a layer — the setter for the name clay_layer_name reads back. Through
 * 0.29.1 a layer was named by whichever call created it and nothing could
 * change it, so a host that let the artist rename a layer kept that name
 * beside the document and lost it on the next save (#92). clay_layer_name made
 * that visible rather than caused it: a reopened document now confidently
 * reports the stale creation name, which looks correct.
 *
 * A command like every other layer edit, so one rename is one undo step and a
 * ghosted or locked layer refuses it. A rename invisible to undo would be a
 * new inconsistency, not a fix.
 *
 * NULL and the empty string are refused: an empty name is what a cleared text
 * field submits, and the document's name is the only one left to lose. There
 * is no length limit — the saved layer record length-prefixes the name and
 * clay_layer_name sizes before it writes, so a long name costs the reader a
 * bigger buffer rather than a truncation.
 *
 * Names are NOT unique, here or at creation: clay_add_sdf_layer,
 * clay_document_add_voxel_layer and clay_document_add_mesh_layer each accept a
 * name another layer already carries, so refusing a duplicate here would buy a
 * guarantee the create calls do not keep. What a duplicate means is stated
 * instead: clay_document_voxel_layer and clay_document_mesh_layer answer with
 * the FIRST layer in stack order carrying the name, so renaming a voxel layer
 * onto a name already in use shadows the other layer's grid, and renaming it
 * away leaves the old name looking up nothing. Hold the id from creation or
 * from clay_document_layer_at when the lookup has to survive a rename — ids
 * are stable across save and load; names are not a key anything enforces.
 *
 * That advice is followable: clay_document_voxel_layer_by_id and
 * clay_document_mesh_layer_by_id reach a layer's grid or its geometry from the
 * id alone, so a duplicate name shadows nothing a host holds an id for. Through
 * 0.56.0 they did not exist and the name was the ONLY route back to a payload,
 * which is what pushed hosts into enforcing a uniqueness on voxel layers that
 * nothing here ever asked for (#365). */
clay_result clay_document_set_layer_name(clay_document* doc, clay_layer_id layer,
                                         const char* name);
/* Protection, both off by default. A GHOSTED layer is still evaluated but is
 * never picked and never edited: "show me this for reference, but stay out of
 * my way". A LOCKED layer is still picked but never edited: "this is
 * finished". Neither changes what the document evaluates to — how a host
 * DRAWS a ghost is its own business.
 *
 * Every edit naming a protected layer returns CLAY_ERROR_INVALID_ARGUMENT and
 * leaves the document unchanged. It is refused rather than silently dropped: a
 * host that greys the layer out wants to know, and one that does not must not
 * quietly discard the artist's work. This call itself is always allowed —
 * locking would otherwise be irreversible. */
clay_result clay_document_set_layer_protection(clay_document* doc, clay_layer_id layer,
                                               int32_t ghost, int32_t locked);
clay_result clay_document_layer_protection(const clay_document* doc, clay_layer_id layer,
                                           int32_t* out_ghost, int32_t* out_locked);
/* Place the whole layer. Same terms as clay_layer_set_transform: `position`
 * and `rotation_axis` are required, the axis must be non-zero, `scale` > 0.
 *
 * SETS THE WHOLE PLACEMENT, so it clears any per-axis scale the layer carried:
 * a placement is one value in this ABI, and a caller setting it means the whole
 * of it rather than "the parts this call can name". */
clay_result clay_document_set_layer_transform(clay_document* doc, clay_layer_id layer,
                                              const float position[3],
                                              const float rotation_axis[3],
                                              float rotation_angle, float scale);

/* -- a layer's PER-AXIS scale (ABI 0.74.0, issue #373) ----------------------
 *
 * The whole-layer half of clay_layer_set_transform_nonuniform, which has placed
 * a NODE per axis since 0.54.0. A ZBrush-style gizmo scales per axis — the
 * three boxes on the arms — on a placed object AND on a whole subtool, and a
 * layer that took one factor is why a host had to hide the boxes in scale mode
 * for the subtool case.
 *
 * COMPOSED INNERMOST in the layer's own frame, before its rotation and
 * translation, exactly as a node's is in its own:
 *
 *     world = layer.xform * diag(layer_scale) * node.xform * diag(node_scale)
 *
 * so a triple of (1, 1, 1) is the identity and a document that never calls this
 * compiles byte-identical tape.
 *
 * WHAT A NON-UNIFORM SCALE COSTS, and what it does not. The evaluated distance
 * is multiplied back by the product of the SMALLEST component of each per-axis
 * scale in the composition, which never overestimates the true distance — so
 * the field stays a conservative bound and stays 1-Lipschitz, and
 * clay_safe_step_scale does not move. What goes is exactness:
 * clay_tape_info's out_is_exact drops, as it does for any non-uniform scale.
 *
 * A world RADIUS mapped inward is divided by the LARGEST component instead —
 * the dual, so a gesture never reaches outside the region it named.
 * clay_layer_move_surface takes the layer's factor into that rule.
 *
 * clay_layer_bounds, the influence bounds, clay_document_raycast_attributed and
 * the placed mesh of a mesh layer all read the three factors. A mesh layer's
 * NORMALS go through the inverse transpose, as clay_mesh_transform_nonuniform
 * documents: rotating a normal is right for a similarity and tilts every one of
 * them off the surface under a squash.
 *
 * ONE THING IS REFUSED RATHER THAN APPROXIMATED. clay_layer_lattice_gizmo
 * returns no warps for a layer carrying a per-axis scale. A cage records its
 * item-to-cage placement as a rigid transform, and on a squashed layer the map
 * it needs is a general affine one — the layer's diagonal sits BETWEEN the two
 * placements. Placing a cage through the narrower record would warp every item
 * in a space it does not occupy, silently. A host that gets nothing back should
 * offer the uniform gizmo. */
clay_result clay_document_set_layer_transform_nonuniform(clay_document* doc, clay_layer_id layer,
                                                         const float position[3],
                                                         const float rotation_axis[3],
                                                         float rotation_angle,
                                                         const float scale[3]);

/* -- what a re-placement guarantees (ABI 0.82.0) ----------------------------
 *
 * Moving or rotating a whole layer changes no shape. A layer's transform
 * reaches the compiled tape only as a change of frame inside each item's
 * inverse matrix, plus a scale on rounding and on the cull gate — so for a
 * placement change that is RIGID the layer's surface afterwards is its surface
 * beforehand moved by one matrix, and for one that adds a UNIFORM scale the
 * field is the old field composed with the inverse and multiplied by the
 * factor. Layers combine by hard union, so no cross-layer term is re-solved
 * either: re-placing one layer leaves every other layer's field bit-identical.
 *
 * The engine does not act on this yet — the invalidation after a layer
 * transform is exactly what it always was — but a host CAN: it already holds
 * the layer's drawn surface, and a matrix multiply over that mesh is the same
 * result for a fraction of a refill's cost.
 *
 * A NON-UNIFORM layer scale classifies as GENERAL and nothing is claimed about
 * it. It changes the field's Lipschitz behaviour, which is why the per-axis
 * scale multiplies distances back by its smallest component. */
typedef enum clay_placement_kind {
    CLAY_PLACEMENT_RIGID = 0,      /* rotation and translation; distances unchanged */
    CLAY_PLACEMENT_SIMILARITY = 1, /* those plus a uniform scale; distances scale by it */
    CLAY_PLACEMENT_GENERAL = 2     /* anything else; no guarantee */
} clay_placement_kind;

typedef struct clay_placement_report {
    uint32_t struct_size; /* = sizeof(clay_placement_report); required */
    int32_t kind;         /* clay_placement_kind */
    /* The uniform factor from the layer's CURRENT placement to the proposed
     * one; 1 for RIGID. Left at 1 for GENERAL rather than at a value a caller
     * might multiply by. */
    float scale;
    /* Column-major affine matrix taking the OLD placement to the NEW one:
     * delta[0..3] is the first COLUMN, and delta[12..14] is the translation.
     * The layout a graphics API wants, and the one math::cfloat4x4 already
     * has, so a host uploads it without transposing. Identity for GENERAL. */
    float delta[16];
} clay_placement_report;

/* Classify a PROPOSED placement against the one the layer currently carries.
 *
 * A QUERY, not a variant of the setters: it changes nothing, so the existing
 * entry points keep their signatures and a host that never asks pays nothing.
 * Call it with the placement you are about to set — afterwards the old and the
 * new are the same placement and the answer is the identity, which is true and
 * useless.
 *
 * scale_axes may be NULL, meaning the identity (1, 1, 1). Passing a non-uniform
 * triple, or naming a layer that carries one, reports GENERAL.
 *
 * CLAY_ERROR_NOT_FOUND when no layer carries the id. Refused for the same
 * malformed arguments the setters refuse: a zero rotation axis, a scale that is
 * not positive and finite. */
clay_result clay_layer_placement_report(const clay_document* doc, clay_layer_id layer,
                                        const float position[3], const float rotation_axis[3],
                                        float rotation_angle, float scale,
                                        const float scale_axes[3],
                                        clay_placement_report* out_report);

/* -- a placement GESTURE, so a drag costs one refill (ABI 0.82.0) -----------
 *
 * A gizmo drag sets the layer's transform on every frame it moves, and every
 * one of those invalidates the layer's whole box and re-walks every brick in
 * it. Measured on a 1000-item layer over 5832 bricks: 95.7 ms a frame, against
 * 0.30 ms to transform the finished 120k-vertex mesh by the same matrix.
 *
 * Inside a gesture the document DOES NOT MOVE. update records the placement and
 * invalidates nothing; commit applies ONE command and takes ONE invalidation,
 * so sixty frames cost what one placement costs and undo gets a single step.
 *
 * HOW A HOST DRAWS THE FRAMES IN BETWEEN, since the document is not moving:
 * once, at the start of the gesture, evaluate the document with this layer
 * EXCLUDED and this layer ALONE — clay_brick_cache_eval_requests_excluding and
 * clay_brick_cache_eval_requests_layer, or clay_document_mesh_sdf_layer on the
 * mesh path. Draw the first where it is and the second under the gesture
 * matrix, which clay_layer_placement_preview hands back. Because layers combine
 * by hard union each surface is exact; what the preview cannot show is their
 * mutual occlusion where they overlap, and that resolves on commit.
 *
 * WHILE A GESTURE IS OPEN EVERY OTHER EDIT IS REFUSED with
 * CLAY_ERROR_INVALID_ARGUMENT, including edits to other layers. The gesture
 * holds no snapshot of the document — a snapshot of a 1000-item document per
 * drag is the allocation pattern a tablet's memory pressure punishes — so it
 * cannot reconcile an edit made underneath it, and refusing is the only answer
 * that cannot be quietly wrong. One gesture at a time, for the same reason.
 *
 * Destroying an open gesture cancels it. */
typedef struct clay_placement_tx clay_placement_tx;

/* Opens a gesture on a layer. NULL when the document already has one open, when
 * no layer carries the id, or when the layer is ghosted or locked — the same
 * refusal an edit to it would take, made at the open rather than at the commit
 * so a host does not draw a drag it cannot land. clay_last_error says which. */
clay_placement_tx* clay_layer_placement_begin(clay_document* doc, clay_layer_id layer);

/* The placement as it stands this frame. TOTAL, from the layer's placement when
 * the gesture opened — never an increment on the last frame, for the reason
 * clay_layer_move_surface takes a total: a chain of increments composes
 * rotations authored against different intermediate frames. */
clay_result clay_layer_placement_update(clay_placement_tx* tx, const float position[3],
                                        const float rotation_axis[3], float rotation_angle,
                                        float scale);

/* What to draw the layer under this frame, classified against the placement the
 * gesture OPENED with. Valid before any update, where it is the identity. */
clay_result clay_layer_placement_preview(const clay_placement_tx* tx,
                                         clay_placement_report* out_report);

/* Applies the last update as one command, with one invalidation and one undo
 * step, and closes the gesture. A commit with no update before it is a no-op
 * that still closes. The handle stays valid until destroyed. */
clay_result clay_layer_placement_commit(clay_placement_tx* tx);

/* Closes the gesture leaving the placement it opened with. Since the document
 * never moved, this restores nothing — it releases the guard. */
clay_result clay_layer_placement_cancel(clay_placement_tx* tx);

void clay_layer_placement_destroy(clay_placement_tx* tx);

/* Reading it back. Every out pointer is optional.
 *
 * The per-axis reader answers the PRODUCT of the layer's two scales, so a layer
 * placed through the uniform setter answers (s, s, s) rather than (1, 1, 1)
 * with the factor hidden somewhere the caller cannot see — which is what lets
 * one manipulator read this call and never branch.
 *
 * The single-factor reader REFUSES a layer carrying three different factors
 * with CLAY_ERROR_INVALID_ARGUMENT, exactly as clay_layer_node_transform
 * refuses a squashed node: one float cannot express three, the uniform factor
 * alone describes a differently-shaped subtool, and a read-change-write through
 * the uniform setter would round the artist's squash away. */
clay_result clay_document_layer_transform(const clay_document* doc, clay_layer_id layer,
                                          float out_position[3], float out_rotation_axis[3],
                                          float* out_rotation_angle, float* out_scale);
clay_result clay_document_layer_transform_nonuniform(const clay_document* doc, clay_layer_id layer,
                                                     float out_position[3],
                                                     float out_rotation_axis[3],
                                                     float* out_rotation_angle,
                                                     float out_scale[3]);
/* Symmetry. Each enabled axis reflects the layer's items through the plane
 * where that LOCAL coordinate is 0 (the layer transform moves the plane with
 * the layer), and every item participates: place a lump on one side and both
 * sides carry it, through evaluation, meshing and both raycast paths alike.
 * The mirror is a property of the layer that evaluation reads, not an edit
 * baked into the items — so it applies to items added before OR after this
 * call, and axes 0/0/0 turns it back off, restoring the unmirrored field.
 *
 * `mirror_k` is the Mirror Blend seam: 0 is a hard crease on the plane, and a
 * positive value smooth-blends an item with its own reflection over that
 * distance, welding the seam where an item crosses its mirror plane.
 *
 * Individual items opt OUT with mirror = -1 (clay_item_desc.mirror or
 * clay_item_set_mirror) — an asymmetric detail on an otherwise symmetric
 * layer. Through 0.27.3 the flag was an opt-IN whose default excluded every
 * item, which made this call a silent no-op unless each item also passed
 * mirror = 1 (#60); 1 is still accepted and means what it meant. Layers with
 * no mirror axes evaluate exactly as before, whatever the items' flags. */
clay_result clay_set_layer_mirror(clay_document* doc, clay_layer_id layer, int32_t axis_x,
                                  int32_t axis_y, int32_t axis_z, float mirror_k);
/* The layer's RADIAL symmetry: `count` copies of every participating item,
 * evenly spaced about the layer-local axis 0/1/2, with `radial_k` smoothing the
 * seam between neighbours exactly as mirror_k smooths the mirror seam.
 *
 * A count of 0 or 1 turns it off and restores the un-arrayed field. Like the
 * mirror this is a property of the layer that evaluation reads, not an edit to
 * the items: one node exists and the copies cannot drift from it. Participation
 * follows the SAME per-item flag as the mirror (clay_item_set_mirror), so an
 * asymmetric detail is excluded from a layer's symmetry once rather than once
 * per mode.
 *
 * An axis outside 0..2 or a negative blend is rejected rather than clamped.
 *
 * This is the MODE. clay_item_set_repeat_radial is the per-item MODIFIER, and
 * it stays the right tool for a large decorative array: this emits `count`
 * instances per item where the modifier folds the query point in O(2). */
clay_result clay_set_layer_radial(clay_document* doc, clay_layer_id layer, int32_t axis,
                                  int32_t count, float radial_k);

/* -- discovering layers ----------------------------------------------------
 *
 * The read half of the layer surface (#69). Everything a host can set about a
 * layer — name, visibility, protection, representation, stack position — is
 * readable back, so a document that comes back from clay_document_load comes
 * back whole rather than anonymous. Before this existed the only recourse was
 * probing ids against clay_layer_bounds, which recovers ids alone, in id
 * order — and id order is creation order, so a document reordered with
 * clay_document_move_layer EVALUATED differently after a reload.
 *
 * Reading is not editing: a ghosted, locked or hidden layer answers every
 * query below normally. */

/* How a layer carries its content. The values match the layer record's kind
 * byte in a saved document, and new kinds are appended, never renumbered. */
typedef enum clay_layer_representation {
    CLAY_LAYER_SDF = 0,   /* an edit tree (clay_add_sdf_layer) */
    CLAY_LAYER_VOXEL = 1, /* a sampled grid (clay_document_add_voxel_layer) */
    CLAY_LAYER_MESH = 2   /* imported triangles (clay_document_add_mesh_layer) */
} clay_layer_representation;

/* Count-then-index enumeration, in STACK ORDER — index 0 is the layer
 * evaluated first, exactly the index clay_document_move_layer takes. An index
 * of count or beyond is CLAY_ERROR_NOT_FOUND. Ids are stable across
 * save/load, so enumerate once and hold the ids; they are not dense — a
 * removal leaves a gap — which is why enumeration goes through an index
 * rather than the id space. */
clay_result clay_document_layer_count(const clay_document* doc, size_t* out_count);
clay_result clay_document_layer_at(const clay_document* doc, size_t index,
                                   clay_layer_id* out_layer);

/* Everything about one layer in one call. An OUTPUT descriptor: set
 * struct_size to the sizeof of the struct you compiled against and the
 * library fills what you declared (see descriptor struct versioning). */
typedef struct clay_layer_info {
    uint32_t struct_size;   /* = sizeof(clay_layer_info); required */
    clay_layer_id id;       /* the id queried, so the struct is self-contained */
    int32_t representation; /* clay_layer_representation */
    int32_t stack_index;    /* position in evaluation order; what
                             * clay_document_move_layer sets */
    int32_t visible;        /* what clay_document_set_layer_visible set */
    int32_t ghost;          /* both what clay_document_set_layer_protection */
    int32_t locked;         /* set; also clay_document_layer_protection */
    /* Appended in 0.58.0 with clay_document_instance_layer: a host compiled
     * against the older layout declares the older struct_size and does not
     * receive them. Both describe the SAME relation from opposite ends, and a
     * subtool panel drawing the link needs both — content_source alone marks
     * the following end and leaves the source indistinguishable from an
     * ordinary layer. */
    clay_layer_id content_source; /* the layer this one shares its edit list
                                   * with, or 0 when it owns it */
    uint32_t share_count;         /* how many layers hold this edit list; 1
                                   * when nobody instances it, and 0 for a
                                   * voxel or mesh layer, which holds none */
} clay_layer_info;

clay_result clay_document_layer_info(const clay_document* doc, clay_layer_id layer,
                                     clay_layer_info* out_info);

/* How `content_source` is decided, because after a reload the only thing that
 * distinguishes an instance is that two layers happen to hold one edit list,
 * and something has to name which of them it belongs to.
 *
 * The answer is the FIRST layer in stack order holding that content: it reports
 * 0, and every other holder reports its id. That is the same rule the saved
 * document uses to decide which layer's record carries the bytes, so what you
 * are told here is exactly what a save would write, and the pair survives a
 * save and reload unchanged.
 *
 * It also means there is nothing to dangle. Remove the layer that was
 * instanced and the first survivor becomes the owner — it starts reporting 0,
 * the others start reporting its id, and `share_count` falls by one. A host
 * redrawing its panel from this after any removal is always right; a host
 * caching "layer 4 is the original" is not, and never was, because the document
 * does not record an original. */



/* The layer's UTF-8 name by the size-query pattern clay_list_backends uses:
 * call with buffer == NULL to receive the required size (incl. NUL) in
 * *size; call again with an adequate buffer to fill it. A buffer that is too
 * small gets CLAY_ERROR_BUFFER_TOO_SMALL with the needed size in *size and
 * writes nothing. The name is a string rather than a clay_layer_info field
 * because it is the one layer property without a fixed size. What a layer is
 * called after creation is clay_document_set_layer_name's to say. */
clay_result clay_layer_name(const clay_document* doc, clay_layer_id layer, char* buffer,
                            size_t* size);

/* -- item builder ---------------------------------------------------------- */

/* An edit composed one call at a time, then appended to a layer. This is the
 * path to everything the flat descriptor cannot express: chained modifiers —
 * call order is application order, because deformers do not commute — and
 * variable-length payloads, which are COPIED into the builder, so the
 * caller's arrays need not outlive the call.
 *
 * A builder is independent of any document and may be added to a layer any
 * number of times. Every call below returns clay_result and records
 * clay_last_error() on failure, leaving the builder as it was. */

typedef struct clay_item clay_item; /* opaque */

/* Creates a builder for one primitive. params/param_count are the parameters
 * documented on that clay_prim value, and a wrong count is rejected;
 * CLAY_PRIM_STROKE takes none (its points are set separately) and a lift
 * takes one (half-depth or axis offset). Returns NULL on invalid input, with
 * the detail in clay_last_error(). Free with clay_item_destroy. */
clay_item* clay_item_create(int32_t prim, const float* params, size_t param_count);
void clay_item_destroy(clay_item* item);

clay_result clay_item_set_position(clay_item* item, const float position[3]);
/* Rotation as an axis (need not be normalized) and an angle in radians. */
clay_result clay_item_set_rotation(clay_item* item, const float axis[3], float radians);
clay_result clay_item_set_scale(clay_item* item, float scale);           /* uniform, > 0 */
/* A PER-AXIS scale (ABI 0.54.0, issue #320), multiplying the uniform one above
 * and applied INNERMOST — in the item's own local frame, before its rotation
 * and position place it. This is what makes a slot a squashed capsule and an
 * oval bolt hole a squashed cylinder without re-authoring the primitive: the
 * kinds that carry their own extents could say it at creation and never
 * afterwards, and the kinds that do not (a capsule, a cylinder, a torus) could
 * not say it at all.
 *
 * Every component must be > 0. A zero collapses the item onto a plane and has
 * no inverse; a negative one mirrors it, which the layer mirror already
 * expresses and which would silently flip the winding of a boolean.
 *
 * WHAT IT COSTS, and it is not what you would guess. A non-uniform scale is
 * not a similarity, so the result is no longer a true distance: the engine
 * evaluates at p / s and multiplies back by the SMALLEST component, which never
 * overestimates.
 *
 * NOTHING GETS SLOWER. The field stays 1-Lipschitz, so the Lipschitz bound and
 * clay_layer_safe_step_scale are UNCHANGED and a marcher takes the steps it
 * always did. What is lost is EXACTNESS — clay_tape_info's out_is_exact goes to
 * 0 — so the value becomes a BOUND on the distance rather than the distance,
 * short by at most the ratio of the largest axis to the smallest. That matters
 * to a consumer that reads the value AS a distance (offsetting by it, measuring
 * with it) and to nothing else.
 *
 * A uniform value here, the default (1, 1, 1) included, keeps the field exact
 * and compiles to identical tape.
 *
 * Not on clay_item_desc, deliberately: that struct is zero-filled by its own
 * contract, and a zeroed per-axis scale would have to mean (1, 1, 1) rather
 * than what it says. Compose it here. */
clay_result clay_item_set_scale_nonuniform(clay_item* item, const float scale[3]);
clay_result clay_item_set_op(clay_item* item, int32_t op);               /* clay_op */
clay_result clay_item_set_blend(clay_item* item, int32_t blend, float k); /* clay_blend, k >= 0 */
clay_result clay_item_set_rounding(clay_item* item, float rounding);     /* >= 0 */
clay_result clay_item_set_color(clay_item* item, const float rgb[3]);
/* Layer-mirror participation, the builder's spelling of clay_item_desc.mirror:
 * -1 excludes the item from the layer's mirror, 0 and 1 follow it (the
 * default). See clay_set_layer_mirror. */
clay_result clay_item_set_mirror(clay_item* item, int32_t mirror);

/* Appends one domain warp (clay_deform) to the item's chain: the local point
 * is warped by the first one added first. params/param_count are the kind's
 * own, listed on clay_deform; ease indexes an easing curve and is read by the
 * taper only. */
clay_result clay_item_add_deformer(clay_item* item, int32_t deform, const float* params,
                                   size_t param_count, int32_t ease);

/* Appends a CLAY_DEFORM_BEND_CURVE to the item's chain. Its own entry point
 * because a guide is not a fixed number of floats, which is the one thing the
 * call above cannot express.
 *
 * `guide_xyz` is point_count x, y, z triples in the item's LOCAL space, and
 * `point_type` (CLAY_POINT_*) is the smooth/sharp toggle — a guide is the same
 * kind of curve every other item takes, so it gets B-spline smoothing for free
 * and tessellates to the document's own curve tolerance.
 *
 * [t0, t1] is the item's span along local X that gets laid onto the guide. A
 * guide of fewer than two points, or one of zero length, is refused, as is
 * t0 == t1. */
clay_result clay_item_add_bend_curve(clay_item* item, const float* guide_xyz, size_t point_count,
                                     int32_t point_type, float t0, float t1);

/* Appends a CLAY_DEFORM_ALPHA to the item's chain. Its own entry point because
 * a stamp is not a fixed number of floats.
 *
 * `samples` is width * height values in [0, 1], ROW-MAJOR with u fastest, and
 * is COPIED — a caller may free its buffer as soon as this returns.
 *
 * The stamp covers a square of side `extent` in the plane through `centre`
 * whose normal is `direction`; `tangent` orients it in that plane and any
 * rough "up" will do, since it is re-orthogonalised (one parallel to
 * `direction` falls back to a derived axis rather than collapsing). `radius`
 * is where the influence ends: outside it the field is untouched EXACTLY,
 * which is what makes this a brush rather than a modifier.
 *
 * `centre`, `direction`, `tangent`, `extent` and `radius` ARE IN THE ITEM'S OWN
 * SPACE — like a bend curve's guide and a lattice's box, and for the same
 * reason: the deformer chain runs on the local point, after the item's inverse
 * transform. So they RIDE the transform. Move the item and the stamp moves with
 * it; scale it and the stamp scales; and a stroke's template alpha arrives in
 * every stamp's own frame, turned by that stamp's rotation and scaled by its
 * radius, which is what makes an alpha usable through the stroke engine at all.
 *
 * A HOST WITH A SURFACE HIT HAS A WORLD FRAME, not this one. The two coincide
 * only on an item at the identity transform. `brush::stamp_placement` derives
 * the world frame and `brush::placement_in` converts it into an item's; use
 * both, or a stamp lands where the artist did not click — and silently, since
 * a misplaced centre outside the radius does NOTHING and one inside it reads a
 * single clamped border texel and SMEARS.
 *
 * `amplitude` is how far the surface moves OUTWARD at a stamp value of 1, so
 * white is raised as it is in every sculpting package; a negative amplitude
 * carves.
 *
 * The Lipschitz bound is DERIVED from the samples, not declared: it comes from
 * the largest difference between adjacent samples over the world distance
 * between them. A flat stamp therefore costs nothing for having large values,
 * and a high-frequency one costs step scale honestly — read it back with
 * clay_document_safe_step_scale.
 *
 * Refused, leaving the item unchanged: a null `samples`, a width or height
 * below 2 (nothing to interpolate), a non-positive `extent` or `radius`, or a
 * `direction` with no length.
 *
 * THE ZERO DIRECTION IS NOT THE MESH BRUSH'S ZERO DIRECTION. There, all-zeroes
 * on `alpha_direction` means "the surface normal under the centre", which it
 * resolves by asking the mesh. An SDF item has no surface at authoring time to
 * ask, so the same spelling cannot mean the same thing here and is refused
 * rather than quietly becoming local +Z. */
clay_result clay_item_add_alpha(clay_item* item, const float* samples, int32_t width,
                                int32_t height, const float centre[3], const float direction[3],
                                const float tangent[3], float extent, float radius, float amplitude,
                                int32_t ease);

/* Appends a CLAY_DEFORM_LATTICE to the item's chain. Its own entry point
 * because a cage is not a fixed number of floats.
 *
 * min/max give the cage's box in the item's LOCAL space; nx, ny, nz are control
 * points per axis and must be in [2, 4]. `offsets_xyz` is nx*ny*nz x, y, z
 * triples in x-fastest order — index (i, j, k) at ((k*ny + j)*nx + i)*3 — or
 * NULL for an untouched cage, which is exactly the identity.
 *
 * The cap is four rather than the mesh lattice's 32 because this is evaluated
 * PER SAMPLE inside the raymarcher, at nx*ny*nz multiply-adds each time.
 *
 * Evaluation is trivariate Bernstein, so two per axis is exactly trilinear and
 * the corner control points are interpolated. A point outside the box travels
 * rigidly with the nearest part of the cage rather than being drawn onto it. */
clay_result clay_item_add_lattice(clay_item* item, const float min[3], const float max[3],
                                  int32_t nx, int32_t ny, int32_t nz, const float* offsets_xyz);

/* Repetition of the item, applied before the deformer chain (so an array
 * repeats the deformed shape). One per item: the last call wins.
 * counts == NULL gives an infinite grid, whose influence is unbounded and so
 * never culled; otherwise counts is the max cell index per axis and one
 * spacing serves all three. */
clay_result clay_item_set_repeat_grid(clay_item* item, const float spacing[3],
                                      const float counts[3]);
/* Circular array of count (>= 2) copies about Y, offset from the axis. */
clay_result clay_item_set_repeat_radial(clay_item* item, int32_t count, float offset);

/* The 2D profile of a lift primitive, with the parameters listed on
 * clay_profile; a lift with no profile set uses a unit circle. Polygons go
 * through the call below, which takes count (>= 3) x, y pairs in local space:
 * the outline is implicitly closed and filled by the even-odd rule, so
 * concave and self-intersecting outlines are allowed. */
clay_result clay_item_set_profile(clay_item* item, int32_t profile, const float* params,
                                  size_t param_count);
clay_result clay_item_set_profile_polygon(clay_item* item, const float* xy, size_t count);

/* Append a profile to a CLAY_PRIM_LOFT item. Two or more are required; they
 * are interpolated evenly along Z in the order added. `params` is the
 * profile's own parameter block as clay_item_set_profile takes it, and
 * `polygon_xy` (count*2 floats, or NULL) supplies the vertices when the
 * profile is CLAY_PROFILE_POLYGON.
 *
 * The profiles are not item parameters: a variable number of them cannot fit
 * in a fixed block, and a polygon's vertices are already out of line. */
clay_result clay_item_add_loft_profile(clay_item* item, int32_t profile, const float* params,
                                       size_t param_count, const float* polygon_xy,
                                       size_t polygon_count);

/* Point chain of a CLAY_PRIM_STROKE item: count points of x, y, z, radius,
 * replacing any previous chain. One point is a sphere and none contributes
 * nothing, as in the Python bindings. blend_k (>= 0) smooths consecutive
 * segments of the chain itself. */
clay_result clay_item_set_stroke_points(clay_item* item, const float* xyzr, size_t count);
/* The topology half of an armature: one parent index per node, where the nodes
 * are the points clay_item_set_stroke_points takes. A node whose parent is
 * itself is a root. Split in two because an armature IS a stroke plus a tree,
 * and the points already had a setter worth reusing.
 *
 * An index outside the node range, or a parent chain that closes a cycle, is
 * refused: a cycle would make the field depend on traversal order rather than
 * on the tree. */
clay_result clay_item_set_armature_parents(clay_item* item, const uint32_t* parents,
                                           size_t count);
/* The sign half of an armature: +1 or -1 per node, positive by default —
 * ZBrush's negative ZSphere. The field is the armature of the positive nodes
 * MINUS the armature of the negative nodes, each half built exactly as the
 * unsigned armature is, the carve applied after the whole positive fold. A
 * link exists only between two nodes of the SAME sign, so skin along a
 * negative node's links is never drawn (the membrane cut) and a carve never
 * sweeps a positive parent's radius (an eye-socket child does not swallow
 * the head). A negative node may carry children; they keep their own signs.
 *
 * An array shorter than the nodes reads as positive-padded, exactly as a
 * short parent array reads as roots. Any value other than +1 or -1 is
 * refused: a magnitude here would be the negative-radius convention this
 * feature deliberately did not take. */
clay_result clay_item_set_armature_signs(clay_item* item, const int8_t* signs, size_t count);
clay_result clay_item_add_stroke_point(clay_item* item, const float position[3], float radius);
/* Append one armature node under `parent`, the incremental authoring path that
 * clay_item_add_stroke_point is for a chain. `parent` < 0 continues from the
 * last node, which is what dragging a new sphere out of the previous one does.
 * Only armatures have children, so the name needs no qualifier. */
clay_result clay_item_add_child(clay_item* item, const float position[3], float radius,
                                int32_t parent);
clay_result clay_item_set_stroke_blend_k(clay_item* item, float k);

/* How a point joins the one after it. A stroke is a curve whose points are all
 * hard corners, so CLAY_POINT_HARD is both the default and exactly what a
 * chain authored before types existed means. */
typedef enum clay_point_type {
    CLAY_POINT_HARD = 0,   /* straight segment to the next point */
    CLAY_POINT_SPLINE = 1, /* Catmull-Rom, passing through the points */
    CLAY_POINT_BSPLINE = 2,/* uniform cubic B-spline; approximating, so it rounds corners */
    CLAY_POINT_BEZIER = 3  /* cubic shaped by the handles below */
} clay_point_type;

/* The typed form of the call above: `count` points of x, y, z, radius, plus
 * optional parallel arrays of count clay_point_type values and of count*3
 * floats for the incoming and outgoing Bezier handles. Passing NULL for an
 * optional array leaves that field at its default, so
 * clay_item_set_curve_points(item, xyzr, count, NULL, NULL, NULL) is exactly
 * clay_item_set_stroke_points.
 *
 * Handles are in the item's LOCAL space, relative to their point. 3DCoat keeps
 * its handles in screen space and its own users call that a wart: the curve
 * then means something different depending on where the camera was.
 *
 * Typed points are tessellated into the same segment chain at compile time, so
 * a curve costs nothing at evaluation time and no backend knows it exists. */
clay_result clay_item_set_curve_points(clay_item* item, const float* xyzr, size_t count,
                                       const int32_t* types, const float* in_handles_xyz,
                                       const float* out_handles_xyz);

/* Close the chain, and set the tessellation tolerance: the largest distance a
 * span's midpoint may sit from its chord, which must be > 0. Tolerance is a
 * property of the DOCUMENT, not of the viewer — two builds have to agree on
 * what a document means, so it is not a rendering setting.
 *
 * Also applies to a CLAY_PRIM_SWEPT item's guide, which is an ordinary curve.
 * A swept guide cannot be CLOSED, and asking for one is refused rather than
 * ignored: transporting a frame around a loop does not generally return it to
 * its starting orientation, and the leftover twist is real geometry. */
clay_result clay_item_set_curve(clay_item* item, int32_t closed, float tolerance);

/* Replace a placed stroke or curve's whole point list, through the command
 * vocabulary, so the edit is undoable and refused on a protected layer. A
 * curve is tens of points: a whole-list replace costs less than the
 * bookkeeping granular commands would need, and its inverse is exact.
 *
 * Also edits a CLAY_PRIM_SWEPT node's guide, which is the same list, and keeps
 * the two rules clay_layer_add_item enforces on one: closing it is refused for
 * the reason clay_item_set_curve refuses it, and so is cutting it below two
 * points, which would leave the sweep with nothing to follow. */
/* Tree edits for a placed armature. ONE undo step whatever the edit, because
 * the command underneath is a whole-tree replace — an armature is tens of
 * nodes, so that costs less than granular bookkeeping and its inverse is
 * exactly the tree that was there.
 *
 *   CLAY_ARMATURE_ADD_CHILD   `value` is the position, under `target`
 *   CLAY_ARMATURE_MOVE        `value` is a DELTA, and `target`'s whole subtree
 *                             moves with it — an arm hangs from a shoulder
 *   CLAY_ARMATURE_SET_RADIUS  `radius` on `target`
 *   CLAY_ARMATURE_DELETE      `target` and everything under it
 *   CLAY_ARMATURE_SET_SIGN    `radius` carries the sign, +1 or -1, on `target`
 *                             — anything else is refused. A new child is
 *                             always positive; this is what flips it.
 *
 * `mirrored` applies to add-child only: it adds the reflection through x = 0 in
 * the same step, under the mirror of the parent where there is one. A node on
 * the plane is its own reflection and is added once. */
#define CLAY_ARMATURE_ADD_CHILD  0
#define CLAY_ARMATURE_MOVE       1
#define CLAY_ARMATURE_SET_RADIUS 2
#define CLAY_ARMATURE_DELETE     3
#define CLAY_ARMATURE_SET_SIGN   4
clay_result clay_layer_armature_edit(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     int32_t op, uint32_t target, const float value[3],
                                     float radius, int32_t mirrored);

clay_result clay_layer_set_stroke_points(clay_document* doc, clay_layer_id layer,
                                         clay_node_id node, const float* xyzr, size_t count,
                                         const int32_t* types, const float* in_handles_xyz,
                                         const float* out_handles_xyz, int32_t closed,
                                         float tolerance);

/* Read a placed stroke or curve's point list back — the exact arguments the
 * call above takes, so what comes out goes straight back in. The item builder
 * is deliberately write-only: the host that filled it already knows what it
 * put there. A host that RELOADS a document knows nothing, and without this it
 * cannot edit a curve — or a swept tube's guide — it did not author.
 *
 * A CLAY_PRIM_ARMATURE node answers here too: its nodes are the same x, y, z,
 * radius list the setter shares with the stroke, so this call serves the
 * geometry half of a rig and clay_layer_armature_parents below the topology
 * half, mirroring the split on the setter side. An armature has no curve
 * settings, so out_closed reads 0 and out_tolerance the default it never
 * consults.
 *
 * Size-query pattern, as clay_cut_polygon_from_curve uses: call with
 * out_xyzr == NULL to receive the point count in *count, then again with a
 * buffer of count*4 floats. *count is the capacity in POINTS going in and the
 * count written coming out; a buffer that is too small gets
 * CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and writes
 * nothing. The three optional arrays are sized off that same count and each may
 * be NULL exactly as on the setter, but a size query takes none of them —
 * nothing sizes them yet, so passing one is refused rather than ignored.
 * out_closed and out_tolerance may be NULL and are written on the size query
 * too, so one call answers "how many points, and is it closed".
 *
 * The points come back AS AUTHORED, not tessellated: tessellation happens when
 * the document is compiled, so a readback is the control points and round trips
 * through the setter unchanged.
 *
 * Reading is not editing: a ghosted, locked or hidden layer answers normally,
 * because protection refuses edits. */
clay_result clay_layer_stroke_points(const clay_document* doc, clay_layer_id layer,
                                     clay_node_id node, float* out_xyzr, size_t* count,
                                     int32_t* out_types, float* out_in_handles_xyz,
                                     float* out_out_handles_xyz, int32_t* out_closed,
                                     float* out_tolerance);

/* The topology half of a placed armature, as clay_item_set_armature_parents
 * took it: one parent index per node, a root naming itself. A separate call
 * rather than another parameter on the reader above because the setters are
 * already split, and for the setters' own reason: an armature IS a stroke plus
 * a tree, and parents are the other half of a different primitive, not an
 * optional attribute a curve happens to lack. A node that is not an armature
 * is therefore CLAY_ERROR_INVALID_ARGUMENT.
 *
 * Same size-query pattern as the call above, counted in NODES: out_parents ==
 * NULL receives the node count in *count, and a buffer that is too small gets
 * CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and writes
 * nothing. The count is the one the xyzr readback reports — the two halves are
 * parallel arrays — and it holds even for a tree authored with fewer parents
 * than points: the missing tail reads back as roots, which is exactly the
 * reading compilation makes of it, so what comes back is always the tree the
 * document EVALUATES. For any tree the setter accepted whole, the round trip
 * is exact.
 *
 * The indices are the ones clay_layer_armature_edit takes, which is what makes
 * a reloaded rig posable at all: read the tree, pick the subtree, edit by
 * index. Reading is not editing: a ghosted, locked or hidden layer answers
 * normally. */
clay_result clay_layer_armature_parents(const clay_document* doc, clay_layer_id layer,
                                        clay_node_id node, uint32_t* out_parents,
                                        size_t* count);

/* The sign half of the same rig, by the same size-query pattern and the same
 * count: one +1 or -1 per node, parallel to the xyzr and parents readbacks.
 * Signs stored shorter than the nodes read back padded positive — the reading
 * compilation makes, so what comes back is the rig the document evaluates,
 * and a rig saved before signs existed reads back all-positive. A node that
 * is not an armature is CLAY_ERROR_INVALID_ARGUMENT; reading is not editing,
 * so a ghosted, locked or hidden layer answers normally. */
clay_result clay_layer_armature_signs(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id node, int8_t* out_signs, size_t* count);

/* Parameters of a spatial morph, in world space. Required by the matching
 * transition op and rejected with any other op: linear morphs along the
 * segment a -> b, radial over the XZ radius from r0 to r1. */
clay_result clay_item_set_transition_linear(clay_item* item, const float a[3],
                                            const float b[3], int32_t ease);
clay_result clay_item_set_transition_radial(clay_item* item, float r0, float r1, int32_t ease);

/* Appends the composed edit to a layer. The builder is left untouched. */
clay_result clay_layer_add_item(clay_document* doc, clay_layer_id layer, const clay_item* item,
                                clay_node_id* out_node);

/* -- groups ----------------------------------------------------------------
 * A group is a node like any other — same id space, same commands, same undo —
 * whose children compile as ONE sub-expression. That is what makes "intersect
 * A with B, then union that into C" sayable from a host: the intersect applies
 * to A alone because it is inside the group, and the group's own op combines
 * the result with whatever the layer already holds. Without one, an op applies
 * to the whole accumulated field, so an intersect meant for a single panel
 * would trim everything added before it.
 *
 * Three rules the compiler already enforces (src/scene/tape_build.cpp,
 * compile_group), stated here because a host has to be able to predict them:
 *   - a carving group (subtract, intersect, the extended modes) with nothing
 *     beneath it in the chain emits nothing at all — as a carving ITEM does;
 *   - so does a group whose children all turned out to be hidden or culled,
 *     down to the last instruction: an empty subtree is rolled back rather
 *     than left as a combine against empty space;
 *   - CLAY_OP_INLINE reads no blend, no rounding and no colour off the group,
 *     which is why those are refused rather than quietly ignored.
 *
 * A group has no transform of its own; see clay_layer_set_transform. */

/* Creates an empty group and returns its id. parent 0 puts it at the layer
 * root and index < 0 appends; a parent that is not a group is refused, which
 * is also how a nested group is built — pass the outer group's id.
 *
 * op is clay_op, blend is clay_blend, and blend_k and rounding must both be
 * >= 0. The transition ops are refused: compile_group emits no transition
 * parameters, so a group carrying one would morph on defaults nobody wrote.
 * CLAY_OP_INLINE requires CLAY_BLEND_HARD with blend_k and rounding 0.
 *
 * The seed colour a CLAY_OP_SHELL or CLAY_OP_REPLACE group paints when it
 * starts a chain against empty space is clay_layer_set_color, as for any other
 * node. */
clay_result clay_layer_add_group(clay_document* doc, clay_layer_id layer, clay_node_id parent,
                                 int32_t index, int32_t op, int32_t blend, float blend_k,
                                 float rounding, clay_node_id* out_node);

/* clay_add_item and clay_layer_add_item append to the layer root and have no
 * argument that could say otherwise, so these are those two calls with a group
 * to put the edit in. index < 0 appends. One command each, hence one undo step
 * each: filling a group needs no clay_document_begin_undo_group. */
clay_result clay_add_item_in_group(clay_document* doc, clay_layer_id layer, clay_node_id group,
                                   int32_t index, const clay_item_desc* item,
                                   clay_node_id* out_node);
clay_result clay_layer_add_item_in_group(clay_document* doc, clay_layer_id layer,
                                         clay_node_id group, int32_t index,
                                         const clay_item* item, clay_node_id* out_node);

/* A group's children, in order, by the size-query pattern
 * clay_layer_stroke_points uses: call with out_children == NULL to receive the
 * count in *count, then again with a buffer of that many ids. *count is the
 * capacity going in and the count written coming out; a buffer that is too
 * small gets CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and
 * writes nothing.
 *
 * A node that is not a group is CLAY_ERROR_INVALID_ARGUMENT — which is also
 * how a host that RELOADED a document tells a group from an item, since before
 * clay_layer_node_prim below nothing else in this ABI answered that question.
 * Reading is not editing, so a ghosted, locked or hidden layer answers
 * normally. */
clay_result clay_layer_children(const clay_document* doc, clay_layer_id layer,
                                clay_node_id node, clay_node_id* out_children, size_t* count);

/* Which primitive a placed item carries — clay_prim, the value
 * clay_item_create took. This is what lets a host that RELOADED a document
 * FIND its armature (or any other item worth a typed reader) instead of
 * probing every node against readers that refuse until one does not: ask what
 * the node is, then call the reader that applies.
 *
 * A group carries no primitive and is CLAY_ERROR_INVALID_ARGUMENT — the dual
 * of clay_layer_children refusing an item, so between the two calls every node
 * answers exactly one question. Reading is not editing: a ghosted, locked or
 * hidden layer answers normally. */
clay_result clay_layer_node_prim(const clay_document* doc, clay_layer_id layer,
                                 clay_node_id node, int32_t* out_prim);

/* -- what a placed node holds (ABI 0.53.0) ---------------------------------
 * The reading half of "editing a placed node" above. clay_layer_node_prim says
 * WHICH primitive a node carries; these three say where it stands, how big it
 * is and how it combines — the values a host that reloaded a document
 * otherwise has to have kept for itself, in a table beside the .clay keyed by
 * node id, and to have kept correct across undo and redo on its own.
 *
 * clay_layer_set_color is the one setter with no reader here; a host that
 * needs one should say so rather than keep the table alive for it.
 *
 * clay_layer_node_influence_bound is NOT the position answer, and a host that
 * reaches for it gets a wrong one: it is dilated by rounding and blend support,
 * and under a layer mirror it covers the reflection too, so an item placed at
 * x = 0.9 in a mirrored layer reports a bound centred on the origin.
 *
 * Each reader takes what its setter takes, so what comes out goes straight back
 * in. Every out-pointer is optional; a call that passes none of them still
 * validates the layer and the node, which is how a host asks "is this id still
 * a node of that layer" without a buffer. Reading is not editing: a ghosted,
 * locked or hidden layer answers normally. */

/* Position, rotation and scale, as clay_layer_set_transform takes them. A
 * GROUP is refused for the reason its setter refuses one — the compiler
 * composes layer * item and a group's transform reaches nothing.
 *
 * The node stores a quaternion, so the axis and angle are A representative of
 * that rotation rather than the exact pair last written: the angle comes back
 * in [0, pi] with the axis flipped when that is what it takes, which is the
 * same rotation by the other route. The axis is always unit length and never
 * zero — an identity rotation reads back as angle 0 about (0, 1, 0) rather
 * than about nothing — because clay_layer_set_transform refuses a zero axis
 * and a reader whose output its own setter rejects would not be a round trip.
 *
 * SINCE 0.54.0 A NODE CARRYING A PER-AXIS SCALE IS REFUSED HERE, with
 * CLAY_ERROR_INVALID_ARGUMENT and nothing written: one float cannot express
 * three, and the alternatives are all lies. Reporting the uniform factor alone
 * would hand back a placement that reads back a differently-shaped item, and a
 * host doing read-change-write through the uniform setter would silently round
 * off the artist's squash. That is the same failure #317 was filed about —
 * clay_layer_node_influence_bound answering a positional question it cannot
 * answer — and the lesson taken from it is that a reader which cannot express
 * what is there must not answer. Use clay_layer_node_transform_nonuniform,
 * which always can. A host that never sets a per-axis scale never meets this. */
clay_result clay_layer_node_transform(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id node, float out_position[3],
                                      float out_rotation_axis[3], float* out_rotation_angle,
                                      float* out_scale);

/* The same reading with the per-axis scale (ABI 0.54.0). Answers for EVERY
 * item, uniform or not: a node with a uniform scale s reports (s, s, s), so a
 * host that has one manipulator for both cases can call this one alone and
 * never branch. out_scale is the three factors clay_layer_set_transform_nonuniform
 * takes, so what comes out goes straight back in.
 *
 * A GROUP is refused as the uniform reader refuses one, for its setter's
 * reason: a group has no transform of its own. */
clay_result clay_layer_node_transform_nonuniform(const clay_document* doc, clay_layer_id layer,
                                                 clay_node_id node, float out_position[3],
                                                 float out_rotation_axis[3],
                                                 float* out_rotation_angle, float out_scale[3]);

/* The primitive's parameter block, as clay_layer_set_prim takes it, by the
 * size-query pattern clay_layer_children uses and counted in FLOATS: call with
 * out_params == NULL to receive the count in *count, then again with a buffer
 * of that many floats. *count is the capacity going in and the count written
 * coming out; a buffer that is too small gets CLAY_ERROR_BUFFER_TOO_SMALL with
 * the needed count in *count and writes nothing.
 *
 * The count is a property of the PRIMITIVE — the arity each CLAY_PRIM_*
 * comment above documents — so a caller that has just asked
 * clay_layer_node_prim what the node is does not also need a table of arities
 * to size a buffer. The primitives whose payload is out of line answer 0 and
 * are read by the typed reader that applies where there is one:
 * CLAY_PRIM_STROKE and CLAY_PRIM_ARMATURE through clay_layer_stroke_points.
 * A lift or a loft answers with its own parameters and not with its profiles,
 * exactly as its setter takes them.
 *
 * A group carries no primitive and is CLAY_ERROR_INVALID_ARGUMENT, as
 * clay_layer_node_prim is. */
clay_result clay_layer_node_params(const clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   float* out_params, size_t* count);

/* Op, blend profile, blend radius and rounding, as clay_layer_set_op_blend
 * takes them. out_op is a clay_op and out_blend a clay_blend.
 *
 * This one answers for a GROUP as well as an item, because a group carries an
 * op and a blend and its setter writes them. A group with CLAY_OP_INLINE reads
 * back the blend, radius and rounding it was required to be created with —
 * CLAY_BLEND_HARD and zeroes — since an inline group consults none of them. */
clay_result clay_layer_node_op_blend(const clay_document* doc, clay_layer_id layer,
                                     clay_node_id node, int32_t* out_op, int32_t* out_blend,
                                     float* out_blend_k, float* out_rounding);

/* The layer's TOP-LEVEL nodes, count-then-index, in the layer's EVALUATION
 * order — index 0 is the node evaluated first, and the index is the one
 * clay_layer_add_group and clay_add_item_in_group place against. An index of
 * count or beyond is CLAY_ERROR_NOT_FOUND, so a host walks to the end without
 * a sentinel; a layer id that is not a layer's is CLAY_ERROR_NOT_FOUND too.
 *
 * Top level ONLY, and it stops there on purpose: this is the sibling of
 * clay_layer_children, which continues to descend, so the whole tree is walked
 * by pairing the two — enumerate the layer's roots here, ask
 * clay_layer_node_prim what each one is, and recurse with clay_layer_children
 * through the ones it refuses as groups. A layer's root is not itself a group
 * and carries no node id, which is why clay_layer_children answers
 * CLAY_ERROR_NOT_FOUND for node 0 and this pair has to exist.
 *
 * Node ids are not dense — a removal leaves a gap, and nothing bounds how long
 * a gap can be — which is why enumeration goes through an index rather than
 * the id space. Probing ids upward against clay_layer_node_prim and tolerating
 * a run of misses is the guess this replaces: it loses every node past the
 * longest gap it happened to tolerate, and no value of "long enough" is
 * defensible.
 *
 * A layer with no SDF content — a voxel or a mesh layer — counts 0 rather than
 * failing, the same reading clay_layer_eval_points makes of it. Reading is not
 * editing: a ghosted, locked or hidden layer answers normally. */
clay_result clay_layer_node_count(const clay_document* doc, clay_layer_id layer,
                                  size_t* out_count);
clay_result clay_layer_node_at(const clay_document* doc, clay_layer_id layer, size_t index,
                               clay_node_id* out_node);


/* -- evaluation ------------------------------------------------------------ */

/* Comma-separated registered backend names via the size-query pattern:
 * call with buffer == NULL to receive the required size (incl. NUL) in
 * *size; call again with an adequate buffer to fill it. */
clay_result clay_list_backends(char* buffer, size_t* size);

/* One operation a backend either runs or does not. A GPU runtime builds one
 * pipeline per kernel and they fail independently, so what a backend can do is
 * a fact about the DEVICE rather than about this library — on Apple
 * Paravirtual GPUs the raycast kernel would not compile while the evaluation
 * kernels did.
 *
 * Batched forms are deliberately not here. A backend whose batch kernel is
 * unavailable loops over the single-item one and returns identical values, so
 * the batch is a speed question, not a capability. */
typedef enum clay_backend_op {
    CLAY_BACKEND_OP_EVAL_POINTS = 0,
    CLAY_BACKEND_OP_EVAL_GRID = 1,
    CLAY_BACKEND_OP_RAYCAST = 2,
} clay_backend_op;

/* Whether a REGISTERED backend can run one operation. *out_supported is 1 or 0.
 *
 * A backend that is not registered is CLAY_ERROR_NOT_FOUND rather than
 * "supports nothing": a backend that cannot do the thing and a backend that is
 * not there are different answers, and only the first is worth falling back
 * from. An unknown op is CLAY_ERROR_INVALID_ARGUMENT.
 *
 * Answerable at any time — no document, no device, no prior call. A host picks
 * a backend BEFORE it has built anything. */
clay_result clay_backend_supports(const char* backend, clay_backend_op op,
                                  int32_t* out_supported);

/* Why a backend is missing or partial, by the size-query pattern above.
 *
 * This is the call that makes clay_list_backends readable. Answering "cpu"
 * means either "no GPU backend is compiled into this build" or "one is, and
 * this machine's was discarded" — two states a host must act on differently
 * and, until this call, could not tell apart. It answers for a backend that is
 * NOT registered, which is its main use.
 *
 * The text is the runtime's own where there is any: a pipeline that failed to
 * compile contributes the compiler's log verbatim, because that is what
 * identifies the cause and a host filing a bug cannot recover what we already
 * discarded. It is diagnostic prose for a human, not a parseable status —
 * do not branch on it; branch on clay_backend_supports.
 *
 * An empty string is a successful answer meaning "nothing to report": either
 * the backend registered with every operation available, or this build does
 * not contain it. Those two are distinguished by the text itself — a backend
 * this build never compiled in says so. */
clay_result clay_backend_diagnostic(const char* backend, char* buffer, size_t* size);

/* Batch field evaluation. points_xyz is count*3 floats; out_distances is
 * count floats; out_colors_rgb (count*3) may be NULL. backend NULL = "cpu". */
clay_result clay_eval_points(const clay_document* doc, const char* backend,
                             const float* points_xyz, size_t count, float* out_distances,
                             float* out_colors_rgb);

/* Normalized field gradients (the tetrahedron trick, so they are unit-length
 * surface normals rather than raw differences) at the same points
 * clay_eval_points takes; out_gradients_xyz is count*3 floats. A call of its
 * own because a parameter cannot be appended to one that already shipped. */
clay_result clay_eval_gradients(const clay_document* doc, const char* backend,
                                const float* points_xyz, size_t count,
                                float* out_gradients_xyz);

/* The same two queries against one layer's own field: only that layer is
 * compiled, so an edit in a layer above cannot change the answer and a layer
 * can be probed while the stack around it is being authored. A layer that
 * shows nothing — hidden, or a voxel layer, which carries no SDF content —
 * evaluates as empty space rather than failing. */
clay_result clay_layer_eval_points(const clay_document* doc, clay_layer_id layer,
                                   const char* backend, const float* points_xyz, size_t count,
                                   float* out_distances, float* out_colors_rgb);
clay_result clay_layer_eval_gradients(const clay_document* doc, clay_layer_id layer,
                                      const char* backend, const float* points_xyz, size_t count,
                                      float* out_gradients_xyz);

/* -- the document WITHOUT one layer (ABI 0.74.0, issue #378) ----------------
 *
 * The third question, beside "the whole document" and "one layer": every
 * visible SDF layer EXCEPT the one named. They answer what the whole-document
 * calls would answer for a document that layer had been removed from.
 *
 * WHAT THIS IS FOR. A live sculpt transaction previews ONE layer — that is
 * what clay_sdf_smooth_preview_delta_take hands over, and it is the reason a
 * dab costs what it touches instead of what the artist has already made. A
 * host drawing only that is drawing the layer alone, and every other visible
 * field layer vanishes for the length of the gesture. With these, a host
 * evaluates the rest of the document ONCE at pointer-down — the layers it
 * excluded do not move while the artist drags — and composes that with its
 * live preview per frame.
 *
 * COMPOSING IS A MINIMUM, AND IT IS EXACT. Visible SDF layers hard-union, and
 * the union of two fields IS the smaller of the two distances, so
 *
 *     min(excluding(L) , your preview of L)
 *
 * is the field the whole document would evaluate to, not an approximation of
 * it. There is no blend parameter to match and no seam to hide.
 *
 * NEITHER CALL EDITS THE DOCUMENT, which is the other half of why they exist.
 * The route a host would otherwise take — hide the layer, sample the rest,
 * show it again — is three edits, and an edit taken between
 * clay_sdf_smooth_begin and its commit is one the commit correctly refuses.
 * These are safe to call at any point inside a transaction and record no undo
 * entry.
 *
 * AN UNKNOWN LAYER IS REFUSED with CLAY_ERROR_NOT_FOUND rather than read as
 * "exclude nothing". A host whose layer id went stale would otherwise be
 * handed the whole document and would draw the excluded layer twice — once
 * from here and once from its own preview — which is the exact defect these
 * calls exist to prevent, and it would look like a shading artefact rather
 * than a bug.
 *
 * A HIDDEN layer, or one carrying no SDF content, SUCCEEDS: it contributes
 * nothing to the union already, so excluding it is a no-op, and refusing would
 * make a host branch on state it has no reason to track. */
clay_result clay_eval_points_excluding(const clay_document* doc, clay_layer_id excluded,
                                       const char* backend, const float* points_xyz, size_t count,
                                       float* out_distances, float* out_colors_rgb);
clay_result clay_eval_gradients_excluding(const clay_document* doc, clay_layer_id excluded,
                                          const char* backend, const float* points_xyz,
                                          size_t count, float* out_gradients_xyz);

/* Multiply a field distance by this before stepping along a ray: the tape's
 * Lipschitz safety factor, which a scaled or displaced edit lowers.
 *
 * A CHAIN OF BRUSHES IS PRICED WHERE ITS LINKS CAN REACH (ABI 0.69.0, issue
 * #386). CLAY_DEFORM_GRAB, _MAGNIFY, _POSE, _BLOB and _ALPHA have finite
 * support — outside their own ball the field is untouched, which is what makes
 * them brushes rather than modifiers — so two whose balls are far apart have no
 * point at which both are anything but the identity, and their factors have
 * nothing to multiply. The bound is therefore the worst GROUP of links that can
 * reach one another, rather than the product of all of them.
 *
 * What that means for a host: a Move-heavy session no longer pays k^n for
 * nudging n different places on a form, which is the normal way a sculptor
 * works — a second drag on the same spot is a correction rather than a stroke.
 * Eight drags of radius 0.3 spaced 3.06 apart used to report 0.0616 here and
 * now report 0.7059, the number one drag reports. Drags PILED on one spot still
 * compound, because there they genuinely do.
 *
 * A deformer that acts everywhere — twist, bend, taper, a lattice, pose along a
 * line — is charged against every group, so nothing is saved by mixing one in.
 * clay_layer_consolidate is still the answer for a chain that has genuinely
 * degraded; this only stops charging for a degradation that is not there. */
clay_result clay_safe_step_scale(const clay_document* doc, float* out_scale);
clay_result clay_layer_safe_step_scale(const clay_document* doc, clay_layer_id layer,
                                       float* out_scale);

/* -- consolidating a degraded chain ----------------------------------------
 *
 * The region verbs — relax, flatten, snakehook, move, the mask brush — each
 * work once and none of them chain. A polish samples a document and hands back
 * a volume, so the second pass samples a VOLUME, and the declared Lipschitz
 * goes 1.7 -> 24 -> 39 over three passes. A move stroke never touches a volume
 * at all: each drag appends a grab to the deformer chain, and those multiply,
 * so the safe step scale decays by a constant factor per drag — 79x the
 * marching cost by nine.
 *
 * Two mechanisms, so the report names them separately. An aggregate step scale
 * says something is wrong; steepest_volume and steepest_deformer_chain say
 * which thing, and `degradation` names it outright.
 *
 * READ `degradation` BEFORE ACTING. `advises_consolidation` used to be
 * `safe_step_scale < advise_below_step_scale` and nothing else, and a grab
 * chain lowers the step scale exactly as a stack of baked volumes does — so
 * the advisory fired hardest on the case consolidation cannot help. It is now
 * keyed on the mechanism and is FALSE for a layer whose degradation is all
 * deformer chain with nothing to absorb, where the bake is a straight loss
 * (issue #387). There is no cure to offer there; the layer is parametric and
 * cheap per sample, and it is the marching that costs.
 *
 * The trigger is ADVISORY. Nothing here bakes on its own: consolidating
 * discards the parameters of everything it absorbs, and an engine that decided
 * on an artist's behalf that a sphere's radius is no longer editable would be
 * making the wrong person pay. `advise_below_step_scale` is the CALLER's
 * tolerance for marching cost, passed per call rather than stored in the
 * document, because that tolerance belongs to a viewport, a device and a frame
 * budget rather than to the artwork. */
/* Which mechanism is costing the marcher, and therefore which cure applies.
 * The two are NOT interchangeable (ABI 0.70.0, issue #387). */
typedef enum clay_degradation {
    CLAY_DEGRADATION_NONE = 0, /* the step scale is within the caller's tolerance */
    /* A stack of baked volumes, or a long edit list. Consolidation is the
     * cure: it absorbs the list and redistances the samples. */
    CLAY_DEGRADATION_VOLUMES = 1,
    /* A chain of brushes on a layer with nothing to absorb. Consolidation is
     * NOT the cure and measured 6x WORSE on a real gesture: it swaps a cheap
     * analytic item for a dense volume, and the marching win — a 29x better
     * step scale — is swamped by what the volume costs per sample. Leave the
     * layer parametric. */
    CLAY_DEGRADATION_DEFORMERS = 2,
    CLAY_DEGRADATION_BOTH = 3
} clay_degradation;

typedef struct clay_field_report {
    uint32_t struct_size; /* = sizeof(clay_field_report); required */
    float lipschitz;      /* the compiled layer's declared bound */
    float safe_step_scale;
    float steepest_volume;         /* largest sample Lipschitz among volume items */
    int32_t longest_deformer_chain;
    int32_t item_count;
    int32_t advises_consolidation; /* degraded AND consolidation is the cure */
    /* Appended in ABI 0.70.0. A caller compiled against the older struct passes
     * the older struct_size and never sees these; the fields below are only
     * written when the size covers them. */
    /* The deformer mechanism's own FACTOR, which longest_deformer_chain is
     * not: a chain of four gentle warps and a chain of one deep grab are the
     * same LENGTH and cost the marcher nothing alike. Weigh this against
     * steepest_volume — both are factors — rather than the count against it. */
    float steepest_deformer_chain;
    /* Of item_count, the nodes that contribute a field. A group is a transform
     * and a name; it is not evaluated, so it is not an edit list to win back. */
    int32_t drawable_count;
    int32_t degradation; /* clay_degradation */
} clay_field_report;

/* What a layer's chain currently costs the marcher, and what is causing it.
 * Pass 0 for advise_below_step_scale to measure without asking for advice. */
clay_result clay_layer_field_report(const clay_document* doc, clay_layer_id layer,
                                    float advise_below_step_scale,
                                    clay_field_report* out_report);

typedef struct clay_consolidation_params {
    uint32_t struct_size; /* = sizeof(clay_consolidation_params); required */
    /* Required, and > 0. A document has no intrinsic scale to derive a
     * resolution from the way a mesh's own bounds give one, so guessing here
     * would fix the shape's resolution at a number nobody chose. */
    float cell_size;
    float band;    /* half-width of the band kept; <= 0 means three cells */
    float padding; /* past the layer's bounds; <= 0 means the band */
    /* Redistancing — replacing the baked samples with the distance to their
     * own zero set — is what actually bounds the Lipschitz. Baking alone does
     * NOT: resampling a steep field reproduces the steepness, and a finer cell
     * makes it worse rather than better. Spelled as a SKIP so that a zeroed
     * struct gets the sound behaviour rather than the fast one. */
    int32_t skip_redistance;
} clay_consolidation_params;

/* What consolidating spends, from what a volume already reports. */
typedef struct clay_consolidation_cost {
    uint32_t struct_size; /* = sizeof(clay_consolidation_cost); required */
    float cell_size;
    float band;
    uint64_t brick_count;
    uint64_t sample_count;
    uint64_t bytes;
    float sample_lipschitz; /* how fast the stored samples vary */
    float lipschitz;        /* what the compiler will declare for them */
    float safe_step_scale;
    float bounds_min[3];
    float bounds_max[3];
} clay_consolidation_cost;

/* What consolidating this layer WOULD cost, without the document changing.
 *
 * The numbers are the ones the real thing produces, because this is the real
 * thing with the result thrown away: an estimate that skipped the sampling
 * could not report a brick count, and the brick count is where the memory is.
 * A caller that means to go ahead should call clay_layer_consolidate and read
 * the same cost out of it rather than paying for two bakes.
 *
 * `region_min`/`region_max` are an optional world-space box, or NULL for the
 * layer's own bounds padded. Pin it when consolidating the SAME region
 * repeatedly: a volume's geometric bound is its whole sampled box, so each
 * bake would otherwise pad the previous padding.
 *
 * The document is not changed, and that includes an INSTANCE layer's sharing:
 * clay_layer_consolidate severs a shared edit list before it bakes, and this
 * call does not, because it does not bake. Asking what a bake would cost must
 * never be the thing that unlinks a subtool. */
clay_result clay_layer_consolidation_cost(const clay_document* doc, clay_layer_id layer,
                                          const clay_consolidation_params* params,
                                          const float region_min[3], const float region_max[3],
                                          clay_consolidation_cost* out_cost);

/* Collapse a layer's edit list into one item carrying samples, as ONE undo
 * step whose inverse restores what it absorbed — ids, parameters, colours and
 * deformers intact, because the undo record carries the removed subtrees by
 * value. No new command was needed for this; the vocabulary could already say
 * it, which is why this is a policy rather than a verb.
 *
 * ON AN INSTANCE THIS SEVERS. A layer created by clay_document_instance_layer
 * shares its edit list, and a bake replaces an edit list — so before baking,
 * a layer sharing its content is given a PRIVATE copy of it and the bake
 * lands on that copy alone. Every other layer over the shared content keeps
 * its items and stays parametric.
 *
 * That is the only sound reading of the gesture. A bake says "this shape is
 * finished", which is a statement about ONE subtool; there is no reading under
 * which baking the tenth instance should turn the other nine into volumes,
 * and doing it in place would have done exactly that, silently. Refusing was
 * the alternative and is worse: an artist who duplicates a subtool would find
 * that neither copy can be baked any more, including the one they started
 * from, for a reason nothing in the UI can explain.
 *
 * The cost of severing is that the layer stops following the source, which is
 * what "finished" means, and it is VISIBLE: clay_document_layer_info reports
 * the link gone. The sever is part of the same undo step as the bake, so one
 * undo puts back both the items and the sharing.
 *
 * Refused on a protected layer, and refused BEFORE the bake, so a locked layer
 * does not cost a full resampling to say no.
 *
 * What survives: the surface, at `cell_size`. What does not: every parameter
 * of every item absorbed, and every colour but the first one's. Hidden items
 * are left alone — they contribute nothing to the field, so absorbing them
 * would spend their parameters on nothing.
 *
 * `out_cost` may be NULL. Undo grouping is the document's own, so this lands
 * as a single step when undo is enabled and as a plain edit when it is not.
 *
 * THE SAMPLING IS ON THE CPU, AND TAKES NO BACKEND. Every call in this header
 * that bakes a field to a lattice — this one and its _cancellable form,
 * clay_layer_consolidation_cost, clay_layer_consolidate_region,
 * clay_sdf_smooth_begin and clay_sdf_move_begin —
 * evaluates through the CPU backend's reference arithmetic, spread across its
 * thread pool. There is no parameter to route it elsewhere and no registered
 * backend it will pick up: a bake lives in a layer this library keeps below
 * the one that names backends, so it is handed an evaluator rather than
 * choosing one, and the evaluator it is handed is the CPU's.
 *
 * That is a DESIGN STATEMENT, not an omission to work around. A baked volume
 * is content the document then carries, so it must not depend on which GPU the
 * machine has, and byte-identity with the serial walk is the contract the pool
 * is held to. What a device backend does or does not agree with cannot reach
 * this result, in either direction: a bake taken on a Metal machine and the
 * same bake taken on a Linux box produce the same samples from the same
 * document. Evaluating that volume AFTERWARDS is where a backend enters, and
 * clay_eval_points, the parity suite and clay_backend_supports are how that
 * half is asked about.
 *
 * IT DOES CHANGE THE MARCH, THOUGH, ON EVERY PLATFORM EQUALLY. A lattice is
 * only a BOUND on the distance to its own surface, so a sampled volume declares
 * sqrt(3) times its samples' Lipschitz and this layer's safe step scale falls
 * from 1.0 to at most 0.577 — roughly three times the sphere-trace steps for
 * the same shape. See clay_sdf_smooth_commit for the measured numbers and for
 * what it means to a host with a fixed step budget. */
clay_result clay_layer_consolidate(clay_document* doc, clay_layer_id layer,
                                   const clay_consolidation_params* params,
                                   const float region_min[3], const float region_max[3],
                                   clay_consolidation_cost* out_cost);

/* -- merging a bake into a REGION of a layer (ABI 0.73.0, issue #390) ------- */

/* What a region merge would absorb, and over what box. */
typedef struct clay_region_merge {
    uint32_t struct_size; /* = sizeof(clay_region_merge); required */
    float box_min[3];     /* the closure: what gets sampled */
    float box_max[3];
    uint64_t absorbed;    /* how many roots it takes */
    int32_t whole_layer;  /* the closure reached every visible root */
} clay_region_merge;

/* Bake a REGION of a layer into one volume and put it back where the items it
 * absorbed were, leaving everything outside parametric.
 *
 * clay_layer_consolidate collapses the whole layer. That is the right scope for
 * a chain that has genuinely degraded and the wrong one for what a sculptor
 * does, which is work a PATCH. A host applying a region bake per gesture could
 * only append a volume each time — so every later bake sampled all the earlier
 * ones — or collapse the entire subtool and lose the parameters of items
 * nowhere near the stroke. Measured on a real form, twelve gestures on one
 * patch: 22 ms and 2 items at the first, 244 ms and 13 at the twelfth.
 *
 * `region_min`/`region_max` say where you worked. What gets SAMPLED is the
 * INFLUENCE CLOSURE of that: the region grown until every item that can reach
 * inside it is wholly inside it. That is not the same as "the items overlapping
 * the region", and the difference is not cosmetic — absorb only those and a
 * Subtract straddling the edge stays behind, the material it had carved comes
 * back, and the volume cannot take it away again. At the closure no remaining
 * item contributes inside the box at all, which is what makes the bake the
 * whole answer there and leaves the field outside it untouched.
 *
 * The closure may swallow the layer — one item reaching everywhere pulls in all
 * the rest — and then this IS clay_layer_consolidate. That is the honest
 * fallback, not a failure; `whole_layer` in the report says when it happened.
 *
 * WHAT IT BUYS: the second gesture on a patch has the first gesture's volume
 * inside its closure, so it is absorbed rather than stacked on. A patch stays
 * at ONE baked item however many times it is worked — O(1) in gestures where
 * appending was O(n).
 *
 * `params.region` is ignored; the closure replaces it. `out_merge` may be NULL.
 * Refused, with the document unchanged, on a missing, non-SDF, protected or
 * empty layer, and when the region reaches nothing at all. */
clay_result clay_layer_consolidate_region(clay_document* doc, clay_layer_id layer,
                                          const float region_min[3], const float region_max[3],
                                          const clay_consolidation_params* params,
                                          clay_consolidation_cost* out_cost,
                                          clay_region_merge* out_merge);

/* What that call WOULD absorb, without baking anything — so a host can show the
 * region whose parameters are about to be lost before the artist commits. */
clay_result clay_layer_plan_region_merge(const clay_document* doc, clay_layer_id layer,
                                         const float region_min[3], const float region_max[3],
                                         clay_region_merge* out_merge);

/* The same, cancellable (add-operation-cancellation, ABI 0.45.0).
 *
 * A SECOND ENTRY POINT rather than a parameter on the first, because adding a
 * parameter would break every host already compiled against it — and the whole
 * point of a token is that a host who does not want one is unaffected. Same
 * shape clay_mesh_validation_report has to clay_mesh_validate: the older call
 * is sugar over this one with a null token, so there is one implementation
 * rather than two that could drift.
 *
 * `token` may be NULL, which is exactly the older call. A cancelled consolidate
 * returns CLAY_ERROR_CANCELLED and leaves the document BYTE-IDENTICAL: the bake
 * builds a volume and installs it at the end, so a cancel is a discard rather
 * than a partial commit, and a host never has to undo one. */
clay_result clay_layer_consolidate_cancellable(clay_document* doc, clay_layer_id layer,
                                               const clay_consolidation_params* params,
                                               const float region_min[3],
                                               const float region_max[3],
                                               clay_consolidation_cost* out_cost,
                                               clay_cancel_token* token);

/* Whether a layer is consolidated — its edit list is a single item carrying
 * samples — and at what resolution, so a host can stop offering parameter
 * edits there rather than failing them.
 *
 * Answered from the CONTENT, not from a stored provenance flag. The promise a
 * host makes is about what the region IS: samples at a fixed resolution, with
 * no parameters to offer. A mesh imported with clay_item_volume_from_mesh is
 * exactly as unparametric as a bake, so a flag marking one of them would split
 * two cases an app has to treat alike — and it would have to be serialised to
 * survive a save.
 *
 * *out_consolidated is 0/1; out_cost may be NULL and is left untouched when
 * the layer is not consolidated. */
clay_result clay_layer_consolidation_state(const clay_document* doc, clay_layer_id layer,
                                           int32_t* out_consolidated,
                                           clay_consolidation_cost* out_cost);

/* Single raycast (origin + normalized direction). *out_hit is 0/1. */
clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]);

/* Batch raycast. rays_origin_dir is count*6 floats, an origin xyz then a
 * direction xyz per ray; unlike clay_raycast the directions are normalized
 * here, so a zero-length one is rejected rather than traced. Each out buffer
 * may be NULL and holds count values, or count*3 for position and normal. */
clay_result clay_raycast_many(const clay_document* doc, const float* rays_origin_dir,
                              size_t count, int32_t* out_hits, float* out_t,
                              float* out_positions_xyz, float* out_normals_xyz);

/* -- picking --------------------------------------------------------------- */

/* Raycast that also reports WHAT was hit: the layer and the item whose field
 * is closest at the hit point, so a subtract item is attributed the surface it
 * carved. *out_layer and *out_node are 0 when nothing attributes, and 0 is
 * never a valid layer or node id. The direction is normalized here.
 *
 * This is not the cheap path — it compiles the document, then one tape per
 * layer and one per candidate item. Use clay_raycast when only the position
 * and normal are wanted. */
clay_result clay_raycast_attributed(const clay_document* doc, const float origin[3],
                                    const float dir[3], int32_t* out_hit, float* out_t,
                                    float out_position[3], float out_normal[3],
                                    clay_layer_id* out_layer, clay_node_id* out_node);

/* Snap count points onto the nearest surface by gradient descent.
 * out_positions_xyz and out_normals_xyz are count*3 floats, out_ok is count
 * flags, and each may be NULL. A point that did not converge still reports the
 * best position and outward normal found, with its flag 0. */
clay_result clay_snap_to_surface(const clay_document* doc, const float* points_xyz,
                                 size_t count, float* out_positions_xyz,
                                 float* out_normals_xyz, int32_t* out_ok);

/* Tight world-space bounds of a layer's content, with no blend or rounding
 * dilation — the box to frame a camera on. *out_has_bounds is 0 when the layer
 * shows nothing, and out_min/out_max are then left alone.
 *
 * ANSWERS FOR ALL THREE REPRESENTATIONS since 0.52.3 (issue #318). It used to
 * walk a layer's SDF shapes only and report nothing for a voxel or mesh layer
 * however much material it held — which is not a tight bound, it is a wrong
 * one: a mesh's vertices ARE a box and a grid says where it is itself. A voxel
 * layer answers from its occupied cells (a cell is a box, so the far corner
 * takes the whole last cell) and a mesh layer from its vertices, both composed
 * with the layer transform exactly as the SDF arm composes it.
 *
 * A layer holding no material still reports 0 — an empty grid is genuinely
 * nowhere, which is a different answer from "this kind cannot say". */
clay_result clay_layer_bounds(const clay_document* doc, clay_layer_id layer, float out_min[3],
                              float out_max[3], int32_t* out_has_bounds);
/* The same box for a subset of the layer's nodes — zoom to selection. nodes is
 * count node ids; an id the layer does not hold contributes nothing, so a
 * selection of only such ids reports no bounds rather than an error. */
clay_result clay_layer_selection_bounds(const clay_document* doc, clay_layer_id layer,
                                        const clay_node_id* nodes, size_t count,
                                        float out_min[3], float out_max[3],
                                        int32_t* out_has_bounds);

/* -- meshing (owner-handle pattern) ---------------------------------------- */

typedef struct clay_mesh clay_mesh; /* opaque */

/* Which mesher runs. No engine enumeration stands behind these — the meshers
 * are three separate entry points — so the value is checked against this list
 * and an unknown one is rejected rather than mapped onto the default. Marching
 * is 0 because that is what a caller whose struct_size predates the field
 * gets, and it is what every caller got before the field existed. */
typedef enum clay_mesher {
    CLAY_MESHER_MARCHING = 0,       /* watertight and 2-manifold by construction */
    CLAY_MESHER_NETS = 1,           /* surface nets: preview speed, NOT manifold */
    CLAY_MESHER_DUAL_CONTOURING = 2 /* sharp features; experimental, see below */
} clay_mesher;

typedef struct clay_mesh_params {
    uint32_t struct_size;   /* = sizeof(clay_mesh_params); required, see above */
    float voxel_size;       /* world units per cell; <= 0 picks from resolution */
    int32_t resolution;     /* used when voxel_size <= 0: cells across the largest extent */
    int32_t decimate;       /* 0/1 */
    float decimate_ratio;   /* target triangle ratio when decimate != 0 */
    /* appended in ABI 0.3.0, after the original layout; both default to 0 */
    int32_t mesher;         /* clay_mesher */
    int32_t experimental;   /* 0/1: the opt-in CLAY_MESHER_DUAL_CONTOURING needs */
} clay_mesh_params;

/* Meshes the document's SDF content. CLAY_MESHER_DUAL_CONTOURING is not
 * hardened — plain dual contouring is not guaranteed manifold — so it is
 * reachable only with experimental set; without it the call returns
 * CLAY_ERROR_INVALID_ARGUMENT rather than meshing, which is the refusal the
 * Python bindings make in the same place. */
clay_result clay_document_mesh(const clay_document* doc, const clay_mesh_params* params,
                               clay_mesh** out_mesh);

/* Mesh ONE SDF layer, in world space under that layer's transform (ABI 0.82.0).
 *
 * The mesh-path sibling of clay_brick_cache_eval_requests_layer, and it exists
 * for the drag case: a host previewing a layer placement draws the rest of the
 * document once and this layer once, then moves this one under the gesture
 * matrix. Layers combine by hard union, so each surface is exact on its own.
 * What a preview drawn that way does NOT show is the mutual occlusion of the
 * union where the two overlap, which resolves when the gesture commits.
 *
 * NOT clay_document_mesh_layer, which BORROWS an imported MESH layer's
 * triangles and does not run a mesher at all. The names are close because the
 * two calls are about the two things a "layer" can be; this one is refused with
 * CLAY_ERROR_INVALID_ARGUMENT for a layer that is not an SDF layer, and that
 * one is refused for a layer that is.
 *
 * A HIDDEN layer meshes by name. Visibility decides what the DOCUMENT
 * evaluates to, and a caller naming one layer has already said which it wants —
 * refusing would leave a host unable to draw the ghost it is dragging. */
clay_result clay_document_mesh_sdf_layer(const clay_document* doc, clay_layer_id layer,
                                         const clay_mesh_params* params, clay_mesh** out_mesh);
/* Frees a mesh the caller owns. A mesh BORROWED from a document layer (see
 * clay_document_mesh_layer) is owned by the document, and destroying one is a
 * no-op that leaves the document intact. It is a silent no-op rather than the
 * reported refusal clay_voxel_grid_destroy gives because this call returns no
 * status, and changing its signature would break every existing consumer for a
 * case that could not previously arise. */
void clay_mesh_destroy(clay_mesh* mesh);

size_t clay_mesh_vertex_count(const clay_mesh* mesh);
size_t clay_mesh_index_count(const clay_mesh* mesh);
/* Borrowed pointers, valid until clay_mesh_destroy. normals/colors may be
 * NULL when absent. */
const float* clay_mesh_positions(const clay_mesh* mesh);
const float* clay_mesh_normals(const clay_mesh* mesh);
const float* clay_mesh_colors(const clay_mesh* mesh);
/* count * 2, or NULL when absent. Meshers and the OBJ importer produce uvs and
 * the OBJ and GLB writers consume them; before this they were the one mesh
 * attribute the boundary could not read. */
const float* clay_mesh_uvs(const clay_mesh* mesh);
const uint32_t* clay_mesh_indices(const clay_mesh* mesh);

/* Where each attribute goes in ONE interleaved vertex, so a mesh reaches a
 * mapped GPU buffer in a single pass instead of an interleave into a staging
 * vector followed by a copy — two passes over geometry that was just produced,
 * on the frame path. This does for meshes what clay_brick_cache_read_bricks
 * does for bricks: the destination is the caller's.
 *
 * Offsets are BYTES from the start of a vertex, and -1 omits the attribute.
 * Every attribute is float32 here because it is float32 in the mesh: this
 * descriptor says WHERE a value goes, not what it is converted to. Format
 * conversion is a second axis and would mean choosing a format enumeration for
 * four attributes before anyone has asked for one.
 *
 * Widths: position 12 bytes, normal 12, colour 12, uv 8. stride 0 means
 * "tightly packed", computed as the end of the last attribute named — which is
 * only well defined because the offsets are yours, so a packed layout is the
 * one you described with no gaps rather than an order this header picks. */
typedef struct clay_vertex_layout {
    uint32_t struct_size; /* = sizeof(clay_vertex_layout); required */
    uint32_t stride;      /* bytes per vertex; 0 = tightly packed */
    int32_t position_offset;
    int32_t normal_offset;
    int32_t color_offset;
    int32_t uv_offset;
} clay_vertex_layout;

/* Copies count vertices into dst in the layout described, and the indices into
 * a caller's buffer. dst_bytes must be exactly stride * clay_mesh_vertex_count
 * and dst_count exactly clay_mesh_index_count — required rather than inferred,
 * as everywhere else here, and a short destination is refused rather than
 * truncated. Both counts are already queryable, so the two-call shape costs
 * nothing.
 *
 * Naming an attribute the mesh does not carry is REFUSED, not zero-filled: a
 * silently black or silently flat model is harder to diagnose than a returned
 * error. So is a layout whose attributes overlap, or a stride that does not
 * clear the attributes it was asked to hold — the two mistakes that produce a
 * buffer which is wrong without being obviously wrong. */
clay_result clay_mesh_copy_vertices(const clay_mesh* mesh, const clay_vertex_layout* layout,
                                    void* dst, size_t dst_bytes);
clay_result clay_mesh_copy_indices(const clay_mesh* mesh, uint32_t* dst, size_t dst_count);

/* -- quad meshing ----------------------------------------------------------
 *
 * A mesh from the calls below carries its QUADS beside its triangles. It does
 * not carry them INSTEAD: `indices` still holds exactly the triangulation of
 * those quads — quad q = (a, b, c, d) is triangles (a, b, c) and (a, c, d) at
 * indices[6q .. 6q+5] — so every accessor above answers as it always did and a
 * host that ignores the quads draws the mesh it always drew.
 *
 * WHAT THIS IS NOT, and it is the first thing to read. These produce a REGULAR
 * QUAD GRID DERIVED FROM A SAMPLING LATTICE. That is NOT field-aligned
 * retopology. The quads follow the lattice and not the form: no edge loops run
 * around a limb or a mouth, no poles are placed at features, density does not
 * follow curvature, and the result is NOT animation-ready — deforming it
 * pinches wherever the topology disagrees with the shape, which is everywhere.
 * This is the input a retopology pass REPLACES, not the output one produces. A
 * host offering this as "remesh to quads" beside ZRemesher, QuadRemesher or
 * Instant Meshes will have its users compare the two, so say what it is.
 *
 * What it IS good for: getting quads into a DCC that prefers them, subdividing
 * a sculpt, and exporting a voxel model as the box faces it actually is.
 *
 * The output is not manifold and not watertight, for the reason
 * clay_voxel_mesh_smooth already gives — a cell the surface crosses twice gets
 * one vertex and the sheets pinch. CLAY_MESHER_MARCHING through
 * clay_document_mesh remains the watertight, 2-manifold export path. */

/* Which lattice the quads come from. Dual is 0 because that is what a caller
 * whose struct_size predates the field gets, and it is the mode that works for
 * both sources. Checked against this list; an unknown value is rejected rather
 * than mapped onto the default, exactly as clay_mesher is. */
typedef enum clay_quad_mode {
    /* The lattice dual (surface nets): the rounded form, quads meeting four to
     * a vertex on average and no T-junctions. Both sources. */
    CLAY_QUAD_DUAL = 0,
    /* One planar, axis-aligned quad per exposed voxel face — the boxes the
     * model actually is. VOXELS ONLY: a document asked for it is refused
     * rather than quietly given the dual, because substituting a smooth mesh
     * for a boxy one is visible in the render and invisible in the return
     * code. Dense (~area / voxel²), corners welded within a palette colour,
     * and it carries NO vertex normals: a welded corner is shared by faces
     * pointing three ways, averaging would round the cube, and duplicating
     * would undo the weld. clay_voxel_mesh is unchanged and remains the
     * merged, per-face-normal, triangle path. */
    CLAY_QUAD_FACES = 1
} clay_quad_mode;

/* What to mesh, and how many quads to aim at.
 *
 * `cell_size` is the lattice the quads come from and the lever on how many
 * there are. `target_quads` asks for a COUNT instead, which is a short search
 * over cell size — see the contract at clay_mesh_quad_report before wiring a
 * slider to it, because a target is approached and never hit. Giving both uses
 * the cell size as the search's starting point.
 *
 * `blur` and `level` are the voxel side's: `blur` is
 * clay_voxel_mesh_smooth's, and `level` names a resolution level explicitly
 * rather than following the active one — in faces mode it IS the count lever,
 * so a target chooses it. Both are ignored for a document.
 *
 * The count controls are appended after the meshing fields, so a caller
 * declaring only the original layout meshes at a cell size with no search. */
typedef struct clay_quad_params {
    uint32_t struct_size; /* = sizeof(clay_quad_params); required */
    float cell_size;      /* world units; <= 0 means "from the target", or the source's own */
    int32_t mode;         /* clay_quad_mode */
    int32_t blur;         /* voxels, dual mode only: 0..8 passes */
    uint32_t level;       /* voxels only: resolution level, 0 = coarsest */
    /* appended after the original layout; all three default to 0 */
    uint64_t target_quads;  /* 0: no search, mesh once at cell_size.
                             * Above CLAY_MAX_BATCH is refused */
    float tolerance;        /* fraction of the target, below 1; <= 0 means 0.10 */
    int32_t max_iterations; /* whole meshes the search may build, 0..64; 0 means 4
                             * and a negative is refused.
                             * Not used in faces mode: see clay_mesh_quad_report */
} clay_quad_params;

/* Quad-meshes the document's SDF content. A NEW entry point: clay_document_mesh
 * returns exactly what it always returned and carries no quads.
 * CLAY_QUAD_FACES here is CLAY_ERROR_INVALID_ARGUMENT — it is a voxel mode.
 * Free the result with clay_mesh_destroy. */
clay_result clay_document_mesh_quads(const clay_document* doc, const clay_quad_params* params,
                                     clay_mesh** out_mesh);

/* The quads of a mesh that has them: four indices per quad, all inside
 * clay_mesh_vertex_count. A mesh carrying none — every mesher that predates
 * quad meshing, anything loaded from a file, anything built from triangles —
 * reports a count of 0 and a NULL pointer rather than a pairing of its
 * triangles invented here. The pointer is borrowed and valid until
 * clay_mesh_destroy, like every other borrowed mesh pointer.
 *
 * dst_count for the copy is 4 * clay_mesh_quad_count exactly, required rather
 * than inferred for the same reason clay_mesh_copy_indices requires one. */
size_t clay_mesh_quad_count(const clay_mesh* mesh);
const uint32_t* clay_mesh_quads(const clay_mesh* mesh);
clay_result clay_mesh_copy_quads(const clay_mesh* mesh, uint32_t* dst, size_t dst_count);

/* How a mesh was produced, which is the ONLY way a host learns that a target
 * of fifty thousand produced thirty-one thousand because a limit stopped the
 * search rather than because something went wrong.
 *
 * THE TARGET IS A HINT WITH A REPORTED ACTUAL. It is not a ceiling and not an
 * exact count. A ceiling cannot be promised because the count is NOT monotonic
 * in cell size: a finer lattice can resolve a thin feature the coarser one
 * missed entirely and so ADD surface. Where two candidates are equally close
 * the search prefers the one that does not exceed the target, so "about this
 * many" usually reads the way a user expects — without being promised.
 *
 * GRANULARITY. The count goes as cell^-2, so a 1% change in cell size moves it
 * about 2%. Landing inside 5-10% is the expectation, `tolerance` defaults to
 * 0.10, and a tolerance much below about 2% will exhaust `max_iterations` and
 * come back with `within_tolerance` 0 and the best attempt.
 *
 * NOR IS THE RESULT MONOTONIC IN THE TARGET. Two nearby targets are two
 * independent searches, each from its own seed and each stopping after its own
 * handful of meshes, so a slightly smaller target can come back with slightly
 * MORE quads. A host wiring a slider should expect the count to move backwards
 * occasionally; a larger `max_iterations` makes it rarer and never rules it
 * out.
 *
 * FACES MODE IS DIFFERENT, because its lattices are the grid's resolution
 * LEVELS and not a continuum. The search walks the stack from the coarsest
 * level toward the finest, stops at the first level that reaches the target,
 * and returns the nearer of the two it lands between — so it is monotonic,
 * it costs at most one mesh per level, and `max_iterations` does not apply to
 * it. The bracketing pair is NOT the price: the walk starts at the coarsest
 * level and meshes every level on the way, so a target met at level k reports
 * `iterations` = k+1, and a host budgeting a slider prices the stack's length.
 * A level step is a factor of about four in count, so `within_tolerance`
 * is usually unreachable there, and `cell_size` names the level that was
 * chosen.
 *
 * COST. EVERY ITERATION IS A WHOLE MESH, including a whole dense field
 * evaluation on the document path. `max_iterations` is a cost knob, not a
 * quality knob.
 *
 * `clamped` means the search wanted a lattice the source cannot give it and
 * settled for the nearest one it can. Over a cell size that is a limit — the
 * fine end being the sample ceiling clay_document_mesh already prices against
 * and, for voxels, the grid's own voxel size, below which a finer lattice
 * resamples the same step field and buys quads without buying detail. In faces
 * mode it is the STACK RUNNING OUT: the target is below what the coarsest
 * level THAT YIELDS ANYTHING gives or above what the finest yields, and no
 * level of this grid is nearer than the one returned. The qualifier is load
 * bearing — a stack is not a strict mip, so a sculpt made only at a fine level
 * leaves the coarse levels empty, and an empty level's 0 quads are below every
 * target without bracketing any of them. A faces target that falls BETWEEN
 * two levels is not clamped even when it lands far off — both were meshed and
 * neither is nearer, which is what `within_tolerance` 0 says.
 *
 * A mesh that was NOT quad-meshed is refused with CLAY_ERROR_INVALID_ARGUMENT
 * rather than answered with zeroes: zeroes are indistinguishable from a search
 * that found nothing. clay_mesh_transform carries the report with the mesh;
 * clay_mesh_concat does not, because a concatenation was produced by no single
 * meshing call. */
typedef struct clay_quad_report {
    uint32_t struct_size;    /* = sizeof(clay_quad_report); required */
    float cell_size;         /* the lattice the mesh was built on */
    uint64_t target_quads;   /* what was asked for; 0 when no target was given */
    uint64_t quad_count;     /* what was actually produced */
    int32_t iterations;      /* whole meshes the SEARCH built; 0 when none ran */
    int32_t within_tolerance;/* 0/1 */
    int32_t clamped;         /* 0/1 */
} clay_quad_report;

clay_result clay_mesh_quad_report(const clay_mesh* mesh, clay_quad_report* out_report);

/* The box enclosing the mesh's positions — how a host frames an imported
 * model. It is answered here rather than by clay_layer_bounds because that
 * query is derived from SDF shapes and would report an empty box for a mesh
 * layer. An empty mesh returns CLAY_ERROR_INVALID_ARGUMENT rather than an
 * inverted box. */
clay_result clay_mesh_bounds(const clay_mesh* mesh, float out_min[3], float out_max[3]);

/* Everything the validator measures, rather than two bits of it.
 *
 * `clay_mesh_validate` below answers watertight and manifold and drops the
 * other nine quantities the same pass already computed, so a host could be
 * told an export was bad and never told WHY. This is that pass, reported.
 *
 * `intersection_budget` is the cap on the SAMPLED self-intersection check and
 * is echoed back, which is the point of carrying it: `intersecting_pairs` is
 * zero both when nothing intersects and when nothing looked, and `clean`
 * requires it to be zero. A report that could not tell those apart would let
 * "clean" mean two different things. Zero skips the pass, matching the
 * engine's own default; a larger cap tests that many spatially-close,
 * non-adjacent triangle pairs exactly, and costs accordingly.
 *
 * `sliver_triangles` is informational and is NOT one of `clean`'s terms: a
 * near-zero-area triangle is legal geometry that some exporters produce and
 * most consumers tolerate. `clean` is
 * watertight && manifold && oriented && degenerate == 0 && intersecting == 0. */
typedef struct clay_validation_report {
    uint32_t struct_size; /* = sizeof(clay_validation_report); required */
    size_t vertices;
    size_t triangles;
    int32_t watertight; /* every edge shared by exactly two triangles */
    int32_t manifold;   /* no edge with more than two incident triangles */
    int32_t oriented;   /* the two triangles of every edge disagree in direction */
    int32_t clean;      /* the conjunction above, derived */
    size_t boundary_edges;
    size_t non_manifold_edges;
    size_t degenerate_triangles; /* repeated indices within a triangle */
    size_t sliver_triangles;     /* near-zero area; informational */
    size_t intersecting_pairs;   /* hits found, within the budget below */
    size_t intersection_budget;  /* the cap this report was produced with; 0 = pass skipped */
    int64_t euler_characteristic; /* V - E + F */
} clay_validation_report;

clay_result clay_mesh_validation_report(const clay_mesh* mesh, size_t max_intersection_pairs,
                                        clay_validation_report* out_report);

/* watertight/manifold checks (mesh validation module). Sugar over
 * clay_mesh_validation_report with no self-intersection pass, kept because
 * hosts call it; anything more than two bits wants the report. */
clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                               int32_t* out_manifold);

/* Signed volume by the divergence theorem, and surface area.
 *
 * DOUBLE, and the only doubles in this header, because that is the precision
 * the engine computes them at and narrowing here would discard it: a signed
 * volume is a sum of triple products that cancels heavily, and a large model
 * far from the origin cancels worst.
 *
 * The sign is an orientation check — positive for outward-facing normals — so
 * this is answered for ANY mesh rather than refused for one that is not
 * watertight. An open mesh still has a divergence-theorem sum, and refusing to
 * state it would hide the number a caller uses to notice the mesh is open.
 *
 * Either output pointer may be NULL to skip that quantity. */
clay_result clay_mesh_measure(const clay_mesh* mesh, double* out_signed_volume,
                              double* out_surface_area);

/* Save by extension: .obj, .ply, .fbx, .glb
 *
 * QUADS: .obj, .ply and .fbx carry them, and a mesh with quads writes
 * four-corner faces in all three. .GLB DOES NOT — glTF 2.0 defines no quad
 * primitive mode, so the writer keeps writing the triangulation, which is the
 * same surface. Said here because "I exported GLB and got triangles" is the
 * one surprising outcome of quad meshing, and it is not a bug.
 *
 * The READERS are unchanged, so a quad file loaded back through clay_mesh_load
 * comes back as TRIANGLES with no quads. Which triangles differs by format.
 * .obj and .ply are read by this library's own parsers, which fan a face on
 * the same diagonal the quad writer used, so the index buffer survives the
 * round trip. .fbx is read through ufbx, which picks its own diagonal per quad
 * — close to half of them come back split the other way, and the solid's
 * measured volume moves with them. Neither triangulation is wrong; a caller
 * checking an FBX round trip must compare the SURFACE, not the indices. */
clay_result clay_mesh_save(const clay_mesh* mesh, const char* path);

/* Guardrails for an importer, checked against the file's DECLARED counts before
 * anything is allocated — which is the point: a malformed or hostile file can
 * claim a billion triangles, and the check has to happen before the allocation
 * rather than after it. Zero on a field means the library's default. */
typedef struct clay_import_budget {
    uint32_t struct_size; /* = sizeof(clay_import_budget); required */
    uint64_t max_vertices;
    uint64_t max_triangles;
} clay_import_budget;

/* Load by extension: .obj, .ply, .fbx, .glb, matched case-insensitively. The
 * counterpart to clay_mesh_save, and what gives clay_item_volume_from_mesh
 * something to sample.
 *
 * .glb reads every mesh in the file, applying each node's world transform, so
 * an exported scene arrives as the shape its author saw. Materials, animation
 * and skinning are ignored — the mesh type has nowhere to put them — but a
 * non-triangle primitive is REFUSED rather than dropped, since importing a
 * line set as an empty mesh looks like a broken reader. `.gltf` is not
 * accepted: its buffers live in separate files, and reading whatever the JSON
 * names would mean reading files the caller never handed us.
 *
 * `budget` may be NULL for the library's defaults. Exceeding it returns
 * CLAY_ERROR_BUDGET_EXCEEDED rather than allocating. */
clay_result clay_mesh_load(const char* path, const clay_import_budget* budget,
                           clay_mesh** out_mesh);

/* The same two, without a path. See clay_blob above for why this exists and
 * why it is an owner handle.
 *
 * FORMAT BY NAME, because a buffer has no extension: "obj", "ply", "fbx",
 * "glb" — the extensions without the dot, matched case-insensitively like the
 * path forms, so a host that already parsed an extension can pass it through.
 * An unknown name is CLAY_ERROR_UNSUPPORTED, never a silent default: writing
 * OBJ because the caller asked for something unrecognised produces a file the
 * host will hand to a user under the wrong name.
 *
 * The bytes are identical to what clay_mesh_save writes for the same mesh and
 * format, with ONE stated difference: an in-memory OBJ carries no `mtllib`
 * line. The path form writes a companion .mtl beside the object file and names
 * it; a buffer has no companion, and naming a file that was never written is
 * worse than naming none.
 *
 * `budget` may be NULL for the library's defaults and is enforced exactly as
 * the path loader enforces it — a buffer from a network or a pasteboard is the
 * untrusted input the guardrails exist for. Free a produced mesh with
 * clay_mesh_destroy and a produced blob with clay_blob_destroy. */
clay_result clay_mesh_save_memory(const clay_mesh* mesh, const char* format,
                                  clay_blob** out_blob);
clay_result clay_mesh_load_memory(const uint8_t* data, size_t size, const char* format,
                                  const clay_import_budget* budget, clay_mesh** out_mesh);

/* -- attribute transfer (add-mesh-attribute-transfer) ----------------------- */

/* Give one mesh the per-vertex attributes of another, by closest point.
 *
 * Everything that leaves a mesh layer loses what a mesh layer was holding:
 * clay_item_volume_from_mesh samples the model onto a lattice, and what a
 * mesher hands back is new geometry with new vertices — the shape survives and
 * the colours and uvs are gone. The nearest point on the ORIGINAL surface knows
 * what belonged there, and this reads it.
 *
 * IT DOES NOT GIVE BACK TOPOLOGY. The target is still the mesher's geometry.
 * This refunds the paint and most of the uvs; it does not refund the mesh.
 *
 * Positions and indices are never modified: this is a transfer, not a
 * projection. A verb that moved the target's vertices toward the source would
 * be a different operation. */
typedef struct clay_transfer_desc {
    uint32_t struct_size; /* = sizeof(clay_transfer_desc); required */
    int32_t colors;       /* 0/1 */
    int32_t uvs;          /* 0/1 */
    /* OFF unless you mean it. A resampled mesh has its own geometry and its
     * normals should describe IT; taking the source's makes new geometry shade
     * like the old shape. */
    int32_t normals;
    /* How far a target vertex may be from the source before it takes the
     * fallback. Geometry can exist where the source never was — after a
     * boolean, or where a mesher bridged a gap — and the closest point to it
     * carries no meaning. 0 derives it: 5% of the source's bounding diagonal. */
    float max_distance;
} clay_transfer_desc;

/* What happened. A transfer that fell back across most of the mesh is
 * otherwise indistinguishable from a good one, which is why this is reported
 * rather than returned as a bool. */
typedef struct clay_transfer_report {
    uint32_t struct_size; /* = sizeof(clay_transfer_report); required */
    uint64_t transferred;
    uint64_t fell_back;
    int32_t colors; /* whether each channel actually moved; a channel the */
    int32_t uvs;    /* SOURCE lacks is left alone on the target, not cleared */
    int32_t normals;
    float max_distance; /* the threshold used, derived or given */
} clay_transfer_report;

clay_result clay_mesh_transfer_defaults(clay_transfer_desc* out_desc);

/* THE UV SEAM LIMITATION, stated because it follows from the representation:
 * uvs are per VERTEX, which is how a seam exists at all — the source
 * duplicates a position into two vertices with different uvs. A target vertex
 * on such a seam has one slot and two correct answers. Colour is unaffected,
 * being continuous across a seam. */
clay_result clay_mesh_transfer_attributes(const clay_mesh* source, clay_mesh* target,
                                          const clay_transfer_desc* desc,
                                          clay_transfer_report* out_report);

/* Build a mesh from caller-owned triangles: positions is count*3 floats and
 * indices is triangle_count*3 vertex indices. Copied, so the caller's buffers
 * may be freed on return. */
clay_result clay_mesh_from_triangles(const float* positions, size_t vertex_count,
                                     const uint32_t* indices, size_t index_count,
                                     clay_mesh** out_mesh);

/* Resample a caller-owned per-vertex scalar — a mask, a weight — from one mesh
 * onto another by closest point (add-voxel-remesher, ABI 0.63.0).
 *
 * WHY THIS EXISTS SEPARATELY FROM clay_mesh_transfer_attributes: a mask is not
 * a mesh attribute here. It is a per-vertex gate a host owns and passes to the
 * sculptors, and moving its storage into the mesh to suit one operation would
 * be changing a representation to make a call convenient. But clay_mesh_voxel_
 * remesh replaces the topology the mask was indexed by, so the mask has to be
 * resampled from geometry like anything else that survives a rebuild.
 *
 * `values` is `value_count` floats and must be the SOURCE's vertex count;
 * `out_values` is `out_count` floats and must be the TARGET's. Both lengths
 * are required rather than inferred — the same rule clay_mesh_copy_indices
 * follows — and a mismatch is CLAY_ERROR_INVALID_ARGUMENT with nothing
 * written.
 *
 * `max_distance` of 0 derives the threshold from the source's size; a target
 * vertex further away than it takes `fallback`. */
clay_result clay_mesh_transfer_vertex_scalar(const clay_mesh* source, const float* values,
                                             size_t value_count, const clay_mesh* target,
                                             float max_distance, float fallback,
                                             float* out_values, size_t out_count);

/* -- welding (add-mesh-weld, ABI 0.65.0) ------------------------------------ */

/* Merge coincident vertices and remove the triangles that collapses.
 *
 * WHAT THIS IS FOR. The default mesher emits ZERO-AREA TRIANGLES — 1458 of
 * 70,140 on a plain sphere, two per cent, with two corners at bit-identical
 * positions. Almost everything downstream tolerates them, which is why nobody
 * had noticed; a half-edge surface cannot, so no mesh this library marched
 * could be converted to an adaptive one until this existed.
 *
 * Not the same verb as the weld epsilon a sculptor takes. That one groups
 * vertices into classes and leaves the mesh alone, so a seam's duplicates move
 * together; this one MERGES them and rewrites the triangles.
 *
 * WATERTIGHTNESS SURVIVES: a triangle whose corners coincide bounds nothing, so
 * removing it cannot open a hole. QUADS ARE DROPPED when anything changes,
 * because the triangles are rewritten. EVERY INDEX IS IN RANGE afterwards. A
 * mesh with nothing to merge comes back byte-identical. */
typedef struct clay_weld_desc {
    uint32_t struct_size; /* = sizeof(clay_weld_desc); required */
    /* As a fraction of the bounding-box diagonal — relative, so it means the
     * same thing in millimetres and in metres. 0 welds only bit-identical
     * positions. Weld at AT LEAST the epsilon whatever consumes the mesh will
     * use: welding below it only moves the problem. */
    float epsilon;
    /* Refuse to merge two vertices whose uvs or colours disagree. ON by
     * default, and the default is the safety of this verb: a UV SEAM IS
     * DUPLICATED POSITIONS WITH DIFFERENT UVS, and merging across one silently
     * destroys the layout. */
    int32_t preserve_attribute_splits;
    float attribute_epsilon; /* how far uvs/colours may differ and still agree */
} clay_weld_desc;

typedef struct clay_weld_report {
    uint32_t struct_size; /* = sizeof(clay_weld_report); required */
    uint64_t vertices_before;
    uint64_t vertices_after;
    uint64_t triangles_before;
    uint64_t triangles_after;
    uint64_t vertices_merged;
    uint64_t triangles_collapsed;
    /* Removed for naming a vertex that does not exist. Counted apart from the
     * collapsed: a non-zero count here means a bug upstream, not geometry this
     * verb is for. */
    uint64_t triangles_invalid;
    uint64_t vertices_unreferenced;
    float epsilon; /* the relative threshold resolved against this mesh's size */
    int32_t quads_dropped;
} clay_weld_report;

clay_result clay_mesh_weld_defaults(clay_weld_desc* out_desc);

clay_result clay_mesh_weld(clay_mesh* mesh, const clay_weld_desc* desc,
                           clay_weld_report* out_report);

/* -- global voxel remesh (add-voxel-remesher, ABI 0.63.0) ------------------- */

/* Rebuild a whole surface through a signed volumetric representation at an
 * explicit spatial resolution: the operation an artist calls DynaMesh.
 *
 * Overlapping shells fuse, self-intersections resolve, stretched triangles
 * disappear and the topology is REPLACED — no source vertex or polygon index
 * means anything about the result. Details finer than the voxel size may be
 * lost, and UVs are dropped rather than reprojected. Both are reported.
 *
 * This is a pure mesh -> mesh operation. It has no document, no layer, no undo
 * record and no revision token: a host holds the before and after meshes and
 * commits them as one history entry itself. */

typedef enum clay_voxel_remesh_resolution {
    /* World units, the canonical form. */
    CLAY_VOXEL_REMESH_VOXEL_SIZE = 0,
    /* The source's longest bounding extent divided by an integer, resolved
     * before any sampling padding is applied. */
    CLAY_VOXEL_REMESH_LONGEST_AXIS = 1
} clay_voxel_remesh_resolution;

typedef enum clay_voxel_remesh_surface {
    /* Marching tetrahedra: watertight and 2-manifold by construction. */
    CLAY_VOXEL_REMESH_SMOOTH = 0,
    /* Dual contouring. EXPERIMENTAL, matching its flagged status elsewhere in
     * this ABI: the watertight guarantee is NOT claimed for it. */
    CLAY_VOXEL_REMESH_SHARP = 1
} clay_voxel_remesh_surface;

typedef enum clay_voxel_remesh_open_policy {
    CLAY_VOXEL_REMESH_OPEN_REJECT = 0,      /* typed failure, no mesh */
    CLAY_VOXEL_REMESH_OPEN_CLOSE = 1,       /* close it, then validate */
    CLAY_VOXEL_REMESH_OPEN_BEST_EFFORT = 2  /* proceed and report what it is */
} clay_voxel_remesh_open_policy;

typedef enum clay_voxel_remesh_component_policy {
    CLAY_VOXEL_REMESH_KEEP_COMPONENTS = 0,
    CLAY_VOXEL_REMESH_REMOVE_BELOW_VOLUME = 1
} clay_voxel_remesh_component_policy;

typedef struct clay_voxel_remesh_params {
    uint32_t struct_size; /* = sizeof(clay_voxel_remesh_params); required */
    int32_t resolution_mode;          /* clay_voxel_remesh_resolution */
    float voxel_size;                 /* read in VOXEL_SIZE mode */
    uint32_t longest_axis_resolution; /* read in LONGEST_AXIS mode */
    int32_t surface_mode;             /* clay_voxel_remesh_surface */
    int32_t open_surface_policy;      /* clay_voxel_remesh_open_policy */
    int32_t small_component_policy;   /* clay_voxel_remesh_component_policy */
    float minimum_component_volume;   /* cubic world units; REMOVE_BELOW only */
    int32_t preserve_volume;          /* 0/1; a clamped correction, skipped
                                       * where the comparison stopped meaning
                                       * anything */
    int32_t project_to_source;        /* 0/1 */
    float projection_strength;        /* 0..1; a lerp, never a snap */
    float max_projection_distance_voxels;
    int32_t preserve_colors; /* 0/1; a source with none produces none */
    uint32_t build_multires_levels; /* reserved; non-zero is UNSUPPORTED */
    uint64_t memory_budget_bytes;   /* 0 = no caller budget */
} clay_voxel_remesh_params;

/* What a remesh would cost, before it is made. Cheap enough for a resolution
 * slider: it walks the source's triangles and marks a brick lattice, and
 * allocates nothing proportional to the sample count it predicts. */
typedef struct clay_voxel_remesh_estimate {
    uint32_t struct_size; /* = sizeof(clay_voxel_remesh_estimate); required */
    float resolved_voxel_size;
    uint32_t grid_dimensions[3];
    /* An UPPER BOUND on the narrow band, not a prediction: the marking keeps
     * every brick whose box comes within the band of a triangle's bounds, and
     * some hold nothing near enough to store. The report's active_samples is
     * what the run actually held and is never larger. */
    uint64_t estimated_active_samples;
    uint64_t estimated_memory_bytes;
    uint64_t estimated_triangle_min;
    uint64_t estimated_triangle_max;
    uint32_t boundary_edge_count;
    uint32_t component_count;
    int32_t has_open_boundaries;
    /* Sampled evidence that the source carries material thinner than a couple
     * of voxels — material this resolution may delete. */
    int32_t thin_feature_warning;
    int32_t exceeds_memory_budget;
} clay_voxel_remesh_estimate;

typedef struct clay_voxel_remesh_report {
    uint32_t struct_size; /* = sizeof(clay_voxel_remesh_report); required */
    float voxel_size;
    uint64_t source_vertices;
    uint64_t source_triangles;
    uint64_t result_vertices;
    uint64_t result_triangles;
    double source_volume;
    double result_volume;
    double relative_volume_error;
    uint32_t source_boundary_edges;
    uint32_t result_boundary_edges;
    uint32_t source_components;
    uint32_t result_components;
    uint32_t removed_components;
    uint64_t active_samples;
    int32_t source_was_open;
    int32_t result_watertight;
    int32_t result_manifold;
    int32_t result_oriented;
    int32_t projected_to_source;
    uint64_t projected_vertices;
    int32_t volume_corrected;
    int32_t colors_transferred;
    /* Always set when the source carried UVs, and not a failure: a spatially
     * reprojected UV across a seam is a stretched layout that looks like a
     * preserved one, so this operation does not pretend to keep them. */
    int32_t uvs_dropped;
    int32_t cancelled;
} clay_voxel_remesh_report;

/* The library's documented defaults, so a caller does not transcribe them. */
clay_result clay_mesh_voxel_remesh_defaults(clay_voxel_remesh_params* out_params);

/* What the remesh would cost. `out_estimate` is filled bounded by the size the
 * caller declared. Returns the same refusals clay_mesh_voxel_remesh would for
 * an unusable resolution or a request over budget, and fills the estimate that
 * justified the refusal where one could be computed. */
clay_result clay_mesh_voxel_remesh_estimate(const clay_mesh* source,
                                            const clay_voxel_remesh_params* params,
                                            clay_voxel_remesh_estimate* out_estimate);

/* Run it. `out_mesh` receives a NEW mesh the caller frees with
 * clay_mesh_destroy; `source` is never modified.
 *
 * Refusals are distinguishable rather than collapsed:
 *   CLAY_ERROR_INVALID_ARGUMENT  the source has no triangles, or the
 *                                resolution or a parameter is not usable
 *   CLAY_ERROR_BUDGET_EXCEEDED   over memory_budget_bytes or the library's
 *                                own ceiling, refused before the field, the
 *                                tree and the result were allocated
 *   CLAY_ERROR_UNSUPPORTED       an open source under OPEN_REJECT, or a
 *                                reserved parameter asked for
 *   CLAY_ERROR_BACKEND           the field produced no surface, or the result
 *                                failed the validation the mode promises
 *   CLAY_ERROR_CANCELLED         `token` was cancelled
 *
 * `out_report` is optional and is filled for a refusal too where the numbers
 * exist — an open-surface refusal carries the source's boundary-edge count,
 * which is what a host puts in front of a user. `token` is optional; progress
 * is read through clay_cancel_token_progress as it is for every other
 * cancellable call here. */
clay_result clay_mesh_voxel_remesh(const clay_mesh* source,
                                   const clay_voxel_remesh_params* params,
                                   clay_cancel_token* token, clay_mesh** out_mesh,
                                   clay_voxel_remesh_report* out_report);

/* -- voxel remesh THROUGH the document (add-voxel-remesh-document, 0.64.0) --- */

/* The pure clay_mesh_voxel_remesh above has no document, no undo and no state.
 * That is what makes it testable, and it is not what a host wants to call: an
 * artist's remesh has to land on a LAYER, replace what was there, and be one
 * step on the undo menu.
 *
 * A MESH LAYER'S GEOMETRY REVISION. Bumped every time a layer's triangles are
 * replaced wholesale, and NOT by a sculpt — a brush moves vertices and leaves
 * the topology alone, which is the fixed-topology contract and is precisely the
 * change a cache over that mesh survives. What it exists for is the change a
 * cache does NOT survive: a rebuild swaps every vertex and every index, and an
 * adjacency, a BVH or a live sculptor built over the old ones is wrong in a way
 * nothing else detects.
 *
 * Read it, do the slow work, then hand it back to the commit: if the layer was
 * rebuilt in between, the commit is refused with CLAY_ERROR_FORWARD_VERSION
 * rather than overwriting work the artist did while waiting.
 *
 * Zero for a layer that is not a mesh layer, or does not exist. */
clay_result clay_document_mesh_layer_revision(const clay_document* doc, clay_layer_id layer,
                                              uint64_t* out_revision);

/* Replace a mesh layer's triangles wholesale, as ONE undo step.
 *
 * For a host that ran clay_mesh_voxel_remesh on a worker thread and now wants
 * to commit it. `expected_revision` is what clay_document_mesh_layer_revision
 * returned before the work started; pass 0 to skip the check, which is only
 * right when nothing could have touched the layer in between.
 *
 * The replacement must carry triangles, must not be the layer's own borrowed
 * mesh, and must be quad-free or quad-consistent — a quad list describing
 * triangles that no longer exist is a lie that survives into a saved document.
 *
 * Refusals: CLAY_ERROR_NOT_FOUND for an unknown layer or one that is not a mesh
 * layer, CLAY_ERROR_INVALID_ARGUMENT for a ghosted or locked layer and for an
 * empty or inconsistent replacement, CLAY_ERROR_FORWARD_VERSION when the layer
 * moved under the caller. A refusal leaves the layer byte-identical. */
clay_result clay_document_replace_mesh_layer(clay_document* doc, clay_layer_id layer,
                                             const clay_mesh* replacement,
                                             uint64_t expected_revision);

/* Remesh a mesh layer in place: capture, rebuild, validate, replace, record —
 * one call and one undo step.
 *
 * The synchronous form, for a host that does not need the work off-thread. It
 * is exactly clay_mesh_voxel_remesh followed by clay_document_replace_mesh_layer
 * with the layer's own revision, and it fails with the same result codes as
 * both. Nothing is written until the rebuild has succeeded AND validated, so a
 * refusal or a cancel leaves the layer byte-identical.
 *
 * `token` may be NULL. A cancelled remesh returns CLAY_ERROR_CANCELLED, changes
 * nothing, and adds no undo step. */
clay_result clay_document_voxel_remesh_layer(clay_document* doc, clay_layer_id layer,
                                             const clay_voxel_remesh_params* params,
                                             clay_cancel_token* token,
                                             clay_voxel_remesh_report* out_report);

/* -- mesh layers: a document CARRIES a mesh -------------------------------- */

/* The other reason to import a model. clay_item_volume_from_mesh samples one
 * into a field so it can be sculpted; a mesh LAYER keeps the triangles the
 * importer produced, verbatim, for display and re-export. A scan, a scale
 * reference, a kit part — geometry that must leave the pipeline as what it
 * entered as.
 *
 * A mesh layer is never evaluated. It is not compiled into a tape, takes no
 * part in a blend, contributes to no influence bound and is not pickable; its
 * geometry is held beside the document rather than in it, so that is
 * structural rather than a rule this ABI has to keep. clay_document_mesh is
 * unaffected: it means "mesh the field" and still does.
 *
 * Its transform, visibility, ghost, lock and ordering are the ordinary layer
 * ones, edited through the ordinary layer calls. */

typedef struct clay_mesh_layer_desc {
    uint32_t struct_size; /* = sizeof(clay_mesh_layer_desc); required */
    const char* name;     /* the layer's name; required */
    /* This layer's own ceiling, tighter than the loader's by default: what a
     * document may CARRY is a different question from what a file may decode
     * into, and the loader's defaults are 50M vertices. Zero means the
     * library's default for that field. */
    uint64_t max_vertices;
    uint64_t max_triangles;
    /* Uniform scale baked into the stored vertices, so unit conversion is
     * resolved once at import rather than approximated by a layer transform.
     * <= 0 means 1. Non-uniform scale is not expressible and is not
     * approximated. */
    float import_scale;
} clay_mesh_layer_desc;

/* Attaches an ALREADY-LOADED mesh (clay_mesh_load, clay_mesh_from_triangles,
 * or one this library meshed) as a new layer, copying its geometry. Taking a
 * handle rather than a path keeps import policy in one place and lets a host
 * attach geometry it generated itself.
 *
 * Goes through the same layer vocabulary every other layer creation uses, so
 * an enabled undo stack records it and it serializes with the document. Over
 * the descriptor's budget returns CLAY_ERROR_BUDGET_EXCEEDED and the document
 * is unchanged. `out_mesh` is BORROWED — see the lifetime note above
 * clay_document_add_voxel_layer.
 *
 * Quads are part of the geometry copied, so a quad mesh attached here comes
 * back a quad mesh through `out_mesh` and survives a save and reload. The quad
 * REPORT does not: it describes a meshing call, and a mesh read out of a
 * document was not produced by one. */
clay_result clay_document_add_mesh_layer(clay_document* doc, const clay_mesh* mesh,
                                         const clay_mesh_layer_desc* desc,
                                         clay_layer_id* out_layer, clay_mesh** out_mesh);

/* Borrows the mesh of an existing mesh layer by name; CLAY_ERROR_NOT_FOUND
 * when the document has no mesh layer carrying that name. Names are not
 * unique, so this answers with the FIRST mesh layer in stack order carrying
 * the name, and it follows clay_document_set_layer_name — see there. When the
 * lookup has to survive a rename, or has to tell two same-named layers apart,
 * use clay_document_mesh_layer_by_id below; this one stays for the ordinary
 * case of a document with one layer of that name. */
clay_result clay_document_mesh_layer(clay_document* doc, const char* name,
                                     clay_layer_id* out_layer, clay_mesh** out_mesh);

/* Borrows the mesh of the mesh layer carrying this id — the same lookup with
 * the name resolution taken out, so nothing about the borrow changes: see the
 * lifetime note above clay_document_add_voxel_layer, which governs a borrowed
 * mesh exactly as it governs a borrowed grid.
 *
 * Ids are stable across a save and load, so this is the lookup that survives a
 * rename and the one that tells two layers sharing a name apart. It refuses on
 * the same terms as its by-name sibling, which is the point of it rather than a
 * detail: CLAY_ERROR_NOT_FOUND when no layer carries the id, when the layer
 * carrying it is not a mesh layer, and when it is a mesh layer whose geometry
 * is not held beside the document — a layer whose payload did not come with the
 * file it was loaded from. Anything the by-name form refuses for a given layer,
 * this refuses too.
 *
 * out_mesh is REQUIRED, unlike the by-name form's. There a NULL means
 * something — the call doubles as an existence probe and still answers through
 * out_layer — while here the caller supplied the id and the handle is the only
 * answer there is, so a NULL asks nothing. clay_document_layer_info is the call
 * that says whether a layer exists and what representation it is. A NULL
 * document or a NULL out_mesh is CLAY_ERROR_INVALID_ARGUMENT, and a refused
 * call writes nothing. */
clay_result clay_document_mesh_layer_by_id(clay_document* doc, clay_layer_id layer,
                                           clay_mesh** out_mesh);

/* The layer a borrowed mesh belongs to — what the ordinary layer calls
 * (transform, visibility, ghost, lock, ordering, removal) take. A mesh the
 * caller owns belongs to no layer and returns CLAY_ERROR_NOT_FOUND. */
clay_result clay_mesh_layer(const clay_mesh* mesh, clay_layer_id* out_layer);

/* -- combining meshes for export -------------------------------------------
 *
 * `clay_document_mesh` means MESHING THE FIELD and keeps meaning exactly that.
 * It prices a dense grid from the tape's own bounds, and geometry that is not
 * in the tape would either inflate that grid or fall outside it — and it would
 * change what an existing call returns for an existing document. Voxel layers
 * are outside it for the same reason. Combining is therefore explicit, and
 * these are the three calls that do it. */

/* A copy of `mesh` with every position moved by the transform, and normals
 * rotated but not translated or scaled — a uniform scale leaves a direction
 * unchanged, and adding the position would turn a direction into a point.
 *
 * Takes a transform on the same terms as every other transform in this ABI:
 * `position` and `rotation_axis` are required and the axis must be non-zero,
 * `scale` must be > 0. A second convention for "no rotation" would be one
 * more thing to get wrong. Free the result with clay_mesh_destroy.
 *
 * Quads survive: this rewrites positions and touches no index, so the quad
 * list still describes the triangles beside it. So does the quad report — it
 * is the same mesh, moved. */
/* The same move with a PER-AXIS scale (ABI 0.54.0, issue #320). A mesh is real
 * vertices and no field, so nothing about exactness applies here: the positions
 * go through the matrix and the NORMALS through its inverse transpose, then
 * renormalized. That last part is why this is not the uniform call with three
 * numbers — under a non-uniform scale a normal is no longer carried by the
 * rotation alone, and transforming it as a direction tilts every one of them
 * off the surface. Every component must be > 0. */
clay_result clay_mesh_transform_nonuniform(const clay_mesh* mesh, const float position[3],
                                           const float rotation_axis[3], float rotation_angle,
                                           const float scale[3], clay_mesh** out_mesh);
clay_result clay_mesh_transform(const clay_mesh* mesh, const float position[3],
                                const float rotation_axis[3], float rotation_angle,
                                float scale, clay_mesh** out_mesh);

/* One mesh from many, with indices rebased onto the concatenated vertices.
 *
 * AN ATTRIBUTE PRESENT ON SOME INPUTS AND ABSENT ON OTHERS IS DROPPED from the
 * result. It is not padded and not truncated: a mesh whose normals, colors or
 * uvs are non-empty and a different length than its positions is malformed,
 * and no call in this ABI may return one. So concatenating a mesh that carries
 * uvs with one that does not yields a mesh with no uvs at all. Said here
 * because the alternative is discovering it in an exported file.
 *
 * QUADS FOLLOW THE SAME RULE, for the same reason: they survive only when
 * EVERY input carries them, rebased as the triangles are, and are dropped
 * entirely otherwise. A result that was quads over part of itself and
 * triangles over the rest is not a quad mesh, and it would break the invariant
 * that `indices` is exactly the quad list's triangulation. The quad REPORT is
 * always dropped — a concatenation was produced by no single meshing call.
 *
 * `meshes` is `count` mesh pointers, none NULL. Free the result with
 * clay_mesh_destroy. */
clay_result clay_mesh_concat(const clay_mesh* const* meshes, size_t count,
                             clay_mesh** out_mesh);

/* The convenience call: mesh the document's field with `params`, then append
 * every VISIBLE mesh layer under its own layer transform, indices rebased.
 *
 * Hidden mesh layers are excluded, because hidden means "contributes nothing".
 * Ghost and lock do NOT change what is exported, consistent with neither flag
 * changing what a document evaluates to.
 *
 * The attribute-drop rule of clay_mesh_concat applies: the meshed field
 * carries normals, so a mesh layer without them costs the result its normals.
 * A document with no visible mesh layer returns exactly what
 * clay_document_mesh would. Free the result with clay_mesh_destroy. */
clay_result clay_document_mesh_combined(const clay_document* doc,
                                        const clay_mesh_params* params,
                                        clay_mesh** out_mesh);

/* -- importing a mesh as a field ------------------------------------------- */

/* Declared here rather than with the mask entry points below, because the
 * relax and flatten parameter blocks freeze against one. See -- masks --. */
typedef struct clay_mask clay_mask;
typedef struct clay_groups clay_groups; /* opaque */

typedef struct clay_volume_params {
    uint32_t struct_size; /* = sizeof(clay_volume_params); required */
    float cell_size;      /* sample spacing; <= 0 picks from the mesh's own size */
    float band;           /* half-width of the band kept; <= 0 means three cells */
    float padding;        /* how far past the mesh's bounds to sample; <= 0 means the band */
    /* How far a BVH node must be before it is summarized by a single term
     * rather than descended. Larger is more accurate and slower; <= 0 means
     * the default. Sampling every triangle exactly is not reachable from here:
     * it is a testing control, not something an app should ask for. */
    float beta;
    /* How far inside the sampled box a CLAY_OP_REPLACE placement crossfades
     * from the surrounding field to this volume, in world units. <= 0 — and
     * any struct_size from before this field existed — is the hard replace,
     * byte-identical to what every prior release did.
     *
     * WHY: a hard replace holds BOTH fields live at the surface. A bake put
     * straight back ties with the field beneath it at every sample plane, and
     * min/max branch switching between two fields that touch is what
     * corrugates the normals at the cell wavelength (issue #67) — the zero
     * set is exact, the shading is not — while the box faces meet the outside
     * field at a hard edge. With a feather, deep inside the box the result IS
     * the volume (one gradient, no ties), outside the box the surrounding
     * field continues untouched, and the two crossfade over this margin.
     *
     * The blend's correction is clamped at the volume's BAND, which keeps the
     * declared field slope at max + band * 1.5 / feather (the document's safe
     * step scale drops to match — feather about one band is the sweet spot)
     * and keeps per-brick culling exact. It also means a verb that moved the
     * surface further than the band from what sits beneath is expressed only
     * up to the band across the feather margin: bake with a band that covers
     * the verb, which is the accuracy contract a volume already has.
     *
     * A feathered volume placed with CLAY_OP_REPLACE does not participate in
     * the layer mirror (the crossfade follows ONE sampled box); every other
     * op ignores the feather. Applies to every producer that takes this
     * struct: the mesh import, the document bake, and the _from verbs. */
    float feather;
} clay_volume_params;

/* Samples `mesh` into an item carrying a volume, which is what turns an
 * imported model into something that can be combined, cut and sculpted.
 *
 * The sign comes from the generalized winding number rather than a ray cast or
 * the nearest triangle's normal, because real assets are not watertight: one
 * hole flips a parity test for a whole half-space, while a winding number
 * degrades continuously across an opening.
 *
 * The returned item is owned by the caller until it is added to a layer, the
 * same as clay_item_create. A mesh with no triangles is refused. */
clay_result clay_item_volume_from_mesh(const clay_mesh* mesh, const clay_volume_params* params,
                                       clay_item** out_item);

/* Samples a DOCUMENT's own field into an item carrying a volume — baking what
 * an edit list evaluates to, rather than importing something from outside.
 *
 * This is what makes the volume operations reachable from an app at all. Relax
 * and flatten act on a volume, and until now the only way to get one through
 * this ABI was from a mesh, so an app could smooth an imported scan but not its
 * own sculpt. It is also the consolidation step for a long session: a document
 * with thousands of stroke items bakes to one volume, dropping the history and
 * the per-item tape cost while keeping the shape.
 *
 * The cost is the one baking always has: the items are gone, and with them the
 * ability to go back and change a radius. `cell_size` is the resolution the
 * shape now has, so it is the caller's decision to make deliberately.
 *
 * `region` is an optional (min[3], max[3]) pair, or NULL for the document's own
 * bounds padded by the band. Sampling an empty document is refused. */
clay_result clay_item_volume_from_document(const clay_document* doc,
                                           const clay_volume_params* params,
                                           const float region_min[3], const float region_max[3],
                                           clay_item** out_item);

typedef struct clay_relax_params {
    uint32_t struct_size;  /* = sizeof(clay_relax_params); required */
    float strength;        /* how much of the smoothed value to take, 0..1, per pass */
    int32_t radius_cells;  /* averaging radius in cells; <= 0 means 1 */
    int32_t iterations;    /* passes; <= 0 means 1 */
    float centre[3];       /* where it acts */
    float region_radius;   /* 0 relaxes everywhere, which is a filter not a brush */
    float falloff;         /* taper at the region's edge; widened if too narrow to hide the seam */
    /* Optional freeze, exactly as the voxel verbs take one: the weight at a
     * sample is scaled by (1 - mask) at its WORLD position, so a fully masked
     * sample keeps its value verbatim. NULL for none. Appended after the
     * original layout, so struct_size decides whether it is read at all and a
     * caller compiled before it existed is unaffected. */
    const clay_mask* mask;
} clay_relax_params;

/* Smooths an item that carries a volume, in place. The last of the core
 * sculpting brushes: voxel layers had smoothing, SDF layers had none.
 *
 * Smoothing destroys EXACTNESS — the field no longer reports the true distance
 * to its own surface — but it cannot break the Lipschitz bound, because an
 * average cannot vary faster than the thing it averages, and a field whose
 * slope is bounded by one is automatically a conservative bound on the distance
 * to its own zero set. So the raymarcher stays correct.
 *
 * The item must carry a volume; anything else is refused rather than ignored.
 *
 * WHERE A DOCUMENT EXISTS, use clay_item_volume_relax_from below instead: it
 * samples the document, which is exact everywhere, in one call. This call is
 * for a volume with no document behind it — an imported mesh: bring in a
 * scan, then smooth it. */
clay_result clay_item_volume_relax(clay_item* item, const clay_relax_params* params);

/* The same relax, sampled from a DOCUMENT rather than applied to an existing
 * volume — the counterpart clay_item_volume_flatten_from is to the flatten,
 * and the one to reach for when a document is what you have.
 *
 * The relationship to bake-then-relax is exact: this call samples the
 * document precisely as clay_item_volume_from_document would and relaxes
 * those samples in place, so inside the band the result is identical to
 * baking first — a test holds that. What it removes is the two-call round
 * trip and the temptation to relax a volume that was itself derived from
 * another volume, where the band inaccuracy the flatten note above describes
 * compounds. Relax moves the surface by less than a cell per pass, so unlike
 * a flatten it can never walk out of the band it was baked with.
 *
 * `relax` is the relax itself, validated exactly as the in-place form
 * validates it. `volume` gives the sampling — cell_size required and > 0,
 * band and padding defaulting as they do for clay_item_volume_from_document,
 * feather honoured the same way. `region_min`/`region_max` are the same
 * optional pair, with the same rule: both NULL means the document's own
 * bounds padded by the band, and one without the other is refused.
 *
 * Returns a NEW item carrying the relaxed volume; the document is not
 * modified. Free it with clay_item_destroy, or place it with
 * clay_layer_add_item. */
clay_result clay_item_volume_relax_from(const clay_document* doc,
                                        const clay_relax_params* relax,
                                        const clay_volume_params* volume,
                                        const float region_min[3],
                                        const float region_max[3],
                                        clay_item** out_item);

/* Which side of its plane a flatten acts on.
 *
 * CLAY_FLATTEN_TWO_SIDED is ZBrush's Flatten: material above the plane goes AND
 * hollows below it fill. CLAY_FLATTEN_CUT_ONLY is the hard-surface family —
 * hPolish, Planar, the Trim brushes — where cutting WITHOUT filling is the whole
 * brush: it leaves a crisp facet against untouched surface, and filling the
 * hollows beside a facet is what a polish must not do. CLAY_FLATTEN_FILL_ONLY is
 * the dual, and fills a scanned hole flat without touching the surface around
 * it. The three differ by one clamp, which is why this is a mode rather than
 * three entry points. */
typedef enum clay_flatten_mode {
    CLAY_FLATTEN_TWO_SIDED = 0,
    CLAY_FLATTEN_CUT_ONLY = 1,
    CLAY_FLATTEN_FILL_ONLY = 2
} clay_flatten_mode;

typedef struct clay_flatten_params {
    uint32_t struct_size;  /* = sizeof(clay_flatten_params); required */
    float plane_point[3];  /* a point on the plane to flatten onto */
    float plane_normal[3]; /* unit; material on this side goes. Zero length is refused */
    float strength;        /* 1 puts the surface on the plane, 0 changes nothing */
    float centre[3];       /* where it acts */
    float region_radius;   /* REQUIRED > 0: flatten is local, and with no region it
                            * replaces the shape with a half-space rather than
                            * flattening it — a ball comes back as a box */
    float falloff;         /* taper at the region's edge; widened when too narrow to declare */
    /* Which side of the plane to act on; APPENDED, so a descriptor sized to the
     * layout that predates it still describes a two-sided flatten. */
    int32_t mode;          /* clay_flatten_mode */
    /* Optional freeze; see clay_relax_params.mask. NULL for none. Appended
     * behind `mode`, which shipped first, so a descriptor sized to either
     * earlier layout still describes exactly what it carried. */
    const clay_mask* mask;
} clay_flatten_params;

/* Pulls an item's volume onto a plane, in place. The verb SDF layers were
 * missing: voxel grids have had clay_voxel_sculpt_flatten all along.
 *
 * TWO-SIDED, matching the voxel verb: material on the normal's side goes AND
 * hollows on the other side fill. It is not a subtract, and it is not ZBrush's
 * Clip — as a solid, Clip is exactly Trim, which clay_cut already does.
 *
 * The item's volume is RE-SAMPLED with the flatten applied, so the new band
 * brackets the flattened surface rather than the original one. Accurate while
 * the surface stays near the band it came from: past that a volume reports a
 * bound rather than a distance, and the facet is placed against the bound.
 *
 * A region blends under a weight that varies across it, so the result may be
 * steeper than a plain volume; it declares that, and the document's safe step
 * scale drops to match. The item must carry a volume; anything else is refused
 * rather than ignored.
 *
 * WHERE A DOCUMENT EXISTS, use clay_item_volume_flatten_from below instead:
 * it samples the document, which has no band, so the accuracy limit above
 * does not apply. This call is for a volume with no document behind it — an
 * imported mesh. */
clay_result clay_item_volume_flatten(clay_item* item, const clay_flatten_params* params);

/* The same flatten, sampled from a DOCUMENT rather than from an existing
 * volume — and the one to reach for when a document is what you have.
 *
 * The difference is accuracy, and it is not small. The call above re-samples
 * the item's own volume, which reports a distance only inside the band it
 * carries and a lower BOUND outside it; a facet that moves further than that
 * band is placed against the bound rather than against the surface, and the
 * result is a wrong shape returned with CLAY_OK. A document has no band. It
 * is exact everywhere, so a facet may move as far as the caller likes.
 *
 * `flatten` is the flatten itself, validated exactly as the in-place form
 * validates it. `volume` gives the sampling of the RESULT — cell_size is
 * required and > 0, because a document has no intrinsic scale to derive one
 * from; band and padding default as they do for
 * clay_item_volume_from_document. `region_min`/`region_max` are the same
 * optional pair that call takes, with the same rule: both NULL means the
 * document's own bounds padded by the band, and one without the other is
 * refused.
 *
 * Returns a NEW item carrying the flattened volume; the document is not
 * modified. Free it with clay_item_destroy, or place it with
 * clay_layer_add_item. */
clay_result clay_item_volume_flatten_from(const clay_document* doc,
                                          const clay_flatten_params* flatten,
                                          const clay_volume_params* volume,
                                          const float region_min[3],
                                          const float region_max[3],
                                          clay_item** out_item);

typedef struct clay_topological_move_params {
    uint32_t struct_size;  /* = sizeof(clay_topological_move_params); required */
    float anchor[3];       /* a point on or near the surface — what a pick gives */
    float radius;          /* the reach, measured ALONG THE MATERIAL; > 0 */
    float displacement[3]; /* how far to drag, in world units */
    int32_t ease;          /* falloff curve index */
} clay_topological_move_params;

/* ZBrush's Move Topological: a drag whose falloff is weighted by distance along
 * the MATERIAL rather than through space, so a part close in space but far along
 * the surface is not dragged with it.
 *
 * `radius` is therefore a distance of travel across the surface, not a straight
 * line, and cannot step over a gap however narrow. Measured on two fingers 0.32
 * apart joined only through a palm: a Euclidean drag at radius 0.5 pulls the far
 * one, this does not, and raising the radius past the path through the palm
 * brings it into reach — which is what makes the weight a distance and not a
 * mask.
 *
 * The item must carry a volume; anything else is refused rather than ignored.
 * It re-samples that volume with the move applied, and declares the Lipschitz
 * the result measured. clay_layer_move_surface is the cheaper Euclidean move and
 * does not bake — prefer it unless the form has parts close in space and far
 * along the surface. */
clay_result clay_item_volume_move_topological(clay_item* item,
                                              const clay_topological_move_params* params);

/* -- voxel grids ----------------------------------------------------------- */

/* Palette-indexed colored voxels on an integer lattice: cell (x, y, z) covers
 * world space [x, x+1) * voxel_size per axis, and palette index 0 means empty
 * everywhere — setting it erases, painting skips it, meshing emits no face,
 * and it is never returned by clay_voxel_palette_add. Cells cross this ABI as
 * int32_t[3] and batches as a packed int32_t array of count*3, so a caller's
 * (N, 3) int32 buffer needs no repacking.
 *
 * Two lifetimes, and the handle records which one it is:
 *  - clay_voxel_grid_create returns a grid the CALLER owns and destroys.
 *  - clay_document_add_voxel_layer, clay_document_voxel_layer and
 *    clay_document_voxel_layer_by_id return a grid BORROWED from a document
 *    layer: the document owns both the grid and the handle, edits through it
 *    are what clay_document_save writes, and clay_voxel_grid_destroy on it is
 *    rejected rather than obeyed.
 * A borrowed handle names its layer rather than pointing at the grid, and
 * looks it up again on every call, so it never caches a pointer that a later
 * edit to the document could move. clay_document_remove_layer removes the
 * LAYER and leaves the grid beside the document — the inverse of a removal
 * could not carry it — so a handle held across a removal goes on resolving and
 * goes on reading the grid it names. What the removal does change is the
 * lookup: clay_document_voxel_layer and clay_document_voxel_layer_by_id both
 * resolve in the document, so neither hands out a NEW handle to a layer that
 * is no longer there. Should a grid ever stop being held beside its document
 * while a handle still names it, calls through that handle are
 * CLAY_ERROR_NOT_FOUND rather than a use after free. Destroying the document
 * does invalidate the handle and nothing can detect that, so a borrowed handle
 * must not outlive its document. */

typedef struct clay_voxel_grid clay_voxel_grid; /* opaque */

/* Mirror axes of a mirrored edit, OR'd together; 0 is the plain edit. The
 * mirror plane passes through lattice coordinate 0, so cell x reflects to
 * -1-x, and the cell given is always written alongside its reflections. */
typedef enum clay_mirror {
    CLAY_MIRROR_X = 1,
    CLAY_MIRROR_Y = 2,
    CLAY_MIRROR_Z = 4
} clay_mirror;

/* Brush footprint. A sphere is the ball of the same diameter, so it is always
 * a subset of the cube of the same size. */
typedef enum clay_brush_shape {
    CLAY_BRUSH_SHAPE_CUBE = 0,
    CLAY_BRUSH_SHAPE_SPHERE = 1
} clay_brush_shape;

/* Falloff curve over the normalized distance from the footprint centre.
 * Occupancy is binary, so a weight between 0 and 1 cannot be stored in a
 * cell: it is resolved by dithering against a hash of the cell coordinate and
 * the seed, which makes a soft stamp reproducible — same stamp, same seed,
 * same cells, on every platform and through every binding. */
typedef enum clay_brush_falloff {
    CLAY_BRUSH_FALLOFF_CONSTANT = 0, /* hard-edged, the usual brush */
    CLAY_BRUSH_FALLOFF_LINEAR = 1,
    CLAY_BRUSH_FALLOFF_SMOOTH = 2, /* smoothstep */
    CLAY_BRUSH_FALLOFF_GAUSSIAN = 3
} clay_brush_falloff;

/* One brush stamp. Every field belongs to the original layout, so none of
 * them can be left out by a shorter struct_size, and none of them has a
 * default: a struct_size shorter than this is rejected outright. Keep it that
 * way — a field appended later takes the zero value when a caller does not
 * declare it, so only fields whose default is zero may be appended.
 *
 * One field is a pointer, which sharpens the struct_size contract: a caller
 * that declares a struct_size LARGER than the layout it actually compiled
 * against has the boundary read a mask handle out of bytes it never wrote,
 * and a handle cannot be validated the way a number can be range-checked.
 * Declaring sizeof of the struct you compiled against is not a formality.
 *
 * strength reaches the engine untouched, so a stamp lands on exactly the
 * cells the same stamp lands on through the Python bindings. It must be > 0:
 * a strength that is not covers no cell at all, which is a zero-initialized
 * descriptor rather than a request, and unlike the Python bindings this
 * boundary rejects it instead of stamping nothing. A strength at or above 1
 * is the full footprint. */
/* clay_mask is declared above, with the volume parameters that freeze against
 * one; the entry points are in -- masks -- below. */

typedef struct clay_brush_params {
    uint32_t struct_size; /* = sizeof(clay_brush_params); required, see above */
    int32_t size;         /* cells the footprint spans per axis; must be > 0 */
    int32_t shape;        /* clay_brush_shape */
    int32_t falloff;      /* clay_brush_falloff */
    float strength;       /* coverage multiplier; must be > 0, >= 1 is full */
    uint32_t seed;        /* dither seed */
    /* Appended at ABI 0.12.0. NULL — the zero value, and what a caller
     * compiled against 0.11.0 effectively declares — means no gating, so an
     * older caller's stamp lands exactly where it always did. The mask is
     * borrowed for the duration of the call and must outlive it. */
    const clay_mask* mask;
} clay_brush_params;

/* A standalone grid, owned by the caller. voxel_size is world units per cell
 * and must be > 0. Returns NULL on invalid input, with the detail in
 * clay_last_error(). Free with clay_voxel_grid_destroy. */
clay_voxel_grid* clay_voxel_grid_create(float voxel_size);
/* Frees a grid created by clay_voxel_grid_create. A handle borrowed from a
 * document layer is owned by that document: this returns
 * CLAY_ERROR_INVALID_ARGUMENT and leaves the document untouched. */
clay_result clay_voxel_grid_destroy(clay_voxel_grid* grid);

/* Adds a voxel layer and borrows its grid. A voxel layer carries no SDF
 * content, so clay_add_item and clay_layer_add_item do not apply to it.
 *
 * ONE COMMAND, so an enabled undo stack records the creation and a single undo
 * removes the layer. The grid is RETAINED across that undo — an AddLayerCmd
 * carries a layer by value and could not carry a sculpt — so a redo brings the
 * layer back with its cells and its id. While the layer is absent the grid is
 * not reachable (clay_document_voxel_layer reports NOT_FOUND) and is not
 * written by clay_document_save.
 *
 * Bracket this with the fill that follows it (clay_document_begin_undo_group)
 * and the whole crossing is one undo, which is what a user who asked for a
 * conversion is taking back. */
clay_result clay_document_add_voxel_layer(clay_document* doc, const char* name,
                                          float voxel_size, clay_layer_id* out_layer,
                                          clay_voxel_grid** out_grid);
/* Borrows the grid of an existing voxel layer by name; CLAY_ERROR_NOT_FOUND
 * when the document has no such layer. The name is not a unique key — nothing
 * has ever required one — so this answers with the FIRST voxel layer in stack
 * order carrying the name, and it follows clay_document_set_layer_name: hold
 * the id when a lookup has to survive a rename, and reach the grid with
 * clay_document_voxel_layer_by_id below. This one stays for the ordinary case
 * of a document with one layer of that name. */
clay_result clay_document_voxel_layer(clay_document* doc, const char* name,
                                      clay_layer_id* out_layer, clay_voxel_grid** out_grid);

/* Borrows the grid of the voxel layer carrying this id — the same lookup with
 * the name resolution taken out. The borrow is unchanged in every respect: see
 * the lifetime note above clay_document_add_voxel_layer.
 *
 * Ids are stable across a save and load, so this is the lookup that survives a
 * rename and the one that tells two layers sharing a name apart. Before it
 * existed a host holding an id had no way to spend it, and the workaround was
 * to forbid duplicate names on voxel layers — a rule this ABI asks for nowhere
 * else, and one an artist naming two spheres the same thing runs into first.
 *
 * CLAY_ERROR_NOT_FOUND when no layer carries the id, when the layer carrying it
 * is not a voxel layer, and when it is a voxel layer whose grid is not held
 * beside the document. The layer is resolved in the DOCUMENT, not in the grids
 * held beside it, and that order is load-bearing: undoing the creation of a
 * voxel layer removes the layer and deliberately KEEPS its cells so a redo can
 * pick them back up, so a lookup that consulted the grids alone would hand back
 * the grid of a layer that is not currently in the document. The by-name form
 * reports NOT_FOUND there and so does this one — anything it refuses for a
 * given layer, this refuses too.
 *
 * out_grid is REQUIRED, unlike the by-name form's. There a NULL means
 * something — the call doubles as an existence probe and still answers through
 * out_layer — while here the caller supplied the id and the handle is the only
 * answer there is, so a NULL asks nothing. clay_document_layer_info is the call
 * that says whether a layer exists and what representation it is. A NULL
 * document or a NULL out_grid is CLAY_ERROR_INVALID_ARGUMENT, and a refused
 * call writes nothing. */
clay_result clay_document_voxel_layer_by_id(clay_document* doc, clay_layer_id layer,
                                            clay_voxel_grid** out_grid);

/* Cell size of the grid's ACTIVE resolution level (see below). */
clay_result clay_voxel_size(const clay_voxel_grid* grid, float* out_voxel_size);

/* -- resolution levels ----------------------------------------------------- */

/* A grid holds a stack of levels: level 0 is the coarsest and level k has half
 * the cell size of level k-1. One is ACTIVE, and every other entry point in
 * this file acts on it — the level is grid state rather than an argument, so
 * nothing here changed shape and a caller that never mentions a level gets a
 * one-level grid behaving exactly as it did before these calls existed.
 *
 * Adding a level subdivides every occupied cell into its eight children, so the
 * solid is unchanged. Editing at a level averages down into the coarser ones
 * and replays into the finer ones from the offsets they hold, which is what
 * makes a broad stroke at a coarse level leave fine detail standing. Dropping a
 * level discards the detail only that level held. */
clay_result clay_voxel_level_count(const clay_voxel_grid* grid, size_t* out_count);
clay_result clay_voxel_active_level(const clay_voxel_grid* grid, size_t* out_level);
/* CLAY_ERROR_INVALID_ARGUMENT for a level the grid does not have, grid
 * untouched. */
clay_result clay_voxel_set_active_level(clay_voxel_grid* grid, size_t level);
/* Appends a level at half the finest cell size and reports its index. The stack
 * is capped, and CLAY_ERROR_INVALID_ARGUMENT at the cap: every level costs
 * eight times the cells of the one below, so a stream naming an arbitrary count
 * would be a request to allocate one. */
clay_result clay_voxel_add_level(clay_voxel_grid* grid, size_t* out_level);
/* The same, refined only over a region: min[3], max[3] in WORLD units, rounded
 * OUT to whole chunks. Outside the region the new level has no storage and
 * reads its parent's value, so the lattice is still uniform and complete — only
 * what is STORED changes, and meshing, bounds and neighbour indexing are as
 * they were.
 *
 * Writing outside the region refines what the write touched, so a brush that
 * straddles the boundary works and the stored set follows what was touched
 * rather than what was reserved.
 *
 * A chunk is 32 cells across, so a region smaller than that still costs one.
 * The saving is on a form spanning many chunks at the resolution being
 * authored, which is the case a level stack is for. */
clay_result clay_voxel_add_level_region(clay_voxel_grid* grid, const float min[3],
                                        const float max[3], size_t* out_level);
/* Drops the finest level. CLAY_ERROR_INVALID_ARGUMENT when only one is left,
 * since a grid always has at least one. */
clay_result clay_voxel_drop_level(clay_voxel_grid* grid);
/* Cell size and occupied cells of ONE level, so a host can report what each
 * level of a stack costs without making it active first. */
clay_result clay_voxel_level_voxel_size(const clay_voxel_grid* grid, size_t level,
                                        float* out_voxel_size);
clay_result clay_voxel_level_occupied_count(const clay_voxel_grid* grid, size_t level,
                                            size_t* out_count);
/* How many chunks a level stores, and whether it stores all of them. A level
 * added with no region is whole, which is what every grid written before
 * regions existed is. */
clay_result clay_voxel_level_chunk_count(const clay_voxel_grid* grid, size_t level,
                                         size_t* out_count);
clay_result clay_voxel_level_is_whole(const clay_voxel_grid* grid, size_t level, int32_t* out_whole);

/* -- palette --------------------------------------------------------------- */

/* Palette indices are [0, 255] and anything else is rejected. Index 0 is the
 * empty slot: it is never added, never matched, and cannot be recolored.
 * The palette saturates at 256 entries, returning 255 rather than growing. */
clay_result clay_voxel_palette_add(clay_voxel_grid* grid, const float rgb[3],
                                   int32_t* out_index);
clay_result clay_voxel_palette_color(const clay_voxel_grid* grid, int32_t index,
                                     float out_rgb[3]);
/* Recolors an entry; every voxel referencing it follows, with no voxel data
 * touched. A no-op for index 0 and for an index the palette does not hold. */
clay_result clay_voxel_palette_set(clay_voxel_grid* grid, int32_t index, const float rgb[3]);
/* Counts the unused index-0 slot, so a fresh grid reports 1. */
clay_result clay_voxel_palette_size(const clay_voxel_grid* grid, size_t* out_size);

/* -- edits ----------------------------------------------------------------- */

clay_result clay_voxel_get(const clay_voxel_grid* grid, const int32_t cell[3],
                           int32_t* out_index);
clay_result clay_voxel_set(clay_voxel_grid* grid, const int32_t cell[3], int32_t index);
clay_result clay_voxel_erase(clay_voxel_grid* grid, const int32_t cell[3]);
/* Recolors an occupied cell; empty cells and index 0 are no-ops. */
clay_result clay_voxel_paint(clay_voxel_grid* grid, const int32_t cell[3], int32_t index);

/* Batch forms: cells_xyz is count*3 int32 values. */
clay_result clay_voxel_set_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count,
                                int32_t index);
clay_result clay_voxel_erase_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count);

clay_result clay_voxel_fill_box(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                int32_t index); /* inclusive corners */
clay_result clay_voxel_fill_line(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                 int32_t index); /* 3D DDA */

/* The edit and every mirror combination of `axes` (clay_mirror bits) in one
 * call; axes == 0 is the plain edit. Bits outside the three axes are
 * rejected. */
clay_result clay_voxel_set_mirrored(clay_voxel_grid* grid, const int32_t cell[3], int32_t index,
                                    int32_t axes);
clay_result clay_voxel_paint_mirrored(clay_voxel_grid* grid, const int32_t cell[3],
                                      int32_t index, int32_t axes);

/* -- brushes --------------------------------------------------------------- */

/* Stamp the footprint centered on a cell. Size n spans exactly n cells per
 * axis for every n: the footprint runs -((n-1)/2) ..= n/2, symmetric for odd
 * n and biased half a cell toward +XYZ for even n. */
clay_result clay_voxel_set_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                 const clay_brush_params* brush, int32_t index);
clay_result clay_voxel_erase_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush);
clay_result clay_voxel_paint_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush, int32_t index);

/* -- sculpt layers --------------------------------------------------------- */

/* A pass you can dial back after making it — ZBrush's layers, on a voxel grid.
 * Bracket any run of edits with begin/end and the grid remembers what those
 * edits CHANGED, so their strength stays adjustable long after the strokes are
 * finished. This is not undo: undo is a stack you pop, a sculpt layer is a
 * slider you keep.
 *
 * A layer stores what its pass DID, not the brushes that did it. Dialling a
 * layer replays recorded cells; it does not re-run the strokes. So a pass whose
 * result depended on the layer under it keeps the result it recorded when that
 * layer is dialled away — which is what a layer stack means, and what ZBrush
 * does. Re-running would make a layer's content depend on what sits below it.
 *
 * On binary occupancy, a fractional strength is a fraction of the CELLS, chosen
 * by the same cell-coordinate hash the falloff brushes dither with: the same
 * strength picks the same cells on every platform and every run, and raising it
 * ADDS cells to the ones already showing rather than reshuffling. Strength 0
 * and 1 are exact — the grid without the pass, and the pass applied directly.
 *
 * Layers composite bottom-up, so the last one recorded wins where two overlap. */

/* Starts recording. A name is optional (NULL for none) and is what a UI shows.
 * Returns the new layer's index in *out_layer. Rejected while a layer is
 * already recording — nesting has no meaning here, since a cell can only
 * belong to one pass. */
clay_result clay_voxel_begin_sculpt_layer(clay_voxel_grid* grid, const char* name,
                                          size_t* out_layer);
/* Stops recording. Edits after this belong to no layer and are not dialled
 * away with one. A no-op when nothing is recording. */
clay_result clay_voxel_end_sculpt_layer(clay_voxel_grid* grid);
clay_result clay_voxel_recording_sculpt_layer(const clay_voxel_grid* grid, int32_t* out_recording);
clay_result clay_voxel_sculpt_layer_count(const clay_voxel_grid* grid, size_t* out_count);
/* By the size-query pattern clay_layer_name uses. */
clay_result clay_voxel_sculpt_layer_name(const clay_voxel_grid* grid, size_t layer, char* buffer,
                                         size_t* size);
/* How many cells the pass changed — its cost, and whether it did anything. */
clay_result clay_voxel_sculpt_layer_cell_count(const clay_voxel_grid* grid, size_t layer,
                                               size_t* out_count);
clay_result clay_voxel_sculpt_layer_strength(const clay_voxel_grid* grid, size_t layer,
                                             float* out_strength);
/* Clamped to [0, 1] rather than rejected: a slider that overshoots is a caller
 * being a caller. */
clay_result clay_voxel_set_sculpt_layer_strength(clay_voxel_grid* grid, size_t layer,
                                                 float strength);
clay_result clay_voxel_sculpt_layer_visible(const clay_voxel_grid* grid, size_t layer,
                                            int32_t* out_visible);
clay_result clay_voxel_set_sculpt_layer_visible(clay_voxel_grid* grid, size_t layer,
                                                int32_t visible);
/* Drops a layer and replays the ones above it. */
clay_result clay_voxel_remove_sculpt_layer(clay_voxel_grid* grid, size_t layer);
/* Folds `layer` into the one below at full strength, keeping the lower layer's
 * name. Rejected for layer 0, which has nothing below it. */
clay_result clay_voxel_merge_sculpt_layer_down(clay_voxel_grid* grid, size_t layer);
/* Moves a layer within the stack, sliding the rest along. Order is meaningful:
 * where two passes touched the same cell, moving one past the other changes
 * which value survives. Replays the recorded diffs in the new order rather
 * than re-running the strokes. */
clay_result clay_voxel_move_sculpt_layer(clay_voxel_grid* grid, size_t from, size_t to);
/* What the layers cost in memory — recorded cells plus the recording index.
 * Nothing is enforced: a cap that silently stopped recording would leave a
 * pass on the grid and un-dialable, which is a correctness bug wearing a
 * memory limit's clothes. A host with a budget merges layers down (one entry
 * per cell instead of two) or stops recording. Pass a layer index for one
 * layer's share; clay_voxel_sculpt_layers_bytes is the whole stack. */
clay_result clay_voxel_sculpt_layer_bytes(const clay_voxel_grid* grid, size_t layer,
                                          size_t* out_bytes);
clay_result clay_voxel_sculpt_layers_bytes(const clay_voxel_grid* grid, size_t* out_bytes);

/* -- sculpting verbs ------------------------------------------------------- */

/* These reshape existing material instead of stamping a footprint. Each reads
 * a snapshot of the region first, so a cell's outcome never depends on a
 * neighbour the same call already changed.
 *
 * EVERY verb here can be a perfectly valid call that changes nothing: a
 * flatten on an already-flat region, a smudge shorter than a cell, a dithered
 * stamp that misses every cell it was offered, a footprint over empty space.
 * All of them return CLAY_OK, because they did what they were asked. There is
 * no "valid but had no effect" result code and there will not be one — an
 * existing entry point returning a new non-zero value would turn a success
 * into a failure for every caller already compiled. Read
 * clay_voxel_change_count before and after instead; the difference is what
 * moved. */

/* Majority filter over the 26-neighbourhood: spurs dissolve, notches fill. */
clay_result clay_voxel_sculpt_smooth(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush);
/* amount > 0 dilates, < 0 erodes, |amount| one-cell passes; 0 is a no-op. */
clay_result clay_voxel_sculpt_inflate(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, int32_t amount);
/* Pull the surface onto the plane through the brush centre: material on the
 * +normal side goes, hollows on the other side that touch material are
 * filled. offset_cells slides the plane along +normal, in CELLS. Unlike the
 * Python bindings, a zero-length normal is rejected rather than evaluated. */
clay_result clay_voxel_sculpt_flatten(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, const float normal[3],
                                      float offset_cells);
/* Move surface cells one step toward the brush centre. */
clay_result clay_voxel_sculpt_pinch(clay_voxel_grid* grid, const int32_t cell[3],
                                    const clay_brush_params* brush);
/* ...and one step away from it: pinch's inverse, sharing its walk so the two
 * cannot drift apart. The SDF side spells the pair as one signed strength on
 * CLAY_DEFORM_MAGNIFY for the same reason. */
clay_result clay_voxel_sculpt_magnify(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush);
/* Translate occupancy through the same map the SDF grab deformer uses. Binary
 * occupancy resamples nearest-cell: a displacement larger than a cell moves
 * material in whole cells rather than flowing, and one shorter than half a
 * cell rounds back to the cell it started in and moves nothing at all. That is
 * not an error and is not reported as one — the call did exactly what it was
 * asked, which is why a drag tool fed raw pointer deltas looks dead: at a
 * voxel_size of 0.12 every per-sample delta of a slow drag is sub-cell, and
 * every call returns CLAY_OK.
 *
 * The rounding is PER AXIS, so a pull of 0.4 cells on each of the three is
 * 0.69 cells long and still moves nothing. Half a cell on the largest
 * component is the necessary condition, not a sufficient one: away from the
 * centre the falloff shrinks the pull further, front_only halves it at the
 * centre outright (the front gate is 0.5 on the plane through it, so the dead
 * zone is twice as wide), and material that moves into a cell holding the same
 * index changes nothing either way.
 *
 * So there are two things to reach for. clay_voxel_size gives the cell size,
 * which is what a host accumulates its drag against before calling — the verb
 * is stateless and has no gesture to accumulate over, so that belongs in the
 * host. And clay_voxel_change_count, read before and after, says what actually
 * happened. clay_voxel_occupied_count is not a substitute: grab conserves
 * material, so a lump dragged a whole cell can leave it identical. */
clay_result clay_voxel_sculpt_grab(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush, const float displacement[3],
                                   int32_t front_only);

/* -- a grab as a GESTURE --------------------------------------------------- */

/* THE CALL ABOVE DOES NOT COMPOSE, and this is what to use for a drag.
 *
 * A grab of N cells is not N grabs of one cell. Each call reads the grid,
 * resamples occupancy through the falloff and writes back, so the next call
 * reads its own output — and the displacement is rounded to whole cells AFTER
 * the falloff weights it, so at one cell only the very middle of the region
 * rounds to a cell, which inside solid material changes no occupancy at all.
 * Split finely enough, the whole drag evaporates. Measured on a solid ball 16
 * cells across, the same total drag of 8 cells split into 1, 2, 4 and 8
 * emissions, counting cells that ended up somewhere new (issue #393):
 *
 *     footprint    1 x 8    2 x 4    4 x 2    8 x 1
 *     24 cells        59       61        0        0
 *     32 cells       205      169      190        0
 *     40 cells       357      376      293      126
 *
 * Occupancy is not conserved across the split either — 2109 cells at rest
 * became 2235, 2298 and 2371 — so composed grabs smear and duplicate rather
 * than translate.
 *
 * That is not a rounding bug to fix inside the call: a stateless verb has no
 * gesture to be idempotent over. Accumulating past the cell size host-side and
 * emitting whole cells, which is what the note above suggests, produces exactly
 * the stream of one-cell grabs that moves nothing.
 *
 * So a drag begins a TRANSACTION. It captures the material as it was, and every
 * update takes the TOTAL displacement from the anchor — never an increment on
 * the last frame — resampled from that capture. update(1), update(2), update(8)
 * ends at exactly what a single update(8) produces; repeating an update changes
 * nothing; and a pointer that comes back to where it started puts the material
 * back. The same shape clay_sdf_move_begin has on the field side, for the same
 * reason.
 *
 * WHAT IT STILL DOES NOT DO. Occupancy is binary and the resample is
 * nearest-cell, so a total under half a cell on every axis moves nothing: there
 * is no sub-cell state for it to move. What changes is that the total is
 * measured from the ANCHOR, so a slow drag accumulates toward that half cell
 * instead of rounding to zero on every frame. */
typedef struct clay_voxel_grab_tx clay_voxel_grab_tx;

/* Begin a drag anchored at `cell`. NULL on a null grid or params, or a brush
 * size below 1, which is not a footprint. A drag that reaches only empty space
 * is a valid transaction that changes nothing.
 *
 * The grid is BORROWED and must outlive the transaction, exactly as a voxel
 * sculpt layer's handle is. */
clay_voxel_grab_tx* clay_voxel_grab_begin(clay_voxel_grid* grid, const int32_t cell[3],
                                          const clay_brush_params* brush, int32_t front_only);

/* The drag so far, measured from the anchor — the TOTAL, never an increment.
 * Idempotent: calling it twice with the same displacement leaves the grid
 * exactly as one call did, so a host may call it every frame without checking
 * whether the pointer moved. */
clay_result clay_voxel_grab_update(clay_voxel_grab_tx* tx, const float total_displacement[3]);

/* The box the gesture writes — the brush's footprint, fixed for the whole
 * gesture whatever the displacement grows to, because a grab writes inside its
 * own footprint and nowhere else. A host invalidating a region has this from
 * the first frame and never needs to widen it. Either pointer may be NULL. */
clay_result clay_voxel_grab_written_box(const clay_voxel_grab_tx* tx, int32_t out_lo[3],
                                        int32_t out_hi[3]);

/* Whether the gesture is still open — non-zero until it is committed or
 * cancelled. A host holding a transaction across frames has this rather than
 * having to remember; a NULL handle reports 0. */
clay_result clay_voxel_grab_live(const clay_voxel_grab_tx* tx, int32_t* out_live);

/* Keep what the last update wrote. The grid already holds it, so this only ends
 * the gesture and releases the capture; the handle must still be destroyed. */
clay_result clay_voxel_grab_commit(clay_voxel_grab_tx* tx);
/* Put the captured material back, exactly as it was at begin. */
clay_result clay_voxel_grab_cancel(clay_voxel_grab_tx* tx);
/* Frees the handle. An uncommitted transaction is CANCELLED first, so a host
 * that drops one on an error path does not leave a half-finished drag in the
 * grid. */
void clay_voxel_grab_destroy(clay_voxel_grab_tx* tx);

/* -- masks ----------------------------------------------------------------- */

/* A paintable scalar mask in [0, 1] gating how strongly an edit may act: the
 * effective brush weight at a cell becomes strength * (1 - mask), so a fully
 * masked cell is frozen and an unmasked one is untouched by the feature.
 *
 * The mask is addressed in WORLD units on its own lattice, deliberately not
 * in a layer's voxel cells, so changing a layer's resolution or moving
 * content between the SDF and voxel representations cannot silently drop or
 * misalign it. Its cell size is independent of any grid's.
 *
 * Masking gates edits where they are AUTHORED. Voxel edits consume a mask at
 * apply time, per cell. SDF edits are declarative items with no per-point
 * strength, so they consume it when a stroke becomes items — a mask painted
 * now does not retroactively gate items already in the edit list.
 *
 * Lifetime mirrors clay_voxel_grid exactly: clay_mask_create returns a mask
 * the CALLER destroys, while clay_document_add_mask and clay_document_mask
 * return one BORROWED from a document layer, which clay_mask_destroy rejects
 * and which must not outlive its document. */

/* A standalone mask, owned by the caller. cell_size is world units per mask
 * cell and must be > 0. NULL on invalid input, detail in clay_last_error. */
clay_mask* clay_mask_create(float cell_size);
/* Frees a mask from clay_mask_create; rejects a borrowed handle. */
clay_result clay_mask_destroy(clay_mask* mask);

/* Attach a mask to a layer and hand back a borrowed handle. Replaces any mask
 * the layer already had. */
clay_result clay_document_add_mask(clay_document* doc, clay_layer_id layer, float cell_size,
                                   clay_mask** out_mask);
/* The layer's mask, or CLAY_ERROR_NOT_FOUND when it has none. */
clay_result clay_document_mask(clay_document* doc, clay_layer_id layer, clay_mask** out_mask);
/* Drop a layer's mask. CLAY_ERROR_NOT_FOUND when there was none. Borrowed
 * handles onto it then fail with CLAY_ERROR_NOT_FOUND rather than dangling. */
clay_result clay_document_remove_mask(clay_document* doc, clay_layer_id layer);

clay_result clay_mask_cell_size(const clay_mask* mask, float* out_cell_size);
clay_result clay_mask_painted_count(const clay_mask* mask, size_t* out_count);
/* Non-zero when nothing has been painted. Equivalent to a zero painted count,
 * and the cheap way to ask. */
clay_result clay_mask_empty(const clay_mask* mask, int32_t* out_empty);

/* Single-cell access, on the MASK's lattice. Values are clamped to [0, 1]. */
clay_result clay_mask_get(const clay_mask* mask, const int32_t cell[3], float* out_value);
clay_result clay_mask_set(clay_mask* mask, const int32_t cell[3], float value);

/* Mask at a world position, 0 where nothing has been painted. */
clay_result clay_mask_sample(const clay_mask* mask, const float point[3], float* out_value);
/* Batch form: points is a packed float array of count*3, out_values receives
 * count floats. Neither is a size query — the caller knows the count. */
clay_result clay_mask_sample_many(const clay_mask* mask, const float* points_xyz, size_t count,
                                  float* out_values);

/* Brush the mask toward `target` (clamped to [0, 1]): 1 masks, 0 erases. The
 * footprint is the ordinary brush vocabulary, sized in MASK cells; the seed
 * is unused, because a mask stores a fractional weight directly rather than
 * dithering it. brush->mask is ignored here — a mask does not gate itself. */
clay_result clay_mask_paint(clay_mask* mask, const float point[3],
                            const clay_brush_params* brush, float target);
clay_result clay_mask_paint_cell(clay_mask* mask, const int32_t cell[3],
                                 const clay_brush_params* brush, float target);

/* Region operations over the painted region. invert flips what has been
 * painted: a sparse field has no finite complement to invert into. */
clay_result clay_mask_invert(clay_mask* mask);
clay_result clay_mask_clear(clay_mask* mask);
clay_result clay_mask_expand(clay_mask* mask, int32_t steps);   /* grey dilation */
clay_result clay_mask_contract(clay_mask* mask, int32_t steps); /* grey erosion */
clay_result clay_mask_smooth(clay_mask* mask, int32_t iterations);

/* Inclusive cell bounds of the painted region. *out_has_bounds is 0 for an
 * empty mask, and out_min/out_max are then left alone. */
clay_result clay_mask_bounds(const clay_mask* mask, int32_t out_min[3], int32_t out_max[3],
                             int32_t* out_has_bounds);

/* Fill a world-space box with a value, and take the complement over one.
 *
 * These are the BOUNDED forms clay_mask_invert cannot be. Inverting the painted
 * region is the only thing a sparse unbounded lattice can do, and it is not
 * what "mask a limb, invert, sculpt everything else" means: the untouched
 * storage stays unmasked and the boundary lands on chunk edges rather than on
 * the painted region. So the caller supplies the finite region — it always has
 * one, from a grid's bounds or an item's.
 *
 * A cell belongs to the box when its CENTRE does. An empty or unbounded box is
 * refused: the cost of both calls is the box's volume in cells. */
clay_result clay_mask_fill(clay_mask* mask, const float box_min[3], const float box_max[3],
                           float value);
clay_result clay_mask_invert_within(clay_mask* mask, const float box_min[3],
                                    const float box_max[3]);

/* clay_mask_apply_stroke — painting a mask along a drag — lives with the other
 * stroke consumers below, since it needs clay_stroke_preset. */

/* -- mask extrude ---------------------------------------------------------- */

/* Mask a patch of a surface and pull it off as a solid of a chosen thickness.
 * ZBrush calls it Extract, 3DCoat reaches it through Extrude from a frozen
 * area, and it is what a mask is FOR once it can do more than freeze.
 *
 * THE MASK IS THE REGION. Relax and flatten both need a region_radius because
 * they have no other way to know where to act; this does not, because the
 * painted region bounds itself. */

/* Which side of the source surface the new material sits on. */
typedef enum clay_extrude_side {
    CLAY_EXTRUDE_OUTWARD = 0, /* the plate sits ON the surface */
    CLAY_EXTRUDE_INWARD = 1,  /* the pocket */
    CLAY_EXTRUDE_CENTRED = 2, /* straddles it */
} clay_extrude_side;

typedef struct clay_mask_extrude_params {
    uint32_t struct_size;  /* = sizeof(clay_mask_extrude_params); required */
    float thickness;       /* wall thickness in world units; must be > 0 */
    int32_t side;          /* clay_extrude_side */
    float threshold;       /* what counts as masked; <= 0 means 0.5 */
    float border_round;    /* rounding radius on the rim; 0 is a hard edge */
    int32_t border_smooth; /* smoothing passes on a COPY of the mask; the caller's is kept */
    float cell_size;       /* sampling of the result; <= 0 means the mask's own */
    float band;            /* <= 0 means three cells */
} clay_mask_extrude_params;

/* Gates this item by a painted MASK, so the item does not act where the mask
 * protects — which is what makes masking protect a surface from ANY operation
 * rather than only from a brush.
 *
 * Masks gate AUTHORING elsewhere: clay_voxel_* edits consume one per cell as
 * they write, and an SDF stroke consumes one when it becomes items. Neither
 * touches an item already in the edit list, so a mask over an ear has never
 * done anything about the next boolean. This does.
 *
 * The mask is MEASURED here, not stored: what the item carries is the signed
 * distance to { mask >= threshold }, which clay_mask_to_field also exposes.
 * That is what gives the gate a Lipschitz bound worth having — a distance is
 * 1-Lipschitz, so the falloff's cost is set by `width`, which you choose,
 * rather than by however hard the brush edge that painted the mask happened to
 * be. Painted softness is therefore re-derived rather than preserved.
 *
 * `width` is how far the protection fades across, in world units. A WIDE gate
 * costs almost no step scale and a NARROW one costs honestly; read the cost
 * back with clay_safe_step_scale. Zero is clamped rather than honoured,
 * because a step in the field has no finite bound and nothing could march it.
 *
 * `threshold` is the paint level that counts as protected; <= 0 means 0.5.
 *
 * THE GATE IS IN WORLD SPACE, and does not travel with the item. A mask is
 * painted in world units on its own lattice (see clay_mask_create), so the
 * region it protects is where you painted it and stays there whatever
 * clay_item_set_position, _set_rotation and _set_scale then do to the gated
 * item. Gate a cut placed over an ear and the ear stays; move the cut and the
 * ear still stays. Before 0.67.0 the gate was placed by the item's own
 * transform, which moved the protected region along with the cut and made a
 * correctly configured gate look inert to any host whose items carried a
 * placement (issue #394).
 *
 * The gate is COPIED into the item, so the caller's mask may change or be
 * destroyed afterwards. Refused, leaving the item ungated, when the mask is
 * empty or nothing reaches the threshold — a gate that protects nothing and
 * reports success is harder to notice than a failure. */
clay_result clay_item_set_gate(clay_item* item, const clay_mask* mask, float threshold,
                               float width);

/* The mask, measured: signed distance to the boundary of the masked region,
 * negative inside, as an item carrying a volume.
 *
 * This is the conversion the extrude is built on, exposed because a host wants
 * to preview that border. A mask is a [0,1] scalar on a lattice and not a
 * distance field; composing one into a field expression directly would put a
 * step in the result and the Lipschitz bound would become a fiction.
 *
 * `pad` widens the sampled region past the masked one, `band` and `cell_size`
 * are the usual sampling controls, and <= 0 takes the default for each. The
 * returned item is owned by the caller until it is added to a layer. An empty
 * mask is refused. */
clay_result clay_mask_to_field(const clay_mask* mask, float threshold, float band, float pad,
                               float cell_size, clay_item** out_item);

/* -- the sculpt handoff: sculpt -> retopo -> UV -> bake (ABI 0.52.0) ---------
 *
 * THE FORMAT IS NOT DEFINED HERE. It is CyberRemesherAndUV's
 * docs/sculpt-handoff-format.md, version 1.0, and that repository ships the
 * READING half — it says so, and says agreement with ClayCore was outstanding
 * because no negotiation ever took place. Their CLI already assumes this half
 * exists:
 *
 *     producer --for-retopo | cyberremesh --target - --output low.obj
 *
 * Where their spec and this header disagree, their spec is right.
 *
 * TWO GUARANTEES THE WRITER MAKES FOR YOU, because both are conditions their
 * reader enforces and neither is something a caller can be expected to know:
 *
 *   THE FACES ARE ALWAYS TRIANGLES. clay_mesh_save declares a mesh's QUADS as
 *   its faces when it has them, and their reader rejects any other arity —
 *   "a sculpt export that is not triangulated is a producer bug". So the quad
 *   export, our best one, is exactly the file it would refuse.
 *
 *   NORMALS ARE ALWAYS PRESENT. They are required, and a mesh meshed without
 *   gradients has none. They are computed into the output; your mesh is not
 *   modified.
 *
 * WHAT material_mix IS. Their spec calls it "the sculpt's per-vertex blend
 * weight between two material slots". ClayCore has no material slots and does
 * not invent them: pass a MASK and its value at each vertex becomes the
 * channel — a mask is already a painted scalar in [0,1] answerable at any
 * point, which is the shape and the meaning asked for. NULL writes zeros,
 * which is the honest answer for a document that never expressed one. */
#define CLAY_HANDOFF_VERSION_MAJOR 1
#define CLAY_HANDOFF_VERSION_MINOR 0

/* Write the PLY profile, which their spec calls the normative one. `producer`
 * may be NULL for the default label. `material_mask` may be NULL. */
clay_result clay_mesh_save_handoff(const clay_mesh* mesh, const char* path,
                                   const char* producer, const clay_mask* material_mask,
                                   int32_t binary);

/* The same bytes, in memory, for the pipe route their CLI documents. */
clay_result clay_mesh_save_handoff_memory(const clay_mesh* mesh, const char* producer,
                                          const clay_mask* material_mask, int32_t binary,
                                          clay_blob** out_blob);

/* The IN-MEMORY BUFFER PROFILE is deliberately not a struct here.
 *
 * Their cyber::handoff::BufferView wants positions, normals, colours, material
 * mix and indices. FOUR OF THOSE FIVE YOU ALREADY HAVE as borrowed pointers —
 * clay_mesh_positions, _normals, _colors, _indices — so a struct of ours would
 * duplicate one they own and give the two engines two places to disagree about
 * the layout. This produces the one array you cannot get from what is already
 * exposed.
 *
 * Capacity in, count out. `mask` NULL fills zeros. Note that clay_mesh_normals
 * may be NULL for a mesh meshed without gradients, which their buffer profile
 * accepts (normals are optional there, unlike the PLY profile) — but a bake is
 * better with them, so compute them first if you have the choice. */
clay_result clay_mesh_handoff_material_mix(const clay_mesh* mesh, const clay_mask* material_mask,
                                           float* out_values, size_t capacity,
                                           size_t* out_count);

/* -- WIRING ClayCore INTO THEIR BAKER ----------------------------------------
 *
 * Their engine bakes; this one answers field queries. That seam was chosen
 * deliberately: baking wants UV semantics — seams, islands, padding, dilation,
 * texel density — which is what their repository owns, and a second
 * implementation here would disagree with theirs about exactly the details that
 * make a bake look right.
 *
 * Their CyberFieldEvaluator takes three C callbacks and a void* user. Nothing
 * further is needed from this ABI to fill them; the correspondence is:
 *
 *   distance(p)            -> clay_eval_points
 *   gradient(p)            -> clay_eval_gradients (already unit length)
 *   occlusion(p, n, r)     -> 1.0f - clay_measure_points(CLAY_MEASURE_OCCLUSION)
 *   curvature(p, h)        -> leave it to their default, which derives it from
 *                             gradient(). CLAY_MEASURE_CURVATURE is a SATURATED
 *                             [0,1] value for masking, while theirs is signed
 *                             mean curvature in 1/length units; they are not
 *                             the same number and substituting one is wrong.
 *
 * NOTE THE INVERSION, which is the trap in this table. Their `occlusion` is
 * OPENNESS — 1 is fully open — and CLAY_MEASURE_OCCLUSION is occlusion, where
 * 1 is fully enclosed. Passing ours straight through bakes an inverted ambient
 * occlusion map, which looks plausible and is wrong everywhere. */

/* -- measuring the surface (ABI 0.51.0) -------------------------------------
 *
 * What the shape IS at a point: how it bends, how enclosed it is, how much
 * material is behind it. The measures a cavity mask, a wear mask and a baked
 * texture all want, and none of which this ABI could ask for.
 *
 * WHY THIS IS CHEAP ON A FIELD and expensive in a mesh engine. Curvature here
 * is the field's LAPLACIAN, and its sign is unambiguous: for f = |p| - R the
 * Laplacian at the surface is 2/R, POSITIVE for a convex surface. So cavity and
 * convexity are one subtraction apart. A mesh has to estimate curvature from a
 * vertex ring, which is a discrete approximation with a valence-dependent error.
 *
 * The same argument runs for occlusion. A mesh traces rays against triangles
 * through an acceleration structure that must be rebuilt when the mesh changes;
 * a field is marched directly, at any resolution, with nothing to build and
 * nothing to invalidate — and it measures the ACTUAL surface rather than a
 * tessellation of it.
 *
 * DETERMINISM. Every query in this library returns the same bits on every
 * backend and every run. A hemisphere sample is the first thing that could
 * quietly break that, so the pattern is a fixed low-discrepancy sequence
 * rotated by a hash of the POINT and an explicit seed — not a random number
 * generator, not thread-dependent, and not order-dependent. Same seed, same
 * bits, every backend. */
typedef enum clay_surface_measure {
    CLAY_MEASURE_CURVATURE = 0,   /* |Laplacian|: anywhere the surface bends */
    CLAY_MEASURE_CAVITY = 1,      /* concave only — crevices, folds, seams */
    CLAY_MEASURE_CONVEXITY = 2,   /* convex only — hard edges, ridges */
    CLAY_MEASURE_NORMAL_DIR = 3,  /* agreement with a direction: "facing up" */
    /* How enclosed a point is: the blocked fraction of a hemisphere around the
     * normal, within ray_length. 0 is open sky, 1 is fully enclosed.
     * OCCLUSION, not lighting — the greater number is the darker place, since
     * tools disagree about which way this runs. */
    CLAY_MEASURE_OCCLUSION = 4,
    /* Material behind the surface: the distance INWARD to where the field turns
     * positive again, over ray_length. 1 is thicker than the probe could see. */
    CLAY_MEASURE_THICKNESS = 5
} clay_surface_measure;

typedef struct clay_measure_params {
    uint32_t struct_size; /* = sizeof(clay_measure_params); required */
    /* Finite-difference step for the Laplacian and the normal, in world units.
     * 0 derives one from `scale`. Smaller measures noise; larger blurs the
     * feature being measured. */
    float h;
    /* Curvature/cavity/convexity: the RADIUS that reads as fully saturated.
     * Curvature is 1/radius, so 0.05 means a 5 cm fillet reads 1.0. */
    float scale;
    float direction[3];  /* NORMAL_DIR only; need not be unit */
    float threshold;     /* NORMAL_DIR only: dot product below which it reads 0 */
    /* OCCLUSION and THICKNESS: how far a probe looks.
     * THIS IS THE PARAMETER THAT DECIDES WHAT THE NUMBER MEANS — occlusion over
     * 1 cm and over 1 m describe different things about the same point and
     * neither is more correct. 0 takes 20x `scale`, and that is a guess. */
    float ray_length;
    /* Rays per hemisphere. Cost is linear in this and noise falls as its square
     * root, so doubling quality costs four times. Thickness ignores it. */
    int32_t ray_count;
    /* Occlusion weight is 1/(1 + falloff * t), so a near blocker counts for
     * more than a far one. 0 makes every hit count the same. */
    float falloff;
    uint32_t seed;  /* same seed, same bits — see the note above */
} clay_measure_params;

clay_result clay_measure_defaults(clay_measure_params* out_params);

/* Measure at many points. points_xyz is count*3 floats, out_values is count.
 *
 * The points are taken AS GIVEN and are not projected onto the surface first: a
 * caller sampling a mesh's vertices already has surface points, and one
 * sampling a lattice wants the value where it asked. Use clay_project_to_surface
 * when you have a cage rather than a surface.
 *
 * Cancellable, since a bake is millions of points and an occlusion sample is a
 * march per ray. Progress is read FROM THE TOKEN with
 * clay_cancel_token_progress, as it is for every other cancellable call here —
 * one mechanism, not a second out-parameter that could disagree with it. A CANCELLED CALL LEAVES out_values UNSPECIFIED and says so —
 * a half-written buffer read as complete is a texture with a band of garbage
 * in it, which is worse than no texture. */
clay_result clay_measure_points(const clay_document* doc, clay_surface_measure measure,
                                const float* points_xyz, size_t count,
                                const clay_measure_params* params, float* out_values,
                                clay_cancel_token* token);

/* Build a MASK from one of the same measures, over a region — the lattice form
 * of the identical numbers, banded to the surface.
 *
 * ONE implementation behind both, so a mask and a measured point cannot
 * disagree about the same surface. `cell_size` 0 derives one from the region
 * (a guess); `band` 0 derives two cells. Cells outside the band stay at zero,
 * because a measure taken deep inside a solid describes nothing an artist can
 * see — the banding is this call's job and not clay_measure_points'.
 *
 * OCCLUSION and THICKNESS work here too and are far more expensive than the
 * stencil measures: a lattice of a million cells is a million hemisphere
 * samples. Prefer a coarse cell_size, and pass a token. */
clay_result clay_mask_from_surface(const clay_document* doc, clay_surface_measure measure,
                                   const float region_min[3], const float region_max[3],
                                   float cell_size, float band,
                                   const clay_measure_params* params, clay_mask** out_mask,
                                   clay_cancel_token* token);

/* -- bounded rays and cage projection (ABI 0.51.0) --------------------------
 *
 * clay_raycast searches to infinity, which cannot express "look 5 mm along this
 * normal" — the query a bake cage and a snap tool both are. Worse, it makes a
 * miss indistinguishable from a hit on the far side of the model, which is
 * precisely what puts garbage in the seams of a baked texture.
 *
 * A second entry point rather than a parameter, because clay_raycast has
 * shipped and its signature cannot grow. */
clay_result clay_raycast_bounded(const clay_document* doc, const float origin[3],
                                 const float dir[3], float tmin, float tmax, int32_t* out_hit,
                                 float* out_t, float out_position[3], float out_normal[3]);

typedef struct clay_projection {
    uint32_t struct_size; /* = sizeof(clay_projection); required */
    int32_t hit;
    /* SIGNED, along `direction`: positive means the surface was found along it,
     * negative against it. Returned by this call rather than recomputed from
     * the position, because it IS the height-map value and deriving it again is
     * a second chance to disagree about the sign. */
    float distance;
    float position[3];
    float normal[3];
} clay_projection;

/* Project a point onto the surface, searching BOTH ways within max_distance.
 *
 * BOTH WAYS is the part a first implementation gets wrong. A cage point built
 * from a low-polygon mesh may sit inside the high-polygon surface or outside
 * it, depending on whether the low-poly bulges or pinches there, and the caller
 * cannot know which. Searching only outward silently misses every point where
 * the low-poly sits inside — most of a concave region, and exactly where a bake
 * looks wrong.
 *
 * A miss within the bound is a miss, not a distant hit. */
clay_result clay_project_to_surface(const clay_document* doc, const float point[3],
                                    const float direction[3], float max_distance,
                                    clay_projection* out_projection);

/* The batched form, since a bake is millions of points. points_xyz and
 * directions_xyz are each count*3 floats; the four outputs are count,
 * count, count*3 and count*3, and any but the first may be NULL. */
clay_result clay_project_to_surface_many(const clay_document* doc, const float* points_xyz,
                                         const float* directions_xyz, size_t count,
                                         float max_distance, int32_t* out_hits,
                                         float* out_distances, float* out_positions_xyz,
                                         float* out_normals_xyz, clay_cancel_token* token);

/* -- surface groups: naming a region of the model (ABI 0.50.0) ---------------
 *
 * ZBrush's PolyGroups, Blender's Face Sets. This library had no such concept on
 * any representation: visibility was per LAYER, so "isolate the head" meant the
 * head had been authored as its own layer — a decision taken before the artist
 * knew they would want it. A layer holds exactly ONE mask, so N named regions
 * could not be emulated with N masks. And clay_layer_group_items groups
 * EDIT-LIST NODES, which says how three items combine and nothing about which
 * part of the resulting surface is the head.
 *
 * ONE WORLD-SPACE LATTICE, asked "which group is this surface point in"
 * identically whatever the surface is made of. The obvious alternative — a
 * per-face id on a mesh, a palette channel on a grid, something else for SDF —
 * is three mechanisms, three sets of semantics for hide/isolate/grow/border,
 * and they will disagree.
 *
 * The free answer for SDF, mapping a surface point to the ITEM that produced
 * it, fails the two cases that matter and fails them for one reason: an
 * artist's groups do not respect the edit list, because the edit list is how
 * the shape was BUILT and a group is about what it IS. An armour panel spanning
 * two items is not an item. A face that is part of one sphere is not one
 * either.
 *
 * WHAT IT COSTS: a group boundary is quantised to this lattice rather than to
 * the representation, so a mesh that could have carried an exact per-face
 * boundary does not. That is a visible edge at the group border.
 *
 * WHAT IT BUYS: groups survive a representation bridge BY CONSTRUCTION. They
 * were never in the SDF, the voxels or the mesh, so rasterizing, meshing or
 * converting cannot lose them — and a voxel grid's 256^3 memory guarantee is
 * untouched, because there is no second palette channel.
 *
 * PER DOCUMENT, not per layer: a mask gates edits to its layer, while a group
 * names a region of the MODEL, and "isolate the head" when the head spans two
 * layers is exactly what per-layer storage makes impossible.
 *
 * Group id 0 is CLAY_NO_GROUP and means "not in a group". A document that never
 * names a region carries nothing and writes no chunk. */
#define CLAY_NO_GROUP 0

/* Create the document's group lattice, or return the one it has. `cell_size`
 * is used only when creating: an existing lattice is not re-scaled, because
 * that would move every boundary an artist placed. Pass <= 0 for a default. */
clay_result clay_document_groups(clay_document* doc, float cell_size, clay_groups** out_groups);
/* Whether the document has one at all, without creating it. */
clay_result clay_document_has_groups(const clay_document* doc, int32_t* out_has);
/* Release a group handle. The LATTICE belongs to the document and is untouched;
 * this frees only the handle, exactly as clay_mask_destroy does for a borrowed
 * mask. Destroying a handle does not un-name a single cell. */
void clay_groups_destroy(clay_groups* groups);

/* Which group a surface point is in — the one query every representation asks,
 * and the reason this is not three mechanisms. */
clay_result clay_groups_at(const clay_groups* groups, const float world_p[3],
                           uint16_t* out_group);
/* Name a box-shaped region. Membership is decided at the cell CENTRE, so two
 * adjacent fills do not overlap by a cell — the rule clay_mask_fill uses. */
clay_result clay_groups_fill(clay_groups* groups, const float box_min[3],
                             const float box_max[3], uint16_t group);
/* Name the region a MASK covers, which is how "addressed by group or by mask"
 * becomes one mechanism rather than two. Paint a mask however you like — a
 * brush, a cavity measure, an extrude — and name the result. Cells at or above
 * `threshold` take the id. The two lattices need not share a cell size. */
clay_result clay_groups_fill_from_mask(clay_groups* groups, const clay_mask* mask,
                                       uint16_t group, float threshold, uint64_t* out_cells);
/* Everything in `from` becomes `to`. Merging into CLAY_NO_GROUP deletes a group
 * without walking the lattice for it, and takes its hidden flag with it. */
clay_result clay_groups_reassign(clay_groups* groups, uint16_t from, uint16_t to,
                                 uint64_t* out_moved);

/* Grow, shrink, border — the set operations an artist expects of a selection,
 * defined on the REGION rather than on any storage, which is what makes them
 * mean the same thing on a mesh and on a voxel grid.
 *
 * VOLUMETRIC, NOT GEODESIC, and this is where it differs from a mesh tool.
 * ZBrush grows a face set ALONG the surface; this dilates in 3D. Where a
 * surface folds back within `steps` cells of itself — the inside of a tight
 * crease, the two sides of a thin wall — growth crosses the gap and claims the
 * other side. The lattice knows regions, not surfaces.
 *
 * Growth claims only ungrouped cells: growing one group into another would
 * silently destroy a region the artist named, and "grow" is not a verb anyone
 * expects to delete. */
clay_result clay_groups_grow(clay_groups* groups, uint16_t group, int32_t steps,
                             uint64_t* out_claimed);
clay_result clay_groups_shrink(clay_groups* groups, uint16_t group, int32_t steps,
                               uint64_t* out_released);
/* The cells of `group` that touch a cell not in it — the seam to mask, crease
 * or polish. Capacity in, count out; pass a null buffer to ask the count. */
clay_result clay_groups_border(const clay_groups* groups, uint16_t group,
                               int32_t* out_cells_xyz, size_t capacity, size_t* out_count);

/* -- partial visibility -----------------------------------------------------
 *
 * A property of the ID, not of the cells: hiding a group is one flag rather
 * than a rewrite of every cell carrying it, which is also what makes isolate
 * cheap — show one, hide the rest.
 *
 * HIDING IS NOT DELETING. Hidden geometry persists through save and load and is
 * restored exactly when shown, matching the guarantee a hidden LAYER already
 * carries. Nothing is cut and no hole is closed: the field is untouched and the
 * produced mesh is filtered, so showing a group again brings back the same
 * triangles rather than a re-meshed approximation of them.
 *
 * CLAY_NO_GROUP is never hidden and isolate leaves it visible — ungrouped
 * surface is not something an artist put away, and isolating a group must not
 * make the rest of the model vanish because it was never named. */
clay_result clay_groups_set_visible(clay_groups* groups, uint16_t group, int32_t visible);
clay_result clay_groups_visible(const clay_groups* groups, uint16_t group, int32_t* out_visible);
/* Show this one, hide every other group that exists. */
clay_result clay_groups_isolate(clay_groups* groups, uint16_t group);
clay_result clay_groups_show_all(clay_groups* groups);
/* Every hidden group shows and every shown group hides. */
clay_result clay_groups_invert_visibility(clay_groups* groups);
clay_result clay_groups_any_hidden(const clay_groups* groups, int32_t* out_any);
/* Whether a surface POINT is hidden by the group it is in. */
clay_result clay_groups_point_hidden(const clay_groups* groups, const float world_p[3],
                                     int32_t* out_hidden);

/* What is here, for building a group list in a UI. `out_ids` is capacity in,
 * count out; a null buffer asks the count. Ids come back ascending and never
 * include CLAY_NO_GROUP. */
clay_result clay_groups_ids(const clay_groups* groups, uint16_t* out_ids, size_t capacity,
                            size_t* out_count);
clay_result clay_groups_cell_count(const clay_groups* groups, uint16_t group, uint64_t* out_cells);
/* The lattice this document's groups are addressed on. Fixed when the lattice
 * was created: re-scaling would move every boundary an artist placed. Also the
 * scale a group border is quantised to, which is what a host showing that edge
 * needs to know. */
clay_result clay_groups_cell_size(const clay_groups* groups, float* out_cell_size);
/* Whether no region is named at all. A created-but-empty lattice is empty, and
 * a host building a group list must not show one. */
clay_result clay_groups_empty(const clay_groups* groups, int32_t* out_empty);

/* -- WHAT RESPECTS THE HIDDEN SET, AND WHAT DOES NOT -------------------------
 *
 * Listed rather than left to be discovered, because a brush that silently
 * reaches hidden surface is worse than one that refuses.
 *
 * RESPECTS IT:
 *   clay_document_mesh, clay_document_mesh_quads  - hidden triangles dropped
 *   clay_raycast, clay_raycast_many,
 *   clay_raycast_attributed                       - march steps over hidden
 *                                                   surface and picks what is
 *                                                   behind it, which is the
 *                                                   whole point of hiding
 *
 * DOES NOT, and each for a stated reason:
 *   clay_document_eval and every field query  - the field is what the document
 *       MEANS, and a group must not change it. That invariant is why
 *       tools/check_layering.py withholds clay/voxel from clay::scene. Making
 *       hidden regions evaluate as empty would also carve a hard boundary into
 *       the surface, so showing a group again would not restore the original
 *       geometry and "hiding is not deleting" would be false.
 *   every brush and voxel verb  - a brush is bounded by its own footprint and
 *       by a MASK, which is the mechanism that already exists for "do not edit
 *       here" and the one an artist reaches for. Gating brushes on visibility
 *       too would give two mechanisms for one intent that can disagree. Isolate
 *       a group and the geometry is still there to be edited; mask it to
 *       protect it.
 *   clay_document_save  - the whole document is written, hidden included.
 *       Anything else would make hiding a form of deleting.
 *   clay_mesh_save and the exporters  - they take a mesh you already have. Mesh
 *       the document first and the filter has already run. */

/* Extrude the masked patch of a layer's surface into a new item carrying a
 * volume. The layer is sampled, so what comes back is a snapshot rather than
 * something that tracks the source — the same bargain flatten makes.
 *
 * Refused, with a typed error rather than an empty item, when the mask is
 * empty, the thickness is not positive, the wall is thinner than a cell, or the
 * masked region never reaches the surface. That last one is the common mistake
 * and the one an empty item would disguise.
 *
 * The mask and the source layer are both left unmodified. */
clay_result clay_document_mask_extrude(clay_document* doc, clay_layer_id layer,
                                       const clay_mask* mask,
                                       const clay_mask_extrude_params* params,
                                       clay_item** out_item);

/* The same, cancellable (add-operation-cancellation, ABI 0.47.0).
 *
 * THIS is the one a host most needs to be able to stop: 4403 ms on the
 * reference iPad, the most expensive verb in the library and the measurement
 * that motivated the token in the first place. A second entry point rather
 * than a parameter, for the reason clay_layer_consolidate_cancellable gives.
 *
 * A cancelled call returns CLAY_ERROR_CANCELLED and changes nothing: the
 * extrude builds a volume and the caller installs it, so a cancel is a
 * discard. `token` may be NULL, which is exactly the older call. */
clay_result clay_document_mask_extrude_cancellable(clay_document* doc, clay_layer_id layer,
                                                   const clay_mask* mask,
                                                   const clay_mask_extrude_params* params,
                                                   clay_item** out_item,
                                                   clay_cancel_token* token);

/* The same verb on a voxel grid, in CELL space: the masked cells of the
 * source's surface, thickened, carrying the source's colours. It does not go
 * through a sampled field — a grid already knows which of its cells are on its
 * surface, so resampling would cost a conversion and lose the palette.
 *
 * The two agree to within a voxel, which is the point. cell_size and band are
 * ignored here; the grid's own resolution is the only one available.
 *
 * The returned grid is owned by the CALLER — destroy it with
 * clay_voxel_grid_destroy — and neither the source nor the mask is modified. */
clay_result clay_voxel_mask_extrude(const clay_voxel_grid* grid, const clay_mask* mask,
                                    const clay_mask_extrude_params* params,
                                    clay_voxel_grid** out_grid);

/* -- the cut tool ---------------------------------------------------------- */

/* A shape drawn over the model, resolved into an ordinary edit item.
 *
 * The caller gives the FRAME the shape was drawn on — an origin and an
 * orthonormal basis, which a viewport already has because it needed one to
 * draw the overlay — and the shape in WORLD units on that frame. Not pixels
 * and not normalized device coordinates: this engine has no viewport and does
 * not want one.
 *
 * The cut is a PRISM, not a frustum. A shape drawn under a perspective camera
 * sweeps a converging wedge, and cutting with one would give a cut face that
 * is not flat and a solid that depends on where the camera was standing. A
 * trim is a straight cut, as it is in ZBrush and 3DCoat.
 *
 * Which side survives is the OP the resolved item is placed with:
 * CLAY_OP_SUBTRACT removes what the shape covers, CLAY_OP_INTERSECT keeps only
 * that. A separate flag would be a second way to say one thing. */

typedef enum clay_cut_shape {
    CLAY_CUT_RECT = 0,
    CLAY_CUT_CIRCLE = 1,
    CLAY_CUT_POLYGON = 2 /* outline in polygon_xy, closed implicitly */
} clay_cut_shape;

typedef struct clay_cut_desc {
    uint32_t struct_size; /* = sizeof(clay_cut_desc); required */
    float origin[3];
    float right[3];   /* unit, in-plane: shape x is measured in these world units */
    float up[3];      /* unit, in-plane */
    float forward[3]; /* unit, the sweep direction */
    int32_t shape;    /* clay_cut_shape */
    float half_width; /* CLAY_CUT_RECT */
    float half_height;
    float radius;   /* CLAY_CUT_CIRCLE */
    float rounding; /* bevels the cut walls; >= 0 */
    /* The region being cut, used only to size the sweep so that a cut goes all
     * the way through instead of stopping inside it. */
    float region_min[3];
    float region_max[3];
    /* Sweep extent either side of the origin along forward. Both zero means
     * "derive it from the region", which is what a caller wants unless it is
     * asking for a deliberate partial cut. */
    float near_extent;
    float far_extent;
} clay_cut_desc;

/* Resolve a cut into an item the caller places like any other, or NULL with
 * the detail in clay_last_error: a frame that is not orthonormal is refused
 * rather than squared up, because the shape the user saw was drawn in the
 * frame they think they have. `polygon_xy` is count*2 floats and is read only
 * for CLAY_CUT_POLYGON. Free the result with clay_item_destroy. */
clay_item* clay_cut_create(const clay_cut_desc* desc, const float* polygon_xy,
                           size_t polygon_count);

/* A closed control-point curve drawn in the cut plane, flattened into a
 * polygon outline through the same tessellator curves use — so a spline lasso
 * follows the same curve a spline item would. Size-query pattern: call with
 * out_xy == NULL to receive the vertex count in *count. `points_xyzr` and the
 * optional `types` are as clay_item_set_curve_points takes them; only x and y
 * are read, since the outline lies in the cut plane. */
/* -- the Tube tool --------------------------------------------------------- */

/* Nomad Sculpt's Tubes: a drawn path becomes a rope, pipe, tentacle or hair
 * strand along it.
 *
 * `point_type` is the smooth/sharp toggle and is the curve's own point type
 * (CLAY_POINT_*), because a tube's path is the same kind of curve every other
 * item takes. The three radii are Nomad's handles and are interpolated by ARC
 * LENGTH, so a path whose control points bunch does not bunch the taper.
 *
 * With no profile the tube is a swept SPHERE — an exact distance field, so the
 * safe step scale stays 1. Give a profile and it becomes a swept item instead:
 * a square or hexagonal cross-section at the cost of a bound field. That choice
 * is the profile rather than a separate flag. */
typedef struct clay_tube_params {
    uint32_t struct_size;  /* = sizeof(clay_tube_params); required */
    int32_t point_type;    /* CLAY_POINT_HARD / _SPLINE / _BSPLINE / _BEZIER */
    float radius_start;
    float radius_mid;
    float radius_end;
    int32_t closed;
    float tolerance;
    float blend_k;         /* smoothing between consecutive segments; 0 is plain */
} clay_tube_params;

/* Resolve a path into a tube. `points_xyz` is count*3 floats.
 *
 * `profile` is CLAY_PROFILE_* with its parameters in profile_params, or < 0 for
 * a round tube. Returns NULL for fewer than two points, or a radius that is
 * positive nowhere. Free the result with clay_item_destroy. */
clay_item* clay_tube_create(const float* points_xyz, size_t count,
                            const clay_tube_params* params, int32_t profile,
                            const float* profile_params, size_t profile_param_count);

/* Which half of the frame a trim's outline covers. The OP still decides that
 * half's fate: CLAY_OP_SUBTRACT removes it, CLAY_OP_INTERSECT keeps only it. */
typedef enum clay_trim_side {
    CLAY_TRIM_BELOW = 0,
    CLAY_TRIM_ABOVE = 1,
    CLAY_TRIM_LEFT = 2,
    CLAY_TRIM_RIGHT = 3
} clay_trim_side;

/* ZBrush's Trim Curve: an OPEN stroke drawn across the form, flattened and then
 * closed against the frame's own bounds on the side it covers, so the result is
 * an ordinary polygon outline for clay_cut_create.
 *
 * NOT clay_cut_polygon_from_curve with a flag. That one tessellates CLOSED and
 * is a spline lasso: joining a trim stroke's endpoints cuts a sliver between
 * them instead of dividing the frame. Different shapes from the same points.
 *
 * `extent_xy` is how far the closing edge reaches in the frame's own units.
 * Size-query pattern, as the lasso flattener uses: call with out_xy == NULL to
 * receive the vertex count in *out_count. */
clay_result clay_cut_polygon_from_open_curve(const float* points_xyzr, size_t count,
                                             const int32_t* types, int32_t side,
                                             const float extent_xy[2], float tolerance,
                                             float* out_xy, size_t* out_count);

clay_result clay_cut_polygon_from_curve(const float* points_xyzr, size_t count,
                                        const int32_t* types, float tolerance, float* out_xy,
                                        size_t* out_count);

/* -- brush strokes --------------------------------------------------------- */

/* A drag becomes stamps, and a stamp becomes an ordinary edit.
 *
 * Resolution is pure: samples and a preset go in, stamps come out, and no
 * document is read or touched. Application turns those stamps into ordinary
 * voxel brush stamps or ordinary edit-list nodes, so undo, stroke coalescing,
 * `.clayspace` serialization and picking apply to a stroked edit without any
 * of them knowing this exists.
 *
 * It is also where a mask reaches SDF edits. An SDF item is declarative and
 * has no per-point strength, so gating it at evaluation time would cost the
 * rigidity and finite support per-brick culling depends on. It is gated here
 * instead, where the item is authored: a stamp in a frozen region emits
 * nothing at all. */

/* One moment of a drag: position, pressure in [0,1], tilt in radians. Samples
 * cross this ABI as a packed float array of count*5, in this order, so a
 * caller's (N, 5) buffer needs no repacking. */
typedef struct clay_stroke_sample {
    float position[3];
    float pressure;
    float tilt;
} clay_stroke_sample;

/* The three channels a tablet reports and the struct above could not carry.
 *
 * A SECOND STRUCT AND A SECOND ENTRY POINT rather than three appended fields.
 * clay_stroke_resolve takes samples as a FLAT array of count*5 floats, and
 * clay_stroke_sample above exists to document that packing. Widening the
 * packing in place would mean a host compiled against the old stride handing
 * over an array the library reads at the wrong offsets — silent corruption
 * rather than a clean refusal, which is the failure mode this ABI works hardest
 * to avoid. Array elements are also exempt from the struct_size rule by design
 * ("a struct_size per element would be absurd", tools/check_c_abi.py), so there
 * is no negotiation to fall back on.
 *
 * The wider form takes a real struct array rather than a flat one: it is
 * self-documenting, and it lets the timestamp be a double, which a float array
 * could not carry usefully.
 *
 * AZIMUTH is the one that unlocks a capability rather than refining one. Tilt
 * says how far the stylus is leaning; azimuth says WHICH WAY. Without it a
 * stamp can follow the path but not the BARREL, so a directional or rake brush
 * — a chisel held at an angle, a comb dragged sideways — is not expressible at
 * all. Radians in the surface plane, 0 is +x, ignored when tilt is 0.
 *
 * VELOCITY in world units per second, for speed-driven size and flow. The host
 * computes it because the host, not the library, knows the wall-clock gap
 * between two events.
 *
 * TIMESTAMP in seconds on the host's clock, kept even though velocity is given:
 * a resolver re-deriving speed across a COALESCED run of samples needs the
 * interval and cannot recover it from per-sample velocities. 0 means none. */
typedef struct clay_stroke_sample_full {
    float position[3];
    float pressure;
    float tilt;
    float azimuth;
    float velocity;
    double timestamp;
} clay_stroke_sample_full;

/* One resolved stamp: where an edit goes and how strong it is. */
typedef struct clay_stamp {
    float position[3];
    float radius;
    float strength;
    float rotation[4]; /* xyzw quaternion */
    float along;       /* [0,1] along the stroke */
} clay_stamp;

/* How overlapping stamps in one stroke combine. */
typedef enum clay_accumulation {
    CLAY_ACCUMULATION_BUILDUP = 0, /* passing twice acts twice */
    CLAY_ACCUMULATION_CLAMPED = 1  /* the stroke reaches its strength once */
} clay_accumulation;

/* How a drag becomes stamps. A versioned descriptor like the others, and
 * additionally a thing a user SAVES: a preset library outlives the engine
 * version that wrote it, which is why clay_stroke_preset_serialize tags its
 * output with a schema version from the first release rather than gaining one
 * later. struct_size is the in-memory contract; the schema version is the
 * on-disk one, and they move independently. */
typedef struct clay_stroke_preset {
    uint32_t struct_size; /* = sizeof(clay_stroke_preset); required */
    float radius;         /* world units; must be > 0 */
    float spacing;        /* stamp spacing as a fraction of the DIAMETER; > 0 */
    /* How much a stamp deposits, [0, 1]. Reaches a voxel stroke as the brush
     * strength. On an SDF layer it reaches the ops that have an amount:
     * CLAY_OP_RELIEF and CLAY_OP_INCISE scale their amplitude by it, and
     * CLAY_OP_ADD scales its whole deposit — 0 deposits nothing, 1 is the item
     * exactly as authored. Every other op ignores it; see
     * clay_layer_apply_stroke. */
    float strength;
    float pressure_size;     /* 0 = pressure does not drive the radius */
    float pressure_strength; /* 0 = pressure does not drive the strength */
    float pressure_curve;    /* exponent applied to pressure before either */
    float jitter_position;   /* fraction of the radius */
    float jitter_size;       /* fraction of the radius */
    float jitter_rotation;   /* radians */
    uint32_t seed;           /* jitter is a hash of the stamp index and this */
    int32_t rotate_along_stroke;
    float taper_start; /* fraction of the stroke the radius ramps in over */
    float taper_end;
    float steady;        /* lazy-mouse lag; 0 follows exactly, ->1 lags more */
    int32_t accumulation; /* clay_accumulation */
} clay_stroke_preset;

/* Fill a descriptor with the engine's defaults: the one way to get a valid
 * preset without knowing every field, and what a caller should start from
 * before overriding what it cares about.
 *
 * SET struct_size BEFORE CALLING. Changed in ABI 0.35.0: this used to set it
 * for you, which left nothing to bound the fill against, so it wrote sizeof as
 * the LIBRARY defines it into whatever the caller had allocated. A descriptor
 * that has not grown yet makes that harmless; clay_brick_config had grown, and
 * it was not. Not setting it is now refused with CLAY_ERROR_INVALID_ARGUMENT
 * rather than overwriting past the end of your struct. */
clay_result clay_stroke_preset_defaults(clay_stroke_preset* out_preset);

/* Preset <-> bytes, tagged with the schema version. Serialization uses the
 * size-query pattern: call with out_data == NULL for the size. Loading a
 * preset from an OLDER schema succeeds, taking defaults for what it did not
 * carry; one from a NEWER schema is refused with CLAY_ERROR_INVALID_ARGUMENT
 * rather than read as a prefix, because the fields that would be skipped can
 * change what the ones read mean. */
clay_result clay_stroke_preset_serialize(const clay_stroke_preset* preset, uint8_t* out_data,
                                         size_t* count);
clay_result clay_stroke_preset_deserialize(const uint8_t* data, size_t size,
                                           clay_stroke_preset* out_preset);
/* The schema version this build writes. */
uint32_t clay_stroke_preset_version(void);

/* Resolve samples into stamps, via the size-query pattern: call with
 * out_stamps == NULL to receive the count, then again with a buffer of that
 * many. *count is the capacity going in and the count written coming out. */
clay_result clay_stroke_resolve(const float* samples_xyzpt, size_t sample_count,
                                const clay_stroke_preset* preset, clay_stamp* out_stamps,
                                size_t* count);

/* The same, taking the channels a tablet actually reports (ABI 0.48.0).
 *
 * The older call is sugar over this one with azimuth, velocity and timestamp
 * left at zero — which is exactly the stroke it resolved before, because a
 * preset that asks for neither the barrel nor a speed response ignores all
 * three. So a host that never adopts this is unaffected. */
clay_result clay_stroke_resolve_full(const clay_stroke_sample_full* samples, size_t sample_count,
                                     const clay_stroke_preset* preset, clay_stamp* out_stamps,
                                     size_t* count);

/* Resolve a stroke and stamp it into a grid. `index` is the palette entry to
 * set, or 0 to erase; `mask` may be NULL. *out_applied receives how many
 * stamps actually ran — a stamp in a frozen region is dropped, not weakened
 * to nothing. */
clay_result clay_voxel_apply_stroke(clay_voxel_grid* grid, const float* samples_xyzpt,
                                    size_t sample_count, const clay_stroke_preset* preset,
                                    int32_t index, int32_t shape, int32_t falloff,
                                    const clay_mask* mask, size_t* out_applied);

/* -- the Move brush -------------------------------------------------------- */

typedef struct clay_move_params {
    uint32_t struct_size; /* = sizeof(clay_move_params); required */
    float radius;         /* the drag's radius in WORLD units; must be > 0 */
    int32_t ease;         /* falloff curve across the region */
    int32_t front_only;   /* non-zero: do not drag the far side of a form */
} clay_move_params;

/* Drag a layer's assembled SURFACE — ZBrush's Move. Named apart from
 * clay_layer_move, which reparents a node in the tree: these move different
 * things and confusing them would be easy.
 *
 * NOT the same as putting a grab deformer on an item, which is what a host
 * reaching for clay_item_add_deformer gets. A deformer is per ITEM and its
 * centre is in that item's LOCAL frame, so a grab drags one item's own field:
 * on a form blended from several, it pulls that item's share and leaves the
 * rest behind. This resolves the drag against every item the region reaches,
 * maps it into each one's frame, and puts it at the FRONT of each chain —
 * which is where a warp has to go to act on the assembled shape.
 *
 * With undo enabled the whole drag is ONE step however many items it touched,
 * and ONE cache invalidation for the same reason: the region a drag can reach
 * is its own ball — the warp's weight is zero outside `radius` of the centre,
 * and a point with zero weight is not moved — so the whole gesture states that
 * once instead of deriving a region per item. Cheaper and TIGHTER than the
 * per-item union, which is why a drag no longer costs the brick cache every
 * item it grazed.
 *
 * The ball is the reach IN THIS LAYER'S PLACEMENT. A layer whose edit list is
 * INSTANCED (clay_document_instance_layer) has the same nodes placed by every
 * layer sharing it, so a drag also changes the field wherever those layers put
 * them, and the invalidation covers each sharer's whole influence bound as
 * well — the union clay_layer_node_influence_bound reports, and what the
 * dirty-bounds contract there already promised. Only a shared edit list pays
 * that; a layer nothing instances invalidates its ball alone.
 *
 * *out_applied receives how many items took a warp, so a host can tell "the
 * drag reached nothing" from "the drag did nothing visible". A drag that
 * reaches nothing succeeds and changes nothing.
 *
 * UNDER A LAYER MIRROR OR RADIAL SYMMETRY the drag is reflected and rotated
 * into every image the layer emits of it — the ball, one reflection per mirror
 * axis, one rotation per radial copy — and an item is selected by what the
 * ball OR one of its images touches, on the item's own bound. An item whose
 * mirror copy sits under the ball therefore moves (through its copy), and an
 * item nowhere near the ball or its images is left alone, where selecting on
 * the mirror-expanded bound used to warp every item on both sides of the
 * plane. Items that opted out of the mirror (clay_item_desc.mirror < 0) see
 * the ball alone. An item both the ball and an image reach — one straddling
 * the plane — composes both pulls, as two brushes would; *out_applied still
 * counts ITEMS, so it counts once. The invalidation covers every image's
 * ball, and with an identity layer transform a drag and its mirror image
 * produce the same field bit for bit.
 *
 * The surface moves LESS than the displacement asked for — the region weight
 * is taken at the sample point rather than at its preimage, so a drag of 0.5
 * over a radius of 0.8 moves a tip about 0.31. That is grab's documented
 * behaviour and the pull is monotonic, so a UI can calibrate against it.
 *
 * A GRAB IS NOT FREE AFTER IT LANDS, and this reads like a bounded local edit
 * that is. Each one is recorded as a WARP on every item it reached, and a warp
 * is evaluated per sample for as long as it is in the edit list — so the layer
 * costs more to evaluate after the drag than before, permanently, and by about
 * the same amount again for every drag after that. Measured on a worked ball
 * (issue #452): a fixed 16,000-point grid evaluates at 1.00x with no grabs and
 * 3.60x after twelve, dead linear at roughly 0.14 ms a grab, and it is paid by
 * every later brush dab, remesh and viewport refresh on that layer rather than
 * by Move alone.
 *
 * It is a property of the representation and not a defect: a warp is a domain
 * deformation, and two of them do not compose into one that could be stored
 * instead. What the library does about it is narrower than that:
 *
 *   - A CULLED tape drops a grab whose radius its region cannot reach, so the
 *     per-brick paths — a refill, and meshing with gradient normals — pay only
 *     for the grabs that actually reach the brick. A whole-document evaluation
 *     has no region to test against and pays for all of them.
 *   - clay_layer_warp_cost reports what the layer is currently carrying, so a
 *     host can decide when the accumulation is worth spending a consolidation
 *     on rather than guessing.
 *
 * clay_layer_consolidate is the only thing that gives the cost back in full,
 * and it is the large hammer it looks like: it bakes the edit list, so the
 * items and their parameters are gone. Its price is
 * clay_layer_consolidation_cost. */
/* Which nodes a move WOULD warp, without touching the document, so a host can
 * preview a drag before committing it. Size-query pattern: call with
 * out_nodes == NULL to receive the count in *out_count, then again with a
 * buffer of that size. */
clay_result clay_layer_move_surface_preview(const clay_document* doc, clay_layer_id layer,
                                            const float centre[3],
                                            const float displacement[3],
                                            const clay_move_params* params,
                                            clay_node_id* out_nodes, size_t capacity,
                                            size_t* out_count);

clay_result clay_layer_move_surface(clay_document* doc, clay_layer_id layer,
                                    const float centre[3],
                            const float displacement[3], const clay_move_params* params,
                            size_t* out_applied);

/* What the warps a layer has accumulated are charging it (issue #452).
 *
 * clay_layer_consolidation_cost answers what it would cost to BAKE the layer.
 * This answers the question a host actually has first: whether the layer has
 * accumulated enough warps to be worth baking at all. A drag records a grab on
 * every item it reaches and every one of those is evaluated per sample for the
 * life of the edit list, so a session of Move gestures gets steadily dearer to
 * evaluate with nothing in the document that looks like a cost.
 *
 * `warps` is the number a WHOLE-DOCUMENT evaluation pays for, which is the one
 * that follows the accumulation. `finite_support_warps` is how many of those a
 * CULLED tape can drop when its region does not reach them — the per-brick
 * paths pay only for the rest — so the difference between the two is what a
 * host would win by working in bricks rather than over the whole field.
 *
 * A layer that is not an SDF layer reports zeroes rather than being refused:
 * "no warps" is the true answer for one that cannot carry any. */
typedef struct clay_layer_warp_cost {
    uint32_t struct_size; /* = sizeof(clay_layer_warp_cost); required */
    uint64_t items;                /* items in the layer */
    uint64_t warped_items;         /* those carrying at least one warp */
    uint64_t warps;                /* warp records in total */
    uint64_t finite_support_warps; /* those a culled tape can drop */
} clay_layer_warp_cost;

clay_result clay_layer_warp_cost_get(const clay_document* doc, clay_layer_id layer,
                                     clay_layer_warp_cost* out_cost);

/* -- magnify / pinch on the assembled surface ------------------------------- */

typedef struct clay_magnify_params {
    uint32_t struct_size; /* = sizeof(clay_magnify_params); required */
    float radius;         /* the region's radius in WORLD units; must be > 0 */
    int32_t ease;         /* falloff curve across the region */
    /* No front_only. A radial scale has no direction to gate a half-space on;
     * a drag has one because a pull heads somewhere. */
} clay_magnify_params;

/* Magnify or PINCH a layer's assembled SURFACE, the radial-scale counterpart to
 * clay_layer_move_surface — and it exists for the same reason that one does.
 *
 * CLAY_DEFORM_MAGNIFY is per ITEM and its centre is in that item's LOCAL frame,
 * exactly as CLAY_DEFORM_GRAB is. Put one on a picked item of a form that is
 * several smooth-unioned pieces and it scales that piece's own field and leaves
 * the others where they were: the surface gathers on one side of the blend and
 * not the other. Nothing errors — it just comes out wrong. Grab had a resolver
 * for that and magnify did not, so Pinch could not be a surface brush on a
 * field at all (issue #391).
 *
 * `strength` is SIGNED and one parameter covers both verbs: POSITIVE swells the
 * surface away from the centre (Magnify), NEGATIVE gathers it toward (Pinch).
 * They are one deformation, and Maxon's own documentation says as much.
 *
 * TOTAL, from the start of the gesture, never an increment on the last frame —
 * as clay_layer_move_surface takes a total displacement, and for the same
 * reason: a chain of increments composes scales that were each authored against
 * a different intermediate surface, and the product is not the pinch the artist
 * made. A live gesture calls this every frame with a growing strength and the
 * engine replaces its own last frame rather than stacking a deformer per frame.
 *
 * *out_applied receives how many items took a warp, so a host can tell "the
 * gesture reached nothing" from "the gesture did nothing visible". One that
 * reaches nothing succeeds and changes nothing. A strength of zero scales by
 * one and is refused for the same reason a drag of zero is: it is not a
 * gesture, and a chain of no-op deformers is worse than none.
 *
 * WHAT IT INVALIDATES is the region's own ball, and one per image the layer's
 * symmetry makes of it — with NO dilation, unlike a drag's. Outside the radius
 * the weight is zero and the point is returned unchanged, so the field cannot
 * differ there. A pinch SAMPLES from outside the ball, its scale factor
 * exceeding one, but it is only evaluated inside it. As with a drag, an
 * INSTANCED edit list (clay_document_instance_layer) widens that by each
 * sharer's whole influence bound, because the same nodes are placed by every
 * layer sharing them.
 *
 * UNDER A LAYER MIRROR OR RADIAL SYMMETRY the region is reflected and rotated
 * into every image the layer emits, and an item is selected by what the ball OR
 * one of its images touches, on the item's own bound — so an item whose mirror
 * copy sits under the cursor is reached through that copy. The STRENGTH crosses
 * every image untouched: a reflection or a rotation of a radial scale is a
 * radial scale of the same strength, where a drag's displacement has to be
 * mapped per image. With an identity layer transform a gesture and its mirror
 * image produce the same field bit for bit.
 *
 * The whole gesture is ONE undo step however many items it touched.
 *
 * NOT OFFERED FOR POSE. CLAY_DEFORM_POSE sits in the same sentence as grab and
 * magnify in the deformer docs and has the same per-item gap, but it does not
 * fit this shape: radial pose carries an AXIS, a direction that would have to be
 * mapped per image, and pose along a line has no finite support at all, so the
 * reachability rule this is built on does not apply to it. */
/* Which nodes a magnify WOULD warp, without touching the document. Size-query
 * pattern, exactly as clay_layer_move_surface_preview: call with
 * out_nodes == NULL to receive the count in *out_count, then again with a
 * buffer of that size. */
clay_result clay_layer_magnify_surface_preview(const clay_document* doc, clay_layer_id layer,
                                               const float centre[3], float strength,
                                               const clay_magnify_params* params,
                                               clay_node_id* out_nodes, size_t capacity,
                                               size_t* out_count);

clay_result clay_layer_magnify_surface(clay_document* doc, clay_layer_id layer,
                                       const float centre[3], float strength,
                                       const clay_magnify_params* params, size_t* out_applied);

/* -- transient SDF sculpt transactions ------------------------------------- */

/* The lifetime a FIELD or DEFORMATION brush needs and an edit-list brush does
 * not (sdf-sculpt-transaction spec).
 *
 * An ordinary SDF stroke becomes nodes as it goes, so undo, picking and
 * serialization already understand the whole gesture. Two verbs cannot be
 * spelled that way and both are core sculpting:
 *
 *  - SMOOTH averages the assembled field, and there is no node meaning "the
 *    average of what was here" — relax BAKES. A host with nowhere to keep the
 *    baked volume between pointer events pays the whole MODEL per dab, so the
 *    only affordable implementation ran once at pointer-up. That is why Smooth
 *    had no live preview.
 *  - MOVE warps the assembled surface, which is a deformer on each item it
 *    reaches. Written into the document per pointer event, a drag churns
 *    revisions, tapes, caches and picking sixty times a second to produce one
 *    edit.
 *
 * The shape is the same for both:
 *
 *      begin    capture the source; build the transient state ONCE
 *      update   mutate only that state; receive the dirty region
 *      commit   one persistent command group, one undo step
 *      cancel   nothing persistent ever happened
 *
 * Between begin and commit the document is untouched: no nodes, no deformers,
 * no undo entries, and a save taken mid-gesture is the one taken before it.
 *
 * DESTROYING WITHOUT COMMITTING IS A CANCEL, so a host that drops the handle on
 * an error path cannot leave half a stroke behind.
 *
 * THE TRANSACTION BORROWS ITS DOCUMENT. Destroy it before the document, and do
 * not edit the layer through other entry points while it is open — a commit
 * that finds the layer changed underneath it FAILS with
 * CLAY_ERROR_INVALID_ARGUMENT rather than overwriting the other edit, which is
 * a refusal a host must be ready for and never a corruption. */

typedef struct clay_sdf_smooth_tx clay_sdf_smooth_tx; /* opaque */
typedef struct clay_sdf_move_tx clay_sdf_move_tx;     /* opaque */

/* Where a gesture samples, and when a session is willing to spend an artist's
 * parametric history to keep the marcher affordable.
 *
 * The three sampling numbers carry clay_consolidation_params' meanings exactly:
 * `cell_size` is REQUIRED and > 0 for Smooth, because a document has no
 * intrinsic resolution and guessing one here would fix a shape's resolution at
 * a number nobody chose; `band` <= 0 means three cells; `padding` <= 0 means
 * the band. A Move transaction ignores all three unless it consolidates.
 *
 * The three budget numbers are the two degradation mechanisms
 * clay_field_report names separately plus the size of the list. ZERO DISABLES
 * A CRITERION, so a policy of all zeros authorises nothing and is never over
 * budget — the safe reading of a struct a caller only partly filled in.
 *
 * `allow_consolidation` is the whole opt-in. Over budget without it is a
 * REPORT: the commit says so in clay_sculpt_budget and changes nothing else. A
 * bake discards the parameters of everything it absorbs, and an engine doing
 * that unasked would be deciding on an artist's behalf that a sphere's radius
 * is no longer editable. With it, the consolidation happens INSIDE the
 * stroke's own undo step, so one undo puts back both. */
typedef struct clay_sculpt_policy {
    uint32_t struct_size; /* = sizeof(clay_sculpt_policy); required */
    float cell_size;      /* required and > 0 for Smooth */
    float band;           /* <= 0 means three cells */
    float padding;        /* <= 0 means the band */
    float min_safe_step_scale;     /* 0 disables */
    int32_t max_deformer_chain;    /* 0 disables */
    int32_t max_item_count;        /* 0 disables */
    int32_t allow_consolidation;   /* non-zero: destructive collapse is authorised */

    /* -- the prefix cache, when clay_sdf_smooth_begin_cached is given one -----
     *
     * The three knobs that decide WHETHER and WHERE a layer's history folds
     * into a cached prefix. They live here, and not in a descriptor of their
     * own, for the reason the consolidation numbers do: the cache is KEYED on
     * its sampling, so a prefix built at one cell size and asked for at another
     * is a MISS -- no error, no wrong answer, just no acceleration, which is
     * the one failure shape indistinguishable from the feature not working. A
     * host that could set the two independently could build a cache nothing
     * will ever hit. There is one cell_size above and it is both.
     *
     * All three zero is TODAY'S BEHAVIOUR EXACTLY: no cache, every window the
     * full walk. A host that has not heard of this is not opted into it. */

    /* Below this many roots, do not cache: the walk is already cheap and a
     * volume would cost more memory than it saves time. 0 caches nothing. */
    uint64_t prefix_min_history_roots;
    /* How many roots stay LIVE in front of the boundary. The suffix is what
     * still costs per evaluation, so this trades rebuild frequency against
     * interactive cost. */
    uint64_t prefix_keep_live_suffix_roots;
    /* The cache's ceiling in bytes. 0 DISABLES the cache -- not "unbounded".
     * A cache with no ceiling is a leak on a device with a memory budget, and
     * "off" is the safe reading of a field nobody filled in. The handle carries
     * its own budget too; this is the policy's view of the same limit and the
     * SMALLER of the two is what a build respects. */
    uint64_t prefix_max_bytes;
} clay_sculpt_policy;

/* What one update changed, so a host invalidates a region rather than a model.
 *
 * `touched_bricks` and the bounds are GEOMETRIC — they describe what the brush
 * selected, not which samples happened to move — so they are reproducible for a
 * given brush over a given lattice however much unrelated model surrounds it.
 * `changed` is the value question, answered separately: a dab whose weight came
 * out zero everywhere still selects its bricks and has nothing to redraw.
 *
 * `has_bounds` is the third state the rest of this ABI already uses: when 0,
 * bounds_min/bounds_max are left untouched. */
typedef struct clay_sculpt_dirty {
    uint32_t struct_size; /* = sizeof(clay_sculpt_dirty); required */
    float bounds_min[3];
    float bounds_max[3];
    uint64_t touched_bricks;
    int32_t changed;
    int32_t has_bounds;
} clay_sculpt_dirty;

/* What the budget said after a committed stroke, and what was done about it.
 * The report fields are clay_field_report's, measured AFTER any consolidation
 * so they describe the layer the artist is now looking at. */
typedef struct clay_sculpt_budget {
    uint32_t struct_size; /* = sizeof(clay_sculpt_budget); required */
    int32_t over_budget;  /* at least one enabled criterion was crossed */
    int32_t consolidated; /* the layer was collapsed, inside the stroke's step */
    float lipschitz;
    float safe_step_scale;
    float steepest_volume;
    int32_t longest_deformer_chain;
    int32_t item_count;
} clay_sculpt_budget;

/* Begin a live Smooth on `layer`. NULL on failure, detail in clay_last_error():
 * no such layer, not an SDF layer, a protected layer, a cell size of zero, a
 * layer whose field cannot be sampled, or a cancelled `token`.
 *
 * THE ONLY evaluation of the source layer in the whole gesture happens here,
 * and it is the same sampling clay_layer_consolidate does — local frame, the
 * pooled evaluator, redistance, compact, measured Lipschitz. That is the
 * pointer-down cost, and it is the trade this design makes deliberately: the
 * whole finite layer once, so that every dab afterwards costs what it touches.
 *
 * The pooled evaluator is the CPU's, and nothing in this transaction reaches a
 * device backend — not this sampling, not clay_sdf_smooth_update's dabs, not
 * the commit. See clay_layer_consolidate for the statement and why it is one.
 * A host comparing a committed Smooth across platforms is comparing what it
 * DRAWS the result with, not what produced it.
 *
 * `token` may be NULL. It cancels only the sampling. */
clay_sdf_smooth_tx* clay_sdf_smooth_begin(clay_document* doc, clay_layer_id layer,
                                          const clay_sculpt_policy* policy,
                                          clay_cancel_token* token);

/* -- the SDF prefix cache -------------------------------------------------- */

/* What stops a deep layer costing its whole history on every window the artist
 * has not touched yet.
 *
 * THE PROBLEM IT SOLVES. A brick that has already been evaluated keeps an fp32
 * seed and resumes from it, so an in-stroke dab is flat in document size. A
 * brick NOBODY HAS TOUCHED has no seed and walks the complete surviving
 * history. Measured on a 20,000-item layer: 242 ms for that first dab, against
 * 2.32 ms once a compatible prefix exists -- and 2.32 ms at 5,000 items too,
 * because the accelerated dab costs its SUFFIX and the suffix does not grow.
 *
 * A cache holds, per layer and per resolution, the layer's field sampled at a
 * BOUNDARY in its root list. A window inside the cached volume is seeded from
 * it and only the roots after the boundary are evaluated.
 *
 * IT BELONGS TO WHOEVER MADE IT, never to a document -- the same rule
 * clay_brick_cache states, and deliberately the same words: this takes no
 * document, destroy returns void, and destroying a document leaves every cache
 * built against it valid. It is a session's policy and a device's memory
 * ceiling, and neither is a property of the artwork.
 *
 * NOTHING IN IT IS AUTHORITATIVE. Every entry is rebuildable from the document,
 * nothing is serialized, and dropping the whole cache changes no output --
 * only how long the next cold window takes. That is what makes trimming it
 * under memory pressure a decision a host may take freely.
 *
 * THREADING, as clay_brick_cache: no lock is taken and none is added, so every
 * call on ONE handle must be serialized by the host. Two handles share
 * nothing. */
typedef struct clay_sdf_prefix_cache clay_sdf_prefix_cache; /* opaque */

/* `max_bytes` is the ceiling. 0 means the cache holds NOTHING -- not
 * "unbounded" -- so a handle created with 0 is a working handle that never
 * caches, which is the safe reading of a number nobody chose, and the same
 * reading clay_sculpt_policy's prefix_max_bytes has. A build against a cache
 * with no budget is REFUSED and says why, rather than quietly caching nothing.
 *
 * Worth knowing if you also link the C++ API: SdfPrefixCache::set_max_bytes
 * reads 0 as UNBOUNDED, which is what lets a default-constructed cache in a
 * benchmark hold everything. Both readings are defensible in their own place;
 * shipping both through one C field would be a trap, so this ABI picks the one
 * every other budget in it uses and enforces it at the boundary.
 *
 * Never returns null except on allocation failure; free with
 * clay_sdf_prefix_cache_destroy. */
clay_sdf_prefix_cache* clay_sdf_prefix_cache_create(uint64_t max_bytes);
void clay_sdf_prefix_cache_destroy(clay_sdf_prefix_cache* cache);

/* Lowering the ceiling evicts down to it at once, least-recently-used first. */
clay_result clay_sdf_prefix_cache_set_max_bytes(clay_sdf_prefix_cache* cache, uint64_t max_bytes);

/* Drop what is cached for one layer, or everything.
 *
 * NEITHER IS REQUIRED FOR CORRECTNESS. An entry records the digest of the roots
 * it represents and is re-checked on every lookup, so an edit that invalidates
 * a prefix is caught whether or not anyone said so. These exist so a host can
 * release memory it knows it will not want, not so it can keep the cache
 * honest -- a missed invalidation here costs nothing, which is the property
 * that lets a host ignore both calls entirely. */
clay_result clay_sdf_prefix_cache_invalidate_layer(clay_sdf_prefix_cache* cache,
                                                   clay_layer_id layer);
clay_result clay_sdf_prefix_cache_clear(clay_sdf_prefix_cache* cache);

/* What the cache is holding and how it has been used.
 *
 * seeded_windows and fallback_windows are the pair a BENCHMARK needs and a host
 * mostly does not: they say whether a window was served from the cached volume
 * or fell back to evaluating the prefix tape for itself. Without them "this got
 * faster" cannot be told from "this test happened to touch the same window
 * twice", which is the difference between a gate and a hope. */
typedef struct clay_sdf_prefix_stats {
    uint32_t struct_size; /* = sizeof(clay_sdf_prefix_stats); required */
    uint64_t entries;
    uint64_t bytes;
    uint64_t hits;
    uint64_t misses;
    uint64_t builds;
    uint64_t evictions;
    uint64_t invalidations;
    uint64_t seeded_windows;
    uint64_t fallback_windows;
} clay_sdf_prefix_stats;

clay_result clay_sdf_prefix_cache_stats(const clay_sdf_prefix_cache* cache,
                                        clay_sdf_prefix_stats* out_stats);

/* Where this layer's boundary would fall under this policy, as a count of
 * top-level roots, or 0 meaning "do not cache this layer".
 *
 * BUILDS NOTHING and evaluates nothing -- it reads the root count and the
 * policy. It is here so a host can decide whether a build is worth scheduling
 * before it schedules one, which matters because the build is the whole
 * layer's cost. */
clay_result clay_sdf_prefix_boundary_for(const clay_document* doc, clay_layer_id layer,
                                         const clay_sculpt_policy* policy, uint64_t* out_roots);

/* Build and cache this layer's prefix. THIS IS THE EXPENSIVE CALL and it is a
 * separate one for exactly that reason.
 *
 * Measured: 576 ms at 5,000 items and 2,170 ms at 20,000, against 2.32 ms for
 * the accelerated dab it enables. Nothing begins a gesture here and no gesture
 * builds one -- clay_sdf_smooth_begin_cached uses a prefix if one is there and
 * takes the full walk if it is not, because the alternative is the whole
 * layer's cost at the moment an artist has just put their finger down.
 *
 * WHEN TO CALL IT, measured rather than guessed. The build pays for itself
 * after about nine cold windows on a 20,000-item layer (2,170 / (242 - 2.32))
 * and about seven at 5,000. So: between gestures, on the layer being worked,
 * when the artist is likely to take more than a handful of cold dabs on it.
 * Never on the pointer-down path.
 *
 * TWO DIFFERENT QUESTIONS WITH TWO DIFFERENT INPUTS, and this is the part that
 * surprises: the BUILD TIME follows the history (576 -> 2,170 ms for 4x the
 * items) and the CACHE SIZE does not -- 268 bricks and 1.43 MiB at both 5,000
 * and 20,000 items, because the volume is a property of the SHAPE. Size a
 * budget from the model; schedule a build from the history.
 *
 * Refuses when the policy declines the layer (see clay_sdf_prefix_boundary_for),
 * when the layer is not the document's last visible SDF layer, or when the bake
 * refuses. `token` may be NULL; a cancelled build caches nothing. */
clay_result clay_sdf_prefix_cache_build(clay_sdf_prefix_cache* cache, const clay_document* doc,
                                        clay_layer_id layer, const clay_sculpt_policy* policy,
                                        clay_cancel_token* token);

/* clay_sdf_smooth_begin, plus the cache to look in.
 *
 * A separate entry point rather than an argument on the original, and rather
 * than a cache attached to the document: a document holding a pointer to memory
 * the host can free is the hazard this ABI already refuses elsewhere, and an
 * acceleration hidden in document state is one a reader of the begin call
 * cannot see. This is the clay_eval_grid / clay_eval_grid_device shape.
 *
 * `cache` may be NULL, which is exactly clay_sdf_smooth_begin. A cache holding
 * no prefix for this layer is also exactly clay_sdf_smooth_begin -- it BUILDS
 * NOTHING here. */
clay_sdf_smooth_tx* clay_sdf_smooth_begin_cached(clay_document* doc, clay_layer_id layer,
                                                 const clay_sculpt_policy* policy,
                                                 clay_sdf_prefix_cache* cache,
                                                 clay_cancel_token* token);

/* One live dab, relaxing the transaction's own working volume in place.
 *
 * `params` is clay_relax_params exactly as clay_item_volume_relax takes it,
 * mask included. `token` may be NULL; cancellation is at whole passes, so a
 * cancelled update has applied some number of complete passes and no fraction
 * of one, and the passes it did apply stay applied.
 *
 * `out_dirty` may be NULL. Touches NOTHING in the document. */
clay_result clay_sdf_smooth_update(clay_sdf_smooth_tx* tx, const clay_relax_params* params,
                                   clay_cancel_token* token, clay_sculpt_dirty* out_dirty);

/* The preview so far, as a fresh volume item the caller owns and destroys with
 * clay_item_destroy.
 *
 * THE SNAPSHOT PATH, and not the per-frame one. It copies the whole working
 * volume, which a host joining mid-gesture or rebuilding a lost preview cache
 * wants and a host drawing every frame does not: for that,
 * clay_sdf_smooth_preview_delta_take below hands over only the bricks that
 * changed. Kept, and kept working, because a simple host should not have to
 * implement patching to draw anything at all.
 *
 * A COPY of the working samples, which is honest about what it costs: the
 * transaction goes on mutating its own volume, and handing out a view of
 * something that is about to change under a compiled tape is the bug this
 * refuses to offer. A host drawing every frame should read the dirty bounds
 * from clay_sdf_smooth_update and decide from those how often it needs the
 * samples themselves. */
clay_result clay_sdf_smooth_preview_item(const clay_sdf_smooth_tx* tx, clay_item** out_item);

/* Install the working volume as the layer's one item, as ONE undo step, then
 * evaluate the complexity policy inside that same step.
 *
 * NEVER re-samples the layer: the volume being installed is the one the dabs
 * were applied to, which is the entire point of the transaction.
 *
 * IT COSTS THE MARCHER, AND A HOST HAS TO KNOW BEFORE IT DRAWS. The layer that
 * comes back is one SAMPLED VOLUME where it was a list of parametric items, and
 * a sampled volume declares a Lipschitz of sqrt(3) times its samples' — so
 * clay_safe_step_scale for that layer falls from 1.0 to at most 0.577, and the
 * sphere trace takes correspondingly more steps. Measured on a roughened ball:
 * 7.1 steps a ray parametric, 22.9 once consolidated AT THE SAME SHAPE, and
 * 33.8 after a Smooth stroke was committed over it — so most of the rise is
 * the lattice and the rest is the relax, which flattens the field and shortens
 * every step (tests/unit/test_parity.cpp, "consolidating a layer really does
 * change the march").
 *
 * That is inherent to baking a field, not a defect: a lattice is only a bound
 * on the distance to its own surface, and the marcher may not overstep a bound.
 * What it means for a caller is that a renderer with a FIXED step budget draws
 * a committed Smooth worse than it drew the parametric layer — dropped or
 * ragged pixels where rays run out of iterations — and that two renderers with
 * different budgets disagree about the same document (issue #379). The budget
 * is the host's, so raising it is the host's; `out_budget`'s `safe_step_scale`
 * and clay_layer_safe_step_scale are what say by how much.
 *
 * CLAY_ERROR_INVALID_ARGUMENT, changing nothing, when the layer was edited,
 * removed or protected since begin. `out_budget` may be NULL. The transaction
 * is spent either way and must still be destroyed. */
clay_result clay_sdf_smooth_commit(clay_sdf_smooth_tx* tx, clay_sculpt_budget* out_budget);

/* -- the incremental preview: only the bricks that changed ------------------
 *
 * clay_sdf_smooth_preview_item above copies the WHOLE working volume, which is
 * the wrong shape for a per-frame loop: a dab moves a ball of bricks and the
 * host re-uploads the model. These two calls hand over exactly the bricks whose
 * bytes are new -- the ones a dab materialized, and the ones its relax moved.
 *
 * Two calls, because the buffers are the CALLER'S: ask what is waiting, size
 * the buffers, take it. That is the same shape clay_brick_cache_take_dirty and
 * clay_voxel_take_dirty_chunks already use, and the same reason -- a host
 * patching a GPU cache wants to own the memory it uploads from.
 *
 * THE DELTA ACCUMULATES until it is taken, deduplicated by brick, so a host
 * that skips a frame loses nothing and one that reads twice is told the same
 * thing twice. `generation` is bumped by every update that changed the preview
 * and by nothing else, which is how a host tells a duplicate read from a
 * skipped frame and drops an upload it began against an older one. */

/* One changed brick. An ARRAY ELEMENT, so no struct_size: a caller receives
 * thousands of them and the layout is the contract, exactly as
 * clay_brick_request and clay_voxel_chunk_mesh_range are. */
typedef struct clay_sdf_preview_brick {
    int32_t key[3];        /* the brick's lattice coordinate */
    float origin[3];       /* world position of its first sample */
    float spacing;         /* cell size; samples are spacing apart */
    uint32_t sample_dim;   /* samples per axis, halo included */
    uint64_t sample_offset; /* where its samples start in the sample buffer */
} clay_sdf_preview_brick;

typedef struct clay_sdf_preview_delta_info {
    uint32_t struct_size; /* = sizeof(clay_sdf_preview_delta_info); required */
    uint64_t generation;
    uint64_t brick_count;   /* bricks waiting to be taken */
    uint64_t sample_floats; /* floats the sample buffer must hold for them */
    int32_t has_bounds;     /* 0: nothing waiting, and the bounds are untouched */
    float bounds_min[3];
    float bounds_max[3];
} clay_sdf_preview_delta_info;

/* What is waiting, without taking it. Takes nothing and changes nothing, so a
 * host may call it every frame to decide whether to bother. */
clay_result clay_sdf_smooth_preview_delta_info(const clay_sdf_smooth_tx* tx,
                                               clay_sdf_preview_delta_info* out_info);

/* Take it. `bricks` receives one record per changed brick and `samples` their
 * values back to back, x-fastest over sample_dim^3 -- the order
 * clay_item_volume_* stores them in.
 *
 * CLAY_ERROR_BUFFER_TOO_SMALL when either buffer is short, and then NOTHING is
 * taken: the delta is still waiting and the out-counts report what it needs, so
 * a caller grows its buffers and asks again. A partial drain would strand
 * bricks that nothing will report a second time.
 *
 * On success the delta is cleared. The generation is not: it names the state
 * the caller now holds. */
clay_result clay_sdf_smooth_preview_delta_take(clay_sdf_smooth_tx* tx,
                                               clay_sdf_preview_brick* bricks,
                                               uint64_t brick_capacity, float* samples,
                                               uint64_t sample_capacity,
                                               uint64_t* out_bricks, uint64_t* out_samples);

/* Discard the preview. The document was never touched, so this only ends the
 * gesture; the handle must still be destroyed. */
void clay_sdf_smooth_cancel(clay_sdf_smooth_tx* tx);
void clay_sdf_smooth_destroy(clay_sdf_smooth_tx* tx);

/* Begin a live Move drag anchored at `centre`. NULL on failure: no such layer,
 * not an SDF layer, a protected layer, or a non-positive radius, which is not
 * a drag. A drag that reaches NOTHING succeeds with zero affected items — the
 * artist pressed on empty space, which is not an error.
 *
 * THE ONLY traversal of the edit list in the whole gesture happens here. A drag
 * holds its anchor and radius fixed and only grows the displacement, so which
 * items it reaches and where its centre lands in each of their frames cannot
 * change; every frame after this costs the items it moves and nothing else.
 *
 * `policy` may be NULL, which means no budget and no consolidation. */
clay_sdf_move_tx* clay_sdf_move_begin(clay_document* doc, clay_layer_id layer,
                                      const float centre[3], const clay_move_params* params,
                                      const clay_sculpt_policy* policy);

/* The drag so far, measured from the ANCHOR — the total, never an increment on
 * the last frame. Updates of 0.1, 0.2 then 0.5 must end at exactly what a
 * single fresh drag of 0.5 produces; a composition of the three would move the
 * surface further than 0.5 ever asked for. `out_dirty` may be NULL, and its
 * bounds are the conservative swept ball: where the surface was, united with
 * where it went. */
clay_result clay_sdf_move_update(clay_sdf_move_tx* tx, const float total_displacement[3],
                                 clay_sculpt_dirty* out_dirty);

/* Which nodes the drag reaches. Fixed for the gesture — that is the point.
 * Size-query pattern, exactly as clay_layer_move_surface_preview: call with
 * out_nodes NULL to receive the count. */
clay_result clay_sdf_move_preview_nodes(const clay_sdf_move_tx* tx, clay_node_id* out_nodes,
                                        size_t capacity, size_t* out_count);

/* How many grabs the last update resolved for one affected node: one, unless
 * the layer carries a mirror or a radial count, when it is one per IMAGE of the
 * drag that reaches the node — a straddler takes the ball's grab and its
 * reflection's, and a host that drew only the first would preview half the
 * drag. CLAY_ERROR_NOT_FOUND for a node the drag does not reach. */
clay_result clay_sdf_move_preview_grab_count(const clay_sdf_move_tx* tx, clay_node_id node,
                                             size_t* out_count);

/* Grab `index` of those, in that node's own frame — the parameters
 * clay_item_add_deformer(CLAY_DEFORM_GRAB, ...) takes, so a host can reproduce
 * the preview through machinery it already has. Together, in index order, they
 * belong at the FRONT of that node's chain. CLAY_ERROR_NOT_FOUND for a node
 * the drag does not reach, CLAY_ERROR_INVALID_ARGUMENT for an index past the
 * count. Any out pointer may be NULL. */
clay_result clay_sdf_move_preview_grab(const clay_sdf_move_tx* tx, clay_node_id node,
                                       size_t index, float out_centre[3], float* out_radius,
                                       float out_displacement[3], int32_t* out_ease,
                                       int32_t* out_front_only);

/* THE PREVIEW AS ORDINARY SCENE CONTENT (ABI 0.72.0, issue #388).
 *
 * A borrowed, read-only document carrying the drag: the real document's layers,
 * with the dragged one replaced by the transaction's preview. It compiles,
 * evaluates, meshes, picks and refills exactly as any document does, so a host
 * draws the drag through machinery it already has —
 * clay_brick_cache_eval_requests, clay_eval_points, clay_document_mesh,
 * clay_safe_step_scale — with the document itself untouched.
 *
 * That is the route the spec designates and the ABI did not offer. Without it a
 * C host had to write each resolved grab onto the layer with
 * clay_layer_add_deformer, sample, and then UNDO every one of them inside the
 * same segment — two document mutations and a full undo round-trip per pointer
 * event, to draw something the transaction was already holding.
 *
 * VALID UNTIL commit, cancel or destroy, and NOT beyond: the handle borrows the
 * transaction's own preview content, so an update is visible through it with no
 * refresh and nothing to keep in step. Do not destroy it — it is not yours.
 * NULL if the transaction is spent.
 *
 * IT CARRIES THE SDF LAYERS AND NOT THE OTHER REPRESENTATIONS. A voxel grid, a
 * mask and a mesh layer are not part of the field tape — only LayerKind::Sdf
 * layers compile — so nothing that reads the FIELD is affected, and a host that
 * also draws voxel or mesh content reads it from the real document as it
 * already does. Copying them here would charge a drag for a grid it cannot
 * change.
 *
 * A drag that reaches NO items still returns a document: it is the layer
 * unchanged, which is the honest preview of a drag that moves nothing. */
const clay_document* clay_sdf_move_preview_document(const clay_sdf_move_tx* tx);

/* One deformer chain per affected node, all inside ONE undo step, then the
 * complexity policy inside that same step. The final chains are rebuilt from
 * the chains captured at begin and the current total displacement, so a commit
 * is what the preview showed even if the host never called update.
 *
 * CLAY_ERROR_INVALID_ARGUMENT, changing nothing, when the layer was edited,
 * removed or protected since begin. `out_budget` may be NULL. */
clay_result clay_sdf_move_commit(clay_sdf_move_tx* tx, clay_sculpt_budget* out_budget);

void clay_sdf_move_cancel(clay_sdf_move_tx* tx);
void clay_sdf_move_destroy(clay_sdf_move_tx* tx);

/* One CAGE over a layer — ZBrush's Gizmo Lattice, which acts on the whole
 * subtool rather than on one item in its own frame.
 *
 * The cage is placed in the WORLD by position/axis/angle/scale and spans
 * [box_min, box_max] in its own space. `offsets_xyz` is nx*ny*nz x, y, z drags
 * in that space, x-fastest — index (i, j, k) at ((k*ny + j)*nx + i)*3 — or NULL
 * for an untouched cage, which does nothing.
 *
 * Resolved into one lattice deformer per item, each carrying the transform that
 * takes that item's frame into the cage's. That is what makes it exact for a
 * ROTATED item: a lattice box is axis-aligned by construction, so no per-item
 * box reproduces a world-placed cage, and resampling one onto a per-item grid
 * would approximate what this does exactly.
 *
 * It reaches EVERY item in the layer, unlike clay_layer_move_surface. A
 * lattice's displacement outside its box is CLAMPED rather than zero, so
 * material out there travels rigidly with the nearest part of the cage —
 * skipping distant items would tear the form.
 *
 * The whole cage is ONE undo step. Divisions must be in [2, 4] per axis; the
 * cage is evaluated per sample, at nx*ny*nz multiply-adds each time. */
typedef struct clay_gizmo_cage {
    uint32_t struct_size;
    float position[3];
    float axis[3];      /* rotation axis; the zero vector means no rotation */
    float angle;        /* radians */
    float scale;        /* uniform, > 0 */
    float box_min[3];
    float box_max[3];
    int32_t nx, ny, nz;
} clay_gizmo_cage;

/* Which nodes a cage WOULD warp, without touching the document. Size-query
 * pattern: call with out_nodes == NULL to receive the count, then again with a
 * buffer of that size. */
clay_result clay_layer_lattice_gizmo_preview(const clay_document* doc, clay_layer_id layer,
                                             const clay_gizmo_cage* cage,
                                             const float* offsets_xyz, clay_node_id* out_nodes,
                                             size_t capacity, size_t* out_count);

clay_result clay_layer_lattice_gizmo(clay_document* doc, clay_layer_id layer,
                                     const clay_gizmo_cage* cage, const float* offsets_xyz,
                                     size_t* out_applied);

/* Add one domain warp to a node ALREADY IN a document, undoably.
 *
 * clay_item_add_deformer builds an item; this edits a placed one, which
 * nothing could do before — a deformer could only be set when its node was
 * created, so no verb built on one could touch an existing sculpt.
 *
 * `at_front` non-zero puts the warp at the head of the chain rather than the
 * tail. It matters: deformers apply in authoring order, so the FIRST is the
 * outermost warp on the geometry, and one appended at the back has its region
 * weight read at a point the earlier deformers already moved. A warp meant to
 * act on the finished shape goes at the front. */
clay_result clay_layer_add_deformer(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t deform, const float* params, size_t param_count,
                                    int32_t ease, int32_t at_front);

/* ...and the inverse, which the ABI had none of (issue #388). A deformer could
 * only be taken back by UNDOING the command that added it, so any host verb
 * that adds a warp and then wants to revise it had to go through the history
 * stack — which also spends an undo entry the gesture never meant to make.
 *
 * All three are undoable edits like the add. `clay_layer_remove_deformer`
 * refuses an index past the end rather than silently doing nothing, because a
 * host that miscounted should hear about it. `_clear_deformers` on a node with
 * no deformers succeeds and changes nothing — there is nothing wrong with
 * clearing an empty chain. `_deformer_count` is the read the other two need to
 * be usable at all. */
clay_result clay_layer_deformer_count(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id node, size_t* out_count);
clay_result clay_layer_remove_deformer(clay_document* doc, clay_layer_id layer,
                                       clay_node_id node, size_t index);
clay_result clay_layer_clear_deformers(clay_document* doc, clay_layer_id layer,
                                       clay_node_id node);

/* The same, for the two kinds whose payload is not a parameter list — a guide
 * and a cage are not a fixed number of floats, so they cannot go through the
 * call above and have their own doors here exactly as they do on a builder
 * (clay_item_add_bend_curve, clay_item_add_lattice).
 *
 * Arguments and refusals are the builder entry points'; `at_front` is the
 * ordering rule described above. */
clay_result clay_layer_add_bend_curve(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                      const float* guide_xyz, size_t point_count,
                                      int32_t point_type, float t0, float t1, int32_t at_front);

clay_result clay_layer_add_lattice(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   const float min[3], const float max[3], int32_t nx, int32_t ny,
                                   int32_t nz, const float* offsets_xyz, int32_t at_front);

/* Resolve a stroke and paint it into a mask. The third stroke consumer, and
 * what makes masking the same gesture as sculpting: spacing, pressure, taper,
 * steady stroke and jitter reach a mask stroke because they are resolved before
 * anything knows what a stamp will become.
 *
 * `target` is where each cell moves TO — 1 masks and 0 releases — so painting
 * and erasing are the same call.
 *
 * The footprint comes from each stamp's WORLD radius, converted to MASK cells
 * here rather than by the caller: a caller doing it by hand gets a stroke whose
 * width tracks the mask's resolution instead of the brush's radius.
 *
 * There is no mask argument: a mask does not gate its own painting, so every
 * stamp runs and *out_applied is the stamp count. */
clay_result clay_mask_apply_stroke(clay_mask* mask, const float* samples_xyzpt,
                                   size_t sample_count, const clay_stroke_preset* preset,
                                   float target, int32_t shape, int32_t falloff,
                                   size_t* out_applied);

/* Resolve a stroke and append one edit per stamp to a layer, using `item` as
 * the stamp template scaled to each stamp's radius. The builder is left
 * untouched. With undo enabled the whole stroke is ONE step.
 *
 * The preset's strength reaches an item where scaling preserves what its op
 * means. CLAY_OP_RELIEF and CLAY_OP_INCISE scale their amplitude (blend_k) by
 * it. CLAY_OP_ADD scales its whole deposit — the stamp's scale, rounding and
 * blend together — so strength 0 authors no node at all, 1 is the item
 * exactly as authored, and the response is monotonic in between; the
 * clamped-accumulation division does not apply, because overlapping unions do
 * not add up the way relief amplitudes do. Every other op ignores strength:
 * its blend_k is a radius, a depth or a half-thickness, and scaling one would
 * change the shape rather than the amount.
 *
 * This is NOT a size-query call: it applies the stroke exactly once, however
 * it is called. `count` is the capacity of out_nodes going in and the number
 * of nodes created coming out; if the buffer was too small the first capacity
 * ids are written and *count still reports the true total. out_nodes may be
 * NULL when the ids are not wanted.
 *
 * A caller that wants every id can size the buffer first with
 * clay_stroke_resolve, which is pure and gives the same stamp count for the
 * same input — an upper bound, since a masked stamp emits no node. */
clay_result clay_layer_apply_stroke(clay_document* doc, clay_layer_id layer,
                                    const float* samples_xyzpt, size_t sample_count,
                                    const clay_stroke_preset* preset, const clay_item* item,
                                    const clay_mask* mask, clay_node_id* out_nodes,
                                    size_t* count);

/* -- fixed-topology mesh brushes ------------------------------------------- */

/* The classical sculpting mode, on a mesh layer's own triangles: vertices move
 * and TOPOLOGY NEVER CHANGES. No verb here creates, splits, deletes or reorders
 * a polygon or a vertex, so a quad mesh sculpted through these entry points is
 * still the same quad mesh, corner for corner — which is the entire reason they
 * exist, since the alternative for editing a mesh layer is
 * clay_item_volume_from_mesh, which resamples the model and throws the
 * retopology away.
 *
 * Stated rather than hidden: a large grab STRETCHES triangles, and
 * CLAY_MESH_BRUSH_SNAKEHOOK stretches them to the extreme. That is the artist's
 * information that the mesh wants retopo, exactly as Blender behaves with
 * Dyntopo off. It is not a defect and no call here will refuse it.
 *
 * This does not change what a document evaluates to. A sculpted mesh layer is
 * still never evaluated, never blended with a field, and exports exactly as its
 * (now edited) vertices say. */

typedef enum clay_mesh_brush {
    CLAY_MESH_BRUSH_GRAB = 0,       /* drag the region by the stroke delta */
    CLAY_MESH_BRUSH_DRAW = 1,       /* displace along the REGION's averaged normal */
    CLAY_MESH_BRUSH_INFLATE = 2,    /* displace along EACH VERTEX's own normal */
    CLAY_MESH_BRUSH_SMOOTH = 3,     /* Laplacian average over the one-ring */
    CLAY_MESH_BRUSH_PINCH = 4,      /* signed: + gathers, - spreads (magnify) */
    CLAY_MESH_BRUSH_FLATTEN = 5,    /* project onto a plane, clay_flatten_mode */
    CLAY_MESH_BRUSH_CLAY = 6,       /* draw's deposit clamped to a plane */
    CLAY_MESH_BRUSH_CREASE = 7,     /* a tight negative draw and a pinch, in one stamp */
    CLAY_MESH_BRUSH_SCRAPE = 8,     /* flatten cut-only and smooth, one snapshot */
    CLAY_MESH_BRUSH_POLISH = 9,     /* smooth gated by dihedral angle */
    CLAY_MESH_BRUSH_SNAKEHOOK = 10, /* grab re-anchored along the drag */
    /* Slide vertices ALONG the surface to even their spacing, rather than
     * toward the Laplacian average. Smooth reshapes; this redistributes.
     *
     * It matters more here than in a tool that can subdivide: topology is fixed
     * by contract, so a large grab stretches the triangles it has, and this is
     * what recovers them without a round trip through a retopo pass. Not
     * exactly shape-preserving — sliding along a tangent PLANE leaves a curved
     * surface by a second-order amount — but measured at a fraction of what
     * smooth moves at the same strength. */
    CLAY_MESH_BRUSH_RELAX = 11,
    /* Deposit up to a fixed height above the surface as it was when the STROKE
     * began, and no further. Every other deposit verb accumulates, so a slow
     * stroke digs deeper than a fast one over the same path; this one does not.
     *
     * Needs the stroke's deltas record to know where it started, and is REFUSED
     * without one — clamping against the current surface instead would make it
     * draw under another name. */
    CLAY_MESH_BRUSH_LAYER = 12,
    /* Push material along the surface in the drag direction. Grab carries the
     * region rigidly, so a drag with a component off the surface lifts material
     * off it; this projects into each vertex's own tangent plane. */
    CLAY_MESH_BRUSH_NUDGE = 13,
    /* The colour pair, and the only verbs that do not move a vertex.
     *
     * PAINT blends each vertex toward clay_mesh_brush_desc.color by the
     * brush's own per-vertex weight, so falloff, strength, the geodesic walk,
     * the mask gate and the alpha stamp all compose with it for free.
     *
     * SMEAR pushes existing colour along `direction`, by blending each vertex
     * toward the one-ring neighbour lying most nearly OPPOSITE the drag. A
     * zero direction is no smear rather than a smooth.
     *
     * Both REFUSE a mesh with no colour attribute rather than creating one —
     * see clay_mesh_sculptor_ensure_colors. Both leave `positions` and
     * `normals` byte-identical, which is the mirror of the guarantee the
     * displacement verbs make about colours. */
    CLAY_MESH_BRUSH_PAINT = 14,
    CLAY_MESH_BRUSH_SMEAR = 15
} clay_mesh_brush;

/* The same four curves, the same values and the same weights as
 * CLAY_FALLOFF_*, declared apart because the mesh brushes live below the voxel
 * engine in the module layering and cannot see its enum. */
typedef enum clay_mesh_falloff {
    CLAY_MESH_FALLOFF_CONSTANT = 0,
    CLAY_MESH_FALLOFF_LINEAR = 1,
    CLAY_MESH_FALLOFF_SMOOTH = 2,
    CLAY_MESH_FALLOFF_GAUSSIAN = 3
} clay_mesh_falloff;

/* "I do not know which class the brush is on; find it." */
#define CLAY_MESH_NO_CLASS 0xffffffffu

/* The most smoothing passes one stamp will run, checked at this boundary
 * because each pass walks the whole region again. */
#define CLAY_MESH_MAX_SMOOTH_ITERATIONS 64

typedef struct clay_mesh_brush_desc {
    uint32_t struct_size; /* = sizeof(clay_mesh_brush_desc); required */
    int32_t verb;         /* clay_mesh_brush */
    float center[3];      /* in the MESH's own space */
    float radius;         /* must be > 0 */
    /* Signed for every verb that has a sign, and scaled into world units by
     * the radius, so a brush behaves the same at any size. */
    float strength;
    int32_t falloff; /* clay_mesh_falloff */
    /* GRAB and SNAKEHOOK: the motion this stamp applies. Ignored by the rest,
     * and ignored by clay_mesh_sculptor_apply_stroke, which takes it from the
     * motion between stamps. */
    float direction[3];
    /* DRAW, CLAY and CREASE: an explicit deposit direction. All zeroes — the
     * default — means the region's averaged normal, which is what makes draw a
     * rounded swell rather than a balloon. */
    float deposit_normal[3];
    /* Measure the falloff ALONG THE SURFACE rather than in a straight line: a
     * brush on the upper lip must not drag the chin through a closed mouth.
     * FLATTEN and SCRAPE want it off — they mean "everything under this disc",
     * and a surface walk refuses to flatten across a groove. */
    int32_t geodesic;
    /* The surface walk's seed, when the pick already told the caller which
     * triangle it hit; CLAY_MESH_NO_CLASS to search. Searching is a linear scan
     * over the mesh and is the wrong thing to do per stamp on a large one. */
    uint32_t seed_class;
    int32_t flatten_mode;    /* clay_flatten_mode, for FLATTEN and SCRAPE */
    int32_t use_given_plane; /* otherwise the region's own centroid and normal */
    float plane_point[3];
    float plane_normal[3];
    /* POLISH's gate, in radians: how far the surface around a vertex may bend
     * before the smoothing fades out. Full strength up to this angle, zero at
     * twice it. clay_mesh_brush_defaults sets it tight on purpose — a loose
     * gate is a plain smooth under another name. */
    float polish_angle;
    int32_t smooth_iterations; /* 1..CLAY_MESH_MAX_SMOOTH_ITERATIONS */
    /* LAYER's ceiling: how far above the stroke's STARTING surface the deposit
     * may reach, in WORLD units. World rather than radius-relative, unlike
     * `strength`, and that is the point rather than an inconsistency — a
     * ceiling that moved when the brush resized would not be a ceiling.
     * Negative digs to a floor instead. */
    float layer_height;
    /* An ALPHA: a scalar stamp scaling this brush's per-vertex weight, so
     * detail work on a mesh layer is alpha-driven as it already is on voxels
     * (clay_voxel_sculpt_carve_alpha) and on SDF items (clay_item_add_alpha).
     *
     * THE ENGINE DECODES NO IMAGES. `alpha` is alpha_width * alpha_height
     * samples in [0,1], row-major with u fastest, BORROWED for the duration of
     * the call — nothing is copied, so the buffer must outlive it. NULL, the
     * default, leaves every verb exactly as it was.
     *
     * Sampled by the same kernel function the SDF alpha uses, so one stamp
     * reads identically on a mesh and on a field. It multiplies the WEIGHT, so
     * it composes with every verb and every falloff at once. */
    const float* alpha;
    int32_t alpha_width;
    int32_t alpha_height;
    /* The square the stamp covers, in the plane through the brush centre whose
     * normal is alpha_direction; alpha_tangent orients it there and any rough
     * "up" works. All zeroes on the direction means the surface normal under
     * the centre; a zero extent means the brush's own diameter. */
    float alpha_direction[3];
    float alpha_tangent[3];
    float alpha_extent;
    /* PAINT's target colour. Whatever space the caller keeps the mesh's
     * colours in: it is blended toward componentwise and never converted, so a
     * linear buffer stays linear and an sRGB one stays sRGB. SMEAR ignores it
     * — its colour comes from the surface it is dragging across, which is the
     * whole difference between the two verbs.
     *
     * Appended after the alpha block, so a caller compiled against the older
     * layout passes the shorter descriptor and gets exactly the fourteen verbs
     * it had. */
    float color[3];
    /* AUTOMASKING: the gates the brush applies to ITSELF — do not cross onto a
     * face pointing the other way, do not drag the mesh's open border, stay in
     * the polygroup this stroke started in, protect the crevices.
     *
     * Composed into the per-vertex weight by multiplication rather than
     * branched into each verb, and applied LAST of the weight's factors, so a
     * descriptor with no factors set is bit-identical to one from before
     * automasking existed. Appended, so a host compiled against the older
     * layout sends zeroes and gets exactly that.
     *
     * THREE OF THE FIVE FACTORS CROSS HERE. The other two — CAVITY and
     * SURFACE_GROUP — need an input this descriptor cannot carry: a field to
     * measure cavity from, and the document's group lattice, both of which are
     * callbacks on the C++ side. Setting their bits from C is inert rather than
     * an error, and the descriptor that carries their inputs is a follow-up
     * rather than a guess made against a sample of one. */
    uint32_t automask_factors; /* clay_automask_factor, OR-ed */
    /* NORMAL_ANGLE: how far the surface may turn from the brush's own facing
     * before the gate closes, in radians. Full strength up to this angle, zero
     * at twice it. Zero reads as the engine default. */
    float automask_normal_angle;
    /* BOUNDARY: how many rings of fade to leave at an open border. */
    int32_t automask_boundary_rings;
    /* CAVITY: how much of the measured cavity to apply, in [0,1]. */
    float automask_cavity_strength;
    /* THE STAMP'S GRAIN: how far the stamp's in-plane axes are turned about its
     * own facing, in radians. This is what makes a rake, a chisel, clay strips,
     * a directional scratch and a rotated alpha presets over one frame rather
     * than five code paths — a stroke resolver that knows the direction of
     * travel sets this, and no verb has to know that it did.
     *
     * ZERO IS NO ROTATION AT ALL, not a rotation by zero, and the difference is
     * observable: turning the basis by cos 0 and sin 0 leaves a -0.0f where an
     * unrotated axis has +0.0f, and the fixed mesh's bit-parity goldens move.
     * The engine branches on it — see make_stamp_frame in
     * include/clay/mesh/stamp_frame.h.
     *
     * Appended, so a host compiled against ABI minor 74 sends the shorter
     * descriptor, this reads as zero, and its stamps are what they were.
     * Observable only through an alpha or a directional kernel: a round brush
     * has nothing to orient. */
    float stamp_azimuth;
    /* WHICH CLASS SPACE `seed_class` WAS PICKED IN — the token
     * clay_mesh_hit.seed_revision handed back, or the one
     * clay_mesh_sculptor_seed_revision reports. Zero, which is what a host
     * compiled against the older layout sends and what a host that does not
     * pick sends, claims nothing and leaves the bounds check that was always
     * here; nothing gets slower and nothing behaves differently.
     *
     * WHY A TOKEN AND NOT A BOUNDS CHECK. A seed is an INDEX, and an index
     * outlives the numbering it was taken from. A hierarchy renumbers its
     * classes whenever the sculpt level moves or a trim releases the level
     * caches — both of which happen behind the host — and a seed picked before
     * either is still comfortably in bounds, so a bounds check sees nothing.
     * What it costs is not a slightly misplaced dab: the surface walk returns
     * an EMPTY region when the seed lies farther than the radius from the
     * centre, so a stale seed loses the stamp whole, and "nothing moved" is
     * indistinguishable from a fully masked stroke.
     *
     * Sending the token turns that into a rejected seed and a scan — one stamp
     * slower, and correct. clay_mesh_sculptor_stale_seeds_rejected counts the
     * rejections, so a host sees it happen rather than inferring it. */
    uint64_t seed_revision;
} clay_mesh_brush_desc;

/* The automask factors. A bit set, so a host sends one integer. */
typedef enum clay_automask_factor {
    CLAY_AUTOMASK_NORMAL_ANGLE = 1u << 0,
    CLAY_AUTOMASK_TOPOLOGY_CONNECTED = 1u << 1,
    CLAY_AUTOMASK_BOUNDARY = 1u << 2,
    /* Declared so the bit values match the C++ vocabulary exactly, and inert
     * from C until the descriptor that carries their inputs lands. */
    CLAY_AUTOMASK_CAVITY = 1u << 3,
    CLAY_AUTOMASK_SURFACE_GROUP = 1u << 4
} clay_automask_factor;

/* The engine's defaults, so a host fills in what it means and takes the rest.
 * The verb defaults to DRAW and the geodesic flag to on. */
clay_result clay_mesh_brush_defaults(clay_mesh_brush_desc* out_desc);

/* -- THE BRUSH MODEL, AND BRUSHES AS DATA ------------------------------------
 *
 * The artist-facing families — Clay Buildup, Dam Standard, hPolish, Trim
 * Dynamic, Snake Hook, Rake — are not new deformations. Each is a kernel plus a
 * falloff plus a frame plus an accumulation rule plus a spacing, and a preset
 * is that tuple. None of them has an engine path of its own; a named brush that
 * needed one would be evidence an axis is missing. */

/* How the region under the brush is REACHED. Not how it is weighed: the walk
 * decides what is reached and the straight line decides how much. */
typedef enum clay_brush_footprint {
    CLAY_BRUSH_FOOTPRINT_BALL = 0,        /* everything under this disc */
    CLAY_BRUSH_FOOTPRINT_SURFACE_WALK = 1 /* everything reachable along the surface */
} clay_brush_footprint;

/* The direction a kernel displaces along, named rather than implied. THIS AXIS
 * IS WHY DRAW AND INFLATE ARE ONE KERNEL: they differ in exactly this column. */
typedef enum clay_brush_frame {
    CLAY_BRUSH_FRAME_NONE = 0,
    CLAY_BRUSH_FRAME_REGION_NORMAL = 1, /* one direction for the whole stamp */
    CLAY_BRUSH_FRAME_VERTEX_NORMAL = 2, /* each vertex along its own */
    CLAY_BRUSH_FRAME_STROKE_DIRECTION = 3,
    CLAY_BRUSH_FRAME_REGION_PLANE = 4
} clay_brush_frame;

/* The deformation itself: a shape of arithmetic rather than a verb. Displace
 * serves draw and inflate under two frames; Laplacian serves smooth, polish,
 * scrape and relax under different readings of one target. */
typedef enum clay_brush_kernel {
    CLAY_BRUSH_KERNEL_TRANSLATE = 0,
    CLAY_BRUSH_KERNEL_DISPLACE = 1,
    CLAY_BRUSH_KERNEL_GATHER = 2,
    CLAY_BRUSH_KERNEL_TANGENTIAL = 3,
    CLAY_BRUSH_KERNEL_PLANE = 4,
    CLAY_BRUSH_KERNEL_PLANE_DEPOSIT = 5,
    CLAY_BRUSH_KERNEL_CUT_AND_GATHER = 6,
    CLAY_BRUSH_KERNEL_LAPLACIAN = 7,
    CLAY_BRUSH_KERNEL_DEPOSIT_CEILING = 8,
    CLAY_BRUSH_KERNEL_COLOR_BLEND = 9,
    CLAY_BRUSH_KERNEL_COLOR_ADVECT = 10
} clay_brush_kernel;

/* Which buffer a kernel writes. Exclusive on purpose: a colour pass over a
 * finished sculpt must not show up as a diff on the geometry. */
typedef enum clay_brush_write_target {
    CLAY_BRUSH_TARGET_POSITION = 0,
    CLAY_BRUSH_TARGET_COLOR = 1
} clay_brush_write_target;

typedef enum clay_brush_post_policy {
    CLAY_BRUSH_POST_NONE = 0,
    CLAY_BRUSH_POST_RECOMPUTE_NORMALS = 1
} clay_brush_post_policy;

typedef struct clay_brush_model {
    uint32_t struct_size; /* = sizeof(clay_brush_model); required */
    int32_t verb;         /* clay_mesh_brush */
    int32_t footprint;    /* clay_brush_footprint */
    int32_t falloff;      /* clay_mesh_falloff */
    int32_t frame;        /* clay_brush_frame */
    int32_t kernel;       /* clay_brush_kernel */
    int32_t target;       /* clay_brush_write_target */
    int32_t post;         /* clay_brush_post_policy */
} clay_brush_model;

/* The axis decomposition of one verb — the table a preset chooses among. */
clay_result clay_brush_model_of(int32_t verb, clay_brush_model* out_model);

/* The longest preset name this ABI carries, including the terminator. A fixed
 * array rather than a pointer because a preset crosses by VALUE, and a name a
 * host has to free is a lifetime question for a string. */
#define CLAY_BRUSH_PRESET_NAME_MAX 64

typedef struct clay_brush_preset {
    uint32_t struct_size; /* = sizeof(clay_brush_preset); required */
    /* NUL-terminated; truncated rather than refused if the source is longer. */
    char name[CLAY_BRUSH_PRESET_NAME_MAX];
    /* How the stroke lays stamps down. */
    clay_stroke_preset stroke;
    /* What each stamp does. */
    clay_brush_model model;
    /* The brush's own settings. `center`, `direction`, `seed_class` and the
     * alpha block are PLACEMENT and are not part of a preset: they come from
     * the stroke and from the caller, and the alpha in particular is image
     * content a preset refuses to carry. */
    clay_mesh_brush_desc brush;
} clay_brush_preset;

clay_result clay_brush_preset_defaults(clay_brush_preset* out_preset);

/* Preset <-> bytes, on the size-query pattern: call with out_data == NULL for
 * the size. Bytes rather than a path, matching every other format this library
 * writes, so a host holding a preset library in its own container never writes
 * a temporary file.
 *
 * An OLDER schema loads, taking defaults for what it did not carry; a NEWER one
 * is refused with CLAY_ERROR_INVALID_ARGUMENT rather than read as a prefix.
 *
 * NO IMAGE BYTES CROSS. An alpha stays caller-owned and borrowed for a call, so
 * a preset library costs kilobytes and a host owns its own resource cache. */
clay_result clay_brush_preset_serialize(const clay_brush_preset* preset, uint8_t* out_data,
                                        size_t* count);
clay_result clay_brush_preset_deserialize(const uint8_t* data, size_t size,
                                          clay_brush_preset* out_preset);
/* The schema version this build writes. */
uint32_t clay_brush_preset_version(void);

/* The reference library: the named families, as data. */
size_t clay_brush_preset_library_count(void);
clay_result clay_brush_preset_library_at(size_t index, clay_brush_preset* out_preset);
/* By name, which is the library's stable key. CLAY_ERROR_NOT_FOUND otherwise. */
clay_result clay_brush_preset_by_name(const char* name, clay_brush_preset* out_preset);

/* A SCULPTING SESSION over one mesh, owning the two structures that are
 * expensive to build and cheap to keep: the vertex adjacency and the ray-query
 * tree. Building an adjacency per stamp is the whole cost of a stroke, which is
 * why this is a handle rather than a free function.
 *
 * The adjacency NEVER needs rebuilding: no verb changes topology, so it cannot
 * go stale. The BVH does — positions move under it — and
 * clay_mesh_sculptor_refresh is how a caller decides when to pay for that.
 *
 * The mesh must outlive the sculptor. A sculptor over a mesh LAYER's triangles
 * refuses every call once that layer is gone, rather than reading freed
 * storage, and refuses one over a LOCKED or GHOSTED layer, because both flags
 * mean "never edited" and a vertex displacement is an edit.
 *
 * THREADING. Building a sculptor is a READ of the mesh, on exactly the footing
 * clay_brick_cache_eval_requests documents: clay_mesh_sculptor_create,
 * clay_mesh_sculptor_refresh and clay_mesh_sculptor_refit may run on any number
 * of threads against one const document, and are NOT safe concurrently with a
 * mutating clay_document_* / clay_layer_* call. None of the three writes the
 * document — the adjacency and the tree are read from `positions` and `indices`
 * into the sculptor's own storage — and clay_last_error is per-thread, so a
 * worker reads its own. Calls on ONE sculptor handle must still be serialized
 * by the host, const readers included, exactly as a cache handle's are; two
 * sculptors share nothing.
 *
 * SO ARM A SCULPTOR OFF THE INTERFACE THREAD. On a large mesh this is the
 * difference between a click and a freeze: the adjacency is ~116 ms at 296k
 * triangles and the tree another ~89 ms, and both are owed before the first
 * pick can be answered. Note that the tree is built LAZILY, on first use — so a
 * host that calls only clay_mesh_sculptor_create on the worker still pays the
 * tree on whichever thread reaches clay_mesh_sculptor_raycast first. Call
 * clay_mesh_sculptor_refresh on the worker too, and hand over a sculptor that
 * is warm for the pick.
 *
 * The host owes the serialization against its OWN edits, and gets no help
 * detecting a missed one: a mesh mutated while a build reads it produces an
 * adjacency over positions that have since moved, which is a wrong
 * neighbourhood rather than a refused call. What IS detected is a layer
 * REBUILT under a finished sculptor — see clay_mesh_sculptor_create. */
typedef struct clay_mesh_sculptor clay_mesh_sculptor;

/* A sparse, coalesced record of what a gesture moved: the undo a mesh stroke
 * cannot get from the edit list, because vertex displacement is destructive and
 * is not an edit item.
 *
 * A vertex touched by forty stamps of one stroke appears ONCE, keeping the
 * first "before" and the last "after", so the record's size is bounded by the
 * vertices the stroke reached rather than by the stamps it took.
 *
 * Reverting restores positions AND normals bit-exactly, and never touches the
 * index or quad buffers — there is nothing there to restore, which is the
 * fixed-topology contract paying off. */
typedef struct clay_mesh_deltas clay_mesh_deltas;

/* Where a mesh sits. Its own descriptor rather than four loose arguments,
 * because it appears in two calls and a host that got the argument order wrong
 * would see a brush land somewhere plausible. All-zero rotation reads as
 * identity and a zero scale reads as 1, so a zeroed struct is the identity.
 *
 * Named a FRAME rather than a transform because clay_mesh_transform is already
 * a call that returns a transformed copy of a mesh. */
typedef struct clay_mesh_frame {
    uint32_t struct_size; /* = sizeof(clay_mesh_frame); required */
    float position[3];
    float rotation[4]; /* quaternion x y z w */
    float scale;       /* uniform */
} clay_mesh_frame;

/* `weld_epsilon` is relative to the mesh's bounding-box diagonal — vertices
 * closer than that are one point of the surface, which is what lets a brush
 * cross a UV seam without opening a crack. Pass 0 for exact-bit welding, or a
 * negative value for the engine's default. */
clay_result clay_mesh_sculptor_create(clay_mesh* mesh, float weld_epsilon,
                                      clay_mesh_sculptor** out_sculptor);
void clay_mesh_sculptor_destroy(clay_mesh_sculptor* sculptor);

clay_result clay_mesh_sculptor_vertex_count(const clay_mesh_sculptor* sculptor, size_t* out_count);
/* Welded classes: fewer than the vertex count exactly where the mesh has
 * seams, which is how a host can tell it imported a split model. */
clay_result clay_mesh_sculptor_class_count(const clay_mesh_sculptor* sculptor, size_t* out_count);

/* The token this sculptor's weld classes are numbered in, to store beside a
 * `seed_class` and send back in clay_mesh_brush_desc.seed_revision. Constant
 * for the life of the handle: a sculptor's adjacency is built once and nothing
 * rebuilds it, so vertices moving under a stroke leave the token alone and a
 * seed stays valid across the stamps of a gesture — which is exactly when
 * re-picking would be wasted work. What retires a token is a NEW sculptor, and
 * a hierarchy makes one on every rebind. */
clay_result clay_mesh_sculptor_seed_revision(const clay_mesh_sculptor* sculptor,
                                             uint64_t* out_revision);
/* How many stamps rejected a seed because its token did not match, over the
 * life of this sculptor. The number that makes a rejection OBSERVABLE: without
 * it a host cannot tell a seed that was refused from one that was accepted and
 * happened to be harmless, and neither can a test. */
clay_result clay_mesh_sculptor_stale_seeds_rejected(const clay_mesh_sculptor* sculptor,
                                                    size_t* out_count);

/* One stamp. `mask` and `deltas` may be NULL. *out_moved receives how many weld
 * classes moved — zero for a stamp that reached nothing, that was fully masked,
 * or whose settings amount to no displacement. */
clay_result clay_mesh_sculptor_stamp(clay_mesh_sculptor* sculptor, const clay_mesh_brush_desc* desc,
                                     const clay_mask* mask, clay_mesh_deltas* deltas,
                                     size_t* out_moved);

/* A LATTICE (free-form deformation) cage over the whole mesh — ZBrush's Gizmo
 * Lattice, Blender's Lattice modifier.
 *
 * This runs FORWARD, and that is why it exists on a mesh and not on an SDF
 * item: a claycore SDF deformer is an inverse point map, and forward FFD has no
 * closed-form inverse. A mesh already knows where its vertices are, so nothing
 * here inverts, iterates or approximates.
 *
 * The cage holds control-point OFFSETS, so a cage nobody has touched is exactly
 * the identity. min/max give the box it spans, in the mesh's own space; nx, ny,
 * nz are control points per axis and are clamped into [2, 32]. Evaluation is
 * trivariate Bernstein, so 2 per axis is exactly trilinear and the corner
 * control points are interpolated — dragging a corner moves that corner of the
 * box exactly.
 *
 * A vertex OUTSIDE the box travels rigidly with the nearest point of the cage
 * rather than being drawn onto it. An axis on which the box is flat reads as
 * the middle, so none of its control points are dead. */
typedef struct clay_mesh_lattice clay_mesh_lattice;

clay_mesh_lattice* clay_mesh_lattice_create(const float min[3], const float max[3], int32_t nx,
                                            int32_t ny, int32_t nz);
void clay_mesh_lattice_destroy(clay_mesh_lattice* lattice);
/* Control points per axis, after clamping. */
clay_result clay_mesh_lattice_divisions(const clay_mesh_lattice* lattice, int32_t* out_nx,
                                        int32_t* out_ny, int32_t* out_nz);
/* How far a control point has been dragged, and where it started. Out-of-range
 * indices read zero and write nowhere. */
clay_result clay_mesh_lattice_set_offset(clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                         int32_t k, const float offset[3]);
clay_result clay_mesh_lattice_offset(const clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                     int32_t k, float out_offset[3]);
clay_result clay_mesh_lattice_rest(const clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                   int32_t k, float out_rest[3]);
/* Where the control point is NOW — rest plus offset, which is what a UI draws
 * for the cage's handles. */
clay_result clay_mesh_lattice_position(const clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                       int32_t k, float out_position[3]);
/* Non-zero while no control point has been dragged. Worth asking before
 * walking a mesh: an untouched cage moves nothing. */
clay_result clay_mesh_lattice_is_identity(const clay_mesh_lattice* lattice, int32_t* out_identity);
/* What the cage moves a point by — exactly zero everywhere for an untouched
 * cage. Exposed so a host can preview the warp without applying it. */
clay_result clay_mesh_lattice_displacement(const clay_mesh_lattice* lattice, const float p[3],
                                           float out_displacement[3]);

/* -- whole-form deformers (add-mesh-deformers) ------------------------------
 *
 * The Deformation-palette transforms on a mesh layer's own vertices. They act
 * on the WHOLE mesh scaled by a mask, not on a brush region: a deformer states
 * something about the form and a brush states something about a dab.
 *
 * They are applied as FORWARD point maps, once per vertex — the opposite
 * direction to the SDF deformers of the same name, which must run backwards to
 * answer "where did the material at p come from". Forwards is both the easier
 * direction and the exact one, so a tapered mesh and a tapered field are the
 * same shape rather than two plausible ones.
 *
 * BEND IS ABSENT, and that is a measurement rather than an omission: the SDF
 * bend takes its angle from a coordinate it then moves, so it has no
 * closed-form forward map — and past a gentle angle it has none at all, since
 * the deformation folds distinct points onto the same place. */
typedef enum clay_mesh_deform {
    CLAY_MESH_DEFORM_TAPER = 0, /* cross-section scale ramps across the span */
    CLAY_MESH_DEFORM_TWIST = 1  /* rotation about the axis ramps across it */
} clay_mesh_deform;

/* A deformer and the frame it acts in — the gizmo, in effect. The canonical
 * taper and twist are maps about one axis; an SDF item supplies that axis from
 * its own transform and a mesh layer has none, so the frame is carried here.
 *
 * `origin` is where the span starts and `axis` is the direction it runs in;
 * `axis` is normalised on use. Material before the span is untouched and
 * material past it travels rigidly with the end, which is what makes a gizmo
 * box's ends mean something. */
typedef struct clay_mesh_deform_desc {
    uint32_t struct_size; /* = sizeof(clay_mesh_deform_desc); required */
    int32_t verb;         /* clay_mesh_deform */
    float origin[3];
    float axis[3];
    float span; /* > 0; how far along `axis` the ramp runs */
    /* TAPER: the cross-section scale at the start and end of the span. 1 and 1
     * is the identity. */
    float scale_start;
    float scale_end;
    /* TWIST: total rotation across the span, in radians. 0 is the identity. */
    float angle;
    int32_t ease; /* clay_ease */
} clay_mesh_deform_desc;

/* Fills a descriptor with the engine's defaults: a unit-Y axis, a span of 1,
 * an identity taper. SET struct_size BEFORE CALLING, as every descriptor
 * requires since ABI 0.35.0. */
clay_result clay_mesh_deform_defaults(clay_mesh_deform_desc* out_desc);

/* Where a point goes, without applying anything — so a host can draw its gizmo
 * and preview the warp, exactly as clay_mesh_lattice_displacement does. */
clay_result clay_mesh_deform_point(const clay_mesh_deform_desc* desc, const float p[3],
                                   float out_p[3]);

/* Apply the deformer to every vertex, scaled by `mask` where one is given.
 * `mask` and `deltas` may be NULL. *out_moved receives how many vertices moved
 * — zero for an identity deformer, which is skipped rather than written back
 * over itself, and zero for a fully masked mesh. */
clay_result clay_mesh_sculptor_deform(clay_mesh_sculptor* sculptor,
                                      const clay_mesh_deform_desc* desc, const clay_mask* mask,
                                      clay_mesh_deltas* deltas, size_t* out_moved);

/* Apply the cage to every vertex. `deltas` may be NULL. *out_moved receives how
 * many vertices moved — zero for an untouched cage, which is skipped rather
 * than written back over itself. */
clay_result clay_mesh_sculptor_lattice(clay_mesh_sculptor* sculptor,
                                       const clay_mesh_lattice* lattice, clay_mesh_deltas* deltas,
                                       size_t* out_moved);

/* Resolve a stroke and apply it — the fourth consumer of the stroke engine,
 * after the grid, the mask and the edit list. Spacing, pressure response,
 * deterministic jitter, taper, steady stroke and buildup-versus-clamped
 * accumulation all reach mesh sculpting with no new machinery.
 *
 * The descriptor's radius and strength are IGNORED: each stamp brings its own
 * from the preset, which is what makes pressure and taper shape a mesh stroke
 * exactly as they shape a voxel one.
 *
 * GRAB anchors on the first stamp and drags by the motion between stamps;
 * SNAKEHOOK re-anchors on every stamp, so its region walks with the pull.
 *
 * `mesh_to_world` is the layer transform and is used ONLY to find each vertex
 * on the mask's world-addressed lattice; NULL means identity. Everything else
 * here is in the mesh's own space.
 *
 * `defer_normals` non-zero recomputes normals once at the end instead of per
 * stamp. Faster, identical result.
 *
 * With one `deltas` record passed through a whole gesture, reverting it is one
 * undo step. */
clay_result clay_mesh_sculptor_apply_stroke(clay_mesh_sculptor* sculptor,
                                            const float* samples_xyzpt, size_t sample_count,
                                            const clay_stroke_preset* preset,
                                            const clay_mesh_brush_desc* desc, const clay_mask* mask,
                                            const clay_mesh_frame* mesh_to_world,
                                            int32_t defer_normals, clay_mesh_deltas* deltas,
                                            size_t* out_applied);

/* THE SAME STROKE, DRIVEN BY A PRESET. A preset already carries the stroke
 * preset, the verb and the brush's own settings, so this is the call a host
 * makes once it has a brush library rather than a set of sliders.
 *
 * `orient_alpha_by_stamp` lets each stamp's rotation turn the brush's alpha,
 * which is what makes a rake or a chisel expressible — the preset's stroke half
 * decides where that rotation comes from (the path, or the stylus barrel).
 *
 * The alpha itself is NOT in the preset and is passed here, because image
 * content stays caller-owned: `alpha` is alpha_width * alpha_height samples in
 * [0,1], borrowed for the duration of the call, or NULL for none. */
clay_result clay_mesh_sculptor_apply_preset(clay_mesh_sculptor* sculptor,
                                            const float* samples_xyzpt, size_t sample_count,
                                            const clay_brush_preset* preset,
                                            const float* alpha, int32_t alpha_width,
                                            int32_t alpha_height,
                                            int32_t orient_alpha_by_stamp, const clay_mask* mask,
                                            const clay_mesh_frame* mesh_to_world,
                                            int32_t defer_normals, clay_mesh_deltas* deltas,
                                            size_t* out_applied);

/* -- ADAPTIVE TOPOLOGY -------------------------------------------------------
 *
 * A surface whose CONNECTIVITY changes under the brush: geometry is created
 * where a stroke needs it and removed where it does not.
 *
 * A DIFFERENT REPRESENTATION, chosen deliberately, never a mode the fixed
 * sculptor slips into. clay_mesh_sculptor's contract is unchanged and stays
 * unchanged: no verb there creates, splits, deletes or reorders a polygon, and
 * `indices` and `quads` come out byte-identical. That is what makes a mesh layer
 * worth holding after a retopology pass, and a host relying on it is not
 * affected by anything below.
 *
 * A dynamic surface is TRIANGLES. Converting to one and back gives triangles,
 * `quads` empty, and no quad pairing is re-derived — a quad workflow does not
 * pass through this representation. */

typedef struct clay_dynamic_surface clay_dynamic_surface;
typedef struct clay_dynamic_sculptor clay_dynamic_sculptor;

/* Why a conversion was refused. A dynamic surface cannot express every triangle
 * soup, and saying which problem it hit is what lets a caller fix the model. */
typedef enum clay_dynamic_build_error {
    CLAY_DYNAMIC_OK = 0,
    CLAY_DYNAMIC_EMPTY_MESH = 1,
    CLAY_DYNAMIC_INDEX_OUT_OF_RANGE = 2,
    CLAY_DYNAMIC_DEGENERATE_TRIANGLE = 3,
    /* Three or more faces on one edge. A half-edge surface cannot express it,
     * and dropping the third face silently would be a conversion that changes
     * the model without saying so. */
    CLAY_DYNAMIC_NON_MANIFOLD_EDGE = 4
} clay_dynamic_build_error;

typedef struct clay_dynamic_surface_desc {
    uint32_t struct_size; /* = sizeof(clay_dynamic_surface_desc); required */
    /* Vertices closer than this are ONE geometric vertex. The same rule the
     * fixed adjacency uses; 0 takes the engine default. */
    float weld_epsilon;
    /* Corners disagreeing by more than this across an edge mark it a UV seam,
     * so the seam survives remeshing as an edge property rather than as a
     * crack. 0 takes the engine default. */
    float uv_seam_epsilon;
} clay_dynamic_surface_desc;

clay_result clay_dynamic_surface_defaults(clay_dynamic_surface_desc* out_desc);

/* Build from a flat mesh. The mesh is READ, not retained. */
clay_result clay_dynamic_surface_from_mesh(const clay_mesh* mesh,
                                           const clay_dynamic_surface_desc* desc,
                                           clay_dynamic_surface** out_surface,
                                           int32_t* out_error);
/* A fresh clay_mesh the caller destroys with clay_mesh_destroy. */
clay_result clay_dynamic_surface_to_mesh(const clay_dynamic_surface* surface,
                                         clay_mesh** out_mesh);
void clay_dynamic_surface_destroy(clay_dynamic_surface* surface);

typedef struct clay_dynamic_surface_stats {
    uint32_t struct_size; /* = sizeof(clay_dynamic_surface_stats); required */
    uint64_t vertices;
    uint64_t edges;
    uint64_t halfedges;
    uint64_t faces;
    uint64_t boundary_edges;
    /* Slots allocated and not live. A surface never compacts, so this is what a
     * caller watches to decide whether to round-trip it. */
    uint64_t dead_slots;
    uint64_t bytes;
} clay_dynamic_surface_stats;

clay_result clay_dynamic_surface_stats_get(const clay_dynamic_surface* surface,
                                           clay_dynamic_surface_stats* out_stats);

/* THREE REVISIONS, NOT ONE. Topology, geometry and attributes advance
 * independently, so a host re-uploads an index buffer only when connectivity
 * changed and vertex data only when it moved. The same distinction serves cache
 * invalidation for anything derived. */
typedef struct clay_surface_revision {
    uint32_t struct_size; /* = sizeof(clay_surface_revision); required */
    uint64_t topology;
    uint64_t geometry;
    uint64_t attributes;
} clay_surface_revision;

clay_result clay_dynamic_surface_revision(const clay_dynamic_surface* surface,
                                          clay_surface_revision* out_revision);

/* Surface <-> bytes, on the size-query pattern. A versioned format of its own:
 * a flat mesh's encoding cannot express a half-edge structure. Generations are
 * preserved, so a handle taken before a save still resolves after a load. */
clay_result clay_dynamic_surface_serialize(const clay_dynamic_surface* surface,
                                           uint8_t* out_data, size_t* count);
clay_result clay_dynamic_surface_deserialize(const uint8_t* data, size_t size,
                                             clay_dynamic_surface** out_surface);

/* Every invariant of the half-edge structure, checked. Cheap enough for a debug
 * build after every edit and far too slow for a release inner loop, which is
 * why it is a call rather than an assertion. */
clay_result clay_dynamic_surface_validate(const clay_dynamic_surface* surface,
                                          int32_t* out_ok, char* out_message, size_t* message_len);

/* -- the topology policy ----------------------------------------------------- */

typedef enum clay_dynamic_detail_mode {
    CLAY_DETAIL_WORLD = 0,        /* target_edge_length in world units */
    CLAY_DETAIL_BRUSH_RELATIVE = 1, /* radius / detail_resolution */
    CLAY_DETAIL_CONSTANT = 2      /* no adaptation; deformation only */
} clay_dynamic_detail_mode;

typedef struct clay_dynamic_topology_desc {
    uint32_t struct_size; /* = sizeof(clay_dynamic_topology_desc); required */
    int32_t enabled;
    int32_t detail_mode; /* clay_dynamic_detail_mode */
    float target_edge_length;
    float detail_resolution;
    /* Split above target * split_factor, collapse below target *
     * collapse_factor. THE GAP IS HYSTERESIS: with one threshold an edge just
     * above it splits into two just below it and they collapse back, for as
     * long as the brush is held still. */
    float split_factor;
    float collapse_factor;
    int32_t max_passes;
    /* A BOUND, and a parameter rather than a constant, so a host can trade
     * detail for latency on a slower device. */
    int32_t max_ops_per_stamp;
    int32_t allow_split;
    int32_t allow_collapse;
    int32_t allow_flip;
    int32_t relax_after_remesh;
    float relax_strength;
    int32_t preserve_boundaries;
    int32_t preserve_uv_seams;
    int32_t preserve_sharp_edges;
} clay_dynamic_topology_desc;

clay_result clay_dynamic_topology_defaults(clay_dynamic_topology_desc* out_desc);

typedef struct clay_dynamic_stamp_report {
    uint32_t struct_size; /* = sizeof(clay_dynamic_stamp_report); required */
    uint64_t moved_vertices;
    uint64_t split_edges;
    uint64_t collapsed_edges;
    uint64_t flipped_edges;
    uint64_t relaxed_vertices;
    /* Whether the operation budget stopped the pass, so a host can tell "the
     * region converged" from "it ran out of budget". */
    int32_t hit_budget;
    float dirty_min[3];
    float dirty_max[3];
    clay_surface_revision revision;
} clay_dynamic_stamp_report;

/* -- the sculptor ------------------------------------------------------------ */

/* The surface must outlive the sculptor. */
clay_result clay_dynamic_sculptor_create(clay_dynamic_surface* surface,
                                         clay_dynamic_sculptor** out_sculptor);
void clay_dynamic_sculptor_destroy(clay_dynamic_sculptor* sculptor);

/* ONE STAMP. `brush` is the same descriptor the fixed path takes, so a host
 * carries one brush model across both representations. LAYER is the one verb an
 * adaptive surface does not offer, and it is refused rather than silently
 * becoming something else — see the C++ header for why. */
clay_result clay_dynamic_sculptor_stamp(clay_dynamic_sculptor* sculptor,
                                        const clay_mesh_brush_desc* brush,
                                        const clay_dynamic_topology_desc* topology,
                                        const clay_mask* mask,
                                        clay_dynamic_stamp_report* out_report);

/* Rebuild the chunked index. BETWEEN STROKES, never mid-drag: a refit stays
 * correct and does not stay fast, and a rebuild is not automatically an
 * improvement. */
clay_result clay_dynamic_sculptor_rebuild_index(clay_dynamic_sculptor* sculptor);

/* -- dirty-chunk transport ---------------------------------------------------
 *
 * A host that copies a whole mesh per dab cannot use this at the sizes it exists
 * for. So the surface is partitioned into CHUNKS, each with its own revision,
 * and a stamp reports which ones it touched.
 *
 * CALLER-OWNED BUFFERS, with a capacity query first. Nothing here allocates a
 * heap object per chunk per frame.
 *
 * NO BORROWED POINTERS INTO THE SURFACE. A mutation can move or free anything,
 * and a pointer a caller held across one would be a use-after-free with no
 * generation to check — so the data is COPIED into the caller's memory and the
 * caller decides when. */

typedef struct clay_dynamic_chunk_info {
    uint32_t struct_size; /* = sizeof(clay_dynamic_chunk_info); required */
    uint32_t index;
    uint64_t revision;
    uint32_t vertex_count; /* how many float3 the chunk needs */
    uint32_t index_count;  /* how many uint32 the chunk needs */
    int32_t geometry_dirty;
    int32_t topology_dirty;
    float bounds_min[3];
    float bounds_max[3];
} clay_dynamic_chunk_info;

size_t clay_dynamic_surface_chunk_count(const clay_dynamic_sculptor* sculptor);
clay_result clay_dynamic_surface_chunk_info(const clay_dynamic_sculptor* sculptor, size_t index,
                                            clay_dynamic_chunk_info* out_info);

/* The chunks the stamps since the last clear touched. Size-query pattern: call
 * with out_indices == NULL for the count; a buffer that is too small gets
 * CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and writes
 * nothing. */
clay_result clay_dynamic_surface_dirty_chunks(const clay_dynamic_sculptor* sculptor,
                                              uint32_t* out_indices, size_t* count);
clay_result clay_dynamic_surface_clear_dirty(clay_dynamic_sculptor* sculptor);

/* Copy one chunk's triangles into the caller's buffers. The indices are LOCAL to
 * the chunk, so a host uploads it as a standalone draw. Both capacities are
 * checked and nothing is written past them. */
clay_result clay_dynamic_surface_copy_chunk(const clay_dynamic_sculptor* sculptor, size_t index,
                                            float* out_positions, size_t position_capacity,
                                            float* out_normals, size_t normal_capacity,
                                            uint32_t* out_indices, size_t index_capacity,
                                            clay_dynamic_chunk_info* out_written);

/* -- multiresolution surfaces (mesh-multires spec, add-mesh-multires) ---------
 *
 * A base cage, a deterministic Catmull-Clark hierarchy over it, and per-level
 * detail that survives an edit to the form beneath it — so an artist can add
 * wrinkles at a fine level, change the skull underneath them, and come back to
 * find the wrinkles still there and still attached.
 *
 *     P(0) = the cage    S(n) = Subdivide(P(n-1))    P(n) = S(n) + Frame(n)*D(n)
 *
 * A THIRD REPRESENTATION beside the fixed mesh and the adaptive surface, never a
 * mode either of them slips into. What may change is what tells them apart:
 * fixed topology never changes, an adaptive surface's changes locally, and this
 * one's changes only by subdivision.
 *
 * OPAQUE AND OWNING, like the adaptive surface: the hierarchy outlives the
 * sculptor, and the host holds it beside its document rather than inside one.
 * clay_multires_serialize hands back the bytes to store with it. */

typedef struct clay_multires clay_multires;
typedef struct clay_multires_sculptor clay_multires_sculptor;

/* Why an operation was refused. Mirrors mesh::MultiresError; use
 * clay_multires_error_text for a message. */
typedef enum clay_multires_error {
    CLAY_MULTIRES_OK = 0,
    CLAY_MULTIRES_EMPTY_BASE = 1,
    CLAY_MULTIRES_INDEX_OUT_OF_RANGE = 2,
    CLAY_MULTIRES_DEGENERATE_FACE = 3,
    CLAY_MULTIRES_NON_MANIFOLD = 4,
    CLAY_MULTIRES_LEVEL_OUT_OF_RANGE = 5,
    CLAY_MULTIRES_NO_LEVEL_TO_REMOVE = 6,
    CLAY_MULTIRES_OVER_BUDGET = 7,
    CLAY_MULTIRES_CANCELLED = 8,
    CLAY_MULTIRES_DETAIL_PRESENT = 9,
    CLAY_MULTIRES_DEPTH_LIMIT = 10,
    CLAY_MULTIRES_DECODE = 11,
    /* No layer with that id on this hierarchy's stack. */
    CLAY_MULTIRES_NO_SUCH_SCULPT_LAYER = 12,
    /* A sculpt write, a merge or a bake aimed at a LOCKED layer. A lock
     * refuses a write to a layer's coefficients and still permits every
     * property change, which is the rule stated rather than discovered. */
    CLAY_MULTIRES_SCULPT_LAYER_LOCKED = 13,
    /* A composition change asked for while a stroke is open. The stroke reads
     * the evaluated surface, which includes every visible layer, so moving a
     * slider between two stamps would author one gesture against two different
     * surfaces. */
    CLAY_MULTIRES_SCULPT_LAYER_STROKE_OPEN = 14,
    CLAY_MULTIRES_CAPACITY_OVERFLOW = 15
} clay_multires_error;

/* Never NULL, for any value. */
const char* clay_multires_error_text(int32_t error);

/* The subdivision rule. RECORDED rather than assumed in the encoding: a
 * hierarchy reconstructed with a different rule than it was authored with is a
 * different surface, and nothing else in the stream reveals the substitution. */
typedef enum clay_subdivision_rule {
    CLAY_SUBDIVISION_CATMULL_CLARK = 0
} clay_subdivision_rule;

typedef struct clay_multires_desc {
    uint32_t struct_size; /* = sizeof(clay_multires_desc); required */
    int32_t rule;         /* clay_subdivision_rule */
    /* Vertices closer than this are one geometric point of the cage. <= 0 takes
     * the same default Adjacency and the fixed sculptor use, so a mesh welds
     * the same way on every path through this library. */
    float weld_epsilon;
    /* What the caller will let a level cost, in bytes. 0 means no budget, which
     * is what a desktop host wants and what a memory-constrained one must not
     * use — see clay_multires_preflight_add_level. */
    uint64_t memory_budget;
} clay_multires_desc;

clay_result clay_multires_defaults(clay_multires_desc* out_desc);

/* Build a hierarchy of ONE level — the cage itself. Adding levels is a separate
 * and priced operation. Refuses rather than repairs: a conversion that quietly
 * welds a face changes the retopology somebody paid for without saying so.
 * `out_error` receives a clay_multires_error. */
clay_result clay_multires_from_mesh(const clay_mesh* mesh, const clay_multires_desc* desc,
                                    clay_multires** out_surface, int32_t* out_error);
void clay_multires_destroy(clay_multires* surface);

uint32_t clay_multires_level_count(const clay_multires* surface);
/* WHERE THE BRUSH WRITES and WHAT THE HOST DRAWS, independently. Editing a
 * coarse level while displaying a fine one is the workflow the feature exists
 * for: move the broad form and watch the pores move with it. */
clay_result clay_multires_sculpt_level(const clay_multires* surface, uint32_t* out_level);
clay_result clay_multires_display_level(const clay_multires* surface, uint32_t* out_level);
clay_result clay_multires_set_sculpt_level(clay_multires* surface, uint32_t level);
clay_result clay_multires_set_display_level(clay_multires* surface, uint32_t level);

/* How many vertices and faces a level holds. */
clay_result clay_multires_level_counts(const clay_multires* surface, uint32_t level,
                                       uint64_t* out_vertices, uint64_t* out_faces);

/* What adding a level WOULD cost, asked before any of it is paid.
 *
 * Catmull-Clark multiplies faces by four, so a 20k-quad cage is 5.1M faces at
 * level 4 and 20.5M at level 5 — and on a memory-constrained device it is the
 * PEAK allocation that kills an app rather than the steady state. This is
 * arithmetic on the level below: it allocates nothing and has no side effects. */
typedef struct clay_multires_preflight {
    uint32_t struct_size; /* = sizeof(clay_multires_preflight); required */
    uint32_t level;       /* the level that would come into existence */
    uint64_t vertices;
    uint64_t faces;
    uint64_t topology_bytes;  /* kept for the life of the level */
    uint64_t detail_bytes;    /* if every vertex of it were detailed */
    uint64_t evaluated_bytes; /* held only while the level is resident */
    uint64_t runtime_bytes;
    uint64_t persistent_bytes; /* what remains after the call */
    uint64_t peak_bytes;       /* the high-water mark during it */
    int32_t allowed;
    int32_t error; /* clay_multires_error when allowed == 0 */
} clay_multires_preflight;

clay_result clay_multires_preflight_add_level(const clay_multires* surface,
                                              clay_multires_preflight* out_preflight);

/* Add one level. BUILD-THEN-PUBLISH: a refusal or a cancellation leaves the
 * surface exactly as it was rather than one level into a state nothing knows
 * how to read. Sets the sculpt and display levels to the new one, which is what
 * an artist means by "subdivide". `token` may be NULL. */
clay_result clay_multires_add_level(clay_multires* surface, clay_cancel_token* token,
                                    int32_t* out_error);
/* Drop the highest level and the detail on it. DESTRUCTIVE; a host that wants
 * it reversible records a barrier or its own copy first. */
clay_result clay_multires_remove_highest_level(clay_multires* surface, int32_t* out_error);

/* A level as an ordinary mesh: positions, the cage's attributes subdivided over
 * their own connectivity so a UV seam is interpolated along itself and never
 * across itself, quads and their triangulation. The handle is the caller's to
 * destroy with clay_mesh_destroy. */
clay_result clay_multires_copy_level_mesh(clay_multires* surface, uint32_t level,
                                          clay_mesh** out_mesh);

/* THREE revisions, not one. A host re-uploads an index buffer only when the
 * hierarchy's shape changed, re-reads detail only when it changed, and redraws
 * only when the evaluated surface moved; one counter cannot say which happened.
 * Any of the three outputs may be NULL. */
clay_result clay_multires_revision(const clay_multires* surface, uint64_t* out_base,
                                   uint64_t* out_detail, uint64_t* out_evaluated);
/* A hash of every level's authoritative detail, so a host can prove to itself
 * that releasing the caches changed nothing that matters. */
clay_result clay_multires_detail_checksum(const clay_multires* surface, uint64_t* out_checksum);

/* What the hierarchy costs, split by what a host under pressure may act on.
 * Authoritative detail is NEVER reported as rebuildable: a host acting on that
 * distinction would delete the user's work. */
typedef struct clay_multires_memory {
    uint32_t struct_size; /* = sizeof(clay_multires_memory); required */
    uint32_t resident_levels;
    uint64_t base;          /* the cage, with its attributes */
    uint64_t topology;      /* every level's face list */
    uint64_t detail;        /* every level's coefficients */
    uint64_t authoritative; /* the three above; none of it droppable */
    uint64_t evaluated;     /* subdivided positions, frames, positions, normals */
    uint64_t runtime_index; /* connectivity, level meshes, adjacency */
    uint64_t rebuildable;   /* the two above, plus chunk_index below */
    uint64_t total;
    /* Appended in ABI 0.76.0, which is why the two sit at the end rather than
     * beside the rows they belong with: a descriptor grows at its tail or every
     * host compiled against the older layout reads the wrong field.
     *
     * `sculpt_layers` is every layer's coefficients and masks. It is counted in
     * `authoritative` and reported apart from `detail` because the two are the
     * same quantity under different owners, and a host deciding what to merge,
     * bake or delete needs to see which of them is costing it — a combined
     * figure would hide the only number an artist can act on.
     *
     * `composed` is the materialized `B + sum(s*m*L)` per level. Derived and
     * droppable, so it is counted in `rebuildable` even though it is the array
     * evaluation actually reads. */
    uint64_t sculpt_layers;
    uint64_t composed;
    /* The per-level chunk tables and their face maps (ABI 0.78.0). Its own line
     * because a host answering a memory warning acts on the categories
     * separately, and this is the one that follows the FACE count rather than
     * the vertex count.
     *
     * APPENDED, not inserted beside `runtime_index` where it belongs by
     * meaning: every field before `total` is at an offset a shipped caller
     * already compiled in, and this struct's compatibility rule is that a
     * smaller struct_size gets the original layout. A caller built against an
     * older header therefore never sees this field, while `rebuildable` and
     * `total` include it either way — the totals stay true rather than the
     * breakdown staying constant, which is the direction a memory report has to
     * err in. */
    uint64_t chunk_index;
} clay_multires_memory;

clay_result clay_multires_memory_get(const clay_multires* surface,
                                     clay_multires_memory* out_memory);
/* Release the rebuildable caches of the levels nothing is using. Rebuilding
 * them reproduces the surface bit-identically. */
clay_result clay_multires_drop_inactive_caches(clay_multires* surface);

/* Serialize the hierarchy: the cage, the rule, the level count, the active
 * levels and each level's detail. The face lists and every evaluated position
 * follow from those and are NOT written.
 *
 * Size-query pattern: call with out_data == NULL for the size. */
clay_result clay_multires_serialize(const clay_multires* surface, uint8_t* out_data,
                                    size_t* size);
/* Refuses a truncated, hostile or newer buffer — including one declaring a
 * depth whose reconstruction over its own cage would exceed this build's
 * ceiling, which is refused BEFORE anything is allocated. */
clay_result clay_multires_deserialize(const uint8_t* data, size_t size,
                                      clay_multires** out_surface);

/* -- projection --------------------------------------------------------------
 *
 * Fit every level to a sculpt made somewhere else — the supported route by
 * which a hierarchy accepts a NEW cage, and the operation the library names
 * when it refuses to swap one under a hierarchy that carries detail. Explicit
 * and expensive by design.
 *
 * NORMAL RAY FIRST, CLOSEST POINT AS THE FALLBACK. A closest-point query alone
 * snaps a vertex to whatever surface is nearest, which across a thin wall — a
 * lip, an eyelid, the gap between two fingers — is routinely the wrong side. */
typedef struct clay_multires_project_desc {
    uint32_t struct_size; /* = sizeof(clay_multires_project_desc); required */
    /* How far a vertex may travel. 0 means "as far as it takes", which an
     * unbounded search will happily use to drag a vertex to the far wall. */
    float max_distance;
    int32_t normal_ray_first; /* nonzero: cast along the normal before falling back */
    float strength;           /* <= 0 means 1: land on the reference */
} clay_multires_project_desc;

typedef struct clay_multires_project_report {
    uint32_t struct_size; /* = sizeof(clay_multires_project_report); required */
    uint64_t moved;
    /* Vertices the reference had no answer for inside max_distance. LEFT WHERE
     * THEY WERE rather than snapped to something arbitrary. */
    uint64_t missed;
    uint64_t by_ray;
    uint64_t by_closest;
    float max_offset;
    float mean_offset;
} clay_multires_project_report;

clay_result clay_multires_project_defaults(clay_multires_project_desc* out_desc);
clay_result clay_multires_project(clay_multires* surface, const clay_mesh* reference,
                                  const clay_multires_project_desc* desc,
                                  clay_cancel_token* token,
                                  clay_multires_project_report* out_report);

/* -- sculpting ---------------------------------------------------------------
 *
 * The same verbs, the same falloffs, the same mask, the same alpha and the same
 * automasking as a mesh layer — because it is the same code: a stamp runs the
 * fixed sculptor over the active level's own mesh, and what this owns is the
 * step that turns the moved positions back into what the hierarchy stores. */

typedef struct clay_multires_stamp_report {
    uint32_t struct_size; /* = sizeof(clay_multires_stamp_report); required */
    uint32_t level;       /* the level the stamp was made on */
    uint64_t moved_vertices;
    uint64_t base_revision;
    uint64_t detail_revision;
    uint64_t evaluated_revision;
} clay_multires_stamp_report;

clay_result clay_multires_sculptor_create(clay_multires* surface,
                                          clay_multires_sculptor** out_sculptor);
void clay_multires_sculptor_destroy(clay_multires_sculptor* sculptor);
/* Start a gesture. Clears the record MeshBrush::Layer measures its ceiling
 * against, so a second stroke over the same place deposits from the surface as
 * THAT stroke found it. */
clay_result clay_multires_sculptor_begin_stroke(clay_multires_sculptor* sculptor);
/* The token the CURRENTLY BOUND level's classes are numbered in, to store
 * beside a `seed_class` picked off that level's mesh and send back in
 * clay_mesh_brush_desc.seed_revision.
 *
 * THIS IS THE CALL THE TOKEN EXISTS FOR, and it is not const: it BINDS, because
 * the answer is a property of the bound level and a host asking before the
 * first stamp would otherwise be handed the token of whatever was bound last. A
 * hierarchy renumbers on every rebind — a level change, or a trim releasing the
 * level caches — and both happen behind the host, so a seed picked before
 * either is in bounds, wrong, and silent: the walk finds nothing within the
 * radius and the dab is lost rather than misplaced.
 *
 * Reports zero for a surface that cannot bind, which is the value that claims
 * nothing. */
clay_result clay_multires_sculptor_seed_revision(clay_multires_sculptor* sculptor,
                                                 uint64_t* out_revision);

/* ONE STAMP at the surface's current sculpt level. `mask` is the freeze, taken
 * exactly as every other representation takes one, and may be NULL. */
clay_result clay_multires_sculptor_stamp(clay_multires_sculptor* sculptor,
                                         const clay_mesh_brush_desc* brush, const clay_mask* mask,
                                         clay_multires_stamp_report* out_report);

/* A WHOLE STROKE at the active sculpt level, resolved into spaced stamps by the
 * same engine that drives a mesh layer.
 *
 * `samples_xyzpt` is five floats per sample — x, y, z, pressure, tilt — in the
 * surface's own space, exactly as clay_mesh_sculptor_apply_stroke takes them,
 * and `preset` is the spacing, taper and jitter. Both sides go through one
 * resolution of what a stroke IS, so a stamp lands in the same place with the
 * same radius and the same pressure-scaled strength on either representation.
 *
 * `mesh_to_world` is the layer transform and is used ONLY to find each vertex
 * on the mask's world-addressed lattice; NULL means identity.
 *
 * `defer_normals` non-zero recomputes normals once at the end instead of per
 * stamp. Faster, identical result.
 *
 * The whole call accumulates into `out_report`'s revisions, and a host that
 * wants it as one undo step records the gesture through pyclay or the C++
 * MultiresDelta — the ABI does not yet carry that record, which is stated here
 * rather than left to be discovered. */
clay_result clay_multires_sculptor_apply_stroke(clay_multires_sculptor* sculptor,
                                                const float* samples_xyzpt, size_t sample_count,
                                                const clay_stroke_preset* preset,
                                                const clay_mesh_brush_desc* brush,
                                                const clay_mask* mask,
                                                const clay_mesh_frame* mesh_to_world,
                                                int32_t defer_normals, size_t* out_applied,
                                                clay_multires_stamp_report* out_report);

/* -- changed-block transport -------------------------------------------------
 *
 * A host that copies the display level after every dab cannot use this at the
 * sizes it exists for. So the surface is addressed in BASE PATCHES: a level-0
 * face owns a subtree that never moves between faces, so a block's identity is
 * stable for the life of the hierarchy and no re-partition can invalidate what
 * a host has already uploaded.
 *
 * CALLER-OWNED BUFFERS, with a capacity query first, and no borrowed pointers
 * into the surface — a mutation can move or free anything, and a pointer held
 * across one would be a use-after-free with no generation to check. */

typedef struct clay_multires_block_info {
    uint32_t struct_size; /* = sizeof(clay_multires_block_info); required */
    uint32_t patch;
    uint32_t level;
    uint32_t vertex_count;
    uint32_t index_count; /* triangle indices, LOCAL to the block */
} clay_multires_block_info;

size_t clay_multires_dirty_block_count(const clay_multires* surface);
/* The base patches the stamps since the last clear touched. Size-query pattern:
 * call with out_patches == NULL for the count. */
clay_result clay_multires_dirty_blocks(const clay_multires* surface, uint32_t* out_patches,
                                       size_t* count);
clay_result clay_multires_clear_dirty(clay_multires* surface);

/* What one block would cost to copy, at a level. */
clay_result clay_multires_block_info_get(clay_multires* surface, uint32_t patch, uint32_t level,
                                         clay_multires_block_info* out_info);
/* Copy one block's geometry into the caller's buffers. The indices are LOCAL to
 * the block, so a host uploads it as a standalone draw. Every capacity is
 * checked and nothing is written past it; pass NULL for a channel to skip it. */
clay_result clay_multires_copy_block(clay_multires* surface, uint32_t patch, uint32_t level,
                                     float* out_positions, size_t position_capacity,
                                     float* out_normals, size_t normal_capacity,
                                     uint32_t* out_indices, size_t index_capacity,
                                     clay_multires_block_info* out_written);

/* -- sculpt layers (mesh-sculpt-layers spec, add-mesh-sculpt-layers) ----------
 *
 * A named, reorderable, dialable CHANNEL of detail over the hierarchy. The
 * artist's statement of it: make a wrinkle pass, dial it to half, hide it,
 * delete it — days later, without redoing the work under it and without
 * replaying a single stroke.
 *
 *     E(n) = B(n) + SUM over i of  s_i * m_i(v) * L_i(n, v)
 *
 * B is the level's own detail — what every stroke wrote before this existed and
 * what clay_multires_detail_checksum still hashes. L_i is layer i's
 * coefficients, in the SAME transported frame at the SAME block size, so a
 * layer contribution and a base coefficient are one quantity under two owners
 * rather than a second displacement representation to keep in step.
 *
 * THE NAME IS DELIBERATE, AND THE DISCIPLINE IS GATED. `CLAY_MESH_BRUSH_LAYER`
 * is a brush ALGORITHM (deposit to a ceiling above the stroke-start surface)
 * and clay_layer_id is a DOCUMENT layer; an artist's sculpt layer is a third
 * thing. So every entry point here says `sculpt_layer` — the same word the
 * voxel stack already spends, so the two artist stacks read alike and neither
 * reads like the brush — and tools/check_c_abi.py refuses a new entry point
 * that says `layer` without saying `sculpt_layer` or `mesh_layer`. Renaming
 * the brush enumerator was the obvious alternative and was rejected: it ships
 * in this header, in the Swift enum and in every host's serialized preset, so
 * renaming it would break all three to fix a documentation problem.
 *
 * ADDITIVE LAYERS COMMUTE. Reordering changes ORGANISATION and not geometry,
 * which is the sharpest difference from the voxel stack: that one replays CELL
 * WRITES and is genuinely order-dependent. A host that invents an ordering
 * rule here is solving a problem this representation does not have.
 *
 * IDS, NEVER INDICES. clay_multires_move_sculpt_layer changes every index at or
 * below the layer it moves, so an index handed to a host, written into a file
 * or held across a drag would name a different pass afterwards. Ids come from a
 * counter that is itself serialized: a save, a load and a reorder leave every
 * id exactly where the host left it. An index is offered for walking the stack
 * in draw order and is valid only until the next structural change.
 *
 * STRENGTH IS COMPOSITION, NOT A SCALE ON THE PEN, and this is the behaviour
 * most likely to be reported as a bug. A stroke into a layer at strength 0.5
 * records its FULL contribution, so the surface moves half as far as the pen
 * asked for; raising the slider to 1 afterwards doubles what is on screen and
 * replays no stroke. Nothing in this ABI divides by a strength — which is also
 * why merge-down and bake are defined by the surface they leave rather than by
 * concatenating coefficients, an arithmetic that is undefined at zero and zero
 * is a state one slider reaches. */

typedef uint64_t clay_sculpt_layer_id;
/* No layer: what an empty stack's active layer reads as, and what a host sets
 * to route the next stroke into the BASE detail — the form under the passes. */
#define CLAY_NO_SCULPT_LAYER 0

/* What KIND of thing a layer stores. Versioned from the first release even
 * though only one kind ships: a reader that meets a kind it does not know
 * REFUSES the document rather than skipping the layer, because a stream
 * carrying a pass a reader silently drops presents a surface missing an
 * artist's work while claiming to be complete. */
typedef enum clay_sculpt_layer_kind {
    CLAY_SCULPT_LAYER_SAMPLED = 0,   /* coefficients per vertex; the only kind that ships */
    CLAY_SCULPT_LAYER_PROCEDURAL = 1 /* reserved, and refused by this build's decoder */
} clay_sculpt_layer_kind;

/* One layer, as a host lists it. The NAME IS NOT HERE: it crosses through
 * clay_multires_sculpt_layer_name into a buffer the caller owns, because a
 * pointer into an engine-owned std::string has no lifetime a host can reason
 * about — the next rename, remove or reorder frees it. `name_bytes` is what
 * that call will ask for, including the terminator, so a host sizes one buffer
 * from this descriptor instead of calling twice. */
typedef struct clay_sculpt_layer_info {
    uint32_t struct_size; /* = sizeof(clay_sculpt_layer_info); required */
    uint64_t id;
    uint32_t index; /* bottom-first; valid until the next structural change */
    int32_t kind;   /* clay_sculpt_layer_kind */
    float strength;
    int32_t visible;
    int32_t locked;
    uint32_t name_bytes; /* including the terminator */
    /* Coefficients and mask, allocated. A layer costs its COVERAGE and not the
     * model, which is what makes a hundred passes over one cheek affordable. */
    uint64_t bytes;
    uint64_t coverage_vertices;
} clay_sculpt_layer_info;

/* What composition actually did, so the two scale claims are MEASUREMENTS
 * rather than assertions: there is no other way to see either from outside,
 * because a correct implementation and a quadratic one produce the same
 * surface. `blocks_recomposed` is what a strength change cost — the layer's
 * allocated blocks, never the level's — and `layer_blocks_visited` is the
 * (block, layer) pairs actually summed, so a stamp on top of a deep stack can
 * be shown not to sum every layer beneath it over unrelated geometry. */
typedef struct clay_sculpt_layer_stats {
    uint32_t struct_size; /* = sizeof(clay_sculpt_layer_stats); required */
    uint64_t blocks_recomposed;
    uint64_t layer_blocks_visited;
    uint64_t compositions; /* calls that recomposed at least one block */
} clay_sculpt_layer_stats;

/* -- reading the stack ------------------------------------------------------ */

clay_result clay_multires_sculpt_layer_count(const clay_multires* surface, size_t* out_count);
/* The id at a stack position, bottom-first. The loop a host draws its list
 * with; past the end is CLAY_ERROR_NOT_FOUND. */
clay_result clay_multires_sculpt_layer_id_at(const clay_multires* surface, size_t index,
                                             uint64_t* out_id);
/* An unknown id is CLAY_ERROR_NOT_FOUND rather than a zeroed descriptor: a
 * zeroed one is indistinguishable from a real layer at strength 0. */
clay_result clay_multires_sculpt_layer_info(const clay_multires* surface, uint64_t id,
                                            clay_sculpt_layer_info* out_info);
/* Size-query pattern, into a buffer the CALLER owns: call with buffer == NULL
 * for the byte count including the terminator. */
clay_result clay_multires_sculpt_layer_name(const clay_multires* surface, uint64_t id,
                                            char* buffer, size_t* size);
/* Which layer the next sculpt write lands in. CLAY_NO_SCULPT_LAYER is the base
 * detail, which is what every stroke did before this existed. */
clay_result clay_multires_active_sculpt_layer(const clay_multires* surface, uint64_t* out_id);

/* -- lifecycle -------------------------------------------------------------- */
/*
 * `out_error` receives a clay_multires_error on every refusal below and may be
 * NULL. It is what separates the three sentences a host UI has to be able to
 * say: no such layer, this layer is locked, and finish the stroke first. A bare
 * failure makes them indistinguishable. */

/* A new empty layer on top, full strength, visible, made active. Refused while
 * a stroke holds the composition. `name` may be NULL for an unnamed pass. */
clay_result clay_multires_add_sculpt_layer(clay_multires* surface, const char* name,
                                           uint64_t* out_id, int32_t* out_error);
/* Discard a layer. Re-evaluates the removed layer's COVERAGE and nothing else:
 * no stroke is replayed, and no other layer's coefficients, strength or
 * relative order change. */
clay_result clay_multires_remove_sculpt_layer(clay_multires* surface, uint64_t id,
                                              int32_t* out_error);
/* Slide a layer through the stack. Organisation only — see the note on
 * commuting above; this cannot move a vertex. */
clay_result clay_multires_move_sculpt_layer(clay_multires* surface, uint64_t id, size_t index,
                                            int32_t* out_error);
/* Fold a layer into the one below it and discard it. Defined by VISUAL PARITY:
 * the evaluated surface before equals the evaluated surface after, at any
 * strength including zero. What is lost is real and is named rather than
 * smoothed over — the merged layer's slider no longer scales what the upper
 * layer contributed independently, which is what merging MEANS. */
clay_result clay_multires_merge_sculpt_layer_down(clay_multires* surface, uint64_t id,
                                                  int32_t* out_error);
/* The same statement with the BASE as the target: the level's own detail, and
 * the cage itself for a level-0 base deformation layer. */
clay_result clay_multires_bake_sculpt_layer_to_base(clay_multires* surface, uint64_t id,
                                                    int32_t* out_error);

/* -- properties ------------------------------------------------------------- */
/*
 * A LOCK REFUSES A COEFFICIENT WRITE AND PERMITS EVERY PROPERTY CHANGE, which
 * is the rule stated rather than discovered: locking exists so an artist can
 * keep working over a finished pass, and a lock that also froze the name and
 * the slider would make "lock" mean "hide from the UI".
 *
 * A COMPOSITION CHANGE REFUSES WHILE A STROKE IS OPEN. The stroke reads the
 * evaluated surface, which includes every visible layer, so a slider moved
 * between two stamps would author one gesture against two different surfaces.
 * Rename, lock and set-active still work, because none of them moves a vertex.
 * Refusing rather than deferring until commit is deliberate: a slider that
 * appears to move and then silently applies later is the worse surprise. */

clay_result clay_multires_rename_sculpt_layer(clay_multires* surface, uint64_t id,
                                              const char* name, int32_t* out_error);
/* 1 contributes fully, 0 contributes nothing, and neither replays a stroke. */
clay_result clay_multires_set_sculpt_layer_strength(clay_multires* surface, uint64_t id,
                                                    float strength, int32_t* out_error);
/* Invisible is exactly zero rather than nearly zero: hiding a layer removes its
 * contribution bit for bit, so a host can compare the two. */
clay_result clay_multires_set_sculpt_layer_visible(clay_multires* surface, uint64_t id,
                                                   int32_t visible, int32_t* out_error);
clay_result clay_multires_set_sculpt_layer_locked(clay_multires* surface, uint64_t id,
                                                  int32_t locked, int32_t* out_error);
clay_result clay_multires_set_active_sculpt_layer(clay_multires* surface, uint64_t id,
                                                  int32_t* out_error);

/* -- content ---------------------------------------------------------------- */

/* One vertex's coefficients on a layer: three floats, tangent, bitangent and
 * normal, in the vertex's transported frame — the same three the base detail
 * stores, because they are the same quantity. Writing marks the block, so the
 * next evaluation recomposes it and the levels above follow. */
clay_result clay_multires_set_sculpt_layer_detail(clay_multires* surface, uint64_t id,
                                                  uint32_t level, uint32_t vertex,
                                                  const float* tbn, int32_t* out_error);
clay_result clay_multires_sculpt_layer_detail(const clay_multires* surface, uint64_t id,
                                              uint32_t level, uint32_t vertex, float* out_tbn);
/* The per-layer MASK, whose identity is 1 and which is a different question
 * from the brush gate: the gate says where a brush writes and is gone when the
 * pointer comes up, this says where a STORED LAYER CONTRIBUTES and is
 * serialized with it. Writing exactly 1 releases the storage again. */
clay_result clay_multires_set_sculpt_layer_mask(clay_multires* surface, uint64_t id,
                                                uint32_t level, uint32_t vertex, float weight,
                                                int32_t* out_error);
clay_result clay_multires_sculpt_layer_mask(const clay_multires* surface, uint64_t id,
                                            uint32_t level, uint32_t vertex, float* out_weight);

/* -- what changed, and what it costs ---------------------------------------- */

/* Every layer's coefficients and mask, and nothing derived from them. Apart
 * from clay_multires_detail_checksum, which still hashes the BASE detail only —
 * so a host can ask "did the form change" and "did a pass change" separately. */
clay_result clay_multires_sculpt_layer_checksum(const clay_multires* surface,
                                                uint64_t* out_checksum);
/* THREE revisions, for the same reason the hierarchy already has three: one
 * counter cannot say which of three things happened.
 *
 *   metadata     a rename, a change of active layer. Invalidates NOTHING.
 *   composition  strength, visibility, mask, order, add, remove.
 *   content      coefficients written.
 *
 * A host keyed on the second or the third does not re-evaluate a model because
 * a layer was renamed. A host that has never heard of these keeps working:
 * clay_multires_revision's detail and evaluated counters already fold in the
 * two that move geometry. Any output may be NULL. */
clay_result clay_multires_sculpt_layer_revision(const clay_multires* surface,
                                                uint64_t* out_metadata,
                                                uint64_t* out_composition,
                                                uint64_t* out_content);
clay_result clay_multires_sculpt_layer_stats(const clay_multires* surface,
                                             clay_sculpt_layer_stats* out_stats);
clay_result clay_multires_reset_sculpt_layer_stats(clay_multires* surface);
/* Release every all-zero coefficient block and every all-identity mask block —
 * the storage a stroke that undid itself left behind. NEVER CALL THIS INSIDE A
 * POINTER EVENT: it walks the stored blocks of every layer, which is
 * proportional to the stack rather than to the dab.
 *
 * The cheapest of the four levers a host under memory pressure has, the other
 * three being merge, bake and delete. There is deliberately no CAP: a cap that
 * silently stopped recording would leave the pass on the surface and
 * un-dialable, which is a correctness bug wearing a memory limit's clothes. */
clay_result clay_multires_compact_sculpt_layers(clay_multires* surface);
/* Hold the composition for the length of a gesture a host drives itself, stamp
 * by stamp. clay_multires_sculpt_layer_stroke_begin takes and releases this; a
 * host using the plain sculptor takes it for the same reason. */
clay_result clay_multires_hold_sculpt_layer_composition(clay_multires* surface, int32_t held);

/* -- high-detail stamps ------------------------------------------------------
 *
 * A height map or a TANGENT-SPACE vector displacement, over the same alpha
 * square the mesh brushes already project. What is new is one thing only: what
 * the sampled value MEANS.
 *
 * VECTOR DISPLACEMENT IS NEVER WORLD-SPACE, and that is not a preference. A
 * world-space stamp is orientation-dependent: the same map applied to the same
 * feature on the left and right of a face produces two different shapes, and
 * across a curved surface it shears. So the three components are read in the
 * vertex's own transported frame — the same frame the coefficients are stored
 * in, so a stamp and a stroke deposit into one representation.
 *
 * THE IMAGE IS PLANAR AND BORROWED. Three channels means three consecutive
 * width*height planes, not interleaved triples, because a plane is exactly the
 * buffer the existing alpha sampler reads; interleaving would need a second
 * lookup with its own stride rules, which is the drift this avoids. The engine
 * decodes no images and copies nothing: the pointer must outlive the call and
 * nothing holds it afterwards. */

typedef enum clay_detail_stamp_mode {
    /* A scalar in [0,1] multiplying the brush's per-vertex weight. This is
     * clay_mesh_brush_desc's alpha, and it is named here so the three modes
     * read as one enumeration — clay_multires_sculpt_layer_stroke_stamp_detail
     * REFUSES it, because routing a scalar alpha through a second entry point
     * would be two ways to say one thing. */
    CLAY_DETAIL_STAMP_WEIGHT = 0,
    CLAY_DETAIL_STAMP_HEIGHT = 1, /* signed displacement along the vertex normal */
    CLAY_DETAIL_STAMP_VECTOR = 2  /* three components in the vertex's frame */
} clay_detail_stamp_mode;

typedef struct clay_detail_stamp_desc {
    uint32_t struct_size; /* = sizeof(clay_detail_stamp_desc); required */
    int32_t mode;         /* clay_detail_stamp_mode */
    /* width*height samples per channel, PLANAR and BORROWED: channel c starts
     * at image + c*width*height. One channel for Height, three for Vector. */
    const float* image;
    int32_t width;
    int32_t height;
    /* World units per unit of sampled value. Signed, so one map deposits or
     * digs without a second image. */
    float amplitude;
    /* What a Height map's zero is. A map cut out of a photograph sits around
     * 0.5 and one authored as a displacement sits around 0, and guessing wrong
     * inflates or deflates the whole stamp. */
    float bias;
    /* The square, with clay_mesh_brush_desc's meanings: the plane through
     * `center` whose normal is `direction`, oriented by `tangent` (any rough
     * "up" works, it is re-orthogonalised), of side `extent`. A zero direction
     * or tangent takes the brush's own. */
    float center[3];
    float direction[3];
    float tangent[3];
    float extent;
} clay_detail_stamp_desc;

/* WHETHER THE LEVEL CAN CARRY WHAT THE STAMP HOLDS, reported rather than
 * smoothed over. A 2048-sample map across a 5 mm square carries features finer
 * than a level whose mean edge is 1 mm can represent, and applying it anyway
 * produces a surface that looks like the map through a blur — which reads as a
 * bug in the map, or in the brush, or in the artist's file, and is none of
 * those. The library that implied the resolution says when it does not have
 * it. */
typedef struct clay_detail_stamp_report {
    uint32_t struct_size; /* = sizeof(clay_detail_stamp_report); required */
    float sample_size;    /* the world size of one image sample */
    float vertex_spacing; /* the level's mean edge length */
    float oversampling;   /* samples per vertex spacing; above 1 is too fine */
    int32_t under_resolved;
} clay_detail_stamp_report;

/* -- the sculpt layer stroke transaction ------------------------------------
 *
 * WHERE A STROKE LANDS, chosen by the caller rather than inferred, because the
 * two cases a host needs are opposite and neither is a default: "sculpt the
 * pass I am working on" and "fix the form UNDER the passes without disturbing
 * them" are both ordinary. */
typedef enum clay_multires_write_domain {
    CLAY_MULTIRES_WRITE_AUTOMATIC = 0, /* the active layer if there is one, the base if not */
    CLAY_MULTIRES_WRITE_GEOMETRY = 1,  /* the base: the cage at level 0, the level's own detail */
    /* The active layer. Refuses to BEGIN when there is none, rather than
     * silently writing the form the caller asked not to touch. */
    CLAY_MULTIRES_WRITE_DETAIL = 2
} clay_multires_write_domain;

/* WHICH FREQUENCY A SMOOTH ACTS ON. Three operations rather than one filter
 * with a cutoff, and the split is REPRESENTATIONAL: the hierarchy already
 * stores the form and the detail apart, so these are three different arrays
 * rather than three settings of one pass. A plain Laplacian over pores removes
 * the pores, which is rarely what was asked. */
typedef enum clay_multires_smooth_mode {
    CLAY_MULTIRES_SMOOTH_GEOMETRY = 0,    /* positions; exactly CLAY_MESH_BRUSH_SMOOTH */
    CLAY_MULTIRES_SMOOTH_DETAIL_ONLY = 1, /* coefficients in the target channel only */
    /* The FORM, with the detail re-applied unchanged. The mode an artist
     * correcting anatomy under pores is asking for, and the one that is
     * impossible on a flat mesh. */
    CLAY_MULTIRES_SMOOTH_PRESERVE_DETAIL = 2
} clay_multires_smooth_mode;

/* A gesture into one channel: begin, stamp, commit, cancel, the same shape the
 * SDF sculpt transaction already established.
 *
 * WHY A TRANSACTION AND NOT A LOOP OF STAMPS. Three reasons, and none of them
 * exists until a stack does. A stroke has to enter ONE layer, fixed at
 * pointer-down rather than read again per dab, or a host that changes the
 * active layer mid-stroke splits one gesture across two channels. A stamp READS
 * the evaluated surface, so the composition is held for the length of the
 * stroke. And CANCEL has to be exact — a layered write is `L += dE`, so the
 * only exact restore is the recorded `before` values, which means the record
 * has to exist from the first stamp rather than be reconstructed at the end.
 *
 * Under symmetry every mirrored stamp is another stamp in the same
 * transaction, so a mirrored stroke is one layer and one record whose coverage
 * is the union of the two sides.
 *
 * THE RECORD ITSELF DOES NOT CROSS THIS ABI YET, which is stated here rather
 * than left to be discovered — the same sentence
 * clay_multires_sculptor_apply_stroke already carries about MultiresDelta. A
 * host that wants a layered gesture in an undo stack reaches it through pyclay
 * or the C++ SculptLayerDelta; commit reports how many entries the record held
 * so a host can at least see that the gesture coalesced. */
typedef struct clay_multires_sculpt_layer_stroke clay_multires_sculpt_layer_stroke;

clay_result clay_multires_sculpt_layer_stroke_create(
    clay_multires* surface, clay_multires_sculpt_layer_stroke** out_stroke);
void clay_multires_sculpt_layer_stroke_destroy(clay_multires_sculpt_layer_stroke* stroke);

/* Set before begin. Changing it while a stroke is open does nothing: the domain
 * is resolved once, which is the whole of the first reason above. */
clay_result clay_multires_sculpt_layer_stroke_set_write_domain(
    clay_multires_sculpt_layer_stroke* stroke, int32_t domain);
/* Open a gesture. Fixes the target channel, holds the composition and clears
 * the record. Refuses — changing nothing — on a stroke that is already open, on
 * a LOCKED target layer, and on CLAY_MULTIRES_WRITE_DETAIL with no active
 * layer. */
clay_result clay_multires_sculpt_layer_stroke_begin(clay_multires_sculpt_layer_stroke* stroke,
                                                    int32_t* out_error);
/* The channel this stroke is writing; CLAY_NO_SCULPT_LAYER means the base. */
clay_result clay_multires_sculpt_layer_stroke_target_layer(
    const clay_multires_sculpt_layer_stroke* stroke, uint64_t* out_id);

/* One stamp at the surface's sculpt level, into the target channel: the same
 * sixteen verbs, the same falloffs, the same mask and the same automasking,
 * because it is the same code. `mask` is the freeze and may be NULL. */
clay_result clay_multires_sculpt_layer_stroke_stamp(clay_multires_sculpt_layer_stroke* stroke,
                                                    const clay_mesh_brush_desc* brush,
                                                    const clay_mask* mask,
                                                    clay_multires_stamp_report* out_report);
/* A height or vector-displacement stamp through the brush's own weight, so the
 * falloff, the mask gate, the automasking and the alpha compose with it exactly
 * as they do with a verb. `out_stamp_report` carries the oversampling reading;
 * either report may be NULL. */
clay_result clay_multires_sculpt_layer_stroke_stamp_detail(
    clay_multires_sculpt_layer_stroke* stroke, const clay_detail_stamp_desc* stamp,
    const clay_mesh_brush_desc* brush, const clay_mask* mask,
    clay_detail_stamp_report* out_stamp_report, clay_multires_stamp_report* out_report);
clay_result clay_multires_sculpt_layer_stroke_smooth(clay_multires_sculpt_layer_stroke* stroke,
                                                     int32_t mode,
                                                     const clay_mesh_brush_desc* brush,
                                                     const clay_mask* mask,
                                                     clay_multires_stamp_report* out_report);
/* The target channel toward zero. Touches neither the base nor any other
 * layer, which is what makes it an eraser for THIS pass rather than a
 * flattening brush. */
clay_result clay_multires_sculpt_layer_stroke_erase(clay_multires_sculpt_layer_stroke* stroke,
                                                    const clay_mesh_brush_desc* brush,
                                                    const clay_mask* mask,
                                                    clay_multires_stamp_report* out_report);
/* The LEVEL'S OWN detail toward zero: the form back toward the pure
 * subdivision, with every layer left alone. Refused at level 0, where the cage
 * has no pure subdivision to return to.
 *
 * Neither of these is undo, and the difference is worth stating because a host
 * will be tempted to wire one to the other: undo walks a step list backwards
 * and restores what a gesture changed, wherever it was; these move the surface
 * toward a named target under the cursor, and are themselves gestures. */
clay_result clay_multires_sculpt_layer_stroke_restore(clay_multires_sculpt_layer_stroke* stroke,
                                                      const clay_mesh_brush_desc* brush,
                                                      const clay_mask* mask,
                                                      clay_multires_stamp_report* out_report);

/* How many stamps this gesture has taken, and how many entries the record
 * holds. A hundred stamps over one vertex is ONE entry — the record's size
 * follows the vertices the stroke reached, not the stamps it took — and
 * comparing these two is how a host sees that. */
clay_result clay_multires_sculpt_layer_stroke_stamps(
    const clay_multires_sculpt_layer_stroke* stroke, size_t* out_stamps);
clay_result clay_multires_sculpt_layer_stroke_record_size(
    const clay_multires_sculpt_layer_stroke* stroke, size_t* out_entries);

/* Close the gesture: releases the composition hold and restores the stack's
 * active layer. A gesture that changed nothing produces an empty record rather
 * than a step. */
clay_result clay_multires_sculpt_layer_stroke_commit(clay_multires_sculpt_layer_stroke* stroke,
                                                     size_t* out_entries);
/* Discard. Restores the target channel EXACTLY — the recorded `before` values,
 * not a recomputation — and leaves the composition and the active layer as they
 * were found. */
clay_result clay_multires_sculpt_layer_stroke_cancel(clay_multires_sculpt_layer_stroke* stroke);
/* -- one transport for all three surfaces ------------------------------------
 *
 * (c-abi spec, add-extreme-poly-runtime.)
 *
 * A host at twenty million vertices asks three questions, and they are the same
 * three whichever representation it is holding: what changed, give me those
 * bytes into a buffer I own, and what does this cost me. Answering them per
 * representation is three sets of entry points and three host code paths whose
 * dirty sets mean different things — a weld class, a face chunk and a base
 * patch are not interchangeable, and a host that treated them as such would
 * upload the wrong thing. So there is ONE chunk unit underneath, and this is
 * the seam over it.
 *
 * THE SHIPPED PATHS STAY. clay_dynamic_surface_chunk_* and
 * clay_multires_copy_block are unchanged, byte for byte, and a host using them
 * keeps working. What this adds is what they structurally cannot express:
 *
 *   FOUR REVISIONS, NOT ONE. A single counter cannot say "geometry moved,
 *   connectivity did not, normals are still deferred, colours are unchanged",
 *   so a host that has only one re-uploads an index buffer on every dab.
 *
 *   AN ACKNOWLEDGEMENT. clay_dynamic_surface_clear_dirty is all-or-nothing: a
 *   host that drains half a set and then drops a frame must either re-upload
 *   everything or lose a change. clay_surface_view_acknowledge retires a chunk
 *   only when its CURRENT revision matches the one the caller actually copied,
 *   so a chunk that changed again in between stays dirty.
 *
 *   STALENESS. A readback echoes the revision the caller asked for beside what
 *   the engine is at now, so a superseded result is IDENTIFIABLE rather than
 *   merely wrong. A host that draws a stale chunk draws something the engine
 *   does not think it made, and nothing in the pixels says so.
 *
 * CALLER-OWNED BUFFERS AND NO BORROWED POINTERS, the rule the shipped transport
 * already states and which has only got stronger at this scale: a mutation can
 * move or free anything, and it does so mid-drag.
 *
 * THE VIEW IS A CALL-SITE CONVENIENCE, NOT A HANDLE TO STORE. It names a
 * surface it does not own, so the surface — the mesh, the sculptor, the
 * hierarchy — must outlive it, and a caller that mutates the surface through
 * another handle should destroy the view and take a new one. It holds no copy
 * of the geometry: everything it reports is read at the moment it is asked. */

typedef struct clay_surface_view clay_surface_view;

/* Which representation is underneath. It changes exactly one thing a caller can
 * see, and the copy call says which. */
typedef enum clay_surface_kind {
    CLAY_SURFACE_FIXED = 0,
    CLAY_SURFACE_ADAPTIVE = 1,
    CLAY_SURFACE_MULTIRES = 2
} clay_surface_kind;

/* The four counters, drawn from one table-wide sequence so a comparison ACROSS
 * chunks is meaningful: "this chunk changed after that one" is a question a
 * host draining incrementally actually asks.
 *
 * AN ARRAY ELEMENT, not a versioned descriptor: a caller passes one per chunk
 * per frame and reads thousands of them, so the layout is the contract. There
 * is nothing here for a struct_size to negotiate, and appending a field would
 * move every element after the first — a break either way. */
typedef struct clay_chunk_revisions {
    uint64_t topology;   /* membership changed: the one an index buffer follows */
    uint64_t geometry;   /* the same faces, in different places */
    uint64_t normals;    /* positions unchanged, shading normals rewritten */
    uint64_t attributes; /* colour, mask, UV */
} clay_chunk_revisions;

/* One chunk, as a host reads it. An array element for the same reason its
 * revisions are: clay_surface_view_chunk_infos fills thousands in one call, and
 * a struct_size per element would forbid the bulk fill that is the point. */
typedef struct clay_chunk_info {
    uint64_t revision; /* the maximum of the four below, and the shipped counter */
    clay_chunk_revisions revisions;
    uint32_t chunk;
    uint32_t vertex_count; /* float3 the chunk needs */
    uint32_t index_count;  /* uint32 the chunk needs; triangles */
    int32_t geometry_dirty;
    int32_t topology_dirty;
    /* Zero when the id names a chunk that has since been released. A dirty set
     * MAY name one: retiring a merged chunk from the list would be an
     * O(dirty) erase on a path that runs during a stroke, to save a check that
     * does not. Skip it; do not treat it as an error. */
    int32_t live;
    float bounds_min[3];
    float bounds_max[3];
} clay_chunk_info;

/* What a chunk aims to hold, for a view over a flat mesh — the one case where
 * the caller chooses, because a fixed mesh has no partitioner of its own yet.
 * NULL takes the library's defaults, which are 128 / 32 / 256 and are MEASURED:
 * benchmarks/bench_surface_chunks.cpp sweeps 64 to 1024 over a 2.08M-vertex
 * plane at five footprints against chunk-query time, false-positive touched
 * vertices, normal recompute, upload bytes, a locality proxy and split/merge
 * cost. A host with a different draw-call budget may want a different number;
 * a host with no opinion should pass NULL. */
typedef struct clay_chunk_options {
    uint32_t struct_size; /* = sizeof(clay_chunk_options); required */
    uint32_t target_faces;
    uint32_t min_faces; /* below this two siblings merge */
    uint32_t max_faces; /* above this a chunk splits; the gap is hysteresis */
} clay_chunk_options;

clay_result clay_chunk_options_defaults(clay_chunk_options* out_options);

/* What one copy did, and against what. */
typedef struct clay_chunk_readback {
    uint32_t struct_size; /* = sizeof(clay_chunk_readback); required */
    uint32_t chunk;
    /* What the chunk NEEDS, whether or not anything was written. A capacity
     * query — every buffer NULL — fills exactly these two and returns. */
    uint32_t vertex_count;
    uint32_t index_count;
    /* What the caller said it had seen, echoed, and what the engine is at now.
     * Equal on a fresh readback. */
    clay_chunk_revisions requested;
    clay_chunk_revisions current;
    /* The engine moved on after the caller took its snapshot. The data written
     * is CURRENT — this is not a failure — but a host applying an older frame's
     * plan can tell that its plan is out of date. */
    int32_t stale;
    /* A buffer was too small, so NOTHING was written into it — not a partial
     * fill a caller might draw. The counts above say what it needed, and the
     * call returns CLAY_ERROR_BUFFER_TOO_SMALL rather than
     * CLAY_ERROR_INVALID_ARGUMENT: the two mean opposite things to a host. A
     * too-small buffer is RETRYABLE — grow to the counts and call again, which
     * is the loop every other size query in this header asks for — while an
     * invalid argument says the call itself was wrong and retrying it is a
     * spin. A drain loop written against the general pattern would have
     * treated this one as a fault and dropped the chunk. */
    int32_t truncated;
    int32_t ok; /* zero when the id names no live chunk */
} clay_chunk_readback;

/* A view over a flat mesh, partitioned on the spot. The chunk table is the
 * VIEW's, because the fixed sculptor keeps its own weld-class dirty list and
 * whether that is retired in favour of a chunk dirty set is a measurement this
 * change has not made yet. So this view reports one partition of a static mesh
 * and an empty dirty set; the two below carry live dirty sets. */
clay_result clay_surface_view_from_mesh(const clay_mesh* mesh, const clay_chunk_options* options,
                                        clay_surface_view** out_view);
/* The adaptive surface, whose table is its own chunked index. */
clay_result clay_surface_view_from_dynamic(clay_dynamic_sculptor* sculptor,
                                           clay_surface_view** out_view);
/* One level of a hierarchy. NOT const: reading a level's chunks evaluates it,
 * exactly as clay_multires_copy_block does. */
clay_result clay_surface_view_from_multires(clay_multires* surface, uint32_t level,
                                            clay_surface_view** out_view);
void clay_surface_view_destroy(clay_surface_view* view);

/* clay_surface_kind, or -1 for a NULL view. */
int32_t clay_surface_view_kind(const clay_surface_view* view);
/* Chunk ids run from 0 to this, and a slot in that range may be dead — see
 * clay_chunk_info.live. */
size_t clay_surface_view_chunk_count(clay_surface_view* view);

/* Fill one clay_chunk_info per id in `chunks`. A NULL `chunks` means "the first
 * `count` ids in order", which is what a host walking the whole surface wants
 * and saves it building an ascending array to say so. */
clay_result clay_surface_view_chunk_infos(clay_surface_view* view, const uint32_t* chunks,
                                          size_t count, clay_chunk_info* out_infos);

/* The chunks the stamps since the last drain touched. Size-query pattern: call
 * with out_chunks == NULL for the count; a buffer that is too small gets
 * CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and writes
 * nothing. */
clay_result clay_surface_view_dirty_chunks(clay_surface_view* view, uint32_t* out_chunks,
                                           size_t* count);

/* Copy one chunk into buffers the caller owns.
 *
 * A NULL `out_positions`, `out_normals` AND `out_indices` is the CAPACITY
 * QUERY: it writes nothing and reports what the chunk needs, so a host sizes
 * once and copies once. `expected` may be NULL, which means "I have not seen
 * this chunk before" and never reports stale.
 *
 * Positions and normals are three floats per vertex; indices are triangles.
 *
 * WELDED WHERE THE REPRESENTATION ALLOWS IT. A fixed mesh and a multires level
 * have a stable per-chunk vertex list, so a chunk copies as its own vertices
 * with local indices. An adaptive surface does not — its topology changes under
 * the stamp being uploaded — so its chunks copy as UNWELDED triangles, byte for
 * byte as clay_dynamic_surface_copy_chunk already does. Read
 * clay_chunk_readback.vertex_count rather than assuming either. */
clay_result clay_surface_view_copy_chunk(clay_surface_view* view, uint32_t chunk,
                                         const clay_chunk_revisions* expected,
                                         float* out_positions, size_t position_capacity,
                                         float* out_normals, size_t normal_capacity,
                                         uint32_t* out_indices, size_t index_capacity,
                                         clay_chunk_readback* out_readback);

/* Retire chunks from the dirty set, each one only if it has not changed since
 * the caller copied it. `seen` is one clay_chunk_revisions per id — the
 * `current` a copy reported, not the one a host wishes it had.
 *
 * `out_clean` receives how many of the ids are clean afterwards, which includes
 * ones that were never dirty; the rest changed again between the copy and this
 * call and are still waiting. Draining across frames is therefore lossless at
 * any rate, which is what makes a preview budget a hint rather than a lie. */
clay_result clay_surface_view_acknowledge(clay_surface_view* view, const uint32_t* chunks,
                                          const clay_chunk_revisions* seen, size_t count,
                                          size_t* out_clean);
/* Drop the whole dirty set. The all-or-nothing form, kept for a host that
 * uploads everything it was told about in one frame and has nothing to
 * reconcile. Prefer the acknowledgement if you drain incrementally. */
clay_result clay_surface_view_clear_dirty(clay_surface_view* view);

/* -- the profile, the ledger and the trim, per surface ------------------------
 *
 * The profile is set on the hierarchy alone, and that is a statement rather
 * than an omission: it is the representation that HOLDS levels, so residency is
 * the only place a budget currently changes what the engine keeps. The other
 * two report and trim without one. */

clay_result clay_multires_set_memory_profile(clay_multires* surface,
                                             const clay_sculpt_memory_profile* profile);
clay_result clay_multires_memory_profile(const clay_multires* surface,
                                         clay_sculpt_memory_profile* out_profile);

/* Each representation answers for itself, in the shared vocabulary, so a host
 * holding one of each gets one set of three roll-ups rather than three reports
 * it has to reconcile. The ledger is FILLED, not merged: a caller accumulating
 * several surfaces adds the fields itself, because only it knows which surfaces
 * belong together. */
clay_result clay_multires_memory_ledger(const clay_multires* surface,
                                        clay_memory_ledger* out_ledger);
clay_result clay_dynamic_sculptor_memory_ledger(const clay_dynamic_sculptor* sculptor,
                                                clay_memory_ledger* out_ledger);
clay_result clay_mesh_sculptor_memory_ledger(clay_mesh_sculptor* sculptor,
                                             clay_memory_ledger* out_ledger);

/* Release rebuildable caches at a stated pressure, in a fixed order: transient
 * scratch beyond its steady capacity, preview staging, evaluated caches,
 * spatial indices for inactive levels, derived positions for inactive levels,
 * other rebuildable caches.
 *
 * NEVER AUTHORITATIVE CONTENT, and never history. Not the cage, not a level's
 * topology, not the detail, not a sculpt layer, not a mask —
 * clay_multires_detail_checksum is unchanged across any trim, which is how a
 * host proves this to itself rather than taking the sentence on trust. Undo is
 * the host's own policy through clay_document_set_history_budget.
 *
 * `pin` may be NULL. A held pin makes the call a no-op that reports what it
 * WOULD have released, with `pinned` set. */
clay_result clay_multires_trim(clay_multires* surface, int32_t pressure,
                               const clay_memory_pin* pin, clay_trim_report* out_report);
clay_result clay_dynamic_sculptor_trim(clay_dynamic_sculptor* sculptor, int32_t pressure,
                                       const clay_memory_pin* pin, clay_trim_report* out_report);

/* -- what an operation would cost, asked before it is paid --------------------
 *
 * clay_multires_preflight_add_level already did this for one operation and was
 * already the right shape: persistent and peak reported apart, a typed refusal,
 * no allocation and no side effects. Four more operations have the same
 * property — a transient PEAK that exceeds the size of their result — and had
 * no answer at all.
 *
 * THE PEAK IS THE NUMBER THAT MATTERS on a device that kills an application
 * rather than warning it, and an engine that discovers this by being terminated
 * cannot tell the user what happened.
 *
 * ONE ESTIMATOR UNDERNEATH ALL FIVE, and the reason is arithmetic rather than
 * tidiness: five bespoke estimates is five places for vertices * bytes to wrap
 * 64 bits and report a SMALL number, and the failure mode of that bug is that
 * the operation is ALLOWED. Every multiply here is checked and an overflow
 * reports CLAY_BUDGET_OVERFLOW rather than a number. */

typedef enum clay_budget_error {
    CLAY_BUDGET_OK = 0,
    /* The predicted peak exceeds the declared budget. Refused whole; nothing
     * was allocated. */
    CLAY_BUDGET_OVER_BUDGET = 1,
    /* The arithmetic itself overflowed. A refusal at ANY budget including no
     * budget: an estimate nobody can compute is not one anybody may rely on. */
    CLAY_BUDGET_OVERFLOW = 2
} clay_budget_error;

/* Never NULL, for any value. */
const char* clay_budget_error_text(int32_t error);

/* These are CEILINGS, deliberately. The structural figures are exact byte
 * costs, but the arrays that carry them are grown rather than reserved, so a
 * figure measured afterwards includes capacity slack the prediction does not. A
 * budget that errs LOW is the one that gets an application killed: it says yes
 * to an operation that does not fit. */
typedef struct clay_surface_preflight {
    uint32_t struct_size; /* = sizeof(clay_surface_preflight); required */
    int32_t allowed;
    uint64_t authoritative_bytes; /* kept, and the user's work */
    uint64_t runtime_bytes;       /* kept, and rebuildable */
    uint64_t persistent_bytes;    /* the two above: what remains after the call */
    uint64_t peak_bytes;          /* the high-water mark during it */
    int32_t error;                /* clay_budget_error when allowed == 0 */
} clay_surface_preflight;

/* `budget` of zero means no budget, which is what a desktop host passes and
 * what every one of these did before there was a budget to pass. */

/* A flat mesh becoming an adaptive surface: the peak holds the source mesh, the
 * half-edge structure and the weld map at once. */
clay_result clay_mesh_preflight_to_dynamic(const clay_mesh* mesh, uint64_t budget,
                                           clay_surface_preflight* out_preflight);
/* A global remesh to a target triangle count. Source and target are live at the
 * same time, which is the whole reason this one is asked. */
clay_result clay_mesh_preflight_global_remesh(const clay_mesh* mesh, uint64_t target_triangles,
                                              uint64_t budget,
                                              clay_surface_preflight* out_preflight);
/* An adaptive surface becoming a flat mesh. The export SPLITS a geometric
 * vertex into as many export vertices as it has distinct corner attributes, so
 * the result is bounded by CORNERS rather than by vertices — the term that
 * makes this bigger than it looks on a seam-heavy model. */
clay_result clay_dynamic_surface_preflight_to_mesh(const clay_dynamic_surface* surface,
                                                   uint64_t budget,
                                                   clay_surface_preflight* out_preflight);
/* Serialization: the blob is a second copy of everything, and it exists while
 * the surface still does. */
clay_result clay_dynamic_surface_preflight_encode(const clay_dynamic_surface* surface,
                                                  uint64_t budget,
                                                  clay_surface_preflight* out_preflight);
clay_result clay_multires_preflight_encode(const clay_multires* surface, uint64_t budget,
                                           clay_surface_preflight* out_preflight);

/* -- peak telemetry ----------------------------------------------------------
 *
 * (add-extreme-poly-runtime, task 7.7.) The four high-water marks a host tunes
 * a clay_sculpt_memory_profile against, measured by the engine while it works.
 *
 * HIGH-WATER MARKS, NOT AVERAGES, and that is the whole point rather than a
 * detail. A buffer sized to the vertex count, allocated once during warm-up and
 * reused forever, allocates nothing on a warm stamp and costs no bytes on one
 * — it satisfies every count-based assertion — and it is still O(model)
 * storage. The only thing that catches it is a PEAK that does not move between
 * a one-million and a twenty-million-vertex model at the same footprint, which
 * is what a host reads here to check the claim on its own content rather than
 * on the library's fixtures.
 *
 * EVERY SCULPTING HANDLE MEASURES ITSELF, always, from the moment it is
 * created. There is no attach step and no telemetry object to own, which is
 * deliberate: the C++ seam takes a BORROWED pointer a host sets, and a borrowed
 * pointer across this boundary is a lifetime a caller can get wrong exactly
 * once, mid-stroke, by destroying a block a sculptor still writes into. Four
 * counters cost a store each; a dangling one costs the process. So the handle
 * owns the block, the host reads a COPY into a descriptor it owns, and there is
 * nothing to keep alive.
 *
 * scratch_bytes is reported for completeness and is ZERO from these handles
 * today: the stamp path does not consume the scratch arena yet (see the change's
 * tasks 3.1 and 3.7), and reporting a number nothing measured would be worse
 * than reporting the truth. The other three are live. */

typedef struct clay_peak_telemetry {
    uint32_t struct_size; /* = sizeof(clay_peak_telemetry); required */
    uint64_t scratch_bytes;    /* the largest working set one stamp needed */
    uint64_t workset_vertices; /* the largest footprint GATHERED, rim included */
    uint64_t dirty_chunks;     /* the deepest the dirty set has been */
    uint64_t topology_ops;     /* splits and collapses in one stamp */
} clay_peak_telemetry;

/* uint64_t rather than size_t in the struct on purpose: a peak is a measurement
 * a host records, compares against a run on another device and puts in a
 * report, and a field whose width follows the pointer size makes two of those
 * runs incomparable at the byte level. size_t stays on the COUNT out-parameters
 * elsewhere, where the value is an index into this process's own memory. */

/* The workset peak of a fixed-topology session. */
clay_result clay_mesh_sculptor_peak_telemetry(const clay_mesh_sculptor* sculptor,
                                              clay_peak_telemetry* out_telemetry);
clay_result clay_mesh_sculptor_reset_peak_telemetry(clay_mesh_sculptor* sculptor);
/* The hierarchy's, which is the bound level's and follows the level: a rebind
 * builds a new level sculptor and this one keeps filling, so the peak belongs
 * to the SESSION rather than to whichever level happened to be bound first. */
clay_result clay_multires_sculptor_peak_telemetry(const clay_multires_sculptor* sculptor,
                                                  clay_peak_telemetry* out_telemetry);
clay_result clay_multires_sculptor_reset_peak_telemetry(clay_multires_sculptor* sculptor);
/* The adaptive surface's, and the only one of the three that fills all three
 * live counters: topology_ops is a split-and-collapse count no other
 * representation has, and it is the number that decides how big the slot pools
 * have to be, while dirty_chunks comes from the same chunked index the transport
 * drains. */
clay_result clay_dynamic_sculptor_peak_telemetry(const clay_dynamic_sculptor* sculptor,
                                                 clay_peak_telemetry* out_telemetry);
clay_result clay_dynamic_sculptor_reset_peak_telemetry(clay_dynamic_sculptor* sculptor);

/* -- work that is not required for correctness --------------------------------
 *
 * (sculpt-runtime spec, add-extreme-poly-runtime, tasks 3.3, 3.4 and 3.5.)
 *
 * A sculpt runtime accumulates jobs that make the NEXT interaction cheaper and
 * THIS one slower: rebuilding a spatial index whose partition has decayed,
 * compacting a chunk arena that splits have left holes in, promoting a detail
 * field that is now dense, compacting a slot pool full of dead slots, recomputing
 * normals a drag deferred. Every one of them is a stall if it happens while a
 * finger is on the glass, and none of them is the engine's decision, because
 * the engine does not own the moment between two interactions. The host does.
 *
 * SO THEY ARE QUEUED AND NOT DONE. The queue is a host's own object here, not a
 * hidden member of a sculptor, for the same reason the memory profile is filled
 * by the host: one application may service maintenance on a background thread
 * between strokes, another only when a document is idle, and a third never. A
 * queue nobody services costs a handful of structs and changes no result.
 *
 * THE STROKE GATE IS A MECHANISM RATHER THAN A CONVENTION. "we only call this
 * between strokes" is a rule that survives until the second caller, so the
 * queue refuses to hand anything out between clay_maintenance_queue_begin_stroke
 * and _end_stroke — a host that wired the drain to the wrong callback finds out
 * by nothing happening rather than by a stutter it will blame on the brush.
 *
 * WHY A TAKE/COMPLETE PAIR RATHER THAN A CALLBACK. The C++ form of this is
 * `MaintenanceQueue::service(budget, run)`, and a function pointer with a void*
 * would have crossed here unchanged. Two reasons it does not. The first is that
 * this header has never taken a callback and gains nothing by starting: the
 * budget loop is four lines in the caller's own language, and the caller is the
 * one holding the clock the budget is measured on. The second is reentrancy —
 * `run` is host code, host code that queues another item while the queue is
 * mid-drain would mutate the vector being walked, and a boundary that cannot
 * prevent that should not offer it. Take one, do it, say it is done.
 *
 * NOTHING HERE PERFORMS THE WORK. An item is a REQUEST naming a kind and a
 * target; what services it is an ordinary entry point the host already has —
 * clay_dynamic_sculptor_rebuild_index for an index, clay_mesh_sculptor_flush_-
 * normals for deferred normals. That is deliberate: an item a host declines,
 * defers forever or performs differently must leave the surface correct either
 * way, and it does, because none of it was correctness in the first place. */

typedef enum clay_maintenance_kind {
    /* The spatial index's partition has decayed under local edits. Advisory,
     * and the most likely one for a host to decline: measured over five
     * deformations, a rebuild produced a better tree in exactly one and a
     * dramatically worse one in two. */
    CLAY_MAINTENANCE_INDEX_REBUILD = 0,
    /* The chunk table's face arena has slack a split left behind. */
    CLAY_MAINTENANCE_CHUNK_COMPACTION = 1,
    /* A sparse detail field whose coverage has passed the promotion threshold.
     * Speed rather than memory: the block table is smaller than the dense form
     * until coverage passes 99.9%, and what argues for promotion is the
     * indirection on every read. */
    CLAY_MAINTENANCE_DETAIL_PROMOTION = 2,
    /* Dead slots an adaptive surface's edits left in its pools. */
    CLAY_MAINTENANCE_SLOT_POOL_COMPACTION = 3,
    /* Normals a drag deferred. THE ONE ITEM THAT IS NOT OPTIONAL: the committed
     * state has to be exact, so a host that never services the queue must still
     * flush these at stroke end with clay_mesh_sculptor_flush_normals. It is
     * here so a host can spend its budget on them FIRST, not so it can decide
     * whether to do them at all. */
    CLAY_MAINTENANCE_NORMAL_FLUSH = 4
} clay_maintenance_kind;

/* Never NULL, for any value, including one this build does not know. */
const char* clay_maintenance_kind_text(int32_t kind);

typedef struct clay_maintenance_item {
    uint32_t struct_size; /* = sizeof(clay_maintenance_item); required */
    int32_t kind;         /* clay_maintenance_kind */
    /* What the item is about: a multires level, a chunk, a surface id. The
     * queue never interprets it — it is what makes two requests the SAME
     * request rather than two entries for the same job. */
    uint32_t target;
    /* How many times this item has been re-requested since it was last
     * serviced. An entry whose count keeps climbing is one the host is
     * starving, and that is worth being able to see. */
    uint32_t requests;
    /* The requester's own estimate. Zero means "unknown", which is what most
     * callers honestly have — clay_dynamic_sculptor_request_index_rebuild fills
     * it with a LEAF COUNT rather than a time, because a rebuild is O(leaves)
     * and this library carries no machine model to turn that into microseconds.
     * A host that has measured its own device replaces it by requesting the
     * same item again with a figure of its own. */
    uint64_t estimated_micros;
} clay_maintenance_item;

/* A host's queue. Owns nothing but its own entries and refers to no surface, so
 * one queue may collect items from every surface a document holds. */
typedef struct clay_maintenance_queue clay_maintenance_queue;

clay_result clay_maintenance_queue_create(clay_maintenance_queue** out_queue);
void clay_maintenance_queue_destroy(clay_maintenance_queue* queue);

/* Queue an item, or FOLD it into the identical one already queued — same kind,
 * same target — bumping that entry's `requests` and taking the latest non-zero
 * estimate. Never allocates once the queue has reached its working size, which
 * is what makes it safe to call from a stamp: a stroke requests the same
 * rebuild on every dab. */
clay_result clay_maintenance_queue_request(clay_maintenance_queue* queue, int32_t kind,
                                           uint32_t target, uint64_t estimated_micros);

clay_result clay_maintenance_queue_count(const clay_maintenance_queue* queue, size_t* out_count);
/* The item at `index`, in queue order, without removing it. Indexed rather than
 * filled in bulk because the queue holds a HANDFUL of entries by construction —
 * five kinds and a target apiece — so a bulk fill would trade a struct_size
 * every descriptor here has for a copy nobody is making per frame. */
clay_result clay_maintenance_queue_item(const clay_maintenance_queue* queue, size_t index,
                                        clay_maintenance_item* out_item);
clay_result clay_maintenance_queue_has(const clay_maintenance_queue* queue, int32_t kind,
                                       uint32_t target, int32_t* out_has);

/* The gate. A pointer event is not a maintenance window. */
clay_result clay_maintenance_queue_begin_stroke(clay_maintenance_queue* queue);
clay_result clay_maintenance_queue_end_stroke(clay_maintenance_queue* queue);
clay_result clay_maintenance_queue_in_stroke(const clay_maintenance_queue* queue,
                                             int32_t* out_in_stroke);

/* The drain, in two halves.
 *
 *     while (budget_left()) {
 *         int32_t have = 0;
 *         clay_maintenance_queue_take_next(q, &item, &have);
 *         if (!have) break;
 *         ... do the work ...
 *         clay_maintenance_queue_complete(q, item.kind, item.target, NULL);
 *     }
 *
 * take_next PEEKS: the item stays queued until _complete says it was done, so a
 * host that took one and then decided it could not afford it has declined
 * rather than dropped it. It reports *out_have as 0 while a stroke is open and
 * while the queue is empty — the same answer to "is there work I may do now",
 * which is the question a drain is actually asking.
 *
 * complete removes the item naming that kind and target and reports whether one
 * was there. Also gated: an item completed mid-stroke would have been performed
 * mid-stroke. `out_completed` may be NULL. */
clay_result clay_maintenance_queue_take_next(clay_maintenance_queue* queue,
                                             clay_maintenance_item* out_item, int32_t* out_have);
clay_result clay_maintenance_queue_complete(clay_maintenance_queue* queue, int32_t kind,
                                            uint32_t target, int32_t* out_completed);

clay_result clay_maintenance_queue_clear(clay_maintenance_queue* queue);
clay_result clay_maintenance_queue_bytes(const clay_maintenance_queue* queue, size_t* out_bytes);

/* -- what the index is worth right now ---------------------------------------
 *
 * The measurement behind CLAY_MAINTENANCE_INDEX_REBUILD, so a host declining
 * the item is declining something it can see rather than a name.
 *
 * `quality` is the mean leaf VOLUME against the volume of their union: 1 is a
 * perfect partition and larger means the leaves overlap, which is what local
 * edits do to a tree over time. LOWER IS BETTER, and it is only meaningful
 * against this same tree's own history — never against another model's.
 *
 * A SURFACE WITH NO VOLUME REPORTS 0, and that follows from the measure rather
 * than from a bug: a flat plane's leaves and their union are both zero-volume
 * boxes, so the ratio is 0, `wants_rebuild` is never true, and the number says
 * nothing about how far the partition has drifted. Any model with thickness —
 * which is every model anybody sculpts — reports a real figure. Stated because
 * a host testing this on a ground plane would otherwise conclude the field is
 * broken.
 *
 * `wants_rebuild` is the engine's opinion of its own partition and NOT an
 * instruction. It is one of the two conditions
 * clay_dynamic_sculptor_request_index_rebuild checks; the other is the host's
 * clay_sculpt_memory_profile.allow_index_rebuild, and keeping them apart is the
 * point: the engine measures, the host decides whether it has the room. */
typedef struct clay_index_quality {
    uint32_t struct_size; /* = sizeof(clay_index_quality); required */
    uint64_t leaf_count;
    float quality;
    int32_t wants_rebuild;
} clay_index_quality;

clay_result clay_dynamic_sculptor_index_quality(const clay_dynamic_sculptor* sculptor,
                                                clay_index_quality* out_quality);

/* Queue an index rebuild if the tree wants one AND the profile allows it, and
 * report whether anything was queued. `profile` may be NULL, which is the
 * default profile and therefore allows it.
 *
 * A convenience over the two calls above and deliberately still a REQUEST: it
 * puts the job where a host can see it and rebuilds nothing.
 * clay_dynamic_sculptor_rebuild_index is what performs it, between strokes. */
clay_result clay_dynamic_sculptor_request_index_rebuild(
    const clay_dynamic_sculptor* sculptor, const clay_sculpt_memory_profile* profile,
    uint32_t target, clay_maintenance_queue* queue, int32_t* out_queued);

/* -- normals a drag deferred --------------------------------------------------
 *
 * Normals follow the vertices, so a moved vertex with a stale normal shades
 * wrong immediately — which is why they are recomputed per stamp by DEFAULT and
 * this is opt-in. What it buys is the recompute of a stroke's overlapping dabs
 * done once instead of once per dab; what it costs is that the shading lags the
 * geometry until the flush.
 *
 * THE FINAL STATE IS EXACT EITHER WAY. That is the whole contract: deferring
 * changes WHEN the work happens and nothing about the result, which is what
 * makes it a hint a memory profile may set (clay_sculpt_memory_profile.defer_-
 * normals_in_stroke) rather than a decision that would make a committed sculpt
 * a function of machine speed.
 *
 * A HOST THAT DEFERS MUST FLUSH. Nothing flushes on its own: the sculptor does
 * not know where a stroke ends, and guessing at it — a timer, a stamp with no
 * predecessor — would flush mid-drag, which is the cost this exists to avoid.
 * clay_mesh_sculptor_apply_stroke's own `defer_normals` argument is unaffected
 * and still flushes at the end of the stroke it drove, because there the
 * library does know where the stroke ended.
 *
 * The deltas argument matters and is easy to miss: a deferred stroke's undo is
 * only exact if the flush records the normals it changed, so pass the same
 * clay_mesh_deltas the stamps were recorded into, or NULL if none was. */
clay_result clay_mesh_sculptor_set_defer_normals(clay_mesh_sculptor* sculptor, int32_t defer);
clay_result clay_mesh_sculptor_defer_normals(const clay_mesh_sculptor* sculptor,
                                             int32_t* out_defer);
clay_result clay_mesh_sculptor_flush_normals(clay_mesh_sculptor* sculptor,
                                             clay_mesh_deltas* deltas);

/* The hierarchy's half. Forwarded to whichever level is bound, INCLUDING one
 * bound later, for the same reason the peak telemetry is: a rebind builds a new
 * level sculptor, and a deferral that stopped applying the moment the host
 * changed level would leave a drag half deferred and half not. Flushing binds
 * nothing — there is nothing to flush on a level nobody has stamped. */
clay_result clay_multires_sculptor_set_defer_normals(clay_multires_sculptor* sculptor,
                                                     int32_t defer);
clay_result clay_multires_sculptor_defer_normals(const clay_multires_sculptor* sculptor,
                                                 int32_t* out_defer);
clay_result clay_multires_sculptor_flush_normals(clay_multires_sculptor* sculptor);

/* Update the ray-query tree for vertices that have moved.
 *
 * REFIT is the per-stamp call. A mesh layer's topology is fixed, so a stamp
 * leaves the tree's shape a valid partition of the same triangles and only its
 * bounds stale; refitting the triangles the last stamp's region touched, and
 * their ancestors, is proportional to the BRUSH. Measured on ~130k triangles
 * with an 800-triangle dab: 0.021 ms, against 34.9 ms to rebuild.
 *
 * REFRESH rebuilds, which is proportional to the MESH — 1.3 s on a 2M-vertex
 * model, against 0.25 ms for the stamp that dirtied it. It is the right call
 * after something that moved most of the mesh (a deformer, a lattice) and after
 * replacing the mesh; it is the wrong call per stamp.
 *
 * What a STALE tree reports is worth stating, because the obvious guess is
 * wrong: it does not report the surface as it was when the tree was built. The
 * hit follows the moved triangle, but it is found through stale bounds, so the
 * reported position drifts OFF the ray — measured at 4.4e-2 before an update
 * and 1.5e-8 after. Invisible to a brush, which wants a depth; the whole error
 * budget of a gizmo, which wants a point.
 *
 * clay_mesh_sculptor_quality reports what the tree's queries cost, so a host
 * can see a refitted tree getting slower. It does NOT mean "rebuild": measured
 * over five deformations, a rebuild produced a better tree in exactly one. */
clay_result clay_mesh_sculptor_refit(clay_mesh_sculptor* sculptor);
clay_result clay_mesh_sculptor_refresh(clay_mesh_sculptor* sculptor);
/* The surface-area cost estimate of the sculptor's ray tree: the expected
 * number of triangle tests a random ray must make. Lower is better; compare it
 * against what the same tree scored when it was built. */
clay_result clay_mesh_sculptor_quality(clay_mesh_sculptor* sculptor, float* out_quality);

/* -- WHAT A STROKE'S SCRATCH COSTS -------------------------------------------
 *
 * Every sculptor keeps ONE bump arena for the buffers a stamp needs and cannot
 * hold as members — the affected-vertex list, the temporary normals, the
 * surface traversal, the alpha samples, the automask's frontiers and floods,
 * the topology candidates. It grows to the largest footprint it has actually
 * been given and is reset, not freed, between stamps, so a warm stamp on a
 * stable surface allocates nothing at all.
 *
 * THIS CROSSES BECAUSE A HOST HAS TO BUDGET FOR IT. The device this library
 * targets kills an application for memory rather than warning it twice — the
 * same reason the multiresolution preflight crosses — and a per-stroke scratch
 * high-water mark is a number such a host budgets against. A host that cannot
 * see it is guessing.
 *
 * THERE IS DELIBERATELY NOTHING TO TUNE. No reserve, no cap, no growth factor.
 * Each would be a number a host tunes against one device and is then wrong
 * about after the brush radius changes, and the arena already sizes itself from
 * the largest footprint it has seen, which is the measurement such a knob would
 * be guessing at.
 *
 * `growths` is the one to watch, and it is why a current usage is not reported:
 * a usage reads zero between stamps whatever the arena is doing. A count that
 * has stopped rising over stamps of similar footprint is the arena having
 * converged; one that rises every stamp is scratch that is never released,
 * which allocates nothing after warm-up and consumes memory without bound. */

typedef struct clay_brush_arena_stats {
    uint32_t struct_size; /* = sizeof(clay_brush_arena_stats); required */
    uint64_t capacity_bytes;   /* what the arena currently OWNS */
    uint64_t high_water_bytes; /* the largest a single stamp has ever used */
    uint64_t growths;          /* how many times it has had to take more */
} clay_brush_arena_stats;

clay_result clay_mesh_sculptor_arena_stats(const clay_mesh_sculptor* sculptor,
                                           clay_brush_arena_stats* out_stats);
clay_result clay_dynamic_sculptor_arena_stats(const clay_dynamic_sculptor* sculptor,
                                              clay_brush_arena_stats* out_stats);
/* The multiresolution sculptor's arena is the BOUND LEVEL SCULPTOR's: a stamp
 * runs the fixed sculptor over the active level's own mesh, so that is the
 * arena that grows, and reporting a second empty one here would tell a host its
 * scratch cost nothing. Before the first stamp there is no bound level and
 * every field reads zero, which is the truth rather than a placeholder. */
clay_result clay_multires_sculptor_arena_stats(const clay_multires_sculptor* sculptor,
                                               clay_brush_arena_stats* out_stats);

typedef struct clay_mesh_hit {
    uint32_t struct_size; /* = sizeof(clay_mesh_hit); required, as every descriptor's is */
    int32_t hit;
    float t;
    float position[3];
    float normal[3]; /* world space, unit */
    uint32_t triangle;
    float u, v; /* barycentrics: p = a*(1-u-v) + b*u + c*v */
    /* A weld class of the triangle hit, ready to hand back as the descriptor's
     * seed_class so the surface walk starts where the finger did. */
    uint32_t seed_class;
    /* The class space that seed_class was picked in, to send back in
     * clay_mesh_brush_desc.seed_revision.
     *
     * Handed out HERE, beside the class, rather than through a call of its own,
     * because the two are only meaningful together: a host that keeps the class
     * and forgets the token has kept exactly the half that cannot be checked,
     * and the stale-seed failure is silent. */
    uint64_t seed_revision;
} clay_mesh_hit;

/* Turn a tap into a brush centre. `xform` is where the mesh sits, or NULL for
 * identity — the ray goes into layer space and the hit comes back out here,
 * because a caller doing that by hand gets a brush whose radius changes when
 * the layer is scaled, and gets it wrong silently.
 *
 * Back faces are NOT culled: a sculptor working on the inside of a shell means
 * it. A miss sets out_hit->hit to 0 and leaves the rest untouched. */
/* Whether the mesh carries a vertex colour attribute, which PAINT and SMEAR
 * require. */
clay_result clay_mesh_sculptor_has_colors(const clay_mesh_sculptor* sculptor,
                                          int32_t* out_has);

/* Give every vertex `color` if the mesh has no colour attribute, and report
 * whether one was created. A mesh that already has colours is left exactly as
 * it is, so this is safe to call before every stroke.
 *
 * The colour verbs refuse a mesh with no colours rather than creating one
 * here: twelve bytes per vertex is a real cost to hide behind a brush stroke,
 * and a silent creation would make "I painted and nothing happened"
 * indistinguishable from "this mesh had no colour attribute". */
clay_result clay_mesh_sculptor_ensure_colors(clay_mesh_sculptor* sculptor, const float color[3],
                                             int32_t* out_created);

clay_result clay_mesh_sculptor_raycast(clay_mesh_sculptor* sculptor, const float origin[3],
                                       const float direction[3], const clay_mesh_frame* xform,
                                       clay_mesh_hit* out_hit);

clay_mesh_deltas* clay_mesh_deltas_create(void);
void clay_mesh_deltas_destroy(clay_mesh_deltas* deltas);
clay_result clay_mesh_deltas_vertex_count(const clay_mesh_deltas* deltas, size_t* out_count);
/* Both are idempotent. Refused against a sculptor bound to a different mesh. */
clay_result clay_mesh_deltas_revert(const clay_mesh_deltas* deltas, clay_mesh_sculptor* sculptor);
clay_result clay_mesh_deltas_apply(const clay_mesh_deltas* deltas, clay_mesh_sculptor* sculptor);
clay_result clay_mesh_deltas_clear(clay_mesh_deltas* deltas);

/* Fill pockets inside the footprint: an empty cell with at least four of its
 * six face neighbours occupied is inside a cavity rather than beside a
 * surface, and `passes` iterations reach that many cells deep. An open face and
 * a wide shallow dent are left alone — smoothing is the verb for surface
 * irregularity. `passes` must be > 0; larger than the model's longest side is
 * clamped, since past that every cell in the window is already decided.
 *
 * A through-hole is left alone only where it is WIDER THAN ONE CELL. The rule
 * is local and counts neighbours, so it cannot tell a pinhole from a pocket: a
 * one-cell perforation has its four lateral neighbours occupied and fills, the
 * same way repair's close-holes seals a pierced wall. Anything two cells or
 * more across stays open.
 *
 * This began as a morphological closing, which cannot do the job: a ball of
 * radius r fits INTO a dent wider than r, so a larger element fills less, and
 * the erosion reaches through from the void behind a one-cell wall and reopens
 * every hole the dilation just sealed.
 *
 * The geometry it acts on is freehand voxel work, and the everyday source is
 * the soft stamp: occupancy is binary, so any strength or falloff below 1 is
 * DITHERED against a hash of the cell coordinate, which leaves a pepper of
 * single-cell holes through the material it just deposited. A narrow erase,
 * magnify or grab leaves the same. This is not cosmetic — greedy meshing emits
 * six faces around every one of those holes, so closing them measurably cuts
 * the mesh a bake has to carry.
 *
 * Whether a call did anything is answerable rather than guessable: bracket it
 * with clay_voxel_change_count. A boolean or a rasterized mesh does not —
 * the void it leaves is sealed, so no cell there has four occupied neighbours
 * and this verb sees nothing. That is clay_voxel_repair_fill_voids' job: this
 * one fills what is NARROW, that one fills what is SEALED. */
clay_result clay_voxel_sculpt_fill_cavities(clay_voxel_grid* grid, const int32_t cell[3],
                                            const clay_brush_params* brush, int32_t passes);

/* Flatten onto the plane AND smooth, from ONE snapshot. Calling flatten then
 * smooth is not the same thing: the flatten's output would feed the smooth's
 * neighbourhood, which is what the snapshot discipline exists to prevent. A
 * zero-length normal is rejected, as it is for flatten. */
clay_result clay_voxel_sculpt_scrape(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush, const float normal[3],
                                     float offset_cells);

/* Drag SURFACE material along a direction, leaving the interior where it was.
 * That is the difference from grab, which translates every cell in its region:
 * grab moves a lump, smudge smears a skin.
 *
 * It shares grab's nearest-cell rounding and so shares its dead zone: a
 * displacement under half a cell on every axis reads each cell's source as
 * itself and writes back what was already there. CLAY_OK, nothing moved. */
clay_result clay_voxel_sculpt_smudge(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush,
                                     const float displacement[3]);

/* Carve modulated by a caller-supplied alpha: `alpha_width * alpha_height`
 * samples in [0, 1], row-major, projected onto the plane perpendicular to
 * `direction`. `index` 0 carves and a non-zero one deposits.
 *
 * The engine decodes no images and never will at this boundary: a host that
 * has an alpha has already loaded the PNG, and handing over the samples costs
 * it nothing while costing the engine no format zoo. */
clay_result clay_voxel_sculpt_carve_alpha(clay_voxel_grid* grid, const int32_t cell[3],
                                          const clay_brush_params* brush, const float* alpha,
                                          int32_t alpha_width, int32_t alpha_height,
                                          const float direction[3], int32_t index);

/* -- repair ---------------------------------------------------------------- */

/* What a pre-bake check wants to know, without performing the repair: a
 * destructive operation whose input is somebody's sculpt should be askable
 * before it is answerable. */
typedef struct clay_repair_report {
    uint32_t struct_size; /* = sizeof(clay_repair_report); required */
    size_t enclosed_voids; /* empty regions the outside cannot reach */
    size_t void_cells;     /* their total size */
    size_t largest_void;
    int32_t airtight; /* non-zero when there are no enclosed voids at all */
} clay_repair_report;

clay_result clay_voxel_repair_report(const clay_voxel_grid* grid,
                                     clay_repair_report* out_report);

/* Seal perforations by the same pocket rule the fill-cavities verb uses. Only
 * ever adds cells, so no material is lost. `passes` must be > 0; `mask` may be
 * NULL, and where one is given a fully masked cell is left alone — a repair
 * that ignored freeze would be the one destructive operation here that does. */
clay_result clay_voxel_repair_close_holes(clay_voxel_grid* grid, int32_t passes,
                                          const clay_mask* mask);

/* Fill every empty cell the outside cannot reach, coloured from the shell that
 * encloses it. Reachability is over EMPTY cells by face adjacency, seeded
 * outside the occupied bounds, so enclosure is decided rather than guessed at
 * from a local neighbourhood. */
clay_result clay_voxel_repair_fill_voids(clay_voxel_grid* grid, const clay_mask* mask);

/* -- queries --------------------------------------------------------------- */

clay_result clay_voxel_occupied_count(const clay_voxel_grid* grid, size_t* out_count);

/* Cell writes that actually CHANGED a cell, since the grid was constructed.
 *
 * Why this exists: an edit that is entirely legal and entirely without effect
 * is normal here, not exotic. A sub-cell grab or smudge rounds back to where
 * it started; a flatten meets an already-flat region; a dithered stamp misses
 * every cell; a footprint lands on empty space. Each returns CLAY_OK, which is
 * correct and also unhelpful to a host trying to tell "the drag reached
 * nothing" from "the drag did something". Diffing the grid is the only other
 * way to ask, and clay_voxel_occupied_count cannot answer it at all: grab and
 * magnify conserve material, so the count is identical either way.
 *
 * A changed cell is a write that changed the stored palette index — 0 to n, n
 * to 0, n to m. Rewriting a cell with the index it already holds is not
 * counted, so clay_voxel_palette_set, which recolours without touching voxel
 * data, does not move it either.
 *
 * Monotone and never reset, so only the DIFFERENCE between two reads means
 * anything; a grid handed back by clay_voxel_mask_extrude starts wherever its
 * construction left it. Two guarantees, of deliberately different strength:
 *   - delta == 0 exactly when the grid is byte-identical to before the
 *     bracketed calls. Universal — every verb writes through one funnel.
 *   - delta is the exact number of changed cells for every verb that writes
 *     each cell at most once, which is all of them except pinch and magnify.
 *     Those two clear a cell and write its colour into a neighbour the same
 *     call may visit later, so a cell can be counted twice and a 0->n->0 round
 *     trip can leave delta > 0 with the grid unchanged. For those it is an
 *     upper bound.
 *
 * This is NOT the unit of the out_applied parameters elsewhere in this header:
 * those count stamps run or items warped, this counts cells changed.
 *
 * uint64 rather than size_t deliberately — it is never reset, and a 32-bit
 * host would wrap it in a long session. A NULL out_count is tolerated. */
clay_result clay_voxel_change_count(const clay_voxel_grid* grid, uint64_t* out_count);

/* Inclusive cell bounds of the occupied voxels. *out_has_bounds is 0 for an
 * empty grid, and out_min/out_max are then left alone. */
clay_result clay_voxel_bounds(const clay_voxel_grid* grid, int32_t out_min[3],
                              int32_t out_max[3], int32_t* out_has_bounds);

/* 6-connected flood select from a seed cell, over the seed's palette index
 * when same_color is non-zero or over any occupied cell otherwise, via the
 * size-query pattern: call with out_cells == NULL to receive the cell count
 * in *count, then again with a buffer of count*3 int32 values. *count is the
 * capacity in CELLS going in and the count written coming out; a buffer that
 * is too small gets CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in
 * *count. An empty seed cell selects nothing, which is not an error. The
 * traversal order is deterministic, so both calls agree, and the selection is
 * capped at 1048576 cells. */
clay_result clay_voxel_flood_select(const clay_voxel_grid* grid, const int32_t seed[3],
                                    int32_t same_color, int32_t* out_cells, size_t* count);

/* The grid as a step field for SDF compositing: -voxel_size/2 inside an
 * occupied cell and +voxel_size/2 elsewhere (a bound, not a distance —
 * classify accordingly). points_xyz is count*3 floats, out_values count. */
clay_result clay_voxel_sample_step_field(const clay_voxel_grid* grid, const float* points_xyz,
                                         size_t count, float* out_values);

/* -- meshing and rasterization --------------------------------------------- */

/* Greedy mesh (merged quads per axis slice, palette color per face) behind
 * the same owner handle as clay_document_mesh, freed with clay_mesh_destroy.
 * An empty grid yields an empty mesh rather than an error.
 *
 * This is the WHOLE grid's active level, always, and its output is unchanged
 * by clay_voxel_mesh_chunks existing: the merge still spans chunk boundaries,
 * which is the tighter one and what an export wants. Displaying a sculpt
 * INTERACTIVELY is the other call — this one costs every occupied chunk on
 * every frame (issue #86). */
clay_result clay_voxel_mesh(const clay_voxel_grid* grid, clay_mesh** out_mesh);

/* The same occupancy as a rounded FORM rather than as boxes (#108).
 *
 * Greedy meshing emits axis-aligned quads, which is what greedy meshing is —
 * correct for hard-surface voxel work and for export, and the wrong picture of
 * an organic sculpt. This is surface nets over occupancy sampled at voxel
 * centres: one vertex per surface cell, at the centroid of that cell's edge
 * crossings. The centroid is what smooths, so a corner rounds without anything
 * being filtered first, and nothing VANISHES — a lone voxel has a sign change
 * on each of its six edges and still gets a surface.
 *
 * `blur` is extra smoothing, in passes of a 3x3x3 box over occupancy, and the
 * trade is real in both directions. At 0 nothing is filtered and nothing can
 * be lost, but the surface still TERRACES — every crossing over binary
 * occupancy interpolates to the same midpoint, so corners round and steps
 * remain. At 1 it reads as clay, and an isolated voxel sits near 0.3
 * occupancy, under the isolevel, and is gone; thin features go the same way.
 *
 * So: pass 1 for an organic sculpt, 0 when thin detail matters. The default is
 * 0 because a default that silently deletes a sculptor's detail is the wrong
 * default however good it looks.
 *
 * The setting is an ARGUMENT rather than grid state, so two hosts sharing a
 * document cannot disagree about what it looks like and one host can show both
 * pictures of one sculpt without mutating it. Neither call changes the grid.
 *
 * A PREVIEW mesh: per the surface-nets contract a cell the surface crosses
 * twice gets one vertex and the sheets pinch, so this is neither manifold nor
 * watertight. clay_voxel_mesh remains the export path and is byte-for-byte
 * unaffected by this existing. Colour is per VERTEX and blends — a vertex sits
 * between up to eight voxels and carries the average of the occupied ones,
 * because a smooth surface has no facet to hold one palette entry.
 *
 * An empty grid yields an empty mesh rather than an error, as above. */
clay_result clay_voxel_mesh_smooth(const clay_voxel_grid* grid, int32_t blur,
                                   clay_mesh** out_mesh);

/* The sculpt as QUADS. The descriptor, the two modes, the count contract and
 * the retopology disclaimer are all at clay_quad_params — read them there;
 * this is that call with a grid as the source.
 *
 * A NEW entry point. clay_voxel_mesh, clay_voxel_mesh_smooth and
 * clay_voxel_mesh_chunks return exactly what they returned before and carry no
 * quads.
 *
 * CLAY_QUAD_DUAL at the level's own voxel size with blur 0 is
 * clay_voxel_mesh_smooth's mesh for that level, vertex for vertex and index
 * for index, plus the quads. A COARSER cell low-passes the occupancy and can
 * drop a one-voxel feature entirely, the same trade `blur` makes; a FINER one
 * is clamped to the voxel size, because occupancy is a step field and
 * resampling it finer buys quads and no detail.
 *
 * Free the result with clay_mesh_destroy. An empty grid yields an empty mesh
 * rather than an error, as clay_voxel_mesh does. */
clay_result clay_voxel_mesh_quads(const clay_voxel_grid* grid, const clay_quad_params* params,
                                  clay_mesh** out_mesh);

/* The sculpt back into the document as an OPERAND, in a new layer (#90).
 *
 * The bridge ran one way. SDF to voxel is clay_voxel_rasterize_document; voxel
 * back was a detour — mesh the grid, sample the triangles into a volume, place
 * that — which resamples twice, builds a BVH to do it, drops the palette, and
 * hands back something no longer being sculpted. This is direct: occupancy is
 * read by trilinear interpolation between cell CENTRES, and the result is
 * redistanced so it carries a Lipschitz bound a marcher and a blend can trust.
 *
 * COLOUR SURVIVES by conversion per palette entry. A field has nowhere to put
 * a palette, so this places one volume item per entry the grid carries, each
 * with that entry's colour, unioned without a blend. The union of the parts is
 * the solid; the interface between two colours is interior to it.
 *
 * NON-DESTRUCTIVE. A new layer, and the grid is untouched — so a host offers
 * "go back" by keeping the original, and one misclick cannot cost a
 * parametric model. Nothing about the document format changes: these are
 * ordinary volume items in an ordinary SDF layer.
 *
 * LOSSY, in both directions, and the spec says so rather than implying
 * otherwise. Going to voxels quantised to the lattice and nothing here
 * recovers it — a boolean's sharp edge went to a staircase and comes back as a
 * rounded one. Coming back turns occupancy into a distance. PRESERVED: the
 * surface within about a cell, and the colour. NOT preserved: exactness, and
 * the procedural history — once converted the items are gone and their
 * parameters are no longer editable.
 *
 * `blur` is the smoothing the conversion reads occupancy through, as in
 * clay_voxel_mesh_smooth: 0 keeps thin features, 1 is smoother and loses them.
 *
 * CLAY_ERROR_INVALID_ARGUMENT when the grid holds nothing convertible. */
/* One palette entry of a sculpt as a placeable ITEM, the counterpart to
 * clay_item_volume_from_mesh. `index` 0 converts every occupied cell into one
 * item; a non-zero index converts only that entry's cells and gives the item
 * that entry's colour, which is how a caller assembles a coloured sculpt by
 * hand. clay_voxel_to_layer is this in a loop, into a new layer.
 *
 * Free with clay_item_destroy; placing it copies it, as every item does. */
clay_result clay_item_volume_from_voxels(const clay_voxel_grid* grid, int32_t blur, int32_t index,
                                         clay_item** out_item);

clay_result clay_voxel_to_layer(clay_document* doc, const clay_voxel_grid* grid, const char* name,
                                int32_t blur, clay_layer_id* out_layer);

/* -- incremental display ---------------------------------------------------
 *
 * The voxel side of the refill shape the brick cache already uses: drain the
 * keys, mesh the keys, patch the ranges. A dab dirties a handful of chunks,
 * so re-meshing those costs the dab rather than the model.
 *
 * Both calls act on the grid's ACTIVE level, as every other cell-addressed
 * call here does; the dirty set is per level, and a level's set describes that
 * level whether or not it was active when the edit landed.
 *
 * A drain that takes more than one call is bound to the level that was active
 * when it BEGAN: the remaining keys are staged, and changing the active level
 * partway through hands back the rest of the old level's keys, while
 * *out_remaining counts those staged keys plus the new level's dirty ones.
 * Finish a drain before changing level — a multi-resolution host that
 * interleaves the two gets a coherent answer for neither. */

/* Drains the chunks whose meshed surface a mutation could have changed, as
 * packed int32 triples — the spelling clay_voxel_flood_select and
 * clay_brick_cache_surface_bricks use.
 *
 * Capacity in, count out — the clay_brick_cache_take_dirty shape, not a size
 * query: *count is the caller's buffer capacity in KEYS going in and the
 * number written coming out, and *out_remaining (may be NULL) is how many are
 * still queued afterwards. out_keys_xyz == NULL is CLAY_ERROR_INVALID_ARGUMENT
 * and never a size query, so there is no BUFFER_TOO_SMALL retry loop. Call it
 * in a loop until *out_remaining is 0, or bound the work per frame by stopping
 * early — the rest stay queued. Keys come back in a deterministic order
 * (lexicographic by x, then y, then z), so a replay of the same edits drains
 * the same list.
 *
 * As with the brick-cache drain, the engine's drain is all-or-nothing, so the
 * first call after a large edit stages every queued key inside the library at
 * 12 bytes each and holds the staging until the queue is fully drained. A
 * write that lands while keys are still staged is queued behind them, so a key
 * may be reported twice across two drains — re-meshing a chunk that did not
 * change is wasted work, never a wrong surface.
 *
 * Every public mutation reports through this: a write that CHANGES a cell
 * dirties its chunk, and one on a chunk face also dirties the chunk across
 * that face, whose exposed faces it changed. A chunk emptied to nothing is
 * dropped from the grid and STILL reported, because that is the key whose
 * quads a host has to remove. A write that changes nothing dirties nothing.
 *
 * A grid that was just created from a file, rasterized, or given a level
 * reports every chunk it wrote, so a first full display and an incremental one
 * are the same code path. clay_voxel_mesh neither reads the set nor clears
 * it. */
clay_result clay_voxel_take_dirty_chunks(clay_voxel_grid* grid, int32_t* out_keys_xyz,
                                         size_t* count, size_t* out_remaining);

/* What one chunk key contributed to a mesh. An array ELEMENT, not a versioned
 * descriptor: a caller receives one per key and thousands at a time, its
 * layout is fixed, and changing that layout is a break rather than something
 * to negotiate — the same reasoning clay_brick_mesh_range carries.
 *
 * Unlike clay_brick_mesh_range, these ranges PARTITION the mesh with no vertex
 * shared between two keys, so a host may overwrite OR DROP one key's slice
 * without consulting its neighbours'. A voxel face belongs to exactly one cell
 * in exactly one chunk, so there is nothing to weld across a seam and no
 * straddler to attribute. */
typedef struct clay_voxel_chunk_mesh_range {
    int32_t key[3];
    uint32_t vertex_first, vertex_count;
    uint32_t index_first, index_count;
} clay_voxel_chunk_mesh_range;

/* Meshes ONLY the named chunks — hand it what clay_voxel_take_dirty_chunks
 * reported and a re-mesh costs the dab rather than the model.
 *
 * keys_xyz is key_count packed int32 triples. The exposure test still reads
 * the neighbour cell wherever it lives, including in a chunk that was not
 * named and in one that does not exist, so the SURFACE is exactly the one
 * clay_voxel_mesh describes over the same chunks: the same exposed faces, the
 * same colours, the same covered area.
 *
 * What differs is the merge. Greedy quads are axis-aligned and exact, so
 * clamping the merge to a chunk boundary emits MORE, SMALLER quads over the
 * identical surface — never a crack. That is why this needs no straddler
 * attribution, unlike clay_brick_cache_mesh, whose marching cells straddle the
 * brick boundary. The cost is triangle count at chunk seams and nothing else
 * (measured at a few percent on a realistic sculpt), which is why
 * clay_voxel_mesh stays whole-grid for export.
 *
 * out_ranges (may be NULL) receives key_count clay_voxel_chunk_mesh_range
 * values in the order the keys were given, and REQUIRES keys_xyz: with no key
 * list there is no count the caller could have sized the buffer from, which is
 * the same combination clay_brick_cache_mesh refuses. A key naming a chunk the
 * grid does not hold contributes nothing and is NOT an error — a drained set
 * routinely names a chunk a stroke emptied, and that is precisely the key
 * whose geometry the host must drop; its range is zero-length and sits where
 * its geometry would have begun. A key repeated in the list is meshed once per
 * occurrence.
 *
 * Behind the same owner handle as clay_document_mesh and freed with
 * clay_mesh_destroy. No keys yields an EMPTY mesh rather than an error, as
 * clay_voxel_mesh does for an empty grid: a frame in which nothing changed is
 * an ordinary state of a session. */
clay_result clay_voxel_mesh_chunks(const clay_voxel_grid* grid, const int32_t* keys_xyz,
                                   size_t key_count, clay_voxel_chunk_mesh_range* out_ranges,
                                   clay_mesh** out_mesh);

/* Rasterize a document's SDF content into the grid: cells whose centre lands
 * inside are set, colored from the field through the nearest palette entry
 * (added as needed). region_min/region_max bound the work in world space and
 * are both NULL to use the document's own bounds; passing one without the
 * other is rejected, as is a region that is empty, unbounded, or carries any
 * non-finite bound — an infinity or a NaN, which is what a region derived
 * from a camera frustum or a degenerate selection box arrives as. A rejected
 * call leaves the grid exactly as it was. */
clay_result clay_voxel_rasterize(clay_voxel_grid* grid, const clay_document* doc,
                                 const float region_min[3], const float region_max[3]);

/* The same conversion from TRIANGLES, in ONE sampling.
 *
 * An imported model could already reach an SDF layer in one step
 * (clay_item_volume_from_mesh) but reached a grid only through a document:
 * triangles to a narrow band, band into a layer, layer rasterized. Each of
 * those places the surface within about half a cell of its own lattice, so the
 * detour quantised a field that was itself quantised, and a feature that
 * survived the first sampling could fall between centres on the second.
 *
 * Membership is the GENERALIZED WINDING NUMBER at the cell centre — the sign
 * that survives a hole, a flipped normal and a self-intersection, because those
 * are what imported meshes have. A model with a missing cap rasterizes without
 * flipping a half-space.
 *
 * Colour comes from the mesh's vertex colours where it has them, interpolated
 * at the closest point on the nearest triangle and quantised to the palette by
 * nearest entry, exactly as clay_voxel_rasterize quantises the tape's colour
 * field. A mesh with no colours takes one neutral entry.
 *
 * The REGION IS OPTIONAL here, unlike clay_voxel_rasterize: a document can be
 * unbounded and a mesh cannot, so NULL means the mesh's own bounds. Passing one
 * bounds the work; content outside it is not rasterized and is not reported as
 * missing. Both pointers or neither, as elsewhere in this ABI.
 *
 * What the sampling costs is the same statement clay_voxel_rasterize carries:
 * the surface moves by up to half a cell, a feature thinner than a cell can
 * vanish (rasterize finer — nothing downstream can invent what was never
 * stored), a sharp edge staircases at the cell size, and two colours closer
 * than the palette tolerance become one.
 *
 * NOT retopology and not remeshing: occupancy sampling, nothing more. The mesh
 * is not modified, and a mesh a document CARRIES stays never-evaluated — this
 * is an explicit conversion a caller asks for, like every bridge. */
clay_result clay_voxel_rasterize_mesh(clay_voxel_grid* grid, const clay_mesh* mesh,
                                      const float region_min[3], const float region_max[3]);

/* -- voxel picking --------------------------------------------------------- */

/* The cube face a picking ray entered a cell through. The neighbour across it
 * is where a voxel placed by that click goes, which is what
 * clay_voxel_raycast's out_adjacent already holds. */
typedef enum clay_voxel_face {
    CLAY_VOXEL_FACE_POS_X = 0,
    CLAY_VOXEL_FACE_NEG_X = 1,
    CLAY_VOXEL_FACE_POS_Y = 2,
    CLAY_VOXEL_FACE_NEG_Y = 3,
    CLAY_VOXEL_FACE_POS_Z = 4,
    CLAY_VOXEL_FACE_NEG_Z = 5
} clay_voxel_face;

/* Cell picking by ray (origin plus a direction normalized here). *out_hit is
 * 0/1 and the rest is written only on a hit: out_cell is the first occupied
 * cell along the ray, *out_face the clay_voxel_face it was entered through,
 * out_adjacent the neighbour across that face, and *out_t the world distance
 * to the entry point. Every out pointer but out_hit may be NULL. */
clay_result clay_voxel_raycast(const clay_voxel_grid* grid, const float origin[3],
                               const float dir[3], int32_t* out_hit, int32_t out_cell[3],
                               int32_t* out_face, int32_t out_adjacent[3], float* out_t);

/* The cell under the ray on the build plane y = plane_cell — where a voxel
 * goes when the ray hits none. Build planes are Y-normal, so the returned
 * cell's y is always plane_cell. *out_hit is 0 when the ray runs parallel to
 * the plane or the plane lies behind its origin, which is not an error. */
clay_result clay_voxel_build_plane_pick(const clay_voxel_grid* grid, const float origin[3],
                                        const float dir[3], int32_t plane_cell,
                                        int32_t* out_hit, int32_t out_cell[3]);

/* -- influence bounds ------------------------------------------------------ */

/* The world-space box outside which an item cannot change the field.
 *
 * NOT clay_layer_bounds, which is deliberately tight — the box to frame a
 * camera on. This is the box to DIRTY, and it differs in exactly three ways,
 * which is what the engine actually does (src/scene/bounds.cpp):
 *   - a GROUP's bound is dilated by the group's own blend support, so a child
 *     blended into its siblings does not leave a stale seam;
 *   - an INVISIBLE node contributes nothing;
 *   - some nodes have NO finite influence and say so, rather than claiming
 *     one: a non-local op (intersect, the spatial morphs) anywhere in the
 *     subtree, an infinite grid repeat, or an unbounded primitive (a plane, an
 *     infinite cylinder).
 * A plain LOCAL item's influence bound IS its geometry bound — already
 * dilated by rounding and blend support, as every bound here is. Nothing is
 * dilated by a deformer chain's Lipschitz factor; the engine does not do that.
 *
 * Three states through two flags, spelled as clay_layer_bounds spells them:
 *   *out_has_bounds 0            nothing to dirty; out_min/out_max untouched
 *   1, *out_infinite 0           the finite box in out_min/out_max
 *   1, *out_infinite 1           unbounded; out_min/out_max are left alone,
 *                                and the honest response is to dirty
 *                                everything, which is what passing NULL to
 *                                clay_brick_cache_mark_dirty means
 * A node the layer does not hold, or one that is hidden, reports no bounds
 * rather than failing: a selection outlives the nodes in it. A layer the
 * document does not hold is CLAY_ERROR_NOT_FOUND. */
/* NOTE (0.53.0, issue #325): this covers every layer sharing the node's
 * content, not only `layer`. Instancing a layer shares one edit list between
 * layers with different transforms, so a node is compiled once per instancing
 * layer and an edit moves every copy; reporting one copy's box left a host
 * dirtying by it with the others stale, measured at 0.103 outside the box
 * against a band of 0.15. clay_brick_cache_mark_dirty_nodes dirties by the same
 * union, so what a host is told and what it dirties cannot disagree. On a
 * document with no instancing the answer is unchanged. */
clay_result clay_layer_node_influence_bound(const clay_document* doc, clay_layer_id layer,
                                            clay_node_id node, float out_min[3],
                                            float out_max[3], int32_t* out_has_bounds,
                                            int32_t* out_infinite);
/* The union over a layer's root nodes — what a first, full fill dirties. A
 * layer that shows nothing reports *out_has_bounds 0. */
clay_result clay_layer_influence_bound(const clay_document* doc, clay_layer_id layer,
                                       float out_min[3], float out_max[3],
                                       int32_t* out_has_bounds, int32_t* out_infinite);

/* -- dense grid evaluation ------------------------------------------------- */

/* nx*ny*nz lattice samples at origin + spacing * (i, j, k), x fastest — the
 * shape every backend already implements (eval::GridQuery) and the one thing
 * this ABI could not ask for. clay_eval_points can sample the same points,
 * but only as an unstructured batch against a tape compiled for the whole
 * document; a grid carries its own region, so the tape can be CULLED to it,
 * and that culling is where the brick cache's measured win lives. */
typedef struct clay_grid_query {
    uint32_t struct_size; /* = sizeof(clay_grid_query); required */
    float origin[3];      /* world position of sample (0, 0, 0) */
    float spacing;        /* world units between samples; must be finite and > 0 */
    int32_t dims[3];      /* nx, ny, nz; each > 0, product <= CLAY_MAX_BATCH */
} clay_grid_query;

/* Evaluates the grid into out_values (dims product floats, x fastest), with
 * out_colors_rgb (three times that) optional as it is on clay_eval_points.
 * backend NULL = "cpu".
 *
 * region_min/region_max are an optional CULL region: items whose influence
 * cannot reach it are dropped from the compiled tape, so a small grid in a
 * large scene costs what the grid covers rather than what the document holds.
 * Both NULL compiles the whole document. Passing one without the other is
 * rejected, as it is on clay_voxel_rasterize, as is a region that is empty or
 * carries a non-finite bound. The culled tape is compiled per call and not
 * cached: consecutive bricks want different regions, so a cache keyed on the
 * document alone would thrash.
 *
 * value_count is the caller's buffer length in FLOATS and must equal the dims
 * product exactly — the one argument this ABI cannot check against the
 * caller's memory, so it is required rather than inferred. */
clay_result clay_eval_grid(const clay_document* doc, const char* backend,
                           const clay_grid_query* grid, const float region_min[3],
                           const float region_max[3], float* out_values,
                           float* out_colors_rgb, size_t value_count);

/* -- the host parity fixture ----------------------------------------------- */

/* The fixture a host preview runs to PROVE it evaluates the same field
 * claycore bakes: a table of composed tapes, probe points, and this library's
 * own reference distance and colour at each probe, as JSON.
 *
 * `clay parity-fixture` writes the same bytes, and until now that CLI was the
 * only way to get them — which is no use to an app whose tests link the
 * framework rather than shell out to a tool that is not in the bundle. This is
 * that gate, reachable from a test target: generate it, evaluate the same tapes
 * with your own shader, and assert agreement within the tolerances the JSON
 * carries.
 *
 * The case table is chosen for what a hand-written preview gets WRONG rather
 * than for coverage: every blend profile against every smooth boolean, every
 * extended combine mode, the material-mix weights, a deformer chain,
 * repetition, the out-of-line blob, and a composed document. The blend cases
 * are probed across the seam, so a support-k quadratic smin where the engine
 * uses 4k fails here rather than at bake time — which is the drift that started
 * all of this.
 *
 * Size-query pattern, exactly as clay_list_backends: call with buffer == NULL
 * for the required size including the NUL, then again with a buffer that large.
 * It is deterministic — no clock, no RNG beyond a fixed seed — so two calls in
 * one build produce identical bytes and a host can diff them.
 *
 * It is a few hundred kilobytes and builds the whole table on each call. That
 * is a test-time cost and this is a test-time entry point; do not call it per
 * frame. */
clay_result clay_parity_fixture_json(char* buffer, size_t* size);

/* -- device interop -------------------------------------------------------- */

/* Backends create and own their devices, which is right for a headless library
 * and wrong for a host that was going to draw on a GPU anyway: results computed
 * on our device are copied to host memory and uploaded again to the device the
 * host draws from. On macOS both claycore and a wgpu host are on Metal, and on
 * Linux both are on Vulkan — the same buffer could serve both sides.
 *
 * These calls let you lend claycore the device you already have.
 *
 * WHAT THIS DOES AND DOES NOT DO. It makes evaluation OUTPUT device-resident.
 * It does NOT make the brick CACHE device-resident: generations, staleness,
 * band classification, fp16 quantization and the memory budget are host code
 * over host memory, and that is where a submitted brick becomes a stored brick.
 * So a host taking this path has its bricks computed straight into its own
 * buffer and then owns quantizing and uploading them — at which point the cache
 * is not in the loop and neither are its guarantees. If you want the cache's
 * correctness, use clay_brick_cache_read_bricks; if you want no host copy, use
 * this. Both are complete paths; neither is both.
 *
 * NO VENDOR HEADER REACHES THIS ONE. Native objects cross as void*, positioned
 * per API. A header that included vulkan.h would break every bindings generator
 * that reads this one, and would make the surface it declares depend on how the
 * library was built. */

typedef enum clay_device_api {
    CLAY_DEVICE_API_METAL = 0,
    CLAY_DEVICE_API_VULKAN = 1,
    CLAY_DEVICE_API_CUDA = 2
} clay_device_api;

/* Which handle goes in which slot, by API. Unused slots are ignored and should
 * be NULL:
 *
 *   METAL   0 id<MTLDevice>   1 id<MTLCommandQueue>
 *   VULKAN  0 VkInstance      1 VkPhysicalDevice   2 VkDevice   3 VkQueue
 *           plus queue_family, which MUST support compute
 *   CUDA    0 CUcontext       1 CUstream  — DECLARED BUT NOT IMPLEMENTED: the
 *           CUDA backend has no adoption path, so clay_device_adopt refuses
 *           this API. Said plainly here rather than left to be discovered:
 *           the enumerator exists so the layout does not shift when it lands.
 *
 * A fixed void*[6] rather than a union, because a union of vendor types is a
 * vendor header and a union of void* is this with worse ergonomics. */
typedef struct clay_device_desc {
    uint32_t struct_size; /* = sizeof(clay_device_desc); required */
    int32_t api;          /* clay_device_api */
    void* handles[6];
    uint32_t queue_family; /* Vulkan only; ignored elsewhere */
} clay_device_desc;

typedef struct clay_device clay_device; /* opaque */

/* Adopts the caller's device. Returns NULL — with the detail in
 * clay_last_error() — when the named API's backend is not compiled in, when the
 * handles are incomplete for that API, or when the backend has no adoption
 * path. That is a capability report, not a failure: a caller whose adoption is
 * refused falls back to the ordinary backend-name calls and gets IDENTICAL
 * values. Adoption changes where work runs, never what it computes.
 *
 * OWNERSHIP. The library retains nothing it did not create and destroys nothing
 * it did not make: your device, queue and instance are yours, and releasing the
 * clay_device leaves them alone. The library creates, destroys and waits on no
 * synchronization primitive of yours, and submits to your queue only inside a
 * call you made. Work issued during a call has COMPLETED when that call
 * returns, so nothing is left in flight with no way to know when it lands.
 *
 * THREADING. Calls on one clay_device are yours to serialize, exactly as they
 * are for clay_brick_cache. A GPU queue is not free-threaded and adding a lock
 * here would be a threading policy you did not ask for.
 *
 * The device must outlive the clay_device. Releasing yours first and then
 * calling through this handle is a use-after-free this ABI cannot detect. */
clay_device* clay_device_adopt(const clay_device_desc* desc);
void clay_device_release(clay_device* device);

/* Which backend the handle resolved to ("vulkan", "metal", ...), borrowed and
 * valid until release, so a host can log or display what it actually got. */
const char* clay_device_backend_name(const clay_device* device);

/* A slice of a buffer YOU own and keep owning. `handle` is a VkBuffer /
 * MTLBuffer / CUdeviceptr per the device's API. `size` is what is available
 * from `offset`, is REQUIRED, and is checked against the lattice: a destination
 * too small is refused with nothing written, as everywhere else here. */
typedef struct clay_device_buffer {
    uint32_t struct_size; /* = sizeof(clay_device_buffer); required */
    void* handle;
    uint64_t offset; /* bytes */
    uint64_t size;   /* bytes available from offset */
} clay_device_buffer;

/* clay_eval_grid with the destination on the device. Same lattice, same cull
 * region rules, same x-fastest order, same float32 elements — only where the
 * results land differs, deliberately, so you can A/B the two and get
 * bit-identical values.
 *
 * out_colors_rgb may be NULL. Values stay float32 and are NOT quantized on the
 * device even though the brick cache stores fp16: quantization and band
 * classification belong to clay_brick_cache_submit, and a device path that did
 * them would be a second implementation of the step most able to drift. */
clay_result clay_eval_grid_device(const clay_document* doc, clay_device* device,
                                  const clay_grid_query* grid, const float region_min[3],
                                  const float region_max[3],
                                  const clay_device_buffer* out_values,
                                  const clay_device_buffer* out_colors_rgb);


/* -- the compiled tape ----------------------------------------------------- */

/* What `docs/06-host-gpu-previews.md` calls route 1: evaluate the document's
 * own field in your own shader, with OUR kernels rather than a copy of them.
 * The published kernel package (`dist/claycore-kernels/`, and in the
 * xcframework under `Headers/clay/kernel/`) gives you `ctape_eval`; this gives
 * you the three buffers to feed it.
 *
 *     ctape_eval(instrs, instr_count, params, blob, p)
 *
 * Without this a host drawing its own frames round-trips PIXELS through the
 * library every frame — computed in the engine, copied to host memory, then
 * uploaded to the GPU it was going to draw on. The tape is a few kilobytes and
 * changes once per edit; the pixels change 60 times a second.
 *
 * If you only want to draw the SURFACE, prefer the brick atlas below: it needs
 * no shader kernels at all, works in shading languages our dialect does not
 * target, and cannot drift for the same reason. Use the tape when you need the
 * field between the samples or away from the surface. */

/* An immutable SNAPSHOT of a compiled tape, which the caller releases.
 *
 * Editing the document CANNOT invalidate an export. The document holds its
 * compiled tape as a shared, const, revision-keyed object and installs a new
 * one on an edit rather than mutating the old, so an export is a reference to
 * the tape as it was — no invalidation callback, no revision to check before
 * dereferencing, no window in which a pointer you hold goes bad. That is why
 * the buffers below can be borrowed pointers rather than a copy into memory you
 * supplied: the lifetime rule is "an export is a snapshot", and the cost of the
 * export itself is a refcount. */
typedef struct clay_tape clay_tape; /* opaque */

/* Exactly kernel::CTapeInstr, asserted field by field with offsetof in
 * bindings/c/clay_c.cpp, because your evaluator is compiled from the header
 * that declares it. An array ELEMENT, not a versioned descriptor. */
typedef struct clay_tape_instr {
    uint32_t op;
    uint32_t param_offset;
} clay_tape_instr;

/* Exports the document's compiled tape. region_min/region_max are the same
 * optional CULL region clay_eval_grid takes, with the same rules: both NULL
 * compiles the whole document, one without the other is rejected, and an empty
 * or non-finite region is rejected. A host streaming a region wants the cull
 * the brick cache uses, so it is here rather than in a second entry point.
 *
 * COST DIFFERS between the two. With no region this is the document's cached
 * tape and the export is a refcount increment. WITH a region it COMPILES — a
 * culled tape is deliberately not cached, because consecutive bricks want
 * different regions and a cache keyed on the document alone would thrash. One
 * cull per brick per frame is an expensive thing to write by accident.
 *
 * Free with clay_tape_release. */
clay_result clay_tape_export(const clay_document* doc, const float region_min[3],
                             const float region_max[3], clay_tape** out_tape);
void clay_tape_release(clay_tape* tape);

/* The version of the tape ENCODING, which is the claycore version the kernel
 * package records in its VERSION file. The two are one number because they only
 * work together: your evaluator is `ctape_eval` from that package's headers, so
 * an opcode added on one side and absent on the other is a wrong answer rather
 * than a link error.
 *
 * The library cannot make this check — it does not know which package you
 * compiled — so publishing the number is its half and comparing it is yours.
 * On a mismatch, REFUSE. Do not reinterpret. */
uint32_t clay_tape_encoding_version(void);

/* Borrowed, valid until clay_tape_release, and unaffected by any edit. Each
 * writes its element count to *out_count, which may not be NULL. A tape with
 * no out-of-line payload has a blob count of 0 and may return NULL for it. */
const clay_tape_instr* clay_tape_instrs(const clay_tape* tape, size_t* out_count);
const float* clay_tape_params(const clay_tape* tape, size_t* out_count);
const float* clay_tape_blob(const clay_tape* tape, size_t* out_count);

/* What the three buffers cannot tell an evaluator, and what a host that
 * guesses gets wrong. Every out pointer is optional.
 *
 * out_safe_step_scale is what a sphere tracer multiplies its step by. It is
 * 1 / max(lipschitz, 1) and `csafe_step_scale` in the published headers
 * computes it — it is returned anyway because "a host that guesses its step
 * scale draws a wrong frame" and a host recomputing a one-line formula is a
 * host that can get it wrong. Four bytes to remove the question.
 *
 * out_bounds_* is the union of item influence bounds: what to clip against. A
 * host that guesses these draws a slow frame instead of a wrong one.
 *
 * out_revision is the document revision the tape was compiled at, so telling
 * whether the copy you uploaded is still current is an integer comparison
 * rather than a comparison of buffers. It is opaque and only equality is
 * meaningful.
 *
 * NOTE on the blob: sampled volumes ride in it, so a document carrying one has
 * a blob that is megabytes rather than kilobytes, and this export publishes the
 * whole tape with no delta encoding. Compare out_revision AND the blob count
 * between edits: a stroke that touches no volume leaves the blob alone and you
 * can skip the re-upload. A finer answer needs to know WHICH region of a
 * sampled volume an edit changed, which is a property of the edit rather than
 * of the tape, and belongs with add-multi-resolution and
 * add-consolidation-policy rather than here. */
clay_result clay_tape_info(const clay_tape* tape, int32_t* out_is_exact, float* out_lipschitz,
                           float* out_safe_step_scale, float out_bounds_min[3],
                           float out_bounds_max[3], uint64_t* out_revision);

/* -- the brick cache ------------------------------------------------------- */

/* What makes sculpting INCREMENTAL. The field is kept as a sparse grid of
 * dim^3 fp16 bricks in a narrow band around the surface; an edit dirties the
 * bricks its influence bound reaches and only those are re-evaluated. Every
 * other brick is left BIT-IDENTICAL, which is the property the whole design
 * exists to provide.
 *
 * The cache evaluates nothing and owns no thread. It hands out plain-data
 * REQUESTS — a brick key, a generation, and the lattice to sample — and takes
 * the resulting distances back. Which backend runs them, on which thread, in
 * what order, and how many per frame is the host's business. There is exactly
 * one path:
 *
 *     mark_dirty -> take_dirty -> eval_requests -> submit
 *
 * The generation is what makes an in-flight request safe. Re-dirtying a brick
 * bumps its generation, so a result computed against the older scene is
 * REJECTED at submit rather than overwriting newer state. That is an ordinary
 * outcome of an interactive session, not a failure, so it arrives through
 * out_results and the call still returns CLAY_OK — the same choice
 * clay_document_undo makes for "nothing to undo".
 *
 * THREADING. A cache handle takes no lock and adds none: the C++ class is a
 * hash map and a vector with no synchronization, and a lock here would be a
 * threading policy the consumer did not ask for. So every call on ONE handle
 * must be serialized by the host, const readers included — a concurrent
 * submit may be rehashing the map a reader is walking. Two handles share
 * nothing. clay_brick_cache_eval_requests takes no cache at all and is
 * free-threaded: it may run on any number of threads against one const
 * document, which is safe for the same reason clay_eval_points is, and is NOT
 * safe concurrently with a mutating clay_document_* / clay_layer_* call.
 *
 * The cache knows nothing about a document, and no edit invalidates anything
 * in it. A host that edits and does not mark the edit's influence bound dirty
 * keeps stale bricks, and nothing here can detect that: prefer
 * clay_brick_cache_mark_dirty_nodes, which computes the bound itself. */

typedef struct clay_brick_cache clay_brick_cache; /* opaque */

/* What a brick holds. Inside and Outside are implicit — the state IS the
 * data, no lattice is allocated — which is why empty space costs nothing. */
typedef enum clay_brick_state {
    CLAY_BRICK_INSIDE = 0,  /* uniformly d <= -band */
    CLAY_BRICK_OUTSIDE = 1, /* uniformly d >= +band */
    CLAY_BRICK_SURFACE = 2, /* the band crosses it: dim^3 fp16 samples */
    /* This boundary's own value, with no engine counterpart (as clay_mesher
     * has none): the cache holds nothing for the key — it was never marked, or
     * was marked and never submitted. clay_brick_cache_read_bricks reports it
     * and leaves that brick's slice of the output untouched. */
    CLAY_BRICK_MISSING = 3
} clay_brick_state;

/* What became of a submission. Only ACCEPTED changed the cache. */
typedef enum clay_brick_submit {
    CLAY_BRICK_SUBMIT_ACCEPTED = 0,
    /* The brick was re-dirtied while this request was in flight, or the cache
     * no longer tracks it. Expected, and not an error: drop the values and
     * wait for the request the next clay_brick_cache_take_dirty hands you. */
    CLAY_BRICK_SUBMIT_STALE = 1,
    /* Storing this brick would put the cache over its memory budget. Every
     * brick already stored stays valid and sampleable, and the ceiling is
     * never breached; the brick simply stays unevaluated. */
    CLAY_BRICK_SUBMIT_BUDGET_EXCEEDED = 2
} clay_brick_submit;

/* Every field belongs to the original layout and none has a default: a
 * struct_size shorter than this is rejected outright, as it is for
 * clay_brush_params. Zero is not a resolution and not a voxel size, so a
 * zeroed descriptor is a mistake rather than a request — start from
 * clay_brick_config_defaults and override what you care about. */
typedef struct clay_brick_config {
    uint32_t struct_size; /* = sizeof(clay_brick_config); required */
    int32_t dim;          /* lattice samples per brick axis; 8 or 16 */
    float voxel_size;     /* world units between lattice samples; finite and > 0 */
    int32_t band_voxels;  /* half-width of the kept band, in voxels; > 0 */
    /* Bytes of SURFACE-brick payload the cache may hold; 0 is unlimited,
     * exactly as brick::BrickConfig has it. It bounds the fp16 lattices and
     * nothing else: the per-brick bookkeeping grows with how much space has
     * ever been marked dirty, which clay_brick_stats.tracked_bricks reports
     * and this does not cover. */
    uint64_t memory_budget;
    /* 0/1: carry an RGBA8 colour lattice beside the distances, so a host can
     * upload a colour atlas without meshing. Appended after the original
     * layout, so a caller compiled against the older struct keeps the
     * distance-only cache it already had.
     *
     * Chosen HERE and not per call, because a colour lattice has to be
     * evaluated to exist: a per-call flag would let a host ask a distance-only
     * cache for colours it never computed, and the only truthful answer would
     * be an error for a mistake made three calls earlier. When this is set,
     * clay_brick_cache_submit REQUIRES colours and clay_brick_cache_read_bricks
     * can return them; when it is not, both refuse to deal in colour at all.
     *
     * It costs two more bytes per sample than the fp16 distance, inside the
     * SAME memory_budget — so a colour cache holds about a third of the bricks
     * a distance-only one holds at the same ceiling. That is why it is opt-in
     * rather than the default. */
    int32_t colors;
} clay_brick_config;

/* Fills a descriptor with the engine's defaults: 8^3 bricks, 0.05 world units
 * per voxel, a 3-voxel band, no budget, no colour.
 *
 * SET struct_size BEFORE CALLING. Changed in ABI 0.35.0 — see
 * clay_stroke_preset_defaults. This is the entry point that made the change
 * necessary: `colors` was appended to clay_brick_config, so every host built
 * against the 24-byte layout had 8 bytes written past the end of its struct. */
clay_result clay_brick_config_defaults(clay_brick_config* out_config);

/* Everything a host needs to draw a progress bar or decide whether to raise
 * the budget, in one versioned descriptor so a counter can be appended later
 * without a new entry point. Reading it walks the tracked bricks to count the
 * surface ones, so it is O(tracked) rather than free — once a frame, not once
 * a brick. */
typedef struct clay_brick_stats {
    uint32_t struct_size;    /* = sizeof(clay_brick_stats); required */
    uint64_t tracked_bricks; /* keys the cache holds bookkeeping for */
    uint64_t surface_bricks; /* of those, the ones storing an fp16 lattice */
    uint64_t dirty_bricks;   /* waiting to be handed out, plus any drained but
                              * not yet taken by clay_brick_cache_take_dirty */
    uint64_t memory_usage;   /* bytes of surface-brick payload held */
    uint64_t memory_budget;  /* what the cache was created with; 0 = unlimited */
    /* What the per-key bookkeeping costs — the growth memory_usage does NOT
     * count, because it bounds only the fp16 payloads. Reported as its own
     * number rather than folded into memory_usage: that one is compared against
     * a budget a host already configured, and making it grow by a term it never
     * included would change the meaning of a number in the field rather than
     * report a new fact. Reclaim it with clay_brick_cache_forget_empty. */
    uint64_t bookkeeping_bytes;
    /* What ONE surface brick's payload costs, so a host can answer "will the
     * batch I am about to evaluate fit" with arithmetic it owns:
     *
     *     memory_usage + pending * brick_bytes <= memory_budget
     *
     * A NUMBER rather than a would_it_fit() predicate, deliberately. A
     * predicate's answer is true only until the next submit, which in a
     * threaded host is immediately — so it would imply a guarantee the cache
     * cannot make. The arithmetic is honest about being a snapshot.
     *
     * Worth having because the expensive part of a refusal is not the refusal:
     * it is the evaluation already spent on a brick that gets dropped. A host
     * that checks first trims BEFORE evaluating rather than after. */
    uint64_t brick_bytes;
} clay_brick_stats;

/* -- is the refill actually resuming? (ABI 0.55.0, issue #342) --------------
 *
 * clay_brick_cache_eval_requests keeps each brick's float32 result as a SEED,
 * and a later refill of that brick evaluates only what the document gained
 * since — bit-identical to walking the whole edit list, and cheap where that is
 * not (#306).
 *
 * Bit-identical is the problem this descriptor solves. Nothing about a refill's
 * output can tell you whether the fast path fired, so a host whose refill loop
 * stopped hitting it sees correct bricks and a frame time that quietly follows
 * the size of the sculpt. These counters are how you see it instead:
 *
 *     resumed_bricks / (resumed_bricks + refilled_bricks)
 *
 * over a stroke. A stroke that is warm and appending should sit high. Bricks
 * the stroke has just grown into have no seed and are counted as refilled,
 * correctly — a moving brush never reaches 1.0, and does not need to. A brick
 * the refill PROVES uniform rather than walks (clay_brick_cache_eval_requests)
 * counts the same way: refilled where the proof is made, resumed where a later
 * dab carries it forward, so the ratio reads as it always did.
 *
 * Both counts are CUMULATIVE over the document's life and never reset, so read
 * them as differences across the interval you care about. A seed is a pure
 * performance cache: dropping every one of them changes no geometry, only the
 * time it takes to produce it.
 *
 * THE SEED STORE BELONGS TO THE DOCUMENT, not to a cache and not to a thread.
 * Two caches over one document share it, and a refill on ONE thread leaves
 * seeds that a refill on ANOTHER can resume from. That is what makes the
 * expensive case schedulable.
 *
 * WARM A COLD WINDOW OFF THE INTERACTION THREAD. A brick with no seed pays the
 * whole surviving edit list, and that cost follows the size of the sculpt: a
 * 12-brick window over a 50,000-item document measures ~108 ms, against ~0.16
 * ms for a dab whose bricks are all warm. It is the tail rather than the median
 * that hurts — a stroke's median dab is flat in document size and its worst dab
 * is not, because a moving brush keeps reaching ground it has not covered.
 *
 * Because clay_brick_cache_eval_requests is free-threaded against a const
 * document and the seeds are the document's, a host can pay that on a worker
 * and hand the frame a warm window. Measured on the same 50,000-item document:
 * the interaction thread pays 107.99 ms with no warming and 0.004 ms after a
 * worker refilled the same requests, bit-identically.
 *
 * WHAT THAT COSTS YOU IS AN EDIT. The warming refill is only safe while the
 * document is not being mutated, so it belongs at pointer-DOWN, between
 * strokes, or on a camera move — not between the dabs of a live stroke, where
 * every dab is a mutating call. A host that warms the neighbourhood the brush
 * is about to enter pays for it once, before the stroke that needs it. */
typedef struct clay_resume_stats {
    uint32_t struct_size;      /* = sizeof(clay_resume_stats); required */
    uint64_t entries;          /* bricks currently holding a seed */
    uint64_t bytes;            /* what those seeds cost */
    uint64_t budget;           /* the ceiling they are evicted against */
    uint64_t resumed_bricks;   /* cumulative: bricks answered from a seed */
    uint64_t refilled_bricks;  /* cumulative: bricks that took the full walk */
} clay_resume_stats;

clay_result clay_document_resume_stats(const clay_document* doc, clay_resume_stats* out_stats);

/* What computing an INTERSECT's bound has cost this document (issue #451).
 *
 * An intersect is bounded by its layer's EXTENT, and computing that extent
 * walks every visible node taking its geometry bound. Before ABI 0.74.0 an
 * intersect answered "everything" in constant time, so nothing cared; after it,
 * every edit pays, and a host whose drag got slower had no way to see why.
 *
 * `walks` is how many of those walks ran. `reuses` is how many were answered
 * from the previous edit's memo instead -- an edit takes the bound on BOTH
 * sides, so a drag would otherwise pay two a frame and this is what makes it
 * one.
 *
 * Both are cumulative and diagnostic. A document with no intersect in any layer
 * reports zeroes forever: nothing else needs a layer's extent. */
typedef struct clay_extent_stats {
    uint32_t struct_size; /* = sizeof(clay_extent_stats); required */
    uint64_t walks;       /* layer walks performed */
    uint64_t reuses;      /* bounds answered from the memo instead */
} clay_extent_stats;

clay_result clay_document_extent_stats(const clay_document* doc, clay_extent_stats* out_stats);

/* One evaluation request, and the only thing that crosses between the cache
 * and the host's evaluator.
 *
 * An array ELEMENT, not a versioned descriptor: it carries no struct_size for
 * the same reason clay_stamp does not — a caller receives thousands at once,
 * its layout is fixed, and changing that layout is a break rather than
 * something to negotiate. The layout is byte-for-byte brick::BrickRequest,
 * asserted with offsetof in bindings/c/clay_c.cpp, so a drain is one memcpy
 * and no field is transcribed.
 *
 * origin/spacing/dims/band are derivable from the key and the config, and are
 * carried anyway so that the host and the library cannot disagree about them.
 * band matters as much as the lattice does: the tape must be culled against the
 * brick DILATED by the band, because a sample keeps its true distance whenever
 * that distance is within the band, so an item a band outside the brick still
 * decides samples inside it. Culling on the bare brick drops it and the brick
 * is then classified empty instead of carrying the surface's approach.
 * Pass the request back to clay_brick_cache_submit UNMODIFIED. */
typedef struct clay_brick_request {
    int32_t key[3];      /* brick coordinate; brick (x,y,z) starts at key * dim * voxel_size */
    uint32_t generation; /* what submit checks; opaque to the host */
    float origin[3];     /* world position of lattice sample (0, 0, 0) */
    float spacing;       /* = config.voxel_size */
    int32_t dims[3];     /* = config.dim on every axis */
    float band;          /* = config.band_voxels * config.voxel_size */
} clay_brick_request;

/* A cache the caller owns. Returns NULL on invalid configuration, with the
 * detail in clay_last_error(). Free with clay_brick_cache_destroy. There is
 * no borrowed form: a cache belongs to whoever made it, never to a document,
 * so this takes no document and destroy returns void as
 * clay_document_destroy does rather than the clay_result
 * clay_voxel_grid_destroy needs to refuse a borrow.
 *
 * Destroying a cache invalidates every clay_brick_request still in flight, in
 * the sense that submitting one afterwards is a use-after-free this ABI
 * cannot detect; a request is otherwise a self-contained value. */
clay_brick_cache* clay_brick_cache_create(const clay_brick_config* config);
void clay_brick_cache_destroy(clay_brick_cache* cache);

/* The configuration the cache was created with, and its counters. Both
 * descriptors are OUTPUTS, so struct_size is the caller declaring how much of
 * the struct exists; the library writes only that prefix. */
clay_result clay_brick_cache_config(const clay_brick_cache* cache,
                                    clay_brick_config* out_config);
clay_result clay_brick_cache_stats(const clay_brick_cache* cache, clay_brick_stats* out_stats);

/* Marks every brick whose band-dilated volume meets the region, tracking
 * bricks it has not seen before. Pass an edit's INFLUENCE bound, not its
 * geometry bound — clay_layer_node_influence_bound is where one comes from,
 * and clay_brick_cache_mark_dirty_nodes does both steps for you.
 *
 * region_min and region_max BOTH NULL dirty everything the cache tracks —
 * what a host does after an edit whose influence is infinite, and the only
 * way to say it: an explicit region carrying an infinity is rejected, because
 * a region derived from a camera frustum or a degenerate selection box
 * arrives that way by accident rather than on purpose. Passing one without
 * the other is rejected, as it is on clay_voxel_rasterize.
 *
 * A region is also refused when it spans more than CLAY_MAX_BATCH bricks, or
 * when a brick coordinate in it would not fit in an int32_t. Three floats
 * name that many bricks easily — a region of 1e6 world units at a brick size
 * of 0.4 is 1e19 of them — and each one costs a tracked entry, so the span is
 * computed in 64-bit and checked BEFORE anything is inserted. A refused call
 * leaves the cache exactly as it was. */
/* -- eviction --------------------------------------------------------------
 *
 * The budget was a wall and is now a ceiling. Before this, a submit past the
 * budget was REFUSED and the only recourse was to destroy the cache and rebuild
 * from nothing — the most expensive thing available, taken when the device can
 * least afford it. On iOS a memory warning ignored is how an app gets killed,
 * and this cache is likely the largest allocation in the process.
 *
 * Eviction loses no information: a dropped brick is one that must be
 * re-evaluated if it is looked at again, which is what the
 * mark_dirty/take_dirty/submit cycle already does.
 *
 * NO AUTOMATIC LOOP. The cache publishes no refill loop, thread pool or timer,
 * and this is the same rule: eviction is something a host ASKS for, on a memory
 * warning, on a view change, or on its own clock. */

/* Get down to `target_bytes`, dropping the bricks FURTHEST from `focus` first,
 * and report how many went in *out_dropped (optional).
 *
 * The policy is spatial rather than temporal, and that is the decision this
 * call had to make. "Least recently used" is the reflex answer and the wrong
 * shape: a sculptor works in a NEIGHBOURHOOD, so what they come back to is near
 * where they are working, not what they touched most recently. Measured on a
 * walking-stroke fixture, dropping furthest-first re-requested 21% of what it
 * evicted against 52% for dropping arbitrarily — and reached the same target
 * having evicted 40% fewer bricks, because it was not dropping things that came
 * straight back.
 *
 * `focus` is a parameter because the cache cannot know it — only the host knows
 * where the camera points and where the last edit landed. One point rather than
 * an ordering, because a memory warning wants an answer now.
 *
 * NULL `focus` means no spatial preference, for a host that genuinely has none
 * (an offline bake, a background document); it takes keys in a deterministic
 * order so an untargeted trim is still reproducible.
 *
 * Never drops a brick that is dirty — that one is already scheduled to be
 * rewritten, so dropping it would trade memory for the thing the host is
 * actively waiting on. So the target may not be reachable; read
 * clay_brick_cache_stats to see where it got to. */
clay_result clay_brick_cache_trim(clay_brick_cache* cache, uint64_t target_bytes,
                                  const float focus[3], uint64_t* out_dropped);

/* Drop one brick's payload and return it to never-evaluated. *out_dropped
 * (optional) is 1 when a brick was reclaimed and 0 when the key held none, so a
 * caller can tell a miss from a reclaim. */
clay_result clay_brick_cache_evict(clay_brick_cache* cache, const int32_t key[3],
                                   uint64_t* out_dropped);

/* Forget keys that an untracked key would answer identically for — never
 * evaluated, or evaluated as OUTSIDE. This reclaims the per-key bookkeeping,
 * which is a different pool from the payloads clay_brick_cache_trim frees; a
 * host under memory pressure wants both, in that order.
 *
 * An INSIDE brick is never forgotten. It holds no lattice but carries real
 * information — this region is solid — and an untracked key reads as OUTSIDE,
 * so dropping one would report the interior as empty. */
clay_result clay_brick_cache_forget_empty(clay_brick_cache* cache, uint64_t* out_forgotten);

clay_result clay_brick_cache_mark_dirty(clay_brick_cache* cache, const float region_min[3],
                                        const float region_max[3]);

/* The same, with the bound computed from the document — the documented
 * default, because the region is the single most likely thing to get silently
 * wrong and a bound that is too tight leaves visibly stale bricks at a blend
 * seam. `nodes` is count node ids; one the layer does not hold, or a hidden
 * one, contributes nothing. A node whose influence is infinite dirties
 * everything the cache tracks.
 *
 * *out_marked (may be NULL) receives how many ids contributed a bound. Every
 * bound is computed and span-checked before any of them is marked, so a
 * refusal leaves the cache untouched rather than half-dirtied. */
clay_result clay_brick_cache_mark_dirty_nodes(clay_brick_cache* cache,
                                              const clay_document* doc, clay_layer_id layer,
                                              const clay_node_id* nodes, size_t count,
                                              size_t* out_marked);
/* The union over a layer — what a first, full fill marks. A layer that shows
 * nothing marks nothing, which is not an error. */
clay_result clay_brick_cache_mark_dirty_layer(clay_brick_cache* cache,
                                              const clay_document* doc, clay_layer_id layer);

/* Drains dirty bricks into requests, bumping each brick's generation.
 *
 * Capacity in, count out — the clay_layer_apply_stroke shape, not a size
 * query: *count is the caller's buffer capacity in REQUESTS going in and the
 * number written coming out, and *out_remaining (may be NULL) is how many are
 * still queued afterwards. out_requests == NULL is CLAY_ERROR_INVALID_ARGUMENT
 * and never a size query, so there is no BUFFER_TOO_SMALL retry loop. Call it
 * in a loop until *out_remaining is 0, or bound the work per frame by stopping
 * early — the rest stay queued.
 *
 * Your capacity bounds what is COPIED OUT, not what the library allocates. The
 * engine's drain is all-or-nothing, so the first call after a large mark stages
 * every queued request inside the library — asking for one request after a
 * million-brick mark still stages the million, at sizeof(clay_brick_request)
 * each, and the staging is held until the queue is fully drained. Size the
 * regions you mark accordingly; a frame budget on this call does not bound the
 * memory. Removing that cliff needs a capacity-aware drain in the engine.
 *
 * A request is a VALUE and stays submittable until its brick is dirtied
 * again, at which point submitting it is merely STALE. It may be copied,
 * queued, sent to any number of worker threads and held across frames; it
 * contains no pointer. */
clay_result clay_brick_cache_take_dirty(clay_brick_cache* cache,
                                        clay_brick_request* out_requests, size_t* count,
                                        size_t* out_remaining);

/* Evaluates `count` requests against a document and nothing else: for each
 * one it derives the cull region from the request's own lattice, compiles the
 * per-brick culled tape and runs it through `backend` (NULL = "cpu"). Brick i
 * fills out_values[i * dim^3 ...] whatever order the work is done in, so
 * values_capacity must be exactly count * dim^3 floats.
 *
 * Each request is culled against its own brick DILATED by its own band, which
 * is what clay_brick_cache_cull_region returns. The band rides on the request
 * rather than being passed in, so there is no way to supply the wrong one: a
 * band of zero would silently drop items that decide samples inside the brick
 * and classify it empty, and a value a caller has to remember to thread through
 * is a value that will one day arrive as zero.
 *
 * It takes NO cache handle: it does not submit, does not touch a cache and
 * starts no thread of its own, so it is free-threaded and any number of threads
 * may run it against one const document at once. It does WRITE the document's
 * own seed store (clay_document_resume_stats reports it), behind a lock of its
 * own that is not yours to take: held to read the seeds and again to store
 * them, and RELEASED across the compile and the evaluation between — so a
 * refill does not block a clay_eval_points on another thread for as long as it
 * evaluates. It may also spread a large batch's own work over the library's
 * process-wide pool while that lock is down, exactly as the full path always
 * has, so a refill can occupy every core for its duration; "starts no thread"
 * means it spawns none, not that it runs on one.
 *
 * The host still calls clay_brick_cache_submit, and still decides how many
 * requests to run and where. Fan out over REQUESTS, one brick per worker: the
 * CPU backend already splits a single grid's z axis over a process-wide pool,
 * and a brick is 8 slices of 64 samples, so a per-brick call is already a small
 * dispatch. That is measured, not reasoned: on an M2 Max it takes a 216-brick
 * fill from 24.7 ms to 8.2 ms on twelve workers.
 *
 * out_colors_rgb (may be NULL) receives the field's colour at the same lattice
 * points, count * dim^3 * 3 floats with brick i at i * dim^3 * 3, and
 * colors_capacity must be exactly that or 0 when it is NULL. It is what a
 * colour-carrying cache's clay_brick_cache_submit wants; a distance-only cache
 * needs neither and passing NULL costs nothing to compute.
 *
 * A BRICK PROVEN UNIFORM IS NOT WALKED. Most of the bricks a dab dirties on a
 * worked model are clay all the way through, and one evaluation at the
 * brick's centre with the tape's own Lipschitz bound proves it: no sample can
 * be within the band, and all carry the centre's sign. Such a brick is
 * classified exactly as its samples would have classified it -- what
 * clay_brick_cache_submit stores for it is bit-identical -- and the values
 * written to its slot are a STAND-IN rather than the field's samples: every
 * one beyond the band with the brick's sign, and the colour at sample dim^3/2
 * (the one submit keeps for a uniform brick) the field's own. That is the
 * whole of what a uniform brick's values say to submit, to a mip, or to a
 * renderer reading the buffer directly; a host that wants the field's own
 * distance past the band should ask clay_eval_points for it. The brick still
 * resumes on later dabs -- its seed is the proof, carried through the appended
 * items -- and counts in clay_resume_stats as any other. A tape whose Lipschitz
 * bound is a stepping bound and not a bound on the field's slope -- one holding
 * an ellipsoid or another underestimating primitive, an overflowing repeat, or
 * a taper, wrap_around or bend_curve deformer -- is never proven: its bricks
 * walk as before, and a brick whose cull drops such an item keeps the gate.
 *
 * ROUTE BY BATCH SIZE. The whole batch reaches the backend as batched
 * evaluations, so a GPU backend runs it as a single device submission rather
 * than one round trip per brick — but a submission still has a fixed cost
 * (~0.25 ms on an M-series Mac) that count * dim^3 samples must cover.
 * Measured there at dim 8: below ~16 bricks "cpu" wins (a single brick is
 * 0.01 ms on the CPU against the submission's 0.3 ms); at a dab's 27 bricks
 * "metal" is ~2x ahead; at 4096+ bricks it is 30x and up. So route small
 * residual batches to "cpu" and anything from a dab upward to "metal", or
 * simply pass "metal" whenever a batch holds a dab's worth of bricks. Both
 * backends produce the same field; this is speed, not results. Re-measure
 * before hardcoding the threshold on a device that is not an M-series Mac. */
clay_result clay_brick_cache_eval_requests(const clay_document* doc, const char* backend,
                                           const clay_brick_request* requests, size_t count,
                                           float* out_values, size_t values_capacity,
                                           float* out_colors_rgb, size_t colors_capacity);

/* The same refill over every visible SDF layer EXCEPT one (issue #378) — the
 * brick-cache half of clay_eval_points_excluding, and what a host actually
 * fills a preview atlas from. Same arguments, same ceilings, same fixed
 * per-brick slots at the same stride; brick i still occupies
 * out_values[i * dim^3 ...].
 *
 * TAKES NO SEED AND LEAVES NONE, and that is deliberate rather than an
 * omission. A seed is a brick's value for THIS document, which a later refill
 * continues with the items the document has gained since; a value computed
 * without one of the layers is not that, and storing it would hand the next
 * whole-document refill a seed with a layer missing from it — silently, since a
 * seeded answer is bit-identical to a walked one by contract and nothing in the
 * values could show it. So this is a plain batched walk, priced like a stroke's
 * first dab rather than its tenth.
 *
 * Which is the right price for what it is for: a host takes this ONCE at
 * pointer-down, because the layers it excludes do not move while the artist
 * drags, and composes the result with its live preview per frame.
 *
 * clay_document_resume_stats is untouched by this call — neither counter moves,
 * because neither a resume nor a seedable full walk happened. */
clay_result clay_brick_cache_eval_requests_excluding(
    const clay_document* doc, clay_layer_id excluded, const char* backend,
    const clay_brick_request* requests, size_t count, float* out_values, size_t values_capacity,
    float* out_colors_rgb, size_t colors_capacity);

/* The other half of the same split (ABI 0.82.0): refill from ONE layer alone,
 * ignoring every other. With clay_brick_cache_eval_requests_excluding over the
 * same layer and the same requests, the pointwise MINIMUM of the two results is
 * what the whole document would have produced — which is what "layers combine
 * by hard union" means, expressed as two values a host can hold and move
 * independently.
 *
 * NEITHER SCOPED FORM SEEDS THE RESUME STORE, and that is load-bearing rather
 * than an optimisation left undone: a partial field stored as a seed would be
 * resumed later as though it were the whole one, which is a wrong field with
 * nothing in the result to indicate it. A scoped refill therefore costs a full
 * walk every time, and an unscoped refill after one is as cold as it would have
 * been without it.
 *
 * CLAY_ERROR_NOT_FOUND when no layer carries the id, and
 * CLAY_ERROR_INVALID_ARGUMENT for a voxel or mesh layer: "this layer alone" is
 * a slice of the SDF field, and a layer that contributes none of it is a
 * caller naming the wrong thing rather than a layer that happens to be empty.
 * A HIDDEN SDF layer is refilled by name, on the same reading as
 * clay_document_mesh_sdf_layer: the caller named it, which says more than the
 * visibility flag does. */
clay_result clay_brick_cache_eval_requests_layer(
    const clay_document* doc, clay_layer_id layer, const char* backend,
    const clay_brick_request* requests, size_t count, float* out_values,
    size_t values_capacity, float* out_colors_rgb, size_t colors_capacity);

/* clay_brick_cache_eval_requests with the destination on the device — the call
 * a host refilling a brick atlas actually wants. Brick i occupies
 * out_values[i * dim^3 ...] floats and out_colors_rgb[i * dim^3 * 3 ...] at the
 * same fixed stride the host-memory form uses, in ONE of your buffers, so a
 * whole drain lands in the allocation you will draw from.
 *
 * Each request is culled against its own brick dilated by its own band, exactly
 * as the host-memory form does, and the same values come out. What differs is
 * only where they land — and what you then owe: these are float32 distances,
 * not the cache's classified, band-clamped fp16 bricks. Nothing was submitted
 * to a cache and no brick state was decided. If you want that, submit them and
 * read them back; this call is for hosts that would rather do the conversion
 * themselves than pay the copy.
 *
 * BATCHED like the host-memory form: the whole batch reaches the adopted
 * backend as batched device evaluations — one device submission per chunk of
 * requests, not one per brick — so a drain costs about what the host-memory
 * "metal" route costs, without the copy back. The per-submission floor
 * (~0.25 ms on an M-series Mac) still applies per CALL, so route by batch
 * size exactly as clay_brick_cache_eval_requests documents. */
clay_result clay_brick_cache_eval_requests_device(const clay_document* doc, clay_device* device,
                                                  const clay_brick_request* requests,
                                                  size_t count,
                                                  const clay_device_buffer* out_values,
                                                  const clay_device_buffer* out_colors_rgb);

/* Submits the evaluated distances for `count` requests: each brick's values
 * are classified (inside / outside / surface), clamped to the band and
 * quantized to fp16. `values` is count * dim^3 floats in the grids' own
 * order, x fastest, brick i at i * dim^3, and values_capacity must equal that
 * product exactly. count == 1 is the per-worker case and costs nothing.
 *
 * out_results (count clay_brick_submit values) and out_accepted (how many were
 * ACCEPTED) are both optional, but not both at once: a caller that skips both
 * cannot tell whether anything landed. The call returns CLAY_OK for all three
 * outcomes — a stale result is the generation counter doing its job, and a
 * budget refusal is the ceiling doing its job. The return code is reserved for
 * a malformed call: a null pointer, a values_capacity that is not exactly
 * count * dim^3, or a request whose spacing or dims do not match the cache's
 * configuration, which means it was modified or came from a different cache.
 *
 * `colors_rgb` is count * dim^3 * 3 floats in the same order, and it is
 * REQUIRED when the cache was created with clay_brick_config.colors and
 * REFUSED when it was not — there is no cache that takes colour optionally,
 * because a brick with a colour lattice and a brick without one are not the
 * same brick to read back. colors_capacity is checked exactly, or must be 0
 * when colors_rgb is NULL. Classification is decided by the DISTANCES alone:
 * colour never makes a brick a surface, and a uniform brick keeps one colour
 * rather than a lattice.
 *
 * The values are not scanned for NaN, and a request's origin is not
 * re-derived from its key. A backend that produces a NaN classifies the brick
 * as surface and stores a NaN half; validating dim^3 floats per brick on the
 * refill path would cost a pass over every brick in the scene to catch a
 * backend bug, which is the cost this cache exists to remove. */
clay_result clay_brick_cache_submit(clay_brick_cache* cache,
                                    const clay_brick_request* requests, size_t count,
                                    const float* values, size_t values_capacity,
                                    const float* colors_rgb, size_t colors_capacity,
                                    int32_t* out_results, size_t* out_accepted);

/* The world box a brick's lattice covers, and the box its evaluation should be
 * CULLED against — the brick dilated by the band. Hand the second one to
 * clay_eval_grid as the cull region when driving evaluation yourself;
 * clay_brick_cache_eval_requests does it for you. Any key answers, tracked or
 * not: these are arithmetic on the configuration. */
clay_result clay_brick_cache_brick_bounds(const clay_brick_cache* cache, const int32_t key[3],
                                          float out_min[3], float out_max[3]);
clay_result clay_brick_cache_cull_region(const clay_brick_cache* cache, const int32_t key[3],
                                         float out_min[3], float out_max[3]);

/* One decoded lattice sample, band-clamped: -band inside, +band outside, and
 * +band for a brick the cache has not evaluated. i, j, k are in [0, dim). The
 * direct mirror of the C++ sample(); read a whole brick with
 * clay_brick_cache_read_bricks. */
clay_result clay_brick_cache_sample(const clay_brick_cache* cache, const int32_t key[3],
                                    int32_t i, int32_t j, int32_t k, float* out_value);

/* Reads whole bricks in their STORED form — IEEE binary16 values per key, the
 * engine's own bits unconverted, which is what a GPU texture upload wants and
 * half the bytes of the decoded form. `keys_xyz` is count packed int32 triples,
 * exactly as clay_voxel_flood_select returns cells.
 *
 * The stride is FIXED at (dim + 2*apron)^3 — call it W below: brick i occupies
 * out_halves[i * W ...] whatever its state, so out_halves can be an MTLBuffer's
 * contents and the upload is one memcpy with no packing pass. values_capacity
 * must be exactly count * W, or 0 when out_halves is NULL and only the states
 * are wanted.
 *
 * `apron` is the halo, in voxels, taken from the NEIGHBOURING bricks and
 * written around each brick's own lattice. 0 is the brick alone and what this
 * call did before the apron existed. Hardware trilinear filtering across a
 * brick boundary needs a one-voxel halo, and without one a host either fetches
 * neighbours in the shader at every brick edge or repacks tiles on the CPU —
 * which throws away the one-memcpy property this call is for. The halo is
 * defined for EVERY neighbour: an implicit or never-evaluated brick answers the
 * same band value a single sample of it reports, so no element is undefined and
 * a tile at the edge of the sculpted region filters against the band rather
 * than against whatever was in the buffer. apron is bounded by dim and a wider
 * one is rejected rather than clamped — past that the tile is mostly neighbour
 * and what the host wants is a coarser lod, not a fatter apron.
 *
 * out_states (count clay_brick_state values) says what each key held — the
 * KEY's own state, never the halo's. It may be NULL when only the payload is
 * wanted, but not together with out_halves and out_colors_rgba: a call that
 * writes nothing has nothing to report. A uniform brick is FILLED with the band
 * value of its sign, so an uploader never branches; CLAY_BRICK_MISSING means
 * the cache holds nothing for that key and its whole W elements are LEFT
 * UNTOUCHED — the one state where the buffer is not written, and the rule is
 * about the key rather than its neighbourhood.
 *
 * out_colors_rgba (may be NULL) receives count * W * 4 bytes of RGBA8 in the
 * same padded layout — directly uploadable as rgba8unorm, which WebGPU filters
 * as it filters r16float. Alpha is 255 and RESERVED: it is not a mask and not
 * coverage. It requires a cache created with clay_brick_config.colors and is
 * refused otherwise, and colors_capacity must be exactly count * W * 4 or 0
 * when it is NULL.
 *
 * `lod` is 0 for the full-resolution brick or 1 for its mip. A value above 1
 * is rejected rather than clamped: there is one mip level, and silently
 * answering level 0 for a request for level 4 would put a brick twice the
 * intended size on screen. A mip carries NO colour — it subsamples distances,
 * and averaging colour across its 2x2x2 block would be a filtering policy
 * chosen on your behalf — so colours at lod 1 are refused, not approximated. */
clay_result clay_brick_cache_read_bricks(const clay_brick_cache* cache, int32_t lod,
                                         const int32_t* keys_xyz, size_t count, int32_t apron,
                                         int32_t* out_states, uint16_t* out_halves,
                                         size_t values_capacity, uint8_t* out_colors_rgba,
                                         size_t colors_capacity);

/* Every brick that stores samples, through the size-query pattern, as packed
 * int32 triples exactly like clay_voxel_flood_select: call with out_keys_xyz
 * == NULL for the count in *count, then again with a buffer of count*3
 * values. This is the ONE size query in this section — the drain is not, and
 * refuses a NULL buffer. The order is the cache's own and is not stable
 * across calls that mutate it. */
clay_result clay_brick_cache_surface_bricks(const clay_brick_cache* cache,
                                            int32_t* out_keys_xyz, size_t* count);

/* -- LOD mips -------------------------------------------------------------- */

/* A coarse brick covering 2x2x2 full-resolution bricks, subsampled from them
 * rather than evaluated: same lattice size, twice the spacing. Read it with
 * clay_brick_cache_read_bricks at lod 1, mesh it with
 * clay_brick_cache_mesh_lod at lod 1.
 *
 * Buildable only when all eight children are evaluated AND clean. *out_built
 * is 0 when they are not — an ordinary "not yet", reported like every other
 * expected negative here rather than as a failure. Dirtying any child drops
 * the mip, so a mip is never downsampled from stale data. */
clay_result clay_brick_cache_build_mip(clay_brick_cache* cache, const int32_t coarse_key[3],
                                       int32_t* out_built);
/* 1 when a valid mip exists for the coarse key, 0 when the region is only
 * available at full resolution — the cheap way to ask before reading. */
clay_result clay_brick_cache_current_lod(const clay_brick_cache* cache,
                                         const int32_t coarse_key[3], int32_t* out_lod);

/* -- what the bricks are for ----------------------------------------------- */

/* How a brick mesh is built. Only the fields mesh::MeshingOptions actually
 * has: this is NOT clay_mesh_params, whose voxel_size, resolution and mesher
 * mean nothing here — the lattice is the cache's and the mesher is the
 * marching one, both already decided. The one thing about the lattice a caller
 * DOES decide is which level of it to march, and that is the `lod` argument of
 * clay_brick_cache_mesh_lod rather than a field here: it changes what the key
 * list means, so it belongs beside the keys. */
typedef enum clay_normal_mode {
    CLAY_NORMAL_NONE = 0,     /* no normals */
    CLAY_NORMAL_FACE = 1,     /* area-weighted face normals; needs no document */
    /* The field gradient, so blends read smooth. Evaluated through PER-BRICK
     * culled tapes — the same culling the refill path uses — so its cost
     * follows the bricks being meshed, not the size of the document: re-meshing
     * a fixed dab's worth of bricks costs the same on a fresh document and on
     * one carrying two hundred earlier strokes (issue #73). Inside a brick's
     * band-dilated cull region the culled tape is band-clamp identical to the
     * whole document's, and mesh vertices sit on the surface with gradient
     * taps of gradient_eps << band, so the normals equal a full-tape
     * evaluation exactly. */
    CLAY_NORMAL_GRADIENT = 2
} clay_normal_mode;

typedef struct clay_brick_mesh_params {
    uint32_t struct_size; /* = sizeof(clay_brick_mesh_params); required */
    int32_t normals;      /* clay_normal_mode */
    int32_t colors;       /* 0/1: sample the document's colour field per vertex */
    float gradient_eps;   /* tetrahedron-tap half-width; <= 0 means the default */
} clay_brick_mesh_params;

/* What one brick key contributed to a mesh. An array ELEMENT, not a versioned
 * descriptor: a caller receives one per key and thousands at a time, its layout
 * is fixed, and changing that layout is a break rather than something to
 * negotiate — the same reasoning clay_brick_request carries.
 *
 * The ranges are contiguous and together PARTITION the mesh, so a host can
 * write a key's slice into a sub-range of a GPU buffer instead of rebuilding
 * it. But vertices are welded on canonical lattice-edge keys and that welding
 * spans brick SEAMS, so a triangle in one key's index range may reference a
 * vertex in an EARLIER key's vertex range — whichever key reached a shared seam
 * vertex first owns it. You may OVERWRITE a key's ranges; you may not free one
 * key's vertices without checking its neighbours'. Breaking the weld to make
 * the ranges independent would produce a seam-duplicated mesh, and this is also
 * the export path, where watertightness is the contract.
 *
 * A key's ranges carry its own cells' triangles first, then the straddlers
 * attributed to it — see clay_brick_cache_mesh for the attribution rule. */
typedef struct clay_brick_mesh_range {
    int32_t key[3];
    uint32_t vertex_first, vertex_count;
    uint32_t index_first, index_count;
} clay_brick_mesh_range;

/* Meshes the cache's surface bricks — marching only the cells those bricks
 * own, which is the point: a re-mesh costs what the SURFACE covers, not what
 * the scene's bounding box does.
 *
 * `keys_xyz` (count packed int32 triples, exactly as
 * clay_brick_cache_surface_bricks returns them) names the bricks to march, and
 * NULL with a key_count of 0 means every surface brick — what this call did
 * before the key list existed, and what an export wants. Hand it what
 * clay_brick_cache_take_dirty reported and a re-mesh costs the dab rather than
 * the model. A key that stores no lattice contributes nothing and is NOT an
 * error: a drained dirty set routinely contains bricks that turned out uniform.
 *
 * Marching a subset samples ACROSS the subset's boundary exactly as the whole
 * does, so the triangles produced for a cell are identical either way. A subset
 * differs only in that a vertex shared with a cell outside it is emitted again
 * by whichever mesh reaches it, at a bit-identical position — a duplicated seam
 * vertex, never a crack.
 *
 * A subset returns every whole-mesh triangle with at least one corner inside
 * a requested brick's closed box — including the STRADDLERS, whose cell is
 * owned by an unrequested brick. Each straddler is attributed to the
 * lexicographically lowest (x, then y, then z) requested key whose closed box
 * contains one of its corners, and lands in that key's ranges after the key's
 * own triangles. This is what makes a subset able to MAINTAIN a surface:
 * without the straddlers those triangles lived in cells no request named, so
 * no sequence of subset calls could reconstruct the whole, and dilating the
 * request only moved the boundary. The cost is that a host holding geometry
 * per brick dedupes by triangle: a straddler touching two requested keys is
 * attributed to one of them per call, and may move to another key's share
 * when a later request names a different set — its content is identical
 * wherever it lands, so keeping either copy is right.
 *
 * THE WHOLE-SURFACE CALL RETURNS STRADDLERS TOO. Marching the cells the surface
 * bricks OWN is not marching every cell that crosses: a cell belongs to the
 * brick its low corner falls in and takes its other seven corners from up to
 * seven neighbours, so a cell owned by a brick that stores no lattice still
 * crosses whenever a neighbour holds a sample of the opposite sign. It takes a
 * field that moves more than the band across one voxel step — which no true
 * distance field does, and a worked document does routinely, since a
 * displacement applied over a region narrower than the displacement is steeper
 * than the band. Left unmarched those cells punched pinholes into the frame
 * mesh that clay_document_mesh of the same document did not have (issue #292).
 * Such a cell is attributed whole, to the lowest requested key whose closed box
 * holds one of the CELL's corners; where the band brackets the field there are
 * none and the mesh is unchanged.
 *
 * out_ranges (may be NULL) receives key_count clay_brick_mesh_range values in
 * the order the keys were given. It REQUIRES keys_xyz: with no key list there
 * is no count for the caller to have sized this buffer from, and inferring one
 * from the cache's current surface set is the kind of length this ABI refuses
 * to infer anywhere else. Wanting ranges means wanting to patch a buffer, which
 * means having a key list — so the combination that is refused is not one a
 * host has a use for.
 *
 * `doc` may be NULL, and then no tape is compiled: the mesh has positions and
 * face normals and no colours. Gradient normals and colours are attributes of
 * the FIELD, so asking for either without a document is rejected rather than
 * quietly downgraded. When a document IS passed, those attributes are
 * evaluated through per-brick CULLED tapes rather than the whole document's,
 * so their cost follows the bricks named — see CLAY_NORMAL_GRADIENT. Behind the same owner handle as clay_document_mesh and
 * freed with clay_mesh_destroy. A cache holding no surface bricks yields an
 * EMPTY mesh rather than an error, as clay_voxel_mesh does for an empty grid:
 * a cache that has not been filled yet is an ordinary state of a session. */
clay_result clay_brick_cache_mesh(const clay_brick_cache* cache, const clay_document* doc,
                                  const clay_brick_mesh_params* params,
                                  const int32_t* keys_xyz, size_t key_count,
                                  clay_brick_mesh_range* out_ranges, clay_mesh** out_mesh);

/* The same, at a LEVEL. clay_brick_cache_mesh is this call at lod 0 and is
 * unchanged, byte for byte, by its existence.
 *
 * Until this call existed the LOD half a host could reach was the half it
 * could not use: clay_brick_cache_build_mip built a level, clay_brick_cache_
 * read_bricks read one and clay_brick_cache_current_lod reported one, and the
 * only way to get triangles out of it was to reimplement the marcher over the
 * fp16 samples — the same lattice, the same band clamping and the same
 * straddler attribution the cache exists to own (issue #93). So a host that
 * meshes had no coarse path at all, rather than a slower one.
 *
 * `lod` is 0 for the full-resolution bricks or 1 for their mips, as
 * clay_brick_cache_read_bricks takes it, and a value above 1 is rejected
 * rather than clamped for the same reason: there is one mip level, and
 * silently meshing level 0 for a request for level 4 puts geometry at twice
 * the intended size on screen.
 *
 * A mip is the cache's own dim^3 lattice with every second point kept, so a
 * level changes exactly two things: the spacing doubles and `keys_xyz` names
 * COARSE keys — the 2x2x2 block keys clay_brick_cache_build_mip and
 * clay_brick_cache_current_lod already take. The march, the seam welding, the
 * ranges and the straddler rule are the ones lod 0 uses, unchanged, and NULL
 * keys still means "every brick this level stores".
 *
 * A LEVEL THAT WAS NEVER BUILT IS CLAY_ERROR_NOT_FOUND, not an empty mesh. An
 * empty mesh already means "no surface bricks" — an ordinary state of a
 * session — and a half-built level answering the same thing would report a
 * missing mip as a missing surface. So a named coarse key with no valid mip is
 * refused (unlike lod 0, where a key that stores no lattice is an ordinary
 * uniform brick and contributes nothing: at lod 1 there is no uniform mip, and
 * an absent one means "not yet"), and a whole-level request is refused when the
 * cache holds surface bricks but not one mip. A cache with nothing in it at all
 * still meshes EMPTY at every valid level. clay_brick_cache_current_lod is the
 * cheap way to ask first, and clay_brick_cache_build_mip's *out_built is the
 * same "not yet" reported where it is expected rather than exceptional.
 *
 * COLOURS AND GRADIENT NORMALS ARE REFUSED AT LOD 1, not downgraded. They are
 * evaluated through per-brick culled tapes whose agreement with the whole
 * document's holds because a vertex sits on the field's surface, well inside
 * the band; a coarse vertex sits on the MIP's surface, which can be most of a
 * coarse cell off it, where the culled tape and the full one are only both
 * out-of-band rather than equal. The mip also carries no colour lattice of its
 * own, which is what clay_brick_cache_read_bricks already reports rather than
 * averaging. CLAY_NORMAL_FACE comes from the triangles, needs no field, and
 * works at every level. */
clay_result clay_brick_cache_mesh_lod(const clay_brick_cache* cache, const clay_document* doc,
                                      const clay_brick_mesh_params* params, int32_t lod,
                                      const int32_t* keys_xyz, size_t key_count,
                                      clay_brick_mesh_range* out_ranges, clay_mesh** out_mesh);

/* Raycast the cached bricks: trilinear samples inside surface bricks, a brick
 * DDA across the rest — so the cost is the ray's path through the band rather
 * than a march against the whole document's tape. Position and normal only;
 * clay_raycast_attributed is the call that names what was hit.
 *
 * The direction is normalized here, and a zero-length one is rejected. A ray
 * that hits nothing sets *out_hit to 0 and is not an error; every out pointer
 * but out_hit may be NULL. */
clay_result clay_brick_cache_raycast(const clay_brick_cache* cache, const float origin[3],
                                     const float dir[3], int32_t* out_hit, float* out_t,
                                     float out_position[3], float out_normal[3]);

/* The same, for many rays: rays_origin_dir is count packed six-float rays and
 * the outputs are the ones clay_raycast_many takes, in the same shapes and each
 * one optional. It is the batched form of the call above and nothing more —
 * one call shape for the cache and for the document is the whole value of it.
 *
 * The batch fans out across the engine's shared worker pool — the same one the
 * CPU backend evaluates points with — and returns only when every ray is done,
 * so no engine thread touches the cache after the call returns. The cache still
 * owns no thread, and serializing mutations against this call is still the
 * host's job. Each slot holds byte-for-byte what the single-ray call above
 * reports for the same ray, in order. */
clay_result clay_brick_cache_raycast_many(const clay_brick_cache* cache,
                                          const float* rays_origin_dir, size_t count,
                                          int32_t* out_hits, float* out_t,
                                          float* out_positions_xyz, float* out_normals_xyz);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLAY_H */
