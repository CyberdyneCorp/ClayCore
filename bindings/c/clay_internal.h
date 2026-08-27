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

#ifdef __cplusplus
}
#endif

#endif /* CLAY_INTERNAL_H */
