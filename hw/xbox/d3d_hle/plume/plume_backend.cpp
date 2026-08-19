/*
 * plume_backend.cpp — Plume (renderbag RHI) host implementation for the
 * game-agnostic xboxrecomp runtime. See plume/README.md and plume_host.h.
 *
 * Backend and native-window ownership live in PlumeContext/factory.
 */
#include "plume_host.h"
#include "plume_context.h"
#include "plume_debug_overlay.h"
#include "plume_draw.h"
#include "plume_f2_capture.h"
#include "plume_frametime.h"
#include "plume_render_interface.h"
#include "plume_render_owner.h"
#include "plume_render_worker.h"
#include "plume_resolution_scale.h"
#include "plume_texture_state.h"
#include "plume_wait_stats.h"
#include "../platform/timing_trace.h"
#include "../platform/cpu_recorder.h"
#include "../platform/xrecomp_tracy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <array>
#include <chrono>
#include <memory>
#include <vector>

using namespace plume;

/* ── M3: game-driven present path (context + draw replay) ─────────────────── */

static xgpu::plume::PlumeContext g_ctx;
static xgpu::plume::PlumeDraw g_draw;
static xgpu::plume::PlumeSurfaceSyncTracker g_surface_sync;
static float g_clear[4] = { 0.0f, 0.0f, 0.12f, 1.0f };
static unsigned g_frames = 0;
static XgpuNativeWindow g_native_window = {};
static bool g_backend_selection_failed = false;
static const char *g_active_backend_name = nullptr;
static bool g_frame_dirty = false;
static bool g_clear_pending = true;
static bool g_present_mode_configured = false;
static int g_requested_vsync = -1;
static std::unique_ptr<plume::RenderTexture> g_scene_texture;
static std::unique_ptr<plume::RenderTextureView> g_scene_view;
static std::unique_ptr<plume::RenderTexture> g_scene_depth;
static std::unique_ptr<plume::RenderFramebuffer> g_scene_framebuffer;
static uint32_t g_scene_width = 0;
static uint32_t g_scene_height = 0;
static bool g_scene_initialized = false;
static uint32_t g_pending_present_reason = 0;
static uint32_t g_position_mode = 0;
static uint32_t g_output_width = XGPU_PANEL_WIDTH;
static uint32_t g_output_height = XGPU_PANEL_HEIGHT;
static uint32_t g_internal_resolution_scale = 1;
static bool g_render_extent_configured = false;
static uint32_t g_ui_canvas_depth = 0;
static XgpuPlumeUiCanvasMode g_ui_canvas_mode =
    XGPU_PLUME_UI_CANVAS_ANCHORED;
static float g_ui_canvas_width = 0.0f;
static float g_ui_canvas_height = 0.0f;
static uint64_t g_texture_binding_serial[4] = {};

static int plume_reconfigure_render_extent(uint32_t width, uint32_t height,
                                           uint32_t scale);

extern "C" const char *xgpu_plume_get_active_backend_name(void)
{
    return g_ctx.ready() ? g_active_backend_name : nullptr;
}

static void timing_trace_present(uint32_t event_id,
                                 uint32_t present_reason,
                                 uint32_t flags,
                                 uint32_t queued,
                                 uint32_t present_guest)
{
    const uint32_t values[4] = {
        present_reason, flags, queued, present_guest
    };
    xrecomp_timing_trace_emit(
        event_id, 0, XRECOMP_TIMING_TRACE_INSTANT,
        nullptr, nullptr, values);
}

struct DebugOverlayRegistration {
    XgpuPlumeDebugOverlayProvider provider;
    int32_t layer;
    uint32_t slot;
};

static std::array<DebugOverlayRegistration,
                  XGPU_PLUME_MAX_DEBUG_OVERLAY_PROVIDERS>
    g_debug_overlay_providers = {};
static uint32_t g_debug_overlay_provider_count = 0;

extern "C" int xgpu_plume_register_debug_overlay_provider(
    XgpuPlumeDebugOverlayProvider provider,
    int32_t layer)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!provider)
        return 0;
    for (uint32_t index = 0; index < g_debug_overlay_provider_count; ++index) {
        if (g_debug_overlay_providers[index].provider == provider) {
            g_debug_overlay_providers[index].layer = layer;
            return 1;
        }
    }
    if (g_debug_overlay_provider_count >=
        XGPU_PLUME_MAX_DEBUG_OVERLAY_PROVIDERS)
        return 0;
    DebugOverlayRegistration &entry =
        g_debug_overlay_providers[g_debug_overlay_provider_count];
    entry.provider = provider;
    entry.layer = layer;
    entry.slot = g_debug_overlay_provider_count;
    ++g_debug_overlay_provider_count;
    return 1;
}

extern "C" void xgpu_plume_set_vsync_enabled(int enabled)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    g_requested_vsync = enabled ? 1 : 0;
    if (g_ctx.ready() && g_ctx.swapChain()) {
        g_ctx.swapChain()->setVsyncEnabled(g_requested_vsync != 0);
        g_present_mode_configured = true;
        fprintf(stderr, "[PLUME] live VSync: %s\n",
                g_requested_vsync ? "enabled" : "disabled");
    }
}

static bool ascii_equal_ignore_case(const char *left, const char *right)
{
    if (!left || !right)
        return left == right;

    while (*left && *right) {
        unsigned char a = static_cast<unsigned char>(*left++);
        unsigned char b = static_cast<unsigned char>(*right++);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return *left == *right;
}

/* Pipelined present (2026-08-04 blocked-time work): defer the present fence
 * wait to the start of the next present so the GPU tail overlaps guest CPU
 * work instead of blocking submitAndPresent. Default off; presents that
 * record surface downloads or an F2 capture, and runs with the async-present
 * worker, always take the synchronous path. */
static bool plume_present_pipeline_enabled()
{
    static const bool enabled = []() {
        const char *value =
            std::getenv("XRECOMP_PLUME_PRESENT_PIPELINE");
        if (!value || !*value)
            return false;
        return std::strcmp(value, "1") == 0 ||
               ascii_equal_ignore_case(value, "true") ||
               ascii_equal_ignore_case(value, "yes") ||
               ascii_equal_ignore_case(value, "on");
    }();
    return enabled && !xgpu::plume::plume_render_worker_enabled();
}

static bool plume_lazy_download_fence_enabled()
{
    static const bool enabled = []() {
        const char *value =
            std::getenv("XRECOMP_PLUME_LAZY_DOWNLOAD_FENCE");
        if (!value || !*value)
            return false;
        return std::strcmp(value, "1") == 0 ||
               ascii_equal_ignore_case(value, "true") ||
               ascii_equal_ignore_case(value, "yes") ||
               ascii_equal_ignore_case(value, "on");
    }();
    return enabled;
}

/* Async WAIT_FOR_IDLE submission ring. Each slot owns its own command list and
 * fence so multiple WAIT batches can be in flight without the CPU blocking on
 * every GPU round trip (the per-slot upload buffers/resource buckets live in
 * PlumeDraw, indexed by the same slot). A slot's fence follows the plume RHI's
 * strict one-execute/one-wait pairing: a submission arms it (inFlight=true) and
 * exactly one later waitForCommandFence consumes it (on reuse or present drain).*/
struct PlumeWaitSlot {
    std::unique_ptr<RenderCommandList> cl;
    std::unique_ptr<RenderCommandFence> fence;
    bool inFlight = false;
};
static PlumeWaitSlot g_wait_ring[xgpu::plume::PlumeDraw::kWaitRingSize];
static uint32_t g_wait_ring_head = 0;
static bool g_wait_ring_ready = false;

static bool plume_wait_ring_ensure()
{
    if (g_wait_ring_ready)
        return true;
    for (auto &slot : g_wait_ring) {
        slot.cl = g_ctx.queue()->createCommandList();
        slot.fence = g_ctx.device()->createCommandFence();
        if (!slot.cl || !slot.fence)
            return false;
    }
    g_wait_ring_ready = true;
    return true;
}

/* Wait for and reclaim every in-flight ring slot. Present calls this before
 * allocating its programmable descriptor batch, so completed async WAIT sets
 * no longer occupy the finite shader-visible sampler heap. Other callers use
 * it before direct CPU surface access. */
static void plume_wait_ring_drain()
{
    if (!g_wait_ring_ready)
        return;
    for (uint32_t i = 0; i < xgpu::plume::PlumeDraw::kWaitRingSize; i++) {
        if (!g_wait_ring[i].inFlight)
            continue;
        uint64_t perf_fence_t0 = xgpu_plume_perf_begin();
        uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
        g_ctx.queue()->waitForCommandFence(g_wait_ring[i].fence.get());
        xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_RING_DRAIN, wait_t0);
        xgpu_plume_perf_end(XGPU_PLUME_PERF_FENCE, perf_fence_t0);
        g_draw.reclaimSub(i);
        g_wait_ring[i].inFlight = false;
    }
}

