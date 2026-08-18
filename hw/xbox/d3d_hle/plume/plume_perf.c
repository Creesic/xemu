#include "plume_perf.h"

#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_perf_enabled = -1;
static int g_replay_spike_enabled = -1;
static int g_perf_frame_initialized;
static int g_perf_present_pending;
static uint32_t g_perf_pending_frame;
static XgpuPlumePerfFrame g_perf_frame;

static double ns_to_ms(uint64_t value)
{
    return (double)value * 1.0e-6;
}

void xgpu_plume_perf_frame_reset(XgpuPlumePerfFrame *frame, uint64_t now_ns)
{
    if (!frame)
        return;
    memset(frame, 0, sizeof(*frame));
    frame->frame_start_ns = now_ns;
}

void xgpu_plume_perf_frame_add(XgpuPlumePerfFrame *frame,
                               XgpuPlumePerfBucket bucket,
                               uint64_t elapsed_ns)
{
    if (!frame || bucket < 0 || bucket >= XGPU_PLUME_PERF_BUCKET_COUNT)
        return;
    frame->bucket_ns[bucket] += elapsed_ns;
    frame->bucket_calls[bucket]++;
}

void xgpu_plume_perf_frame_record_gpu(XgpuPlumePerfFrame *frame,
                                      uint32_t vertices)
{
    if (!frame)
        return;
    frame->gpu_draws++;
    frame->gpu_vertices += vertices;
}

void xgpu_plume_perf_frame_record_cpu(XgpuPlumePerfFrame *frame,
                                      XgpuPlumeGpuDrawResult reason,
                                      uint32_t vertices)
{
    if (!frame || reason <= XGPU_PLUME_GPU_ACCEPTED ||
        reason >= XGPU_PLUME_GPU_DRAW_RESULT_COUNT)
        return;
    frame->cpu_draws++;
    frame->cpu_vertices += vertices;
    frame->fallback_draws[reason]++;
    frame->fallback_vertices[reason] += vertices;
}

void xgpu_plume_perf_frame_take(XgpuPlumePerfFrame *frame,
                                XgpuPlumePerfFrame *snapshot,
                                uint64_t now_ns)
{
    if (!frame || !snapshot)
        return;
    *snapshot = *frame;
    snapshot->frame_end_ns = now_ns;
    xgpu_plume_perf_frame_reset(frame, now_ns);
}

const char *xgpu_plume_gpu_draw_result_name(XgpuPlumeGpuDrawResult result)
{
    static const char *const names[XGPU_PLUME_GPU_DRAW_RESULT_COUNT] = {
        "accepted",
        "topology",
        "quads",
        "shader",
        "state",
        "mixed_streams",
        "latch_input",
        "format",
        "dma_bounds",
        "record_rejection",
    };

    if (result < 0 || result >= XGPU_PLUME_GPU_DRAW_RESULT_COUNT)
        return "invalid";
    return names[result];
}

