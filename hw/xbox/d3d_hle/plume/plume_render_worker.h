#ifndef XGPU_PLUME_RENDER_WORKER_H
#define XGPU_PLUME_RENDER_WORKER_H

/* Stage-3 render worker (docs/technical/plume-render-worker-ownership.md,
 * rollout stages): executes the render-owner job bodies on a dedicated
 * thread when XRECOMP_PLUME_ASYNC_PRESENT=1 (default off). Queue capacity
 * is ZERO at this stage — every submit is a rendezvous for WAIT batches: the guest blocks
 * until the job completes on the worker thread, so RHI/cache ownership
 * moves threads with no concurrent access anywhere. Stage 4 raises
 * capacity through the RenderQueue state machine and its completion
 * tickets; until then there is nothing to drain because nothing is ever
 * left queued or in flight behind the guest's back. */

#include <cstdint>

namespace xgpu {
namespace plume {

/* Cached XRECOMP_PLUME_ASYNC_PRESENT gate; first call decides. */
bool plume_render_worker_enabled();
/* False after any worker-side failure (sticky) or after stop(). */
bool plume_render_worker_healthy();

/* Run one owner job on the worker thread; blocks until it completes.
 * Returns false when the worker is disabled, unhealthy, or stopped —
 * the caller must then run the job inline (degraded but correct). */
bool plume_render_worker_run_wait(uint32_t slot, bool capture_wait_source,
                                  bool lazy_download_fence,
                                  uint32_t wait_draws,
                                  uint64_t wait_draw_hash);
bool plume_render_worker_run_present(uint32_t present_reason);

/* Completion barrier: returns once no worker job is executing. Every
 * guest entry point that touches RHI state or reads GPU results calls
 * this before proceeding (no-op when the worker is disabled). */
void plume_render_worker_sync();

/* Called by the owner present body (worker placement only) after replay
 * has consumed the recording and every guest-latched input; releases the
 * guest from its async submit. No-op inline. */
void plume_render_worker_mark_inputs_consumed();

/* Idempotent: completes any executing job, joins the thread, marks the
 * worker unhealthy so later submits fall back inline. Registered with
 * atexit on first start; safe to call earlier. */
void plume_render_worker_stop();

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_RENDER_WORKER_H */
