#ifndef XGPU_PLUME_PERF_H
#define XGPU_PLUME_PERF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgpuPlumePerfBucket {
    XGPU_PLUME_PERF_PFIFO = 0,
    XGPU_PLUME_PERF_GUEST_WAIT,
    XGPU_PLUME_PERF_CPU_TRANSFORM,
    XGPU_PLUME_PERF_TEXTURE_HASH,
    XGPU_PLUME_PERF_UNSWIZZLE,
    XGPU_PLUME_PERF_UPLOAD,
    XGPU_PLUME_PERF_SHADER,
    XGPU_PLUME_PERF_PIPELINE,
    XGPU_PLUME_PERF_SURFACE_UPDATE,
    XGPU_PLUME_PERF_RECORD,
    XGPU_PLUME_PERF_REPLAY,
    XGPU_PLUME_PERF_SUBMIT,
    XGPU_PLUME_PERF_FENCE,
    XGPU_PLUME_PERF_PRESENT,
    /* Whole-draw emission at SET_BEGIN_END(0), split three ways. These are the
     * only buckets that cover the per-draw work between "the guest finished
     * describing a draw" and "it is recorded": vertex assembly, texture/state
     * binding, combiner build. Bulk-decoding a third of all dispatched methods
     * changed frame time by nothing, which ruled out per-method dispatch and
     * left this path as the unattributed remainder. */
    XGPU_PLUME_PERF_DRAW_INDEXED,
    XGPU_PLUME_PERF_DRAW_INLINE,
    XGPU_PLUME_PERF_DRAW_IMMEDIATE,
    /* Subdivision of the above. Both draw paths cost ~1 ms each regardless of
     * vertex count — inline draws average 11 vertices and still take ~1.05 ms —
     * so the cost is fixed per-draw work, and these split the block the two
     * paths share. Whatever the named buckets do not account for is residual. */
    XGPU_PLUME_PERF_DRAW_VS_PROGRAM,
    XGPU_PLUME_PERF_DRAW_RT_BIND,
    XGPU_PLUME_PERF_DRAW_TEX_BIND,
    XGPU_PLUME_PERF_DRAW_COMBINER,
    /* The whole pushbuffer drain, timed once per drain rather than per method.
     * Everything the renderer does on the guest's behalf happens inside this,
     * so frame_ms - drain_ms - guest_wait_ms is the recompiled game's own CPU
     * time — which no bucket covered, leaving ~10 ms of a 47.8 ms frame
     * unattributed and the renderer possibly blamed for it. Tens of calls a
     * frame, so unlike per-method timing this cannot distort the result. */
    XGPU_PLUME_PERF_DRAIN,
    XGPU_PLUME_PERF_BUCKET_COUNT
} XgpuPlumePerfBucket;

typedef enum XgpuPlumeGpuDrawResult {
    XGPU_PLUME_GPU_ACCEPTED = 0,
    XGPU_PLUME_GPU_FALLBACK_TOPOLOGY,
    XGPU_PLUME_GPU_FALLBACK_QUADS,
    XGPU_PLUME_GPU_FALLBACK_SHADER,
    XGPU_PLUME_GPU_FALLBACK_STATE,
    XGPU_PLUME_GPU_FALLBACK_MIXED_STREAMS,
    XGPU_PLUME_GPU_FALLBACK_LATCH_INPUT,
    XGPU_PLUME_GPU_FALLBACK_FORMAT,
    XGPU_PLUME_GPU_FALLBACK_DMA_BOUNDS,
    XGPU_PLUME_GPU_FALLBACK_RECORD_REJECTION,
    XGPU_PLUME_GPU_DRAW_RESULT_COUNT
} XgpuPlumeGpuDrawResult;