int xgpu_plume_perf_format(char *buffer, size_t capacity,
                           uint32_t frame_number,
                           const XgpuPlumePerfFrame *s)
{
    uint64_t frame_ns;
    uint64_t pfifo_ns;
    uint64_t guest_wait_ns;
    uint64_t service_ns;
    uint64_t drain_ns;
    uint64_t outside_drain_ns;

    if (!buffer || capacity == 0 || !s)
        return -1;
    frame_ns = s->frame_end_ns >= s->frame_start_ns
        ? s->frame_end_ns - s->frame_start_ns : 0;
    pfifo_ns = s->bucket_ns[XGPU_PLUME_PERF_PFIFO];
    guest_wait_ns = s->bucket_ns[XGPU_PLUME_PERF_GUEST_WAIT];
    service_ns = pfifo_ns >= guest_wait_ns ? pfifo_ns - guest_wait_ns : 0;
    drain_ns = s->bucket_ns[XGPU_PLUME_PERF_DRAIN];
    outside_drain_ns = frame_ns >= drain_ns ? frame_ns - drain_ns : 0;

    return snprintf(
        buffer, capacity,
        "[PLUME-PERF] frame=%u frame_ms=%.3f service_ms=%.3f "
        "outside_drain_ms=%.3f "
        "pfifo_ms=%.3f pfifo_calls=%llu "
        "guest_wait_ms=%.3f guest_wait_calls=%llu "
        "cpu_xf_ms=%.3f cpu_xf_calls=%llu "
        "texhash_ms=%.3f texhash_calls=%llu "
        "unswizzle_ms=%.3f unswizzle_calls=%llu "
        "upload_ms=%.3f upload_calls=%llu "
        "shader_ms=%.3f shader_calls=%llu "
        "pipeline_ms=%.3f pipeline_calls=%llu "
        "surface_update_ms=%.3f surface_update_calls=%llu "
        "record_ms=%.3f record_calls=%llu "
        "replay_ms=%.3f replay_calls=%llu "
        "submit_ms=%.3f submit_calls=%llu "
        "fence_ms=%.3f fence_calls=%llu "
        "present_ms=%.3f present_calls=%llu "
        "draw_indexed_ms=%.3f draw_indexed_calls=%llu "
        "draw_inline_ms=%.3f draw_inline_calls=%llu "
        "draw_imm_ms=%.3f draw_imm_calls=%llu "
        "draw_vsprog_ms=%.3f draw_vsprog_calls=%llu "
        "draw_rtbind_ms=%.3f draw_rtbind_calls=%llu "
        "draw_texbind_ms=%.3f draw_texbind_calls=%llu "
        "draw_combiner_ms=%.3f draw_combiner_calls=%llu "
        "drain_ms=%.3f drain_calls=%llu "
        "methods=%llu draws=%llu vertices=%llu waits=%llu host_presents=%llu "
        "texbind_skips=%llu "
        "texbind_miss_surface=%llu texbind_miss_dirty=%llu "
        "texbind_miss_state=%llu "
        "gpu_draws=%llu gpu_vertices=%llu cpu_draws=%llu cpu_vertices=%llu "
        "fb_topology=%llu/%llu fb_quads=%llu/%llu "
        "fb_shader=%llu/%llu fb_state=%llu/%llu "
        "fb_mixed_streams=%llu/%llu fb_latch_input=%llu/%llu "
        "fb_format=%llu/%llu fb_dma_bounds=%llu/%llu "
        "fb_record_rejection=%llu/%llu",
        frame_number, ns_to_ms(frame_ns), ns_to_ms(service_ns),
        ns_to_ms(outside_drain_ns),
        ns_to_ms(pfifo_ns),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_PFIFO],
        ns_to_ms(guest_wait_ns),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_GUEST_WAIT],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_CPU_TRANSFORM]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_CPU_TRANSFORM],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_TEXTURE_HASH]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_TEXTURE_HASH],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_UNSWIZZLE]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_UNSWIZZLE],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_UPLOAD]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_UPLOAD],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_SHADER]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_SHADER],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_PIPELINE]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_PIPELINE],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_SURFACE_UPDATE]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_SURFACE_UPDATE],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_RECORD]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_RECORD],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_REPLAY]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_REPLAY],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_SUBMIT]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_SUBMIT],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_FENCE]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_FENCE],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_PRESENT]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_PRESENT],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_INDEXED]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_INDEXED],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_INLINE]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_INLINE],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_IMMEDIATE]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_IMMEDIATE],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_VS_PROGRAM]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_VS_PROGRAM],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_RT_BIND]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_RT_BIND],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_TEX_BIND]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_TEX_BIND],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAW_COMBINER]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAW_COMBINER],
        ns_to_ms(s->bucket_ns[XGPU_PLUME_PERF_DRAIN]),
        (unsigned long long)s->bucket_calls[XGPU_PLUME_PERF_DRAIN],
        (unsigned long long)s->methods,
        (unsigned long long)s->draws,
        (unsigned long long)s->vertices,
        (unsigned long long)s->waits,
        (unsigned long long)s->host_presents,
        (unsigned long long)s->texbind_skips,
        (unsigned long long)s->texbind_miss[0],
        (unsigned long long)s->texbind_miss[1],
        (unsigned long long)s->texbind_miss[2],
        (unsigned long long)s->gpu_draws,
        (unsigned long long)s->gpu_vertices,
        (unsigned long long)s->cpu_draws,
        (unsigned long long)s->cpu_vertices,
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_TOPOLOGY],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_TOPOLOGY],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_QUADS],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_QUADS],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_SHADER],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_SHADER],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_STATE],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_STATE],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_MIXED_STREAMS],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_MIXED_STREAMS],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_LATCH_INPUT],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_LATCH_INPUT],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_FORMAT],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_FORMAT],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_DMA_BOUNDS],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_DMA_BOUNDS],
        (unsigned long long)s->fallback_draws[
            XGPU_PLUME_GPU_FALLBACK_RECORD_REJECTION],
        (unsigned long long)s->fallback_vertices[
            XGPU_PLUME_GPU_FALLBACK_RECORD_REJECTION]);
}