/* WAIT slots that were already in flight when the deferred present was
 * submitted. Their fences signaled before the present fence by queue order,
 * so reclaiming them at retire time never blocks. */
static uint32_t g_present_ring_snapshot = 0;

/* Retire the deferred previous present: wait its dedicated fence, reclaim the
 * snapshot WAIT slots, and release the present-only output-scale descriptor
 * sets. The programmable descriptor cache and retired textures are NOT
 * released here — WAIT batches recorded since the deferred present may still
 * reference them, so they wait for a total-order sync point (descriptor-batch
 * flush or a synchronous present). */
static void plume_retire_pending_present()
{
    if (!g_ctx.presentInFlight())
        return;
    g_ctx.waitPendingPresent();
    for (uint32_t i = 0; i < xgpu::plume::PlumeDraw::kWaitRingSize; i++) {
        if (!(g_present_ring_snapshot & (1u << i)) || !g_wait_ring[i].inFlight)
            continue;
        g_ctx.queue()->waitForCommandFence(g_wait_ring[i].fence.get());
        g_draw.reclaimSub(i);
        g_wait_ring[i].inFlight = false;
    }
    g_present_ring_snapshot = 0;
    g_draw.completeDeferredSurfaceDownloads();
    g_draw.releaseOutputScaleDescriptors();
}

enum PlumePresentReason : uint32_t {
    PLUME_PRESENT_UNKNOWN = 0,
    PLUME_PRESENT_FRAME = 1,
    PLUME_PRESENT_NO_WAIT = 2,
    PLUME_PRESENT_DEVICE = 3,
    PLUME_PRESENT_SWAP = 4,
    PLUME_PRESENT_HOST_FRAME = 5,
    PLUME_PRESENT_VBLANK = 6,
};

static const char *plume_present_reason_name(uint32_t reason)
{
    switch (reason) {
    case PLUME_PRESENT_FRAME: return "frame";
    case PLUME_PRESENT_NO_WAIT: return "no_wait";
    case PLUME_PRESENT_DEVICE: return "device_present";
    case PLUME_PRESENT_SWAP: return "device_swap";
    case PLUME_PRESENT_HOST_FRAME: return "host_frame";
    case PLUME_PRESENT_VBLANK: return "vblank_fallback";
    default: return "unknown";
    }
}

struct PlumeF2Readback {
    std::unique_ptr<plume::RenderBuffer> source_readback;
    std::unique_ptr<plume::RenderBuffer> wait_readback;
    uint32_t readback_width = 0;
    uint32_t readback_height = 0;
    uint32_t readback_pitch = 0;
    bool readback_error_logged = false;
};

static PlumeF2Readback g_f2_readback;

static bool f2_readback_prepare(void)
{
    if (!xgpu_plume_f2_active())
        return false;

    const uint32_t width = g_draw.physicalOutputWidth();
    const uint32_t height = g_draw.physicalOutputHeight();
    if (!width || !height || width > UINT32_MAX / 4u)
        return false;
    const uint32_t pitch = (width * 4u + 255u) & ~255u;
    if (g_f2_readback.source_readback && g_f2_readback.wait_readback &&
        g_f2_readback.readback_width == width &&
        g_f2_readback.readback_height == height &&
        g_f2_readback.readback_pitch == pitch)
        return true;

    g_f2_readback.source_readback = g_ctx.device()->createBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(uint64_t(pitch) * height));
    g_f2_readback.wait_readback = g_ctx.device()->createBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(uint64_t(pitch) * height));
    if (!g_f2_readback.source_readback || !g_f2_readback.wait_readback) {
        if (!g_f2_readback.readback_error_logged) {
            fprintf(stderr,
                    "[PLUME-F2] failed to create %ux%u readback buffers\n",
                    width, height);
            g_f2_readback.readback_error_logged = true;
        }
        g_f2_readback.source_readback.reset();
        g_f2_readback.wait_readback.reset();
        return false;
    }

    g_f2_readback.readback_width = width;
    g_f2_readback.readback_height = height;
    g_f2_readback.readback_pitch = pitch;
    g_f2_readback.readback_error_logged = false;
    return true;
}

/* F2 capture: inspect a just-fenced BGRA readback of the resolved present
 * surface. The final image is also saved so the log can be correlated with
 * the corruption that was actually displayed. */
static void f2_log_readback_avg(const char *tag, plume::RenderBuffer *buf,
                                uint32_t draws, uint64_t draw_hash)
{
    if (!buf || !xgpu_plume_f2_active())
        return;
    const uint32_t w = g_f2_readback.readback_width;
    const uint32_t h = g_f2_readback.readback_height;
    const uint32_t pitch = g_f2_readback.readback_pitch;
    if (!w || !h || !pitch)
        return;
    const plume::RenderRange range(0, uint64_t(pitch) * h);
    const uint8_t *px = static_cast<const uint8_t *>(buf->map(0, &range));
    if (!px)
        return;
    uint64_t sum[4] = {0, 0, 0, 0};
    const bool dump_image = std::strcmp(tag, "s4-final") == 0;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> alpha;
    if (dump_image) {
        rgb.resize(size_t(w) * h * 3u);
        alpha.resize(size_t(w) * h);
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = px + size_t(y) * pitch;
        for (uint32_t x = 0; x < w; x++) {
            sum[0] += row[x * 4 + 0];
            sum[1] += row[x * 4 + 1];
            sum[2] += row[x * 4 + 2];
            sum[3] += row[x * 4 + 3];
            if (dump_image) {
                const size_t pixel = size_t(y) * w + x;
                rgb[pixel * 3u + 0u] = row[x * 4u + 2u];
                rgb[pixel * 3u + 1u] = row[x * 4u + 1u];
                rgb[pixel * 3u + 2u] = row[x * 4u + 0u];
                alpha[pixel] = row[x * 4u + 3u];
            }
        }
    }
    buf->unmap();
    const uint64_t n = uint64_t(w) * h;
    xgpu_plume_f2_log("%s draws=%u hash=%016llX avg_bgra=%llu,%llu,%llu,%llu",
                      tag, draws, (unsigned long long)draw_hash,
                      (unsigned long long)(sum[0] / n),
                      (unsigned long long)(sum[1] / n),
                      (unsigned long long)(sum[2] / n),
                      (unsigned long long)(sum[3] / n));

    if (dump_image) {
        static uint32_t serial;
        const uint64_t stamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        char rgb_path[128];
        char alpha_path[128];
        std::snprintf(rgb_path, sizeof(rgb_path),
                      "plume_f2_present_%llu_%u.ppm",
                      (unsigned long long)stamp, serial);
        std::snprintf(alpha_path, sizeof(alpha_path),
                      "plume_f2_present_%llu_%u_a.pgm",
                      (unsigned long long)stamp, serial);
        ++serial;

        FILE *rgb_file = std::fopen(rgb_path, "wb");
        FILE *alpha_file = std::fopen(alpha_path, "wb");
        if (rgb_file && alpha_file) {
            std::fprintf(rgb_file, "P6\n%u %u\n255\n", w, h);
            std::fwrite(rgb.data(), 1, rgb.size(), rgb_file);
            std::fprintf(alpha_file, "P5\n%u %u\n255\n", w, h);
            std::fwrite(alpha.data(), 1, alpha.size(), alpha_file);
            std::fclose(rgb_file);
            std::fclose(alpha_file);
            xgpu_plume_f2_log("presentdump size=%ux%u rgb=%s alpha=%s",
                              w, h, rgb_path, alpha_path);
        } else {
            if (rgb_file)
                std::fclose(rgb_file);
            if (alpha_file)
                std::fclose(alpha_file);
            xgpu_plume_f2_log("presentdump failed rgb=%s alpha=%s",
                              rgb_path, alpha_path);
        }
    }
}

extern "C" void xgpu_plume_set_native_window(const XgpuNativeWindow *window)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    g_native_window = window ? *window : XgpuNativeWindow{};
}