typedef struct XgpuPlumePerfFrame {
    uint64_t frame_start_ns;
    uint64_t frame_end_ns;
    uint64_t bucket_ns[XGPU_PLUME_PERF_BUCKET_COUNT];
    uint64_t bucket_calls[XGPU_PLUME_PERF_BUCKET_COUNT];
    uint64_t methods;
    uint64_t draws;
    uint64_t vertices;
    uint64_t waits;
    /* Draws whose texture/combiner state matched the previous draw exactly, so
     * the four-stage bind block was skipped (texbind_can_skip). The ratio
     * against draws says whether redundant state re-emission is still forcing
     * the rebind. */
    uint64_t texbind_skips;
    /* Why the other draws could not skip, by the guard that stopped them:
     * 0 = an enabled stage samples a render surface (structural — its contents
     * can advance mid-frame), 1 = the combiner program was genuinely rebuilt,
     * 2 = texture or combiner state actually differed (or first draw of a
     * frame). Says whether the remaining misses are addressable at all. */
    uint64_t texbind_miss[3];
    uint64_t host_presents;
    uint64_t gpu_draws;
    uint64_t gpu_vertices;
    uint64_t cpu_draws;
    uint64_t cpu_vertices;
    uint64_t fallback_draws[XGPU_PLUME_GPU_DRAW_RESULT_COUNT];
    uint64_t fallback_vertices[XGPU_PLUME_GPU_DRAW_RESULT_COUNT];
} XgpuPlumePerfFrame;

void xgpu_plume_perf_frame_reset(XgpuPlumePerfFrame *frame, uint64_t now_ns);
void xgpu_plume_perf_frame_add(XgpuPlumePerfFrame *frame,
                               XgpuPlumePerfBucket bucket,
                               uint64_t elapsed_ns);
void xgpu_plume_perf_frame_record_gpu(XgpuPlumePerfFrame *frame,
                                      uint32_t vertices);
void xgpu_plume_perf_frame_record_cpu(XgpuPlumePerfFrame *frame,
                                      XgpuPlumeGpuDrawResult reason,
                                      uint32_t vertices);
void xgpu_plume_perf_frame_take(XgpuPlumePerfFrame *frame,
                                XgpuPlumePerfFrame *snapshot,
                                uint64_t now_ns);
const char *xgpu_plume_gpu_draw_result_name(XgpuPlumeGpuDrawResult result);
int xgpu_plume_perf_format(char *buffer, size_t capacity,
                           uint32_t frame_number,
                           const XgpuPlumePerfFrame *snapshot);

int xgpu_plume_perf_enabled(void);
int xgpu_plume_replay_spike_enabled(void);
/* Per-method PFIFO timing, opt-in via XRECOMP_PLUME_PERF_METHODS on top of
 * XRECOMP_PLUME_PERF. Two clock reads times ~137k methods a frame distorts the
 * measurement badly enough to be misleading, so plain PERF leaves it off. */
int xgpu_plume_perf_methods_enabled(void);
uint64_t xgpu_plume_perf_method_begin(void);
uint64_t xgpu_plume_perf_begin(void);
void xgpu_plume_perf_end(XgpuPlumePerfBucket bucket, uint64_t start_ns);
void xgpu_plume_perf_record_methods(uint32_t count);
void xgpu_plume_perf_record_draw(uint32_t vertices);
void xgpu_plume_perf_record_gpu(uint32_t vertices);
void xgpu_plume_perf_record_cpu(XgpuPlumeGpuDrawResult reason,
                                uint32_t vertices);
void xgpu_plume_perf_record_wait(void);
void xgpu_plume_perf_record_texbind_skip(void);
/* reason: 0 surface-sampling stage, 1 combiner dirty, 2 state changed. */
void xgpu_plume_perf_record_texbind_miss(uint32_t reason);
void xgpu_plume_perf_record_host_present(void);
void xgpu_plume_perf_mark_present(uint32_t frame_number);
void xgpu_plume_perf_flush_pending(void);

#ifdef __cplusplus
}
#endif

#endif