int xgpu_plume_perf_enabled(void)
{
    if (g_perf_enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_PERF");
        g_perf_enabled =
            value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
        if (g_perf_enabled) {
            xgpu_plume_perf_frame_reset(
                &g_perf_frame, xrecomp_host_monotonic_ns());
            g_perf_frame_initialized = 1;
            fprintf(stderr, "[PLUME-PERF] enabled\n");
        }
    }
    return g_perf_enabled;
}

/* Per-method PFIFO timing is opt-in on top of XRECOMP_PLUME_PERF. It brackets
 * every dispatched method, and an in-game MM3 frame dispatches ~137k of them —
 * two clock reads each, which inflated frame time roughly 2.5x and made the
 * profile unusable for magnitude (bug-530). Everything else in this telemetry
 * is per-draw or per-submission and costs nothing by comparison. */
int xgpu_plume_perf_methods_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_PERF_METHODS");
        enabled = value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
    }
    return enabled && xgpu_plume_perf_enabled();
}

uint64_t xgpu_plume_perf_method_begin(void)
{
    if (!xgpu_plume_perf_methods_enabled())
        return 0;
    return xrecomp_host_monotonic_ns();
}

int xgpu_plume_replay_spike_enabled(void)
{
    if (g_replay_spike_enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_REPLAY_SPIKES");
        g_replay_spike_enabled =
            value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
    }
    return g_replay_spike_enabled;
}

uint64_t xgpu_plume_perf_begin(void)
{
    if (!xgpu_plume_perf_enabled())
        return 0;
    return xrecomp_host_monotonic_ns();
}

void xgpu_plume_perf_end(XgpuPlumePerfBucket bucket, uint64_t start_ns)
{
    uint64_t now_ns;

    if (!start_ns || !xgpu_plume_perf_enabled())
        return;
    now_ns = xrecomp_host_monotonic_ns();
    if (now_ns >= start_ns)
        xgpu_plume_perf_frame_add(&g_perf_frame, bucket, now_ns - start_ns);
}

void xgpu_plume_perf_record_methods(uint32_t count)
{
    if (xgpu_plume_perf_enabled())
        g_perf_frame.methods += count;
}

void xgpu_plume_perf_record_draw(uint32_t vertices)
{
    if (!xgpu_plume_perf_enabled())
        return;
    g_perf_frame.draws++;
    g_perf_frame.vertices += vertices;
}

void xgpu_plume_perf_record_gpu(uint32_t vertices)
{
    if (xgpu_plume_perf_enabled())
        xgpu_plume_perf_frame_record_gpu(&g_perf_frame, vertices);
}

void xgpu_plume_perf_record_cpu(XgpuPlumeGpuDrawResult reason,
                                uint32_t vertices)
{
    if (xgpu_plume_perf_enabled())
        xgpu_plume_perf_frame_record_cpu(&g_perf_frame, reason, vertices);
}

void xgpu_plume_perf_record_wait(void)
{
    if (xgpu_plume_perf_enabled())
        g_perf_frame.waits++;
}

void xgpu_plume_perf_record_texbind_skip(void)
{
    if (xgpu_plume_perf_enabled())
        g_perf_frame.texbind_skips++;
}

void xgpu_plume_perf_record_texbind_miss(uint32_t reason)
{
    if (xgpu_plume_perf_enabled() && reason < 3)
        g_perf_frame.texbind_miss[reason]++;
}

void xgpu_plume_perf_record_host_present(void)
{
    if (xgpu_plume_perf_enabled())
        g_perf_frame.host_presents++;
}

void xgpu_plume_perf_mark_present(uint32_t frame_number)
{
    if (!xgpu_plume_perf_enabled())
        return;
    g_perf_pending_frame = frame_number;
    g_perf_present_pending = 1;
}

void xgpu_plume_perf_flush_pending(void)
{
    XgpuPlumePerfFrame snapshot;
    char line[2048];
    int length;

    if (!g_perf_present_pending || !xgpu_plume_perf_enabled() ||
        !g_perf_frame_initialized)
        return;
    xgpu_plume_perf_frame_take(
        &g_perf_frame, &snapshot, xrecomp_host_monotonic_ns());
    g_perf_present_pending = 0;
    length = xgpu_plume_perf_format(
        line, sizeof(line), g_perf_pending_frame, &snapshot);
    if (length > 0 && (size_t)length < sizeof(line)) {
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
}