static void plume_reset_guest_session_state()
{
    /* Stop the optional worker before touching owner-state/RHI objects. */
    xgpu::plume::plume_render_worker_sync();
    plume_retire_pending_present();
    plume_wait_ring_drain();
    g_draw.releaseSubmittedResources();

    for (auto &slot : g_wait_ring) {
        slot.cl.reset();
        slot.fence.reset();
        slot.inFlight = false;
    }
    g_wait_ring_head = 0;
    g_wait_ring_ready = false;
    g_present_ring_snapshot = 0;
    g_scene_framebuffer.reset();
    g_scene_depth.reset();
    g_scene_view.reset();
    g_scene_texture.reset();
    g_f2_readback = PlumeF2Readback{};
    g_draw.reset();
    g_surface_sync = xgpu::plume::PlumeSurfaceSyncTracker{};

    g_backend_selection_failed = false;
    g_frames = 0;
    g_frame_dirty = false;
    g_clear_pending = true;
    g_present_mode_configured = false;
    g_requested_vsync = -1;
    g_scene_width = 0;
    g_scene_height = 0;
    g_scene_initialized = false;
    g_pending_present_reason = 0;
    g_position_mode = 0;
    g_output_width = XGPU_PANEL_WIDTH;
    g_output_height = XGPU_PANEL_HEIGHT;
    g_internal_resolution_scale = 1;
    g_render_extent_configured = false;
    g_ui_canvas_depth = 0;
    g_ui_canvas_mode = XGPU_PLUME_UI_CANVAS_ANCHORED;
    g_ui_canvas_width = 0.0f;
    g_ui_canvas_height = 0.0f;
    for (uint64_t &serial : g_texture_binding_serial)
        serial = 0;
    g_clear[0] = 0.0f;
    g_clear[1] = 0.0f;
    g_clear[2] = 0.12f;
    g_clear[3] = 1.0f;
}

extern "C" void xgpu_plume_reset_session(void)
{
    plume_reset_guest_session_state();
}

extern "C" void xgpu_plume_teardown_output(void)
{
    xgpu_plume_reset_session();
    g_ctx.reset();
    g_active_backend_name = nullptr;
    g_native_window = XgpuNativeWindow{};
}

extern "C" int xgpu_plume_set_output_extent(uint32_t width, uint32_t height)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!width || !height)
        return 0;
    if (g_ctx.ready() && g_draw.ready()) {
        const int changed = plume_reconfigure_render_extent(
            width, height, g_internal_resolution_scale);
        if (!changed) {
            fprintf(stderr,
                    "[PLUME] live output extent change failed "
                    "(%ux%u requested, %ux%u active)\n",
                    width, height, g_output_width, g_output_height);
        }
        return changed;
    }
    const uint32_t oldWidth = g_output_width;
    const uint32_t oldHeight = g_output_height;
    g_output_width = width;
    g_output_height = height;
    if (g_render_extent_configured && !g_ctx.ready()) {
        g_render_extent_configured = g_draw.configureRenderExtent(
            width, height, g_internal_resolution_scale);
        if (!g_render_extent_configured) {
            g_output_width = oldWidth;
            g_output_height = oldHeight;
            return 0;
        }
    }
    return 1;
}

extern "C" int xgpu_plume_set_internal_resolution_scale(uint32_t scale)
{
    xgpu::plume::plume_render_worker_sync();
    if (!xgpu::plume::plumeValidInternalResolutionScale(scale))
        return 0;
    if (!g_ctx.ready()) {
        const uint32_t oldScale = g_internal_resolution_scale;
        g_internal_resolution_scale = scale;
        if (g_render_extent_configured) {
            g_render_extent_configured = g_draw.configureRenderExtent(
                g_output_width, g_output_height, scale);
            if (!g_render_extent_configured)
                g_internal_resolution_scale = oldScale;
            return g_render_extent_configured ? 1 : 0;
        }
        return 1;
    }
    return plume_reconfigure_render_extent(
        g_output_width, g_output_height, scale);
}

extern "C" uint32_t xgpu_plume_get_internal_resolution_scale(void)
{
    return g_internal_resolution_scale;
}

extern "C" void xgpu_plume_ui_canvas_begin(float logical_width,
                                             float logical_height,
                                             XgpuPlumeUiCanvasMode mode)
{
    if (logical_width <= 0.0f || logical_height <= 0.0f)
        return;
    if (g_ui_canvas_depth++ == 0u) {
        g_ui_canvas_mode = mode;
        g_ui_canvas_width = logical_width;
        g_ui_canvas_height = logical_height;
    }
}

extern "C" void xgpu_plume_ui_canvas_end(void)
{
    if (!g_ui_canvas_depth)
        return;
    if (--g_ui_canvas_depth == 0u) {
        g_ui_canvas_mode = XGPU_PLUME_UI_CANVAS_ANCHORED;
        g_ui_canvas_width = 0.0f;
        g_ui_canvas_height = 0.0f;
    }
}

extern "C" int xgpu_plume_ui_canvas_active(void)
{
    return g_ui_canvas_depth != 0u;
}

static bool present_ensure_init(void)
{
    if (g_ctx.ready() && g_draw.ready())
        return true;
    if (g_ctx.failed())
        return false;
    if (g_backend_selection_failed)
        return false;

    if (!g_render_extent_configured) {
        const char *value = std::getenv(
            "XRECOMP_INTERNAL_RESOLUTION_SCALE");
        if (value && *value) {
            char *end = nullptr;
            errno = 0;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (!errno && end != value && !*end &&
                parsed <= UINT32_MAX &&
                xgpu::plume::plumeValidInternalResolutionScale(
                    static_cast<uint32_t>(parsed))) {
                g_internal_resolution_scale =
                    static_cast<uint32_t>(parsed);
            } else {
                fprintf(stderr,
                        "[PLUME] invalid "
                        "XRECOMP_INTERNAL_RESOLUTION_SCALE='%s'; "
                        "expected 1..6, using 1\n",
                        value);
                g_internal_resolution_scale = 1;
            }
        }
        if (!g_draw.configureRenderExtent(
                g_output_width, g_output_height,
                g_internal_resolution_scale)) {
            fprintf(stderr,
                    "[PLUME] invalid logical/physical render extent "
                    "%ux%u scale=%u\n",
                    g_output_width, g_output_height,
                    g_internal_resolution_scale);
            return false;
        }
        g_render_extent_configured = true;
    }

    if (!g_ctx.ready()) {
        xgpu::plume::PlumeContext::Desc desc;
        desc.window = g_native_window;
        desc.width = g_output_width;
        desc.height = g_output_height;
        const char *backend = std::getenv("XEMU_PLUME_BACKEND");
        if (backend == nullptr || backend[0] == '\0')
            backend = std::getenv("XRECOMP_GPU_BACKEND");
        if (!xgpu::plume::parsePlumeBackend(backend, desc.backend)) {
            fprintf(stderr,
                    "[PLUME] invalid XEMU_PLUME_BACKEND='%s'; expected "
                    "auto, d3d12, vulkan, or metal\n",
                    backend ? backend : "");
            g_backend_selection_failed = true;
            return false;
        }
        const xgpu::plume::PlumeBackend resolved_backend =
            xgpu::plume::resolvePlumeBackend(desc.backend);
        if (!g_ctx.init(desc))
            return false;
        g_active_backend_name =
            xgpu::plume::plumeBackendName(resolved_backend);
        g_draw.setPipelinedPresent(plume_present_pipeline_enabled());
    }

    if (!g_present_mode_configured) {
        const char *mode = std::getenv("XRECOMP_PRESENT_MODE");
        bool vsync = g_requested_vsync < 0 || g_requested_vsync != 0;
        if (g_requested_vsync < 0) {
            if (ascii_equal_ignore_case(mode, "immediate") ||
                ascii_equal_ignore_case(mode, "unlocked"))
                vsync = false;
            else if (mode && !ascii_equal_ignore_case(mode, "vsync"))
                fprintf(stderr,
                        "[PLUME] unknown XRECOMP_PRESENT_MODE='%s'; "
                        "using vsync\n",
                        mode);
        }
        g_ctx.swapChain()->setVsyncEnabled(vsync);
        fprintf(stderr, "[PLUME] present mode: %s\n",
                vsync ? "vsync" : "immediate/unlocked");
        g_present_mode_configured = true;
    }

    if (!g_draw.ready()) {
        g_draw.setPipelinedPresent(plume_present_pipeline_enabled());
        if (!g_draw.initPipelines(g_ctx))
            return false;
    }

    return g_ctx.ready() && g_draw.ready();
}

