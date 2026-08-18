#ifndef XGPU_PLUME_RENDER_OWNER_H
#define XGPU_PLUME_RENDER_OWNER_H

/* Internal contract between the guest-facing Plume entry points and the
 * render-owner job bodies factored out of them (Task 4 stage 2). The
 * disabled path calls these inline on the guest thread; the render worker
 * calls exactly the same functions on the owner thread. Nothing here is
 * part of the public plume_host.h surface. */

#include <cstdint>

/* One WAIT batch on `slot`: reclaim if still in flight, replay, submit,
 * then eager-fence (legacy/F2) or leave in flight. */
void xgpu_plume_owner_execute_wait_batch(
    uint32_t slot, bool capture_wait_source, bool lazy_download_fence,
    uint32_t wait_draws, uint64_t wait_draw_hash);

/* One present request: F2 bookkeeping, skip/defer gating, swapchain
 * acquire, present-slot replay, output scaling, synchronous
 * submit-and-present, download completion, wait-ring reclamation. */
void xgpu_plume_owner_execute_present(uint32_t present_reason);

#endif /* XGPU_PLUME_RENDER_OWNER_H */
