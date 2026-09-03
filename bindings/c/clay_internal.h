/* ClayCore internal entry points -- NOT PART OF THE C ABI.
 *
 * Everything here exists so that a test can reach a piece of state the public
 * surface deliberately does not expose. It carries no struct_size negotiation,
 * no version guarantee, and no promise to exist in the next release: this
 * header is not installed, and a host that links against a symbol declared
 * here is relying on something that may be deleted without a minor bump.
 *
 * If a host ever genuinely wants one of these, the answer is to promote it to
 * clay.h under the versioned-descriptor convention, not to include this.
 */
#ifndef CLAY_INTERNAL_H
#define CLAY_INTERNAL_H

#include "clay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- the seed store's eviction bookkeeping (issue #346) ---------------------
 *
 * The store keeps a per-brick float32 seed (see clay_resume_stats) under a byte
 * budget, evicting least-recently-USED first. Two things about it are testable
 * only from inside:
 *
 * clay_internal_resume_order_size returns how many entries the eviction order
 * holds. It must equal clay_resume_stats::entries: one node per live seed, no
 * dead keys and no duplicates. It is not a host-facing number -- a store whose
 * two counts disagreed would be reporting its own bug, not a state to react to.
 *
 * clay_internal_set_resume_budget lowers (or raises) that byte budget, and
 * evicts down to it at once. The shipped 64 MB is 32,768 distance-only dim-8
 * bricks -- 2,048 B each, measured through clay_document_resume_stats -- so a
 * test that reached it by filling would be measuring the machine rather than
 * the policy. Eviction keeps the most recently used value whatever the budget,
 * so a budget of 0 leaves one entry rather than none.
 *
 * Both return CLAY_ERROR_INVALID_ARGUMENT on a null document.
 */
clay_result clay_internal_resume_order_size(const clay_document* doc, uint64_t* out_entries);
clay_result clay_internal_set_resume_budget(clay_document* doc, uint64_t bytes);

/* -- the uniform-brick gate --------------------------------------------------
 *
 * A brick the full refill can PROVE uniform from one evaluation and its tape's
 * Lipschitz bound is not walked (bindings/c/clay_c.cpp, prove_uniform): a stub
 * that classifies the same way stands in for its tape, and it stores no seed.
 * What the cache stores for it is bit-identical to what the walk would have
 * stored, so nothing about a refill's output says whether the gate fired --
 * these two seams are how a test sees it.
 *
 * clay_internal_gated_bricks: cumulative count of bricks the gate answered,
 * over the document's life. A subset of clay_resume_stats::refilled_bricks
 * (the gate sits inside the full path). A test holds the proof rate on a fixed
 * fixture above a floor, so a change that quietly switched the gate off would
 * fail rather than merely slow down.
 *
 * clay_internal_set_uniform_gate: switches the gate off (0) or on (non-zero)
 * for one document, so a gated fill can be held against an ungated one over
 * the same items. On by default; never off for a host.
 *
 * Both return CLAY_ERROR_INVALID_ARGUMENT on a null argument.
 */
clay_result clay_internal_gated_bricks(const clay_document* doc, uint64_t* out_gated);
clay_result clay_internal_set_uniform_gate(clay_document* doc, int32_t enabled);

/* -- the frontier half of one brick's seed (issue #360) ---------------------
 *
 * dirty_from / prefix_boundary / prefix_structure of the resume entry serving
 * `request`, or CLAY_ERROR_NOT_FOUND when the store holds none for that brick.
 * Tests pin the min-merge (two edits at two ordinals leave the EARLIER one),
 * the clear-on-accepted-submit (only a refill whose plan is still current
 * resets dirty_from), and the structure tagging (a prefix from before a
 * structural edit reads as stale) on these three numbers. A host has no
 * business with any of them: the fast path they steer is bit-identical to the
 * full walk by contract, and clay_document_resume_stats is the observable.
 *
 * dirty_from == 0xFFFFFFFF means clean; prefix_structure == 0 means no prefix
 * recorded. CLAY_ERROR_INVALID_ARGUMENT on any null argument.
 */
clay_result clay_internal_resume_frontier(const clay_document* doc,
                                          const clay_brick_request* request,
                                          uint32_t* out_dirty_from, uint32_t* out_boundary,
                                          uint64_t* out_structure);

/* -- the stale-submit half of the generation invariant (issue #360) ---------
 *
 * A resumed refill captures the revision its plan was made at, runs its seeded
 * walks off the lock, and only then retakes it to store the results as the
 * next frame's seeds -- behind a plan->now == now gate, so a submit that raced
 * an edit can never clear a dirty_from that newer edit set. Single-threaded
 * through the public ABI the race is unreachable: nothing can edit the
 * document between a refill's walks and its store, because the same thread is
 * inside the refill. This seam exists ONLY so that gate is testable. `fn` is
 * invoked ONCE, with `user`, at exactly that point -- after the walks, before
 * the retaken lock -- on the next refill of `doc` that has anything to store,
 * and is cleared before it runs so the edit it makes cannot re-trigger it.
 * Pass a null `fn` to clear an armed seam. Never set outside a test.
 *
 * Returns CLAY_ERROR_INVALID_ARGUMENT on a null document.
 */
clay_result clay_internal_set_resume_store_interleave(clay_document* doc,
                                                      void (*fn)(void* user), void* user);

#ifdef __cplusplus
}
#endif

#endif /* CLAY_INTERNAL_H */