static bool present_ensure_scene_target(void)
{
    const uint32_t width = g_draw.physicalOutputWidth();
    const uint32_t height = g_draw.physicalOutputHeight();
    if (g_scene_texture && g_scene_view && g_scene_depth &&
        g_scene_framebuffer &&
        g_scene_width == width && g_scene_height == height)
        return true;

    std::unique_ptr<plume::RenderTexture> texture =
        g_ctx.device()->createTexture(plume::RenderTextureDesc::ColorTarget(
            width, height, plume::RenderFormat::B8G8R8A8_UNORM));
    if (!texture)
        return false;

    std::unique_ptr<plume::RenderTextureView> view =
        texture->createTextureView(
            plume::RenderTextureViewDesc::Texture2D(
                plume::RenderFormat::B8G8R8A8_UNORM));
    if (!view)
        return false;

    std::unique_ptr<plume::RenderTexture> depth =
        g_ctx.device()->createTexture(plume::RenderTextureDesc::DepthTarget(
            width, height, plume::RenderFormat::D32_FLOAT));
    if (!depth)
        return false;

    const plume::RenderTexture *color = texture.get();
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = depth.get();
    std::unique_ptr<plume::RenderFramebuffer> framebuffer =
        g_ctx.device()->createFramebuffer(desc);
    if (!framebuffer)
        return false;

    g_scene_texture = std::move(texture);
    g_scene_view = std::move(view);
    g_scene_depth = std::move(depth);
    g_scene_framebuffer = std::move(framebuffer);
    g_scene_width = width;
    g_scene_height = height;
    g_scene_initialized = false;
    return true;
}

static int plume_reconfigure_render_extent(uint32_t width, uint32_t height,
                                           uint32_t scale)
{
    if (!width || !height ||
        !xgpu::plume::plumeValidInternalResolutionScale(scale)) {
        return 0;
    }
    if (width == g_output_width && height == g_output_height &&
        scale == g_internal_resolution_scale) {
        return 1;
    }
    if (!present_ensure_init())
        return 0;

    /* This is the same total-order boundary used by CPU surface access. No
     * command list, descriptor set, or cached framebuffer may outlive the
     * physical resources it references. */
    plume_retire_pending_present();
    if (g_draw.hasQueuedWork() && !xgpu_plume_wait_for_idle(0))
        return 0;
    plume_wait_ring_drain();
    g_draw.materializeRecordedSurfaces(g_ctx);
    g_draw.releaseSubmittedResources();

    if (!g_draw.reconfigureRenderExtent(g_ctx, width, height, scale))
        return 0;

    g_scene_framebuffer.reset();
    g_scene_depth.reset();
    g_scene_view.reset();
    g_scene_texture.reset();
    g_scene_width = 0;
    g_scene_height = 0;
    g_scene_initialized = false;
    g_f2_readback = PlumeF2Readback{};
    g_output_width = width;
    g_output_height = height;
    g_internal_resolution_scale = scale;
    g_render_extent_configured = true;
    g_frame_dirty = true;
    g_clear_pending = true;
    fprintf(stderr, "[PLUME] live render extent: %ux%u at %uX (%ux%u)\n",
            width, height, scale,
            g_draw.physicalOutputWidth(), g_draw.physicalOutputHeight());
    return 1;
}

void xgpu_plume_present_set_clear(float r, float g, float b, float a)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    g_clear[0] = r;
    g_clear[1] = g;
    g_clear[2] = b;
    g_clear[3] = a;
    g_frame_dirty = true;
    g_clear_pending = true;
}

void xgpu_plume_set_render_target(const XgpuSurfaceBinding *binding)
{
    if (binding && present_ensure_init())
        g_draw.setRenderTarget(*binding);
}

void xgpu_plume_clear_target(float r, float g, float b, float a,
                             uint32_t color_write_mask,
                             const XgpuRect *rect)
{
    if (!present_ensure_init())
        return;
    uint64_t perf_record_t0 = xgpu_plume_perf_begin();
    g_draw.clearTarget(r, g, b, a, color_write_mask, rect);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perf_record_t0);
    g_frame_dirty = true;
}

void xgpu_plume_clear_depth_stencil(uint32_t clear_depth,
                                    uint32_t clear_stencil,
                                    float depth, uint32_t stencil,
                                    const XgpuRect *rect)
{
    if (!present_ensure_init())
        return;
    uint64_t perf_record_t0 = xgpu_plume_perf_begin();
    g_draw.clearDepthStencil(clear_depth != 0, clear_stencil != 0,
                             depth, (uint8_t)stencil, rect);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perf_record_t0);
    g_frame_dirty = true;
}

void xgpu_plume_set_surface_texture(uint32_t stage, uint32_t guest,
                                    uint32_t unnormalized_coords)
{
    if (stage >= 4u || !present_ensure_init())
        return;
    /* Ordinary surface aliases resolve entirely through the guest recorder's
     * generation shadow. Only the device-backbuffer alias needs an immediate
     * RHI mirror, and only before that guest key has a recorded generation. */
    if (guest && guest == g_draw.presentGuest() && !g_draw.hasSurface(guest)) {
        xgpu::plume::plume_render_worker_sync();
        if (!present_ensure_init())
            return;
        g_draw.ensureBackbufferMirror(g_ctx, guest);
    }
    g_draw.setSurfaceTexture(stage, guest, unnormalized_coords);
    ++g_texture_binding_serial[stage];
}

void xgpu_plume_set_present_surface(uint32_t guest)
{
    if (!present_ensure_init())
        return;
    /* Guest latch only. The owner snapshots this value before it releases the
     * guest from an async present, so the next frame may update it without
     * waiting for the previous present's GPU/fence tail. Do not dirty:
     * presenting with an empty queue flashed the swap clear tint between real
     * composites. */
    g_draw.setPresentSurface(guest);
}

int xgpu_plume_wait_for_idle(uint32_t surface_sync_flags)
{
    (void)surface_sync_flags;
    if (!present_ensure_init())
        return 0;
    xgpu_plume_perf_record_wait();
    if (!g_draw.hasQueuedWork()) {
        g_surface_sync.recordWaitSubmit(false);
        return 1;
    }
    if (!present_ensure_scene_target())
        return 0;
    const uint32_t wait_draws = (uint32_t)g_draw.queuedCount();
    const xgpu::plume::PlumeDraw::F2QueueFingerprint wait_fp =
        g_draw.captureF2QueueFingerprint();
    const bool capture_wait_source = wait_fp.draws_to_present > 0 &&
                                     f2_readback_prepare();

    if (!plume_wait_ring_ensure())
        return 0;
    const uint32_t slot = g_wait_ring_head;
    g_wait_ring_head = (g_wait_ring_head + 1) % xgpu::plume::PlumeDraw::kWaitRingSize;
    if (!xgpu::plume::plume_render_worker_run_wait(
            slot, capture_wait_source, plume_lazy_download_fence_enabled(),
            wait_draws, wait_fp.draw_hash))
        xgpu_plume_owner_execute_wait_batch(
            slot, capture_wait_source, plume_lazy_download_fence_enabled(),
            wait_draws, wait_fp.draw_hash);
    g_surface_sync.recordWaitSubmit(true);
    g_clear_pending = false;
    return 1;
}

/* Owner-side body of one WAIT batch on `slot`: reclaim the slot if a prior
 * submission still holds it, replay the recorded frame into the slot's
 * command list, submit with the slot fence, then either eager-fence
 * (legacy download mode or F2 capture) or leave the batch in flight.
 * Runs inline on the guest thread today; the stage-3 render worker calls
 * exactly this function on the owner thread (ownership doc, section on
 * rollout). Reads and writes render-owner state only. */
void xgpu_plume_owner_execute_wait_batch(
    uint32_t slot, bool capture_wait_source, bool lazy_download_fence,
    uint32_t wait_draws, uint64_t wait_draw_hash)
{
    /* Reclaim this slot if a prior submission still holds it (its fence is
     * usually long signaled, so the wait returns immediately). */
    if (g_wait_ring[slot].inFlight) {
        uint64_t perf_fence_t0 = xgpu_plume_perf_begin();
        uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
        g_ctx.queue()->waitForCommandFence(g_wait_ring[slot].fence.get());
        xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_SLOT_RECLAIM, wait_t0);
        xgpu_plume_perf_end(XGPU_PLUME_PERF_FENCE, perf_fence_t0);
        g_draw.reclaimSub(slot);
        g_wait_ring[slot].inFlight = false;
    }
    g_draw.setCurrentSub(slot);
    g_draw.materializeRecordedSurfaces(g_ctx);

    plume::RenderCommandList *cl = g_wait_ring[slot].cl.get();
    cl->begin();
    cl->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(g_scene_texture.get(),
                                             plume::RenderTextureLayout::COLOR_WRITE));
    cl->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(g_scene_depth.get(),
                                             plume::RenderTextureLayout::DEPTH_WRITE));
    cl->setFramebuffer(g_scene_framebuffer.get());
    const uint32_t w = g_scene_width;
    const uint32_t h = g_scene_height;
    cl->setViewports(plume::RenderViewport(0.0f, 0.0f, float(w), float(h)));
    cl->setScissors(plume::RenderRect(0, 0, w, h));
    if (g_clear_pending || !g_scene_initialized)
        cl->clearColor(0, plume::RenderColor(g_clear[0], g_clear[1],
                                             g_clear[2], g_clear[3]));
    if (!g_scene_initialized)
        cl->clearDepth(true, 1.0f);

    /* End with the present-surface copy: in-game the entire draw stream
     * drains through WAIT_FOR_IDLE submissions (presents see an empty
     * queue), so this is the only reliable point to refresh the scene
     * from the completed flip-chain buffer. The present surface only
     * changes at the guest's swap copy, so mid-frame waits re-copy the
     * last completed frame — never a partial one. */
    uint64_t perf_replay_t0 = xgpu_plume_perf_begin();
    g_draw.replay(g_ctx, cl, g_scene_texture.get(),
                  g_scene_framebuffer.get(), true, false);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_REPLAY, perf_replay_t0);
    g_draw.recordSurfaceDownloads(g_ctx, cl);
    g_scene_initialized = true;

    bool captured_wait_source = false;
    if (capture_wait_source) {
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        plume::RenderTextureLayout source_layout =
            plume::RenderTextureLayout::UNKNOWN;
        plume::RenderTexture *source = g_draw.resolvedPresentSurface(
            &source_width, &source_height, &source_layout);
        if (source && source_width == g_output_width &&
            source_height == g_output_height &&
            source_layout != plume::RenderTextureLayout::UNKNOWN) {
            if (source_layout != plume::RenderTextureLayout::COPY_SOURCE) {
                cl->barriers(plume::RenderBarrierStage::COPY,
                             plume::RenderTextureBarrier(
                                 source,
                                 plume::RenderTextureLayout::COPY_SOURCE));
            }
            const plume::RenderTextureCopyLocation readback_dst =
                plume::RenderTextureCopyLocation::PlacedFootprint(
                    g_f2_readback.wait_readback.get(),
                    plume::RenderFormat::B8G8R8A8_UNORM,
                    w, h, 1, g_f2_readback.readback_pitch / 4u);
            const plume::RenderTextureCopyLocation readback_src =
                plume::RenderTextureCopyLocation::Subresource(source, 0, 0);
            cl->copyTextureRegion(readback_dst, readback_src);
            if (source_layout != plume::RenderTextureLayout::COPY_SOURCE) {
                const plume::RenderBarrierStages restore_stage =
                    (source_layout == plume::RenderTextureLayout::COPY_DEST)
                        ? plume::RenderBarrierStage::COPY
                        : plume::RenderBarrierStage::GRAPHICS;
                cl->barriers(restore_stage,
                             plume::RenderTextureBarrier(source,
                                                         source_layout));
            }
            captured_wait_source = true;
        }
    }
    cl->end();

    const plume::RenderCommandList *submitted = cl;
    uint64_t perf_submit_t0 = xgpu_plume_perf_begin();
    g_ctx.queue()->executeCommandLists(&submitted, 1, nullptr, 0, nullptr, 0,
                                       g_wait_ring[slot].fence.get());
    xgpu_plume_perf_end(XGPU_PLUME_PERF_SUBMIT, perf_submit_t0);
    /* Legacy mode preserves the eager small-surface download fence exactly.
     * Experimental lazy mode leaves that batch in flight until a CPU-read
     * serial synchronization point reclaims it. F2 capture remains eager
     * because it maps the readback immediately. Read pendingDownloads()
     * before retireSub() moves the list onto the slot. */
    const bool wait_now =
        (!lazy_download_fence && g_draw.pendingDownloads()) ||
        capture_wait_source;
    g_draw.retireSub(slot);
    if (wait_now) {
        uint64_t perf_fence_t0 = xgpu_plume_perf_begin();
        uint64_t wait_t0 = xgpu_plume_wait_stats_begin();
        g_ctx.queue()->waitForCommandFence(g_wait_ring[slot].fence.get());
        xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_DOWNLOAD_FENCE, wait_t0);
        xgpu_plume_perf_end(XGPU_PLUME_PERF_FENCE, perf_fence_t0);
        g_draw.reclaimSub(slot);
    } else {
        g_wait_ring[slot].inFlight = true;
    }
    if (captured_wait_source) {
        f2_log_readback_avg("s4", g_f2_readback.wait_readback.get(),
                            wait_draws, wait_draw_hash);
    }
}

void xgpu_plume_set_texture(uint32_t stage, uint32_t guest, const void *pixels,
                           uint32_t w, uint32_t h, uint32_t pitch, uint32_t bytes,
                           uint32_t format, uint64_t version)
{
    if (!present_ensure_init())
        return;
    g_draw.setTexture(stage, guest, pixels, w, h, pitch, bytes, format,
                      version);
    if (stage < 4u)
        ++g_texture_binding_serial[stage];
}

extern "C" void xgpu_plume_set_present_reason(uint32_t reason)
{
    g_pending_present_reason = reason;
}

void xgpu_plume_set_texture_ex(const XgpuTextureBinding *binding)
{
    if (!binding || !present_ensure_init())
        return;
    g_draw.setTexture(binding->stage, binding->guest_ptr,
                      binding->pixels, binding->width, binding->height,
                      binding->pitch, binding->bytes, binding->format,
                      binding->version, binding->depth, binding->levels,
                      binding->dimensionality, binding->cube,
                      binding->unnormalized_coords);
    if (binding->stage < 4u)
        ++g_texture_binding_serial[binding->stage];
}

int xgpu_plume_bind_texture_if_cached(const XgpuTextureBinding *binding)
{
    if (!binding || !present_ensure_init())
        return 0;
    if (!g_draw.bindTextureIfCached(*binding))
        return 0;
    if (binding->stage < 4u)
        ++g_texture_binding_serial[binding->stage];
    return 1;
}

uint64_t xgpu_plume_texture_binding_serial(uint32_t stage)
{
    return stage < 4u ? g_texture_binding_serial[stage] : 0u;
}

void xgpu_plume_set_sampler(uint32_t stage, uint32_t address, uint32_t filter,
                            uint32_t border_color)
{
    if (!present_ensure_init())
        return;
    g_draw.setSampler(xgpu::plume::plumeDecodeNv2aSampler(
        stage, address, filter, border_color));
}

void xgpu_plume_set_sampler_ex(const XgpuSamplerBinding *binding)
{
    if (!binding || !present_ensure_init())
        return;
    g_draw.setSampler(*binding);
}

extern "C" uint32_t xgpu_plume_create_pixel_shader(const char *text)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    return g_draw.createPixelShader(text);
}

extern "C" int xgpu_plume_set_vertex_program(const uint32_t *microcode,
                                             uint32_t length,
                                             const uint32_t *vertex_format)
{
    if (!present_ensure_init())
        return 0;
    return g_draw.setVertexProgram(microcode, length, vertex_format);
}

extern "C" XgpuPlumeGpuDrawResult
xgpu_plume_record_prog_indexed_draw(const XgpuProgIndexedDraw *desc)
{
    if (!desc || !present_ensure_init())
        return XGPU_PLUME_GPU_FALLBACK_RECORD_REJECTION;
    XgpuProgIndexedDraw drawDesc = *desc;
    XgpuPlumeRenderState drawState = desc->render_state
        ? *desc->render_state : XgpuPlumeRenderState{};
    drawState.ui_canvas_active = g_ui_canvas_depth ? 1u : 0u;
    drawState.ui_canvas_mode = g_ui_canvas_mode;
    drawState.ui_canvas_width = g_ui_canvas_width;
    drawState.ui_canvas_height = g_ui_canvas_height;
    drawDesc.render_state = &drawState;
    uint64_t perf_record_t0 = xgpu_plume_perf_begin();
    XgpuPlumeGpuDrawResult result =
        g_draw.recordProgIndexedDraw(g_ctx, drawDesc);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perf_record_t0);
    if (result == XGPU_PLUME_GPU_ACCEPTED) {
        g_frame_dirty = true;
    }
    return result;
}

extern "C" void xgpu_plume_blit_surface(uint32_t dst_guest, uint32_t src_guest,
                                        uint32_t width, uint32_t height)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!present_ensure_init())
        return;
    uint64_t perf_record_t0 = xgpu_plume_perf_begin();
    bool recorded = g_draw.blitSurface(
        g_ctx, dst_guest, src_guest, width, height);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perf_record_t0);
    if (recorded)
        g_frame_dirty = true;
}

extern "C" int xgpu_plume_surface_known(uint32_t guest)
{
    /* The device backbuffer renders through the screen path and may have no
     * registered surface yet; report it known so the HLE alias path never
     * falls through to the version-keyed upload cache (bug-850).  The
     * mirror is minted when the binding arrives. */
    return (g_draw.hasSurface(guest) ||
            (guest && guest == g_draw.presentGuest())) ? 1 : 0;
}

extern "C" int xgpu_plume_color_surface_pending(uint32_t guest)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    /* Diagnostic A/B control: report every surface as pending so all CPU
     * lock/readback callers restore their former unconditional drains. */
    static int legacy_sync = -1;
    if (legacy_sync < 0) {
        const char *value = getenv("XRECOMP_D3D_HLE_SURFACE_LOCK_SYNC");
        legacy_sync = value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
    }
    if (legacy_sync)
        return 1;
    if (!present_ensure_init())
        return 0;
    return g_draw.colorSurfacePendingCpuSync(guest) ? 1 : 0;
}

extern "C" int xgpu_plume_color_surface_sync(uint32_t guest)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!present_ensure_init())
        return 0;
    /* Any CPU read forces the deferred present's downloads to land first. */
    plume_retire_pending_present();
    /* Use the real content serial rather than the diagnostic legacy override. */
    if (!g_draw.colorSurfacePendingCpuSync(guest))
        return 1;
    /* This is a blocking all-slot drain today. Reclaiming a completed slot
     * also packs its queued small-surface download into guest RAM. */
    plume_wait_ring_drain();
    if (!g_draw.colorSurfacePendingCpuSync(guest))
        return 1;
    /* A stale serial can also belong to recorded-but-unsubmitted work. */
    if (g_draw.hasQueuedWork() && !xgpu_plume_wait_for_idle(0))
        return 0;
    plume_wait_ring_drain();
    /* A serial still pending here is not a synchronization failure. Surfaces
     * above the WAIT-download size limit and unknown generations have no pack
     * path, so their serials cannot resolve. The completed drains reproduce
     * the existing synchronized-success contract for those surfaces. */
    return 1;
}

extern "C" int xgpu_plume_download_color_surface(
    uint32_t resource_id, void *pixels, uint32_t width, uint32_t height,
    uint32_t pitch)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!pixels || !present_ensure_init())
        return 0;
    /* Any CPU read forces the deferred present's downloads to land first. */
    plume_retire_pending_present();
    /* Color-content dirtiness advances at record time, so an equal serial
     * proves no recorded or in-flight work can still change this surface;
     * the direct copy below then reads current content without draining
     * unrelated queued render work. */
    if (xgpu_plume_color_surface_pending(resource_id)) {
        if (g_draw.hasQueuedWork() && !xgpu_plume_wait_for_idle(0))
            return 0;
        plume_wait_ring_drain();
    }
    return g_draw.downloadColorSurface(g_ctx, resource_id, pixels, width,
                                       height, pitch) ? 1 : 0;
}

extern "C" int xgpu_plume_download_zeta_surface(
    uint32_t resource_id, void *pixels, uint32_t width, uint32_t height,
    uint32_t pitch)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!pixels || !present_ensure_init())
        return 0;
    /* Depth-write dirtiness is advanced while draws are recorded, before
     * their queued host work executes.  Check it before flushing: an equal
     * serial proves guest memory already holds this zeta generation, whereas
     * any queued depth-writing draw makes the serial stale and retains the
     * synchronization below. */
    if (!g_draw.zetaSurfaceNeedsDownload(
            resource_id, width, height, pitch))
        return 0;
    if (g_draw.hasQueuedWork() && !xgpu_plume_wait_for_idle(0))
        return 0;
    plume_wait_ring_drain();
    return g_draw.downloadZetaSurface(g_ctx, resource_id, pixels, width,
                                      height, pitch) ? 1 : 0;
}

extern "C" int xgpu_plume_bind_zeta_alias(uint32_t stage,
                                          uint32_t zeta_guest,
                                          uint32_t alias_width,
                                          uint32_t alias_height,
                                          uint32_t alias_pitch,
                                          uint32_t unnormalized_coords)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    if (!present_ensure_init())
        return 0;
    return g_draw.bindZetaAliasStage(g_ctx, stage, zeta_guest, alias_width,
                                     alias_height, alias_pitch,
                                     unnormalized_coords) ? 1 : 0;
}

extern "C" int xgpu_plume_upload_color_surface(
    uint32_t resource_id, const void *pixels, uint32_t width,
    uint32_t height, uint32_t pitch)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    bool uploaded;

    if (!pixels || !present_ensure_init())
        return 0;
    if (g_draw.hasQueuedWork() && !xgpu_plume_wait_for_idle(0))
        return 0;
    plume_wait_ring_drain();
    uploaded = g_draw.uploadColorSurface(g_ctx, resource_id, pixels, width,
                                         height, pitch);
    g_surface_sync.recordSurfaceUpload(uploaded, resource_id);
    if (uploaded)
        g_frame_dirty = true;
    return uploaded ? 1 : 0;
}

extern "C" void xgpu_plume_set_active_ps(uint32_t handle)
{
    g_draw.setActivePS(handle);
}

extern "C" void xgpu_plume_set_combiner_consts(const float *values)
{
    if (present_ensure_init())
        g_draw.setCombinerConsts(values);
}

extern "C" void xgpu_plume_set_combiner_consts_ex(const float *values,
                                                   uint32_t float4_count)
{
    if (present_ensure_init())
        g_draw.setCombinerConstsEx(values, float4_count);
}

extern "C" int xgpu_plume_register_vertex_shader(uint32_t handle,
                                                  const char *hlsl,
                                                  uint16_t inputs_read,
                                                  uint16_t outputs_written,
                                                  const XgpuPlumeVertexDeclaration *declaration)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    return present_ensure_init() &&
           g_draw.registerVertexShader(
               handle, hlsl, inputs_read, outputs_written, declaration);
}

extern "C" void xgpu_plume_set_active_vertex_shader(uint32_t handle)
{
    if (present_ensure_init())
        g_draw.setActiveVertexShader(handle);
}

extern "C" void xgpu_plume_set_vertex_shader_constants(
    const float *values, uint32_t float4_count)
{
    if (present_ensure_init())
        g_draw.setVertexShaderConstants(values, float4_count);
}

extern "C" void xgpu_plume_span_begin(uint32_t key)
{
    if (present_ensure_init())
        g_draw.spanBegin(key);
}

extern "C" uint32_t xgpu_plume_span_end(uint32_t key)
{
    if (!present_ensure_init())
        return 0;
    return g_draw.spanEnd(key);
}

extern "C" int xgpu_plume_span_try_replay(uint32_t key)
{
    if (!present_ensure_init())
        return 0;
    return g_draw.spanTryReplay(key) ? 1 : 0;
}

extern "C" void xgpu_plume_span_invalidate(uint32_t key)
{
    if (present_ensure_init())
        g_draw.spanInvalidate(key);
}

extern "C" void xgpu_plume_span_invalidate_mask(uint32_t key_mask,
                                                uint32_t key_bits)
{
    if (present_ensure_init())
        g_draw.spanInvalidateMask(key_mask, key_bits);
}

extern "C" void xgpu_plume_span_invalidate_all(void)
{
    if (present_ensure_init())
        g_draw.spanInvalidateAll();
}

extern "C" uint32_t xgpu_plume_recorded_draw_count(void)
{
    if (!present_ensure_init())
        return 0;
    return g_draw.recordedDrawCount();
}

extern "C" void xgpu_plume_set_vertex_data4f(uint32_t reg,
                                              const float *value)
{
    g_draw.setVertexData4f(reg, value);
}

void xgpu_plume_set_ps_const(uint32_t start, const float *values, uint32_t count)
{
    g_draw.setPSConst(start, values, count);
}

extern "C" void xgpu_plume_set_position_mode(uint32_t mode)
{
    g_position_mode = mode;
}

void xgpu_plume_record_draw(uint32_t primType, uint32_t primCount,
                           const void *verts, uint32_t stride, uint32_t fvf,
                           const XgpuPlumeRenderState *renderState)
{
    XgpuPlumeRenderState drawState = renderState ? *renderState
                                                  : XgpuPlumeRenderState{};
    if (!present_ensure_init())
        return;
    drawState.position_mode = g_position_mode;
    drawState.ui_canvas_active = g_ui_canvas_depth ? 1u : 0u;
    drawState.ui_canvas_mode = g_ui_canvas_mode;
    drawState.ui_canvas_width = g_ui_canvas_width;
    drawState.ui_canvas_height = g_ui_canvas_height;
    uint64_t perf_record_t0 = xgpu_plume_perf_begin();
    g_draw.recordDraw(g_ctx, primType, primCount, verts, stride, fvf,
                      &drawState);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perf_record_t0);
    g_frame_dirty = true;
}

int xgpu_plume_record_indexed_draw(
    uint32_t primType, uint32_t primCount,
    const void *verts, uint32_t vertexCount,
    uint32_t stride, uint32_t fvf,
    const uint32_t *indices, uint32_t indexCount,
    const XgpuPlumeRenderState *renderState)
{
    XgpuPlumeRenderState drawState = renderState ? *renderState
                                                  : XgpuPlumeRenderState{};
    if (!present_ensure_init())
        return 0;
    drawState.position_mode = g_position_mode;
    drawState.ui_canvas_active = g_ui_canvas_depth ? 1u : 0u;
    drawState.ui_canvas_mode = g_ui_canvas_mode;
    drawState.ui_canvas_width = g_ui_canvas_width;
    drawState.ui_canvas_height = g_ui_canvas_height;
    uint64_t perfRecordT0 = xgpu_plume_perf_begin();
    bool recorded = g_draw.recordIndexedDraw(
        g_ctx, primType, primCount, verts, vertexCount, stride, fvf,
        indices, indexCount, &drawState);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perfRecordT0);
    if (recorded)
        g_frame_dirty = true;
    return recorded ? 1 : 0;
}

int xgpu_plume_record_cached_indexed_draw(
    const XgpuPlumeCachedIndexedDraw *draw)
{
    XgpuPlumeCachedIndexedDraw local;
    if (!draw || !present_ensure_init())
        return 0;
    local = *draw;
    if (local.render_state) {
        XgpuPlumeRenderState drawState = *local.render_state;
        drawState.position_mode = g_position_mode;
        drawState.ui_canvas_active = g_ui_canvas_depth ? 1u : 0u;
        drawState.ui_canvas_mode = g_ui_canvas_mode;
        drawState.ui_canvas_width = g_ui_canvas_width;
        drawState.ui_canvas_height = g_ui_canvas_height;
        local.render_state = &drawState;
        if (!g_draw.recordCachedIndexedDraw(g_ctx, local))
            return 0;
    } else if (!g_draw.recordCachedIndexedDraw(g_ctx, local)) {
        return 0;
    }
    g_frame_dirty = true;
    return 1;
}

void xgpu_plume_mesh_cache_invalidate_va(uint32_t resource_data_va)
{
    if (present_ensure_init())
        g_draw.meshCacheInvalidateVa(resource_data_va);
}

int xgpu_plume_present_host_frame(const void *pixels, uint32_t w, uint32_t h,
                                  uint32_t pitch)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    static uint64_t version;
    static bool logged;

    if (!present_ensure_init())
        return 0;
    uint64_t perf_record_t0 = xgpu_plume_perf_begin();
    bool recorded =
        g_draw.queueHostFrame(g_ctx, pixels, w, h, pitch, ++version);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_RECORD, perf_record_t0);
    if (!recorded)
        return 0;
    g_frame_dirty = true;

    if (!logged) {
        fprintf(stderr, "[PLUME] host RGBA frame path active (%ux%u)\n", w, h);
        logged = true;
    }
    g_pending_present_reason = PLUME_PRESENT_HOST_FRAME;
    xgpu_plume_present_frame();
    return 1;
}

extern "C" void xgpu_plume_retire_host_frame(void)
{
    /* Owner-state/RHI entry: finish any in-flight worker job first. */
    xgpu::plume::plume_render_worker_sync();
    const bool retired = g_draw.retireStickyHostFrame();
    g_surface_sync.recordHostFrameRetired(retired);
    if (retired)
        g_frame_dirty = true;
}

void xgpu_plume_present_frame(void)
{
    XRECOMP_TRACY_ZONE_SCOPED("Plume Present");
    XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Present");
    if (!present_ensure_init())
        return;
    const uint32_t present_reason = g_pending_present_reason;
    g_pending_present_reason = PLUME_PRESENT_UNKNOWN;
    if (!xgpu::plume::plume_render_worker_run_present(present_reason))
        xgpu_plume_owner_execute_present(present_reason);
}

/* Owner-side body of a present request: F2 bookkeeping, skip/defer gating,
 * swapchain acquire, present-slot replay, output scaling, the synchronous
 * submit-and-present, download completion, and wait-ring reclamation.
 * Runs inline on the guest thread today; the stage-3 render worker calls
 * exactly this function on the owner thread (ownership doc, rollout
 * stages). Reads and writes render-owner state only. */
void xgpu_plume_owner_execute_present(uint32_t present_reason)
{
    /* F2 draw-stream capture: user-triggered, bounded (plume_f2_capture.h). */
    xgpu_plume_f2_poll();
    xgpu_plume_f2_present_begin();
    const uint32_t f2_queued = (uint32_t)g_draw.queuedCount();
    /* Snapshot every guest-latched value used by this job before the
     * inputs-consumed handshake lets the guest record the next frame. */
    const uint32_t present_guest = g_draw.presentGuest();

    const bool has_queued_work = g_draw.hasQueuedWork();
    const bool uploaded_present_surface =
        g_surface_sync.hasUploadedSurface(present_guest);
    if (uploaded_present_surface)
        g_draw.retireStickyHostFrame();
    const bool needs_sticky_frame = g_draw.needsStickyHostFrame();
    const bool sync_needs_present = g_surface_sync.needsPresent(
        has_queued_work, needs_sticky_frame);

    /* Skip host presents that would drain nothing useful. */
    if (!g_frame_dirty || !sync_needs_present) {
        if (g_frame_dirty && !g_draw.hasQueuedWork() &&
            !g_draw.needsStickyHostFrame())
            g_frame_dirty = false;
        timing_trace_present(
            XRECOMP_TIMING_EVENT_PRESENT_SKIPPED, present_reason,
            (g_frame_dirty ? 1u : 0u)
                | (has_queued_work ? 2u : 0u)
                | (needs_sticky_frame ? 4u : 0u)
                | (sync_needs_present ? 8u : 0u),
            f2_queued, present_guest);
        xgpu_plume_f2_present(0, "skip", present_guest, f2_queued);
        return;
    }

    /* Defer offscreen-only queues: keep draws until a later flip that also
     * targeted the latched present surface. Avoids flashing mid-RTT without
     * coalescing multiple full frames across vblank. */
    if (g_draw.hasQueuedWork() && present_guest != 0 &&
        g_draw.drawsToPresentSurface() == 0) {
        timing_trace_present(
            XRECOMP_TIMING_EVENT_PRESENT_DEFERRED, present_reason,
            (g_frame_dirty ? 1u : 0u)
                | (has_queued_work ? 2u : 0u)
                | (needs_sticky_frame ? 4u : 0u)
                | (sync_needs_present ? 8u : 0u),
            f2_queued, present_guest);
        xgpu_plume_f2_present(0, "defer_offscreen", present_guest,
                              f2_queued);
        /* Keep dirty + queue for the next present-surface flip. */
        return;
    }

    /* Retire the previous deferred present before the swapchain (whose
     * acquire-failure path resizes) or any present-submission resource is
     * touched again. A full guest frame has elapsed, so this wait is
     * normally already signaled. */
    plume_retire_pending_present();

    uint32_t idx = 0;
    if (!g_ctx.acquire(&idx)) {
        timing_trace_present(
            XRECOMP_TIMING_EVENT_PRESENT_SKIPPED, present_reason,
            0x100u, f2_queued, present_guest);
        return;
    }
    if (!present_ensure_scene_target()) {
        timing_trace_present(
            XRECOMP_TIMING_EVENT_PRESENT_SKIPPED, present_reason,
            0x200u, f2_queued, present_guest);
        return;
    }
    const bool capture_source = f2_readback_prepare();

    plume::RenderCommandList *cl = g_ctx.cmdList();
    cl->begin();
    plume::RenderTexture *swapTexture = g_ctx.swapChain()->getTexture(idx);
    cl->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(g_scene_texture.get(),
                                             plume::RenderTextureLayout::COLOR_WRITE));
    cl->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(g_scene_depth.get(),
                                             plume::RenderTextureLayout::DEPTH_WRITE));
    cl->setFramebuffer(g_scene_framebuffer.get());
    const uint32_t w = g_scene_width;
    const uint32_t h = g_scene_height;
    cl->setViewports(plume::RenderViewport(0.0f, 0.0f, float(w), float(h)));
    cl->setScissors(plume::RenderRect(0, 0, w, h));
    /* Native-size host movies occupy only their centered content rectangle.
     * Clear every held host frame to black so the uncovered output columns
     * are deterministic letterbox bars rather than stale scene pixels. */
    if (g_draw.hasStickyHostFrame())
        cl->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    else if (g_clear_pending || !g_scene_initialized)
        cl->clearColor(0, plume::RenderColor(g_clear[0], g_clear[1],
                                             g_clear[2], g_clear[3]));
    if (!g_scene_initialized)
        cl->clearDepth(true, 1.0f);

    /* The present submission is synchronous (submitAndPresent waits its fence),
     * so it uses its own dedicated upload buffers. */
    g_draw.setCurrentSub(xgpu::plume::PlumeDraw::kPresentSub);
    g_draw.materializeRecordedSurfaces(g_ctx);
    g_draw.ensureStickyHostFrame(g_ctx);
    g_draw.ensurePresentSurfaceComposite(g_ctx);
    if (g_debug_overlay_provider_count) {
        std::array<uint32_t, XGPU_PLUME_MAX_DEBUG_OVERLAY_PROVIDERS> order = {};
        for (uint32_t index = 0; index < g_debug_overlay_provider_count;
             ++index) {
            uint32_t position = index;
            order[index] = index;
            while (position > 0) {
                const DebugOverlayRegistration &left =
                    g_debug_overlay_providers[order[position - 1u]];
                const DebugOverlayRegistration &right =
                    g_debug_overlay_providers[order[position]];
                if (left.layer < right.layer ||
                    (left.layer == right.layer && left.slot < right.slot))
                    break;
                const uint32_t swap = order[position - 1u];
                order[position - 1u] = order[position];
                order[position] = swap;
                --position;
            }
        }
        for (uint32_t position = 0;
             position < g_debug_overlay_provider_count; ++position) {
            const DebugOverlayRegistration &entry =
                g_debug_overlay_providers[order[position]];
            XgpuPlumeDebugOverlayFrame overlay = {};
            if (!entry.provider(&overlay))
                continue;
            if (overlay.space == XGPU_PLUME_DEBUG_OVERLAY_SPACE_LOGICAL &&
                xgpu::plume::plumeDebugOverlayFrameValid(
                    overlay, XGPU_PANEL_WIDTH, XGPU_PANEL_HEIGHT)) {
                g_draw.queueHostOverlay(g_ctx, overlay, entry.slot);
            } else if (overlay.space == XGPU_PLUME_DEBUG_OVERLAY_SPACE_HOST &&
                       xgpu::plume::plumeDebugOverlayFrameValid(
                           overlay, g_ctx.width(), g_ctx.height())) {
                g_draw.queueHostOutputOverlay(g_ctx, overlay, entry.slot);
            }
        }
    }
    uint64_t perf_replay_t0 = xgpu_plume_perf_begin();
    g_draw.replay(g_ctx, cl, g_scene_texture.get(),
                  g_scene_framebuffer.get(), true, true);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_REPLAY, perf_replay_t0);
    g_draw.recordSurfaceDownloads(g_ctx, cl);
    g_scene_initialized = true;
    /* Recording and guest-latched inputs are fully consumed; an async
     * guest may resume recording the next frame while the GPU tail
     * (output scale, submit, present fence, downloads) completes here. */
    xgpu::plume::plume_render_worker_mark_inputs_consumed();

    bool captured_source = false;
    if (capture_source) {
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        plume::RenderTexture *source = g_draw.resolvedPresentSurface(
            &source_width, &source_height);
        if (source && source_width == g_output_width &&
            source_height == g_output_height) {
            const plume::RenderTextureCopyLocation readback_dst =
                plume::RenderTextureCopyLocation::PlacedFootprint(
                    g_f2_readback.source_readback.get(),
                    plume::RenderFormat::B8G8R8A8_UNORM,
                    w, h, 1, g_f2_readback.readback_pitch / 4u);
            const plume::RenderTextureCopyLocation readback_src =
                plume::RenderTextureCopyLocation::Subresource(source, 0, 0);
            cl->copyTextureRegion(readback_dst, readback_src);
            captured_source = true;
        }
    }

    const plume::RenderTextureBarrier scaleBarriers[2] = {
        plume::RenderTextureBarrier(g_scene_texture.get(),
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(swapTexture,
                                    plume::RenderTextureLayout::COLOR_WRITE),
    };
    cl->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0,
                 scaleBarriers, 2);
    if (!g_draw.recordOutputScale(
            g_ctx, cl, g_scene_texture.get(), g_scene_view.get(),
            g_ctx.framebuffer(idx), g_ctx.width(), g_ctx.height())) {
        fprintf(stderr, "[PLUME] output scale recording failed\n");
        cl->setFramebuffer(g_ctx.framebuffer(idx));
        cl->setViewports(plume::RenderViewport(
            0.0f, 0.0f, float(g_ctx.width()), float(g_ctx.height())));
        cl->setScissors(plume::RenderRect(
            0, 0, g_ctx.width(), g_ctx.height()));
        cl->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    }
    if (!g_draw.recordHostOutputOverlays(
            g_ctx, cl, g_ctx.framebuffer(idx),
            g_ctx.width(), g_ctx.height()))
        fprintf(stderr, "[PLUME] host overlay recording failed\n");
    cl->barriers(plume::RenderBarrierStage::NONE,
                 plume::RenderTextureBarrier(swapTexture,
                                             plume::RenderTextureLayout::PRESENT));
    cl->end();

    /* F2 source captures must complete synchronously (the capture maps its
     * readback immediately). Ordinary surface downloads are deferred one
     * present instead: the proven per-frame consumer is MM3's exposure meter
     * (mm3_lum_exposure_update @ 0x2305D1), which reads its 4-deep luminance
     * ring at a 3-frame distance into a 0.15/frame exponential filter, and
     * the plume CPU-read entry points retire the deferred present first as a
     * fail-safe for any other consumer. */
    const bool pipelined_present = plume_present_pipeline_enabled() &&
                                   !capture_source;
    xgpu_plume_perf_record_host_present();
    {
        XRECOMP_TRACY_ZONE_SCOPED("Plume Submit and Present");
        XRECOMP_CPU_RECORDER_ZONE_SCOPED("Plume Submit and Present");
        if (pipelined_present) {
            uint32_t mask = 0;
            for (uint32_t i = 0; i < xgpu::plume::PlumeDraw::kWaitRingSize;
                 i++) {
                if (g_wait_ring[i].inFlight)
                    mask |= 1u << i;
            }
            g_present_ring_snapshot = mask;
            g_ctx.submitAndPresentDeferred(idx);
            g_draw.deferSurfaceDownloads();
            xgpu_plume_wait_stats_present_class(
                XGPU_PLUME_PRESENT_CLASS_PIPELINED);
        } else {
            g_ctx.submitAndPresent(idx);
            xgpu_plume_wait_stats_present_class(
                !plume_present_pipeline_enabled()
                    ? XGPU_PLUME_PRESENT_CLASS_SYNC_OFF
                    : (capture_source
                           ? XGPU_PLUME_PRESENT_CLASS_SYNC_CAPTURE
                           : XGPU_PLUME_PRESENT_CLASS_SYNC_DOWNLOADS));
        }
    }
    timing_trace_present(
        XRECOMP_TIMING_EVENT_PRESENT_ISSUED, present_reason,
        (g_frame_dirty ? 1u : 0u)
            | (has_queued_work ? 2u : 0u)
            | (needs_sticky_frame ? 4u : 0u)
            | (sync_needs_present ? 8u : 0u),
        f2_queued, present_guest);
    XRECOMP_TRACY_FRAME_MARK("Host Present");
    XRECOMP_TRACY_PLOT_I("Plume queued draws", f2_queued);
    xgpu_plume_frametime_present();
    xrecomp_cpu_recorder_present_boundary();
    if (!pipelined_present) {
        g_draw.completeSurfaceDownloads();
        if (captured_source)
            f2_log_readback_avg("s4-final",
                                g_f2_readback.source_readback.get(), 0, 0);
        g_draw.releaseSubmittedResources();
        /*
         * The present fence guarantees all earlier WAIT submissions
         * completed. Reclaim their per-submission resources before accepting
         * another frame. (The pipelined path defers all of this to
         * plume_retire_pending_present at the next present.)
         */
        plume_wait_ring_drain();
    }
    g_surface_sync.recordPresent();
    xgpu_plume_f2_present(1, plume_present_reason_name(present_reason),
                          present_guest, f2_queued);
    g_frame_dirty = false;
    g_clear_pending = false;

    ++g_frames;
    /*
     * The direct D3D HLE route bypasses NV097_FLIP_STALL, so it must close
     * the same aggregate performance frame at its accepted host present.
     * Raw PGRAPH NO_WAIT presents are marked with the guest frame number by
     * NV097_FLIP_STALL after this function returns.
     */
    if (present_reason != PLUME_PRESENT_NO_WAIT) {
        xgpu_plume_perf_mark_present(g_frames);
        xgpu_plume_perf_flush_pending();
    }

}
