/**
 * Xbox D3D8 compatibility device backed exclusively by the Plume RHI.
 *
 * Guest resources and state remain expressed in Xbox terms. This translation
 * unit snapshots them into the platform-neutral renderer contract; backend
 * selection and native graphics API ownership stay below that boundary.
 */

#include "d3d8_internal.h"
#include "d3d8_cpu_surface_sync.h"
#include "d3d8_index_range.h"
#include "d3d_hle_guest.h"
#include "kernel/xbox_memory_layout.h"
#include "plume/plume_f2_capture.h"
#include "plume/plume_host.h"
#include "platform/host_events.h"
#include "platform/cpu_recorder.h"
#include "xgpu_renderer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3D8_PANEL_WIDTH XGPU_PANEL_WIDTH
#define D3D8_PANEL_HEIGHT XGPU_PANEL_HEIGHT
#define MAX_RENDER_STATES 256
#define MAX_TEXTURE_STAGES 4
#define MAX_TSS_STATES 32
#define MAX_TRANSFORMS 512
#define MAX_LIGHTS 8
#define PGRAPH_MAX_SURFACES 32
#define PGRAPH_MAX_TEXTURES 512
#define D3DFVF_POSITION_MASK 0x00eu

enum {
    PLUME_PRESENT_FRAME = 1,
    PLUME_PRESENT_NO_WAIT = 2,
    PLUME_PRESENT_DEVICE = 3,
    PLUME_PRESENT_SWAP = 4,
    PLUME_PRESENT_VBLANK = 6,
};

extern void xgpu_plume_set_present_reason(uint32_t reason);

typedef struct D3D8DeviceState {
    void *native_window;
    UINT width;
    UINT height;
    D3DFORMAT backbuffer_format;
    DWORD render_states[MAX_RENDER_STATES];
    DWORD tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    BOOL fixed_state_valid;
    D3DMATRIX transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8 viewport;
    D3DMATERIAL8 material;
    D3DLIGHT8 lights[MAX_LIGHTS];
    BOOL light_enable[MAX_LIGHTS];
    DWORD vertex_shader;
    DWORD pixel_shader;
    BOOL in_scene;
    BOOL pgraph_scissor_enable;
    UINT pgraph_scissor_x;
    UINT pgraph_scissor_y;
    UINT pgraph_scissor_width;
    UINT pgraph_scissor_height;
    LONG ref_count;
    /* The swap-chain backbuffer is distinct from the currently bound render
     * target. SetRenderTarget may replace the latter with an offscreen surface,
     * while Present/Swap and GetBackBuffer must continue to address the
     * device-owned backbuffer. */
    D3D8Surface *back_buffer;
    /* Xbox GetBackBuffer(-1) exposes the displayed front buffer. Keep a
     * separate surface snapshot rather than aliasing it to back buffer 0. */
    D3D8Surface *front_buffer;
    D3D8Surface *render_target;
    D3D8Surface *depth_surface;
} D3D8DeviceState;

typedef struct PgraphHostSurface {
    uint32_t offset;
    UINT width;
    UINT height;
    /* Allocated extent of the host image, which the SET_SURFACE_CLIP rectangle
     * above is only a sub-rectangle of: the pitch gives the true row stride and
     * the clip carries a Y origin. Plume allocates from these (plume_draw.cpp),
     * so a readback must use them too or it resolves a sub-rectangle and any
     * texture aliasing the buffer samples zero. Width/height stay the clip
     * values -- the present-surface heuristic compares them to the panel. */
    UINT image_width;
    UINT image_height;
    UINT pitch;
    uint32_t format;
    uint32_t layout;
    uint64_t cpu_hash;
    BOOL cpu_hash_valid;
    BOOL cpu_lock_dirty;
    /* Sync epoch this surface was last fingerprinted in; 0 means never.
     * See g_cpu_surface_epoch and sync_cpu_surface. */
    uint32_t cpu_hash_epoch;
} PgraphHostSurface;

typedef struct PgraphHostTexture {
    uint32_t offset;
    UINT width;
    UINT height;
    D3DFORMAT format;
    UINT last_used;
    BYTE *pixels;
    UINT bytes;
    uint64_t version;
} PgraphHostTexture;

static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized;
static volatile LONG64 g_texture_version;
static volatile LONG g_surface_id;
static IDirect3DVertexBuffer8 *g_cur_vb;
static UINT g_cur_vb_stride;
static IDirect3DIndexBuffer8 *g_cur_ib;
static UINT g_cur_ib_base_vertex;
static IDirect3DBaseTexture8 *g_cur_textures[MAX_TEXTURE_STAGES];
static PgraphHostSurface g_pgraph_surfaces[PGRAPH_MAX_SURFACES];

/* Rendered DEPTH surfaces, tracked separately from colour: a guest texture can
 * alias the depth buffer (MM3 samples its Z24S8 buffer as a Y16 luminance
 * texture), and the colour list cannot satisfy that. Only the geometry needed
 * to resolve the image back to guest memory is kept. */
typedef struct PgraphHostZeta {
    uint32_t offset;
    UINT width;
    UINT height;
    UINT pitch;
} PgraphHostZeta;
static PgraphHostZeta g_pgraph_zeta[PGRAPH_MAX_SURFACES];
static UINT g_pgraph_zeta_count;
static PgraphHostTexture g_pgraph_textures[PGRAPH_MAX_TEXTURES];
static UINT g_pgraph_surface_count;
static UINT g_pgraph_texture_count;
static UINT g_pgraph_texture_clock;
static PgraphHostSurface *g_pgraph_current_surface;
static PgraphHostSurface *g_pgraph_present_surface;
static D3D8VblankScanoutCallback g_vblank_scanout_callback;

/* Advances once per present. Bounds how stale a hosted surface's CPU-side
 * fingerprint may be: guest code can write a surface between presents, but not
 * during the pushbuffer drain that issues a batch of draws, so every draw in a
 * batch can share one hash. Starts at 1 so a zeroed cpu_hash_epoch means
 * "never hashed". */
static uint32_t g_cpu_surface_epoch = 1;

static DWORD g_begin_count;
static DWORD g_end_count;
static DWORD g_clear_count;
static volatile LONG g_draw_count;
static volatile LONG g_guest_present_count;
static DWORD g_transform_count;
static DWORD g_state_count;
static DWORD g_texture_count;

void d3d8_SetVblankScanoutCallback(D3D8VblankScanoutCallback callback)
{
    g_vblank_scanout_callback = callback;
}

int d3d8_VblankScanout(void)
{
    return g_vblank_scanout_callback
        ? g_vblank_scanout_callback()
        : 0;
}

uint32_t d3d8_HlePresentCount(void)
{
    return (uint32_t)InterlockedCompareExchange(&g_guest_present_count, 0, 0);
}

uint32_t d3d8_HleDrawCount(void)
{
    return (uint32_t)InterlockedCompareExchange(&g_draw_count, 0, 0);
}

static UINT panel_width(void)
{
    return g_device_state.width ? g_device_state.width : D3D8_PANEL_WIDTH;
}

static UINT panel_height(void)
{
    return g_device_state.height ? g_device_state.height : D3D8_PANEL_HEIGHT;
}

static PgraphHostSurface *pgraph_fallback_surface(void)
{
    if (g_pgraph_present_surface)
        return g_pgraph_present_surface;
    if (g_pgraph_current_surface &&
        g_pgraph_current_surface->width == panel_width() &&
        g_pgraph_current_surface->height == panel_height())
        return g_pgraph_current_surface;
    return NULL;
}

static void present_with_reason(uint32_t reason)
{
    /* PCRTC fallback presents must not make the guest-present stream look
     * active or they would continually postpone their own next refresh. */
    if (reason != PLUME_PRESENT_VBLANK)
        InterlockedIncrement(&g_guest_present_count);
    xgpu_plume_set_present_reason(reason);
    xgpu_plume_present_frame();
}

static void present_device_backbuffer_with_reason(uint32_t reason)
{
    if (g_device_state.back_buffer) {
        if (g_device_state.front_buffer &&
            g_device_state.front_buffer != g_device_state.back_buffer) {
            XgpuSurfaceBinding destination;
            /* Model the front/back flip without rotating guest-visible COM
             * objects: preserve the frame being presented in the dedicated
             * front-buffer surface before the host swapchain advances. */
            memset(&destination, 0, sizeof(destination));
            destination.color_resource =
                g_device_state.front_buffer->resource_id;
            destination.width = g_device_state.front_buffer->width;
            destination.height = g_device_state.front_buffer->height;
            destination.image_width = destination.width;
            destination.image_height = destination.height;
            destination.color_pitch = g_device_state.front_buffer->pitch;
            destination.color_format =
                (uint32_t)g_device_state.front_buffer->format;
            destination.layout = XGPU_SURFACE_PITCH;
            destination.sample_count = 1;
            xgpu_plume_blit_surface(
                &destination,
                g_device_state.front_buffer->guest_address,
                g_device_state.back_buffer->resource_id,
                g_device_state.back_buffer->guest_address);
        }
        xgpu_plume_set_present_surface(
            g_device_state.back_buffer->resource_id);
    }
    present_with_reason(reason);
}

uint64_t d3d8_plume_next_texture_version(void)
{
    return (uint64_t)InterlockedIncrement64(&g_texture_version);
}

static UINT texture_byte_size(D3DFORMAT format, UINT width, UINT height,
                              UINT row_pitch)
{
    if (format == D3DFMT_DXT1)
        return ((width + 3u) / 4u) * ((height + 3u) / 4u) * 8u;
    if (format == D3DFMT_DXT3 || format == D3DFMT_DXT5)
        return ((width + 3u) / 4u) * ((height + 3u) / 4u) * 16u;
    return row_pitch * height;
}

static void mirror_texture(uint32_t stage, IDirect3DBaseTexture8 *base)
{
    D3D8Texture *texture;
    XgpuTextureBinding binding;
    BYTE *snapshot;

    if (!base) {
        d3d8_combiners_set_texture_binding(stage, 2, FALSE, FALSE, 0);
        xgpu_plume_set_texture(stage, 0, NULL, 0, 0, 0, 0, 0, 0);
        return;
    }
    texture = (D3D8Texture *)base;
    if (!texture->sys_mem || !texture->width || !texture->height)
        return;
    snapshot = d3d8_texture_make_upload_snapshot(texture);
    if (!snapshot)
        return;

    memset(&binding, 0, sizeof(binding));
    binding.stage = stage;
    binding.guest_ptr = (uint32_t)(uintptr_t)texture;
    binding.pixels = snapshot;
    binding.width = texture->width;
    binding.height = texture->height;
    binding.depth = 1;
    binding.levels = texture->levels;
    binding.dimensionality = 2;
    binding.pitch = texture->pitch;
    binding.bytes = (uint32_t)texture->data_size;
    binding.format = (uint32_t)texture->d3d8_format;
    binding.version = texture->plume_version;
    d3d8_combiners_set_texture_binding(
        stage, 2, FALSE, texture->d3d8_format == (D3DFORMAT)0x35u,
        (UINT)texture->d3d8_format);
    switch (texture->d3d8_format) {
    case D3DFMT_LIN_A8R8G8B8:
    case D3DFMT_LIN_X8R8G8B8:
    case D3DFMT_LIN_R5G6B5:
    case D3DFMT_LIN_A1R5G5B5:
    case D3DFMT_LIN_A4R4G4B4:
        binding.unnormalized_coords = 1;
        break;
    default:
        break;
    }
    xgpu_plume_set_texture_ex(&binding);
    free(snapshot);
}

void d3d8_plume_on_texture_unlock(IDirect3DTexture8 *iface)
{
    DWORD stage;
    if (!iface)
        return;
    for (stage = 0; stage < MAX_TEXTURE_STAGES; ++stage) {
        if (g_cur_textures[stage] == (IDirect3DBaseTexture8 *)iface)
            mirror_texture(stage, (IDirect3DBaseTexture8 *)iface);
    }
}

static PgraphHostSurface *find_surface(uint32_t offset)
{
    UINT i;
    for (i = 0; i < g_pgraph_surface_count; ++i)
        if (g_pgraph_surfaces[i].offset == offset)
            return &g_pgraph_surfaces[i];
    return NULL;
}

static PgraphHostSurface *find_surface_within(uint32_t address)
{
    PgraphHostSurface *best = NULL;
    uint64_t best_span = UINT64_MAX;
    UINT i;
    for (i = 0; i < g_pgraph_surface_count; ++i) {
        PgraphHostSurface *surface = &g_pgraph_surfaces[i];
        uint64_t span = (uint64_t)surface->pitch * surface->height;
        uint64_t end = (uint64_t)surface->offset + span;
        if (address >= surface->offset && (uint64_t)address < end &&
            span < best_span) {
            best = surface;
            best_span = span;
        }
    }
    return best;
}

void d3d8_PgraphMarkCpuSurfaceLock(uint32_t guest_address,
                                   uint32_t lock_flags,
                                   int preserve_scanout)
{
    PgraphHostSurface *surface;
    uint32_t offset;

    if (!d3d8_cpu_surface_lock_is_writable(lock_flags))
        return;

    offset = d3d8_cpu_surface_physical_offset(guest_address);
    surface = find_surface(offset);
    if (!surface)
        surface = find_surface_within(offset);
    if (surface) {
        size_t active_row_bytes = (size_t)surface->width * 4u;
        size_t span;
        BYTE *pixels = NULL;
        BOOL inherited = FALSE;

        if (surface->layout == XGPU_SURFACE_PITCH &&
            d3d8_cpu_surface_format_is_32bpp(surface->format) &&
            surface->width && surface->height &&
            active_row_bytes <= surface->pitch &&
            (size_t)(surface->height - 1u) <=
                (SIZE_MAX - active_row_bytes) / surface->pitch) {
            span = (size_t)(surface->height - 1u) * surface->pitch +
                   active_row_bytes;
            pixels = xbox_guest_phys_ptr(surface->offset, span);
        }

        /* COPY swap chains preserve the displayed frame when the title moves
         * its writable CPU backbuffer to another full-panel allocation. Read
         * that frame straight into guest memory before the partial CPU update;
         * the ordinary vblank upload below remains the sole GPU owner change. */
        if (pixels && preserve_scanout && g_pgraph_present_surface &&
            surface != g_pgraph_present_surface &&
            surface->width == panel_width() &&
            surface->height == panel_height() &&
            g_pgraph_present_surface->width == surface->width &&
            g_pgraph_present_surface->height == surface->height &&
            xgpu_plume_download_color_surface(
                g_pgraph_present_surface->offset, pixels, surface->width,
                surface->height, surface->pitch)) {
            g_pgraph_present_surface = surface;
            inherited = TRUE;
        }

        if (pixels && !inherited &&
            d3d8_cpu_surface_lock_needs_readback(
                lock_flags, xgpu_plume_surface_known(surface->offset))) {
            inherited = xgpu_plume_download_color_surface(
                        surface->offset, pixels, surface->width,
                        surface->height, surface->pitch) != 0;
        }
        if (inherited) {
            D3D8CpuSurfaceFingerprint current =
                d3d8_cpu_surface_fingerprint(
                    pixels, surface->width, surface->height, surface->pitch);
            surface->cpu_hash = current.hash;
            surface->cpu_hash_valid = TRUE;
        }
        surface->cpu_lock_dirty = TRUE;
    }

}

static int sync_cpu_surface(PgraphHostSurface *surface,
                            int allow_initial_upload)
{
    const BYTE *pixels;
    D3D8CpuSurfaceFingerprint current;
    size_t active_row_bytes;
    size_t span;
    int needs_upload;

    if (!surface ||
        surface->layout != XGPU_SURFACE_PITCH ||
        !d3d8_cpu_surface_format_is_32bpp(surface->format) ||
        !surface->width || !surface->height)
        return 0;

    active_row_bytes = (size_t)surface->width * 4u;
    if (active_row_bytes > surface->pitch)
        return 0;
    if ((size_t)(surface->height - 1u) >
        (SIZE_MAX - active_row_bytes) / surface->pitch)
        return 0;
    span = (size_t)(surface->height - 1u) * surface->pitch +
           active_row_bytes;
    pixels = xbox_guest_phys_ptr(surface->offset, span);
    if (!pixels)
        return 0;

    /* The fingerprint below hashes the whole surface a byte at a time — 1.2 MB
     * for a 640x480 target, about 0.9 ms. It used to run on every draw, and at
     * MM3's ~200 draws a frame that was ~250 MB of hashing per frame and 79% of
     * total frame time.
     *
     * It exists to notice guest CPU writes into a hosted surface (the loading
     * meter's RAW overlay). Those writes come from guest code, which cannot run
     * while the pushbuffer drain issuing a batch of draws is executing, so the
     * many per-draw calls within one batch can share a single hash.
     *
     * The epoch advances on every present, NOT on guest flips: the loading
     * screen issues no flips at all — it presents from the PCRTC vblank
     * fallback — so a flip-scoped stamp froze there and the animation stopped
     * (bug-543). Presents are also the cadence at which that overlay is
     * consumed, so one hash per present is both correct and sufficient. */
    if (!surface->cpu_lock_dirty &&
        surface->cpu_hash_epoch == g_cpu_surface_epoch)
        return 0;
    surface->cpu_hash_epoch = g_cpu_surface_epoch;

    current = d3d8_cpu_surface_fingerprint(
        pixels, surface->width, surface->height, surface->pitch);
    /* Binding a render target establishes GPU ownership. Nonzero bytes in a
     * newly reused allocation are not proof of a CPU write; uploading them
     * here restores stale color or packed depth over the new target. A
     * present/texture read may still seed an otherwise untouched surface. */
    if (!surface->cpu_hash_valid && !surface->cpu_lock_dirty &&
        !allow_initial_upload) {
        surface->cpu_hash = current.hash;
        surface->cpu_hash_valid = TRUE;
        return 0;
    }
    needs_upload = d3d8_cpu_surface_needs_upload(
        surface->cpu_hash_valid, surface->cpu_hash, current,
        surface->cpu_lock_dirty);

    if (!needs_upload) {
        surface->cpu_hash = current.hash;
        surface->cpu_hash_valid = TRUE;
        return 0;
    }

    if (!xgpu_plume_upload_color_surface(
            surface->offset, pixels, surface->width, surface->height,
            surface->pitch))
        return 0;

    surface->cpu_hash = current.hash;
    surface->cpu_hash_valid = TRUE;
    surface->cpu_lock_dirty = FALSE;
    return 1;
}

static void prepare_draw(void)
{
    uint32_t color_write_mask =
        g_device_state.render_states[D3DRS_COLORWRITEENABLE] & 0xFu;

    /* A CPU lock may complete between draw batches while the render target
     * remains bound. Consume it before recording the next GPU write; waiting
     * until Present would restore the older CPU snapshot over later draws.
     * A fresh target also needs its guest bytes when the draw preserves any
     * channel: MM3 keeps its screen-space light mask in destination alpha
     * while clearing and drawing RGB only. */
    sync_cpu_surface(g_pgraph_current_surface,
                     color_write_mask != 0xFu);
    (void)d3d8_vsh_prepare_draw(g_device_state.vertex_shader);
    (void)d3d8_combiners_prepare_draw();
}

static void rebaseline_cpu_surfaces_after_gpu_download(uint32_t start,
                                                       uint64_t bytes)
{
    uint64_t want_end = (uint64_t)start + (bytes ? bytes : 1u);
    UINT i;

    for (i = 0; i < g_pgraph_surface_count; ++i) {
        PgraphHostSurface *surface = &g_pgraph_surfaces[i];
        size_t active_row_bytes;
        uint64_t span;
        uint64_t end;
        BYTE *pixels;

        if (!surface->offset || surface->layout != XGPU_SURFACE_PITCH ||
            !d3d8_cpu_surface_format_is_32bpp(surface->format) ||
            !surface->width || !surface->height || !surface->pitch)
            continue;
        active_row_bytes = (size_t)surface->width * 4u;
        if (active_row_bytes > surface->pitch)
            continue;
        span = (uint64_t)(surface->height - 1u) * surface->pitch +
               active_row_bytes;
        end = (uint64_t)surface->offset + span;
        if (end <= (uint64_t)start || (uint64_t)surface->offset >= want_end)
            continue;
        pixels = xbox_guest_phys_ptr(surface->offset, (size_t)span);
        if (!pixels)
            continue;
        D3D8CpuSurfaceFingerprint current = d3d8_cpu_surface_fingerprint(
            pixels, surface->width, surface->height, surface->pitch);
        surface->cpu_hash = current.hash;
        surface->cpu_hash_valid = TRUE;
        surface->cpu_hash_epoch = g_cpu_surface_epoch;
    }
}

static int download_color_surface_to_guest(PgraphHostSurface *surface)
{
    UINT image_width;
    UINT image_height;
    size_t active_row_bytes;
    size_t span;
    BYTE *pixels;

    if (!surface || !surface->offset ||
        surface->layout != XGPU_SURFACE_PITCH ||
        !d3d8_cpu_surface_format_is_32bpp(surface->format) ||
        !surface->width || !surface->height || !surface->pitch ||
        !xgpu_plume_surface_known(surface->offset))
        return 0;

    image_width = surface->image_width ? surface->image_width : surface->width;
    image_height = surface->image_height ? surface->image_height
                                         : surface->height;
    active_row_bytes = (size_t)image_width * 4u;
    if (active_row_bytes > surface->pitch ||
        (size_t)(image_height - 1u) >
            (SIZE_MAX - active_row_bytes) / surface->pitch)
        return 0;
    span = (size_t)(image_height - 1u) * surface->pitch + active_row_bytes;
    pixels = xbox_guest_phys_ptr(surface->offset, span);
    if (!pixels || !xgpu_plume_download_color_surface(
                       surface->offset, pixels, image_width, image_height,
                       surface->pitch))
        return 0;

    rebaseline_cpu_surfaces_after_gpu_download(surface->offset, span);
    return 1;
}

void d3d8_PgraphResourcesShutdown(void)
{
    UINT i;
    for (i = 0; i < g_pgraph_texture_count; ++i)
        free(g_pgraph_textures[i].pixels);
    memset(g_pgraph_surfaces, 0, sizeof(g_pgraph_surfaces));
    memset(g_pgraph_textures, 0, sizeof(g_pgraph_textures));
    g_pgraph_surface_count = 0;
    g_pgraph_texture_count = 0;
    g_pgraph_texture_clock = 0;
    g_pgraph_current_surface = NULL;
    g_pgraph_present_surface = NULL;
}

int d3d8_PgraphSetRenderTarget(const XgpuSurfaceBinding *binding)
{
    PgraphHostSurface *surface;
    uint32_t offset;
    UINT pitch;
    UINT image_width;
    UINT image_height;
    BOOL metadata_changed;
    if (!binding || binding->color_resource > UINT32_MAX ||
        !binding->color_resource || !binding->width || !binding->height)
        return 0;
    offset = (uint32_t)binding->color_resource;
    pitch = binding->color_pitch ? binding->color_pitch :
                                  binding->width * 4u;
    surface = find_surface(offset);
    if (!surface) {
        if (g_pgraph_surface_count >= PGRAPH_MAX_SURFACES)
            return 0;
        surface = &g_pgraph_surfaces[g_pgraph_surface_count++];
    }
    image_width = binding->image_width ? binding->image_width : binding->width;
    image_height =
        binding->image_height ? binding->image_height : binding->height;
    metadata_changed =
        surface->offset != offset || surface->width != binding->width ||
        surface->height != binding->height || surface->pitch != pitch ||
        surface->image_width != image_width ||
        surface->image_height != image_height ||
        surface->format != binding->color_format ||
        surface->layout != binding->layout;
    /* Xbox render-target storage is guest VRAM. Before rebinding one address
     * with different geometry, preserve the old host generation in that
     * backing memory so channels excluded by the new target's first write do
     * not come from stale RAM. */
    if (metadata_changed && surface->offset == offset)
        (void)download_color_surface_to_guest(surface);
    surface->offset = offset;
    surface->width = binding->width;
    surface->height = binding->height;
    surface->image_width = image_width;
    surface->image_height = image_height;
    surface->pitch = pitch;
    surface->format = binding->color_format;
    surface->layout = binding->layout;
    if (metadata_changed) {
        surface->cpu_hash_valid = FALSE;
        /* Different geometry or a recycled slot: the epoch skip in
         * sync_cpu_surface must not carry over to different content. */
        surface->cpu_hash_epoch = 0;
    }
    /* Track the depth surface too. A guest texture may alias it rather than a
     * colour surface, and only the image extent can satisfy such a read. */
    if (binding->zeta_resource && binding->zeta_pitch) {
        uint32_t zeta_offset = binding->zeta_guest_address;
        PgraphHostZeta *zeta = NULL;
        UINT z;
        if (!zeta_offset && binding->zeta_resource <= UINT32_MAX)
            zeta_offset = (uint32_t)binding->zeta_resource;
        if (zeta_offset) {
            for (z = 0; z < g_pgraph_zeta_count; ++z) {
                if (g_pgraph_zeta[z].offset == zeta_offset) {
                    zeta = &g_pgraph_zeta[z];
                    break;
                }
            }
            if (!zeta && g_pgraph_zeta_count < PGRAPH_MAX_SURFACES)
                zeta = &g_pgraph_zeta[g_pgraph_zeta_count++];
            if (zeta) {
                zeta->offset = zeta_offset;
                zeta->width = binding->zeta_width
                    ? binding->zeta_width : image_width;
                zeta->height = binding->zeta_height
                    ? binding->zeta_height : image_height;
                zeta->pitch = binding->zeta_pitch;
            }
        }
    }

    g_pgraph_current_surface = surface;
    xgpu_plume_set_render_target(binding);
    return 1;
}

int d3d8_PgraphBindSurfaceTextureStage(
    uint32_t stage, uint32_t offset, uint32_t unnormalized_coords,
    uint32_t texture_format)
{
    PgraphHostSurface *surface = find_surface(offset);
    if (stage >= MAX_TEXTURE_STAGES)
        return 0;
    /*
     * One Xbox VRAM allocation may be rebound with a different logical
     * extent and then sampled immediately (for example, a 640x480 light
     * buffer through a 320x240 linear texture). Width and height are not
     * compatibility requirements, but the storage format is.
     */
    if (!xgpu_plume_surface_known(offset))
        return 0;
    if (surface) {
        if (texture_format != UINT32_MAX &&
            surface->format != texture_format)
            return 0;
        sync_cpu_surface(surface, 1);
    } else if (texture_format != UINT32_MAX) {
        /*
         * Hosted handles (notably the device backbuffer) may be known to
         * Plume without a PGRAPH cache record.  Only the explicit opaque
         * host path may skip that record; guest-memory aliases still need
         * its format metadata before they can bind as surfaces.
         */
        return 0;
    }
    xgpu_plume_set_surface_texture(
        stage, offset, unnormalized_coords);
    return 1;
}

/* Resolve rendered surfaces overlapping [start, start+bytes) back into guest
 * memory. A zeta-only resolve must not overwrite an aliased depth allocation
 * with colour data before the packed Z24S8 download below.
 *
 * A guest texture can alias rendered memory and read a range that spans more
 * than one surface. MM3 samples a 1280x480 Y16 texture at 0x0076C000 whose
 * 1,228,800 bytes cover 480 rows of the 2560-byte stride, but the surface
 * bound there renders only 240 rows -- the rest belongs to a second surface
 * further up the same buffer. Plume renders into host images, so unless each
 * overlapping surface is resolved back to guest memory the upload reads stale
 * bytes and the stage samples as zero.
 *
 * Returns the number of surfaces refreshed. */
int d3d8_PgraphDownloadSurfaceRange(uint32_t start, uint32_t bytes,
                                    int zeta_only)
{
    uint64_t want_end = (uint64_t)start + (bytes ? bytes : 1u);
    int refreshed = 0;
    UINT i;

    for (i = 0; !zeta_only && i < g_pgraph_surface_count; ++i) {
        PgraphHostSurface *surface = &g_pgraph_surfaces[i];
        size_t active_row_bytes;
        uint64_t span;
        uint64_t end;
        UINT image_width;
        UINT image_height;

        if (!surface->offset || surface->layout != XGPU_SURFACE_PITCH ||
            !d3d8_cpu_surface_format_is_32bpp(surface->format) ||
            !surface->width || !surface->height || !surface->pitch)
            continue;
        if (!xgpu_plume_surface_known(surface->offset))
            continue;
        /* Resolve the whole allocated image, not the clip rectangle. Host
         * pixel (0,0) is surface->offset and row r is offset + r * pitch, so
         * this mirrors the allocation exactly. */
        image_width = surface->image_width ? surface->image_width
                                           : surface->width;
        image_height = surface->image_height ? surface->image_height
                                             : surface->height;
        active_row_bytes = (size_t)image_width * 4u;
        if (active_row_bytes > surface->pitch)
            continue;
        span = (uint64_t)(image_height - 1u) * surface->pitch +
               active_row_bytes;
        end = (uint64_t)surface->offset + span;
        /* Overlap test against the requested range. */
        if (end <= (uint64_t)start || (uint64_t)surface->offset >= want_end)
            continue;
        if (download_color_surface_to_guest(surface))
            refreshed++;
    }

    /* Depth surfaces overlapping the range. MM3's sky mask reads its Z24S8
     * buffer as a Y16 texture, and no colour surface can satisfy that. */
    for (i = 0; i < g_pgraph_zeta_count; ++i) {
        PgraphHostZeta *zeta = &g_pgraph_zeta[i];
        size_t active_row_bytes;
        uint64_t span;
        uint64_t end;
        BYTE *pixels;

        if (!zeta->offset || !zeta->width || !zeta->height || !zeta->pitch)
            continue;
        active_row_bytes = (size_t)zeta->width * 4u;
        if (active_row_bytes > zeta->pitch)
            continue;
        span = (uint64_t)(zeta->height - 1u) * zeta->pitch + active_row_bytes;
        end = (uint64_t)zeta->offset + span;
        if (end <= (uint64_t)start || (uint64_t)zeta->offset >= want_end)
            continue;
        pixels = xbox_guest_phys_ptr(zeta->offset, (size_t)span);
        if (!pixels)
            continue;
        if (xgpu_plume_download_zeta_surface(zeta->offset, pixels, zeta->width,
                                             zeta->height, zeta->pitch)) {
            /* This guest-memory mutation belongs to the GPU depth owner, not
             * the CPU. Re-baseline overlapping color surfaces so their next
             * sync cannot upload packed Z24S8 as BGRA. */
            rebaseline_cpu_surfaces_after_gpu_download(zeta->offset, span);
            refreshed++;
        }
    }
    return refreshed;
}

int d3d8_PgraphBindSurfaceTexture(
    uint32_t offset, uint32_t unnormalized_coords)
{
    return d3d8_PgraphBindSurfaceTextureStage(
        0, offset, unnormalized_coords, UINT32_MAX);
}

static PgraphHostTexture *find_texture(uint32_t offset)
{
    UINT i;
    for (i = 0; i < g_pgraph_texture_count; ++i)
        if (g_pgraph_textures[i].offset == offset)
            return &g_pgraph_textures[i];
    return NULL;
}

int d3d8_PgraphBindPhysicalTexture(uint32_t offset, UINT width, UINT height,
                                   D3DFORMAT format, const void *pixels,
                                   UINT row_pitch)
{
    PgraphHostTexture *texture;
    UINT bytes;
    if (!pixels || !width || !height || !row_pitch)
        return 0;
    bytes = texture_byte_size(format, width, height, row_pitch);
    texture = find_texture(offset);
    if (!texture) {
        if (g_pgraph_texture_count < PGRAPH_MAX_TEXTURES) {
            texture = &g_pgraph_textures[g_pgraph_texture_count++];
        } else {
            UINT lru = 0;
            UINT i;
            for (i = 1; i < g_pgraph_texture_count; ++i)
                if (g_pgraph_textures[i].last_used <
                    g_pgraph_textures[lru].last_used)
                    lru = i;
            texture = &g_pgraph_textures[lru];
            free(texture->pixels);
            memset(texture, 0, sizeof(*texture));
        }
    }
    if (texture->width != width || texture->height != height ||
        texture->format != format || texture->bytes != bytes) {
        BYTE *replacement = (BYTE *)realloc(texture->pixels, bytes);
        if (!replacement)
            return 0;
        texture->pixels = replacement;
        texture->bytes = bytes;
        texture->version = 0;
    }
    if (!texture->version || memcmp(texture->pixels, pixels, bytes) != 0) {
        memcpy(texture->pixels, pixels, bytes);
        texture->version = d3d8_plume_next_texture_version();
    }
    texture->offset = offset;
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->last_used = ++g_pgraph_texture_clock;
    xgpu_plume_set_texture(0, offset, pixels, width, height, row_pitch, bytes,
                           (uint32_t)format, texture->version);
    return 1;
}

int d3d8_PgraphIsKnownSurface(uint32_t offset)
{
    PgraphHostSurface *surface;
    if (offset && xgpu_plume_surface_known(offset))
        return 1;
    surface = find_surface(offset);
    if (!surface && offset)
        surface = find_surface_within(offset);
    return surface && surface->width == panel_width() &&
           surface->height == panel_height();
}

int d3d8_PgraphPresentSurface(uint32_t offset)
{
    PgraphHostSurface *surface;

    /* Open a new epoch first: guest code has run since the last present and may
     * have written the surface directly, so this present's own sync must hash
     * rather than reuse the previous epoch's result. This is what keeps the
     * loading-meter animation live, since that phase issues no draws and no
     * flips — only presents. */
    g_cpu_surface_epoch++;
    if (g_cpu_surface_epoch == 0)
        g_cpu_surface_epoch = 1;          /* 0 means "never hashed" */

    surface = find_surface(offset);
    if (!surface && offset)
        surface = find_surface_within(offset);
    if (surface && surface->width == panel_width() &&
        surface->height == panel_height())
        g_pgraph_present_surface = surface;
    if (!surface || surface->width != panel_width() ||
        surface->height != panel_height())
        surface = pgraph_fallback_surface();
    xgpu_plume_f2_log(
        "pgraph scanout request=%08X resolved=%08X dims=%ux%u "
        "plume-known=%u fallback=%u",
        offset, surface ? surface->offset : 0,
        surface ? surface->width : 0, surface ? surface->height : 0,
        offset && xgpu_plume_surface_known(offset),
        surface && surface->offset != offset);
    if (offset && xgpu_plume_surface_known(offset)) {
        sync_cpu_surface(find_surface(offset), 1);
        xgpu_plume_set_present_surface(offset);
        return 1;
    }
    if (!surface)
        return 0;
    sync_cpu_surface(surface, 1);
    xgpu_plume_set_present_surface(surface->offset);
    return 1;
}

void d3d8_PgraphSetWindowClip(int enabled, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height)
{
    g_device_state.pgraph_scissor_enable = enabled ? TRUE : FALSE;
    g_device_state.pgraph_scissor_x = x;
    g_device_state.pgraph_scissor_y = y;
    g_device_state.pgraph_scissor_width = width;
    g_device_state.pgraph_scissor_height = height;
}

int d3d8_PgraphRefreshSurface(uint32_t offset)
{
    PgraphHostSurface *surface;

    /* Unlike an explicit guest present, a PCRTC refresh has no reason to enter
     * the renderer unless guest RAM actually changed. A writable lock can stay
     * live across many VBlanks, so fingerprint every refresh epoch but publish
     * only a new hash. */
    g_cpu_surface_epoch++;
    if (g_cpu_surface_epoch == 0)
        g_cpu_surface_epoch = 1;

    surface = find_surface(offset);
    if (!surface && offset)
        surface = find_surface_within(offset);
    if (!surface || surface->width != panel_width() ||
        surface->height != panel_height())
        surface = pgraph_fallback_surface();
    if (!surface || !sync_cpu_surface(surface, 1))
        return 0;

    xgpu_plume_f2_log(
        "pgraph scanout refresh request=%08X resolved=%08X dims=%ux%u",
        offset, surface->offset, surface->width, surface->height);
    xgpu_plume_set_present_surface(surface->offset);
    return 1;
}

int d3d8_PgraphPresentSurfaceForSwap(uint32_t offset)
{
    if (!d3d8_PgraphPresentSurface(offset))
        return 0;
    (void)xrecomp_host_pump_messages();
    present_with_reason(PLUME_PRESENT_SWAP);
    return 1;
}

int d3d8_PgraphWaitForIdle(uint32_t surface_sync_flags)
{
    return xgpu_plume_wait_for_idle(surface_sync_flags);
}

int d3d8_HostPresentFrameRGBA(const void *pixels, UINT width, UINT height,
                              UINT row_pitch)
{
    if (!g_device_initialized || !pixels || !width || !height || !row_pitch)
        return 0;
    return xgpu_plume_present_host_frame(pixels, width, height, row_pitch);
}

void d3d8_PresentFrame(void)
{
    (void)xrecomp_host_pump_messages();
    present_with_reason(PLUME_PRESENT_FRAME);
}

void d3d8_PresentFrameNoWait(void)
{
    (void)xrecomp_host_pump_messages();
    present_with_reason(PLUME_PRESENT_NO_WAIT);
}

void d3d8_PresentFrameVblank(void)
{
    (void)xrecomp_host_pump_messages();
    present_with_reason(PLUME_PRESENT_VBLANK);
}

void d3d8_BlitCpuBuffer(const void *pixels, UINT width, UINT height, UINT pitch)
{
    if (pixels && width && height && pitch)
        (void)xgpu_plume_present_host_frame(pixels, width, height, pitch);
}

IDirect3DDevice8 *d3d8_GetDevice(void)
{
    return &g_device;
}

void *d3d8_GetNativeWindow(void)
{
    return g_device_state.native_window;
}

UINT d3d8_GetBackbufferWidth(void)
{
    return panel_width();
}

UINT d3d8_GetBackbufferHeight(void)
{
    return panel_height();
}

const DWORD *d3d8_GetRenderStates(void)
{
    return g_device_state.render_states;
}

const DWORD *d3d8_GetTSS(DWORD stage)
{
    return stage < MAX_TEXTURE_STAGES ? g_device_state.tss[stage] : NULL;
}

BOOL d3d8_FixedFunctionStateKnown(void)
{
    return g_device_state.fixed_state_valid;
}

const D3DMATRIX *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type)
{
    return (DWORD)type < MAX_TRANSFORMS
               ? &g_device_state.transforms[(DWORD)type]
               : NULL;
}

const D3DLIGHT8 *d3d8_GetLight(DWORD index)
{
    return index < MAX_LIGHTS ? &g_device_state.lights[index] : NULL;
}

BOOL d3d8_GetLightEnable(DWORD index)
{
    return index < MAX_LIGHTS ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8 *d3d8_GetMaterial(void)
{
    return &g_device_state.material;
}

UINT d3d8_GetNumLights(void)
{
    return MAX_LIGHTS;
}

static D3D8Surface *surface_from_iface(IDirect3DSurface8 *iface)
{
    return (D3D8Surface *)iface;
}

static HRESULT __stdcall surface_QueryInterface(IDirect3DSurface8 *self,
                                                const IID *riid, void **ppv)
{
    (void)self;
    (void)riid;
    (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall surface_AddRef(IDirect3DSurface8 *self)
{
    return (ULONG)InterlockedIncrement(&surface_from_iface(self)->ref_count);
}

static ULONG __stdcall surface_Release(IDirect3DSurface8 *self)
{
    D3D8Surface *surface = surface_from_iface(self);
    LONG ref = InterlockedDecrement(&surface->ref_count);
    if (ref <= 0) {
        free(surface->sys_mem);
        free(surface);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall surface_GetDevice(IDirect3DSurface8 *self,
                                           IDirect3DDevice8 **device)
{
    (void)self;
    if (!device)
        return E_INVALIDARG;
    *device = xbox_GetD3DDevice();
    return S_OK;
}

static HRESULT __stdcall surface_GetDesc(IDirect3DSurface8 *self,
                                         D3DSURFACE_DESC *desc)
{
    D3D8Surface *surface = surface_from_iface(self);
    if (!desc)
        return E_INVALIDARG;
    memset(desc, 0, sizeof(*desc));
    desc->Format = surface->format;
    desc->Type = 1; /* D3DRTYPE_SURFACE */
    desc->Usage = surface->usage;
    desc->Pool = D3DPOOL_DEFAULT;
    desc->Size = surface->pitch * surface->height;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->Width = surface->width;
    desc->Height = surface->height;
    return S_OK;
}

static BOOL surface_download(D3D8Surface *surface)
{
    BYTE *bgra;
    UINT bgra_pitch;
    size_t bgra_bytes;
    BOOL ok;
    if (!(surface->usage & D3DUSAGE_RENDERTARGET) || surface->dirty ||
        !xgpu_plume_surface_known(surface->resource_id))
        return TRUE;
    if (surface->width > UINT_MAX / 4u)
        return FALSE;
    bgra_pitch = surface->width * 4u;
    bgra_bytes = (size_t)bgra_pitch * surface->height;
    if (surface->height && bgra_bytes / surface->height != bgra_pitch)
        return FALSE;
    bgra = (BYTE *)malloc(bgra_bytes);
    if (!bgra)
        return FALSE;
    ok = xgpu_plume_download_color_surface(
             surface->resource_id, bgra, surface->width, surface->height,
             bgra_pitch) &&
         d3d8_surface_from_bgra(
             surface->format, bgra, bgra_pitch, surface->sys_mem,
             surface->pitch, surface->width, surface->height);
    free(bgra);
    return ok;
}

static BOOL surface_upload(D3D8Surface *surface)
{
    BYTE *bgra;
    UINT bgra_pitch;
    size_t bgra_bytes;
    BOOL ok;
    if (!(surface->usage & D3DUSAGE_RENDERTARGET) || !surface->dirty)
        return TRUE;
    if (!xgpu_plume_surface_known(surface->resource_id))
        return TRUE;
    if (surface->width > UINT_MAX / 4u)
        return FALSE;
    bgra_pitch = surface->width * 4u;
    bgra_bytes = (size_t)bgra_pitch * surface->height;
    if (surface->height && bgra_bytes / surface->height != bgra_pitch)
        return FALSE;
    bgra = (BYTE *)malloc(bgra_bytes);
    if (!bgra)
        return FALSE;
    ok = d3d8_surface_to_bgra(
             surface->format, surface->sys_mem, surface->pitch, bgra,
             bgra_pitch, surface->width, surface->height) &&
         xgpu_plume_upload_color_surface(
             surface->resource_id, bgra, surface->width, surface->height,
             bgra_pitch);
    free(bgra);
    if (ok)
        surface->dirty = FALSE;
    return ok;
}

static HRESULT __stdcall surface_LockRect(IDirect3DSurface8 *self,
                                         D3DLOCKED_RECT *locked,
                                         const RECT *rect, DWORD flags)
{
    D3D8Surface *surface = surface_from_iface(self);
    UINT x = rect ? (UINT)rect->left : 0;
    UINT y = rect ? (UINT)rect->top : 0;
    UINT right = rect ? (UINT)rect->right : surface->width;
    UINT bottom = rect ? (UINT)rect->bottom : surface->height;
    UINT bytes_per_pixel = d3d8_format_bpp(surface->format) / 8u;
    if (!locked || surface->locked || !surface->sys_mem ||
        x >= right || y >= bottom || right > surface->width ||
        bottom > surface->height || !bytes_per_pixel)
        return E_INVALIDARG;
    if (!surface_download(surface))
        return E_FAIL;
    locked->Pitch = (INT)surface->pitch;
    locked->pBits = surface->sys_mem + (size_t)y * surface->pitch +
                    (size_t)x * bytes_per_pixel;
    surface->locked = TRUE;
    surface->lock_flags = flags;
    return S_OK;
}

static HRESULT __stdcall surface_UnlockRect(IDirect3DSurface8 *self)
{
    D3D8Surface *surface = surface_from_iface(self);
    BOOL uploaded = TRUE;
    if (!surface->locked)
        return E_FAIL;
    surface->locked = FALSE;
    if (!(surface->lock_flags & D3DLOCK_READONLY)) {
        surface->dirty = TRUE;
        uploaded = surface_upload(surface);
    }
    surface->lock_flags = 0;
    return uploaded ? S_OK : E_FAIL;
}

static const IDirect3DSurface8Vtbl g_surface_vtbl = {
    surface_QueryInterface, surface_AddRef, surface_Release,
    surface_GetDevice, surface_GetDesc, surface_LockRect, surface_UnlockRect,
};

static HRESULT create_surface(UINT width, UINT height, D3DFORMAT format,
                              DWORD usage, IDirect3DSurface8 **out_surface)
{
    D3D8Surface *surface;
    size_t bytes;
    if (!out_surface || !width || !height)
        return E_INVALIDARG;
    surface = (D3D8Surface *)calloc(1, sizeof(*surface));
    if (!surface)
        return E_OUTOFMEMORY;
    surface->pitch = d3d8_row_pitch(format, width);
    bytes = (size_t)surface->pitch * height;
    surface->sys_mem = (BYTE *)calloc(1, bytes);
    if (!surface->sys_mem) {
        free(surface);
        return E_OUTOFMEMORY;
    }
    surface->iface.lpVtbl = &g_surface_vtbl;
    surface->ref_count = 1;
    surface->resource_id = 0xf0000000u |
                           ((uint32_t)InterlockedIncrement(&g_surface_id) &
                            0x0fffffffu);
    surface->width = width;
    surface->height = height;
    surface->format = format;
    surface->usage = usage;
    surface->dirty = TRUE;
    *out_surface = &surface->iface;
    return S_OK;
}

static void bind_direct_surfaces(void)
{
    XgpuSurfaceBinding binding;
    D3D8Surface *color = g_device_state.render_target;
    D3D8Surface *zeta = g_device_state.depth_surface;
    if (!color)
        return;
    memset(&binding, 0, sizeof(binding));
    binding.color_resource = color->resource_id;
    binding.zeta_resource = zeta ? zeta->resource_id : 0;
    binding.zeta_guest_address = zeta ? zeta->guest_address : 0;
    binding.width = color->width;
    binding.height = color->height;
    binding.zeta_width = zeta ? zeta->width : 0;
    binding.zeta_height = zeta ? zeta->height : 0;
    binding.color_pitch = color->pitch;
    binding.zeta_pitch = zeta ? zeta->pitch : 0;
    binding.color_format = (uint32_t)color->format;
    binding.zeta_format = zeta
        ? ((zeta->format == D3DFMT_D16 || zeta->format == D3DFMT_F16)
               ? XGPU_ZETA_Z16
               : XGPU_ZETA_Z24S8)
        : XGPU_ZETA_NONE;
    binding.zeta_float =
        zeta && (zeta->format == D3DFMT_F16 ||
                 zeta->format == D3DFMT_F24S8);
    binding.layout = XGPU_SURFACE_PITCH;
    binding.zeta_layout = XGPU_SURFACE_PITCH;
    binding.sample_count = 1;
    xgpu_plume_set_render_target(&binding);
    if (!surface_upload(color))
        fprintf(stderr,
                "[D3D8] unable to seed render target %08X from CPU memory\n",
                color->resource_id);
}

int d3d8_PgraphAttachDefaultZeta(XgpuSurfaceBinding *binding)
{
    D3D8Surface *zeta = g_device_state.depth_surface;

    if (!binding || !zeta || !zeta->resource_id || !zeta->width ||
        !zeta->height || !zeta->pitch)
        return 0;
    binding->zeta_resource = zeta->resource_id;
    binding->zeta_guest_address = zeta->guest_address;
    binding->zeta_width = zeta->width;
    binding->zeta_height = zeta->height;
    binding->zeta_pitch = zeta->pitch;
    binding->zeta_format =
        (zeta->format == D3DFMT_D16 || zeta->format == D3DFMT_F16)
            ? XGPU_ZETA_Z16 : XGPU_ZETA_Z24S8;
    binding->zeta_float =
        zeta->format == D3DFMT_F16 || zeta->format == D3DFMT_F24S8;
    binding->zeta_layout = XGPU_SURFACE_PITCH;
    return 1;
}

static void init_default_states(D3D8DeviceState *state)
{
    int i;
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE] = TRUE;
    state->render_states[D3DRS_FILLMODE] = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE] = 2;
    state->render_states[D3DRS_ZWRITEENABLE] = TRUE;
    state->render_states[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE] = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    state->render_states[D3DRS_COLORWRITEENABLE] = 0x0f;
    state->render_states[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    for (i = 0; i < MAX_TEXTURE_STAGES; ++i) {
        state->tss[i][D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        state->tss[i][D3DTSS_COLORARG0] = D3DTA_CURRENT;
        state->tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        state->tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        state->tss[i][D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        state->tss[i][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        state->tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        state->tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        state->tss[i][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
        state->tss[i][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    }
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;
    for (i = 0; i < MAX_TRANSFORMS; ++i) {
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }
}

static XgpuPlumeRenderState snapshot_render_state(void)
{
    const DWORD *rs = g_device_state.render_states;
    XgpuPlumeRenderState state;
    uint32_t stage;
    memset(&state, 0, sizeof(state));
    state.depth_enable = rs[D3DRS_ZENABLE] != 0;
    state.z_perspective = rs[D3DRS_ZENABLE] == 2u;
    state.depth_write = rs[D3DRS_ZWRITEENABLE] != 0;
    state.depth_func = rs[D3DRS_ZFUNC];
    state.blend_enable = rs[D3DRS_ALPHABLENDENABLE] != 0;
    state.src_blend = rs[D3DRS_SRCBLEND];
    state.dst_blend = rs[D3DRS_DESTBLEND];
    state.blend_op = rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1;
    state.blend_color = rs[XRECOMP_D3DRS_BLEND_COLOR];
    state.alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] != 0;
    state.alpha_func = rs[D3DRS_ALPHAFUNC];
    state.alpha_ref = rs[D3DRS_ALPHAREF] & 0xFFu;
    state.cull_mode = rs[D3DRS_CULLMODE];
    state.color_write_mask = rs[D3DRS_COLORWRITEENABLE];
    state.stencil_enable = rs[D3DRS_STENCILENABLE] != 0;
    state.stencil_func = rs[D3DRS_STENCILFUNC];
    state.stencil_ref = rs[D3DRS_STENCILREF];
    state.stencil_read_mask = rs[D3DRS_STENCILMASK];
    state.stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
    state.stencil_fail = rs[D3DRS_STENCILFAIL];
    state.stencil_zfail = rs[D3DRS_STENCILZFAIL];
    state.stencil_pass = rs[D3DRS_STENCILPASS];
    state.depth_bias_bits =
        rs[XRECOMP_D3DRS_DEPTH_BIAS_CONSTANT];
    state.slope_scaled_depth_bias_bits =
        rs[XRECOMP_D3DRS_DEPTH_BIAS_SLOPE];
    for (stage = 0; stage < MAX_TEXTURE_STAGES; ++stage) {
        state.fixed_color_op[stage] =
            g_device_state.tss[stage][D3DTSS_COLOROP];
        state.fixed_color_arg0[stage] =
            g_device_state.tss[stage][D3DTSS_COLORARG0];
        state.fixed_color_arg1[stage] =
            g_device_state.tss[stage][D3DTSS_COLORARG1];
        state.fixed_color_arg2[stage] =
            g_device_state.tss[stage][D3DTSS_COLORARG2];
        state.fixed_alpha_op[stage] =
            g_device_state.tss[stage][D3DTSS_ALPHAOP];
        state.fixed_alpha_arg0[stage] =
            g_device_state.tss[stage][D3DTSS_ALPHAARG0];
        state.fixed_alpha_arg1[stage] =
            g_device_state.tss[stage][D3DTSS_ALPHAARG1];
        state.fixed_alpha_arg2[stage] =
            g_device_state.tss[stage][D3DTSS_ALPHAARG2];
    }
    state.fixed_texture_factor = rs[D3DRS_TEXTUREFACTOR];
    state.fixed_state_valid = g_device_state.fixed_state_valid != FALSE;
    state.viewport_x = g_device_state.viewport.X;
    state.viewport_y = g_device_state.viewport.Y;
    state.viewport_width = g_device_state.viewport.Width;
    state.viewport_height = g_device_state.viewport.Height;
    state.viewport_min_z = g_device_state.viewport.MinZ;
    state.viewport_max_z = g_device_state.viewport.MaxZ;
    state.scissor_enable = g_device_state.pgraph_scissor_enable != FALSE;
    state.scissor_x = g_device_state.pgraph_scissor_x;
    state.scissor_y = g_device_state.pgraph_scissor_y;
    state.scissor_width = g_device_state.pgraph_scissor_width;
    state.scissor_height = g_device_state.pgraph_scissor_height;
    return state;
}

static int primitive_vertex_count(D3DPRIMITIVETYPE type, UINT primitive_count,
                                  UINT *vertex_count)
{
    uint64_t count;
    switch (type) {
    case D3DPT_POINTLIST: count = primitive_count; break;
    case D3DPT_LINELIST: count = (uint64_t)primitive_count * 2u; break;
    case D3DPT_LINELOOP:
    case D3DPT_LINESTRIP: count = (uint64_t)primitive_count + 1u; break;
    case D3DPT_TRIANGLELIST: count = (uint64_t)primitive_count * 3u; break;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
    case D3DPT_POLYGON: count = (uint64_t)primitive_count + 2u; break;
    case D3DPT_QUADLIST: count = (uint64_t)primitive_count * 4u; break;
    case D3DPT_QUADSTRIP:
        count = (uint64_t)primitive_count * 2u + 2u;
        break;
    default: return 0;
    }
    if (count > UINT_MAX)
        return 0;
    *vertex_count = (UINT)count;
    return 1;
}

static void *convert_fan_quad_or_polygon(
    D3DPRIMITIVETYPE type, const void *source, UINT primitive_count,
    UINT stride, UINT *out_vertex_count)
{
    BYTE *output;
    const BYTE *input = (const BYTE *)source;
    UINT i;
    uint64_t count = (uint64_t)primitive_count *
                     (type == D3DPT_QUADLIST ? 6u : 3u);
    if (count > UINT_MAX || count * stride > SIZE_MAX)
        return NULL;
    output = (BYTE *)malloc((size_t)count * stride);
    if (!output)
        return NULL;
    if (type == D3DPT_TRIANGLEFAN || type == D3DPT_POLYGON) {
        for (i = 0; i < primitive_count; ++i) {
            memcpy(output + (size_t)(i * 3u) * stride, input, stride);
            memcpy(output + (size_t)(i * 3u + 1u) * stride,
                   input + (size_t)(i + 1u) * stride, stride);
            memcpy(output + (size_t)(i * 3u + 2u) * stride,
                   input + (size_t)(i + 2u) * stride, stride);
        }
    } else {
        for (i = 0; i < primitive_count; ++i) {
            const BYTE *quad = input + (size_t)i * 4u * stride;
            const UINT map[6] = {0, 1, 2, 0, 2, 3};
            UINT j;
            for (j = 0; j < 6; ++j)
                memcpy(output + (size_t)(i * 6u + j) * stride,
                       quad + (size_t)map[j] * stride, stride);
        }
    }
    *out_vertex_count = (UINT)count;
    return output;
}

static void *convert_line_loop(const void *source, UINT vertex_count,
                               UINT stride, UINT *out_vertex_count)
{
    BYTE *output;
    const BYTE *input = (const BYTE *)source;
    uint64_t count = (uint64_t)vertex_count * 2u;
    UINT i;
    if (vertex_count < 2u || count > UINT_MAX || count * stride > SIZE_MAX)
        return NULL;
    output = (BYTE *)malloc((size_t)count * stride);
    if (!output)
        return NULL;
    for (i = 0; i + 1u < vertex_count; ++i) {
        memcpy(output + (size_t)(i * 2u) * stride,
               input + (size_t)i * stride, stride);
        memcpy(output + (size_t)(i * 2u + 1u) * stride,
               input + (size_t)(i + 1u) * stride, stride);
    }
    memcpy(output + (size_t)((vertex_count - 1u) * 2u) * stride,
           input + (size_t)(vertex_count - 1u) * stride, stride);
    memcpy(output + (size_t)((vertex_count - 1u) * 2u + 1u) * stride,
           input, stride);
    *out_vertex_count = (UINT)count;
    return output;
}

static D3DMATRIX multiply_matrix(const D3DMATRIX *left,
                                 const D3DMATRIX *right)
{
    D3DMATRIX result;
    const float *a = (const float *)left;
    const float *b = (const float *)right;
    float *out = (float *)&result;
    UINT row;
    UINT column;
    UINT k;
    memset(&result, 0, sizeof(result));
    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 4; ++column) {
            for (k = 0; k < 4; ++k)
                out[row * 4u + column] +=
                    a[row * 4u + k] * b[k * 4u + column];
        }
    }
    return result;
}

static HRESULT transform_fixed_xyz(const void *source, UINT vertex_count,
                                   UINT stride, void **out_vertices,
                                   UINT *out_stride)
{
    const D3DMATRIX *world = &g_device_state.transforms[D3DTS_WORLD];
    const D3DMATRIX *view = &g_device_state.transforms[D3DTS_VIEW];
    const D3DMATRIX *projection =
        &g_device_state.transforms[D3DTS_PROJECTION];
    D3DMATRIX world_view = multiply_matrix(world, view);
    D3DMATRIX wvp = multiply_matrix(&world_view, projection);
    const float *m = (const float *)&wvp;
    const BYTE *input = (const BYTE *)source;
    BYTE *output;
    UINT expanded_stride;
    UINT i;
    if (!source || !out_vertices || !out_stride || stride < 12u ||
        stride > UINT_MAX - 4u)
        return E_INVALIDARG;
    expanded_stride = stride + 4u;
    if ((uint64_t)vertex_count * expanded_stride > SIZE_MAX)
        return E_INVALIDARG;
    output = (BYTE *)malloc((size_t)vertex_count * expanded_stride);
    if (!output)
        return E_OUTOFMEMORY;
    for (i = 0; i < vertex_count; ++i) {
        BYTE *destination = output + (size_t)i * expanded_stride;
        float position[3];
        float clip[4];
        float screen[4];
        UINT component;
        memcpy(position, input + (size_t)i * stride, sizeof(position));
        for (component = 0; component < 4; ++component) {
            clip[component] = position[0] * m[component] +
                              position[1] * m[4u + component] +
                              position[2] * m[8u + component] +
                              m[12u + component];
        }
        if (clip[3] > -1.0e-20f && clip[3] < 1.0e-20f) {
            free(output);
            return E_INVALIDARG;
        }
        screen[3] = 1.0f / clip[3];
        screen[0] = (float)g_device_state.viewport.X +
                    (clip[0] * screen[3] + 1.0f) * 0.5f *
                        (float)g_device_state.viewport.Width;
        screen[1] = (float)g_device_state.viewport.Y +
                    (1.0f - clip[1] * screen[3]) * 0.5f *
                        (float)g_device_state.viewport.Height;
        screen[2] = g_device_state.viewport.MinZ +
                    clip[2] * screen[3] *
                        (g_device_state.viewport.MaxZ -
                         g_device_state.viewport.MinZ);
        memcpy(destination, screen, sizeof(screen));
        memcpy(destination + 16u,
               input + (size_t)i * stride + 12u, stride - 12u);
    }
    *out_vertices = output;
    *out_stride = expanded_stride;
    return S_OK;
}

static HRESULT submit_draw(D3DPRIMITIVETYPE type, UINT primitive_count,
                           const void *vertices, UINT stride)
{
    const void *draw_vertices = vertices;
    void *converted = NULL;
    void *transformed = NULL;
    UINT vertex_count;
    UINT draw_stride = stride;
    DWORD draw_fvf = g_device_state.vertex_shader;
    UINT draw_primitive_count = primitive_count;
    D3DPRIMITIVETYPE draw_type = type;
    XgpuPlumeRenderState render_state;
    HRESULT result;
    if (!vertices || !stride || !primitive_count ||
        !primitive_vertex_count(type, primitive_count, &vertex_count))
        return E_INVALIDARG;
    if ((uint64_t)vertex_count * stride > UINT_MAX)
        return E_INVALIDARG;
    if (type == D3DPT_TRIANGLEFAN || type == D3DPT_QUADLIST ||
        type == D3DPT_POLYGON) {
        converted = convert_fan_quad_or_polygon(
            type, vertices, primitive_count, stride, &vertex_count);
        if (!converted)
            return E_OUTOFMEMORY;
        draw_vertices = converted;
        draw_type = D3DPT_TRIANGLELIST;
        draw_primitive_count = vertex_count / 3u;
    } else if (type == D3DPT_LINELOOP) {
        converted = convert_line_loop(vertices, vertex_count, stride,
                                      &vertex_count);
        if (!converted)
            return E_OUTOFMEMORY;
        draw_vertices = converted;
        draw_type = D3DPT_LINELIST;
        draw_primitive_count = vertex_count / 2u;
    } else if (type == D3DPT_QUADSTRIP) {
        draw_type = D3DPT_TRIANGLESTRIP;
        draw_primitive_count = primitive_count * 2u;
    }
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_compat_prepare_zone, "D3D Compat Prepare Draw");
    prepare_draw();
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_prepare_zone);
    if (!d3d8_vsh_is_programmable(g_device_state.vertex_shader)) {
        const DWORD position =
            g_device_state.vertex_shader & D3DFVF_POSITION_MASK;
        if (position == D3DFVF_XYZ) {
            XRECOMP_CPU_RECORDER_ZONE_BEGIN(
                cpu_compat_transform_zone, "D3D Compat Transform Vertices");
            result = transform_fixed_xyz(draw_vertices, vertex_count,
                                         draw_stride, &transformed,
                                         &draw_stride);
            XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_transform_zone);
            if (FAILED(result)) {
                free(converted);
                return result;
            }
            draw_vertices = transformed;
            draw_fvf = (draw_fvf & ~D3DFVF_POSITION_MASK) | D3DFVF_XYZRHW;
            /* The resulting XYZRHW coordinates already include the active
             * viewport. UI-canvas remapping must undo that scale first. */
            xgpu_plume_set_position_mode(2);
        } else if (position != D3DFVF_XYZRHW) {
            free(converted);
            return E_NOTIMPL;
        } else {
            xgpu_plume_set_position_mode(1);
        }
    } else {
        xgpu_plume_set_position_mode(0);
    }
    render_state = snapshot_render_state();
    if (d3d8_vsh_is_programmable(g_device_state.vertex_shader) &&
        xgpu_plume_ui_canvas_active() &&
        d3d8_vsh_calculate_position_bounds(
            g_device_state.vertex_shader, draw_vertices, vertex_count,
            draw_stride, &render_state.programmable_position_min_x,
            &render_state.programmable_position_max_x)) {
        render_state.programmable_position_bounds_valid = 1u;
    }
    xgpu_plume_record_draw((uint32_t)draw_type, draw_primitive_count,
                           draw_vertices, draw_stride, draw_fvf, &render_state);
    xgpu_plume_set_position_mode(0);
    free(transformed);
    free(converted);
    return S_OK;
}

/* Default-off validation gate. The path is shared/title-neutral, but remains
 * opt-in until its visual and same-binary performance gates pass. */
static int persistent_mesh_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *value = getenv("XRECOMP_PLUME_PERSISTENT_MESH");
        cached = value && value[0] && strcmp(value, "0") != 0 &&
                 strcmp(value, "false") != 0;
    }
    return cached;
}

static HRESULT submit_persistent_indexed_draw(
    D3DPRIMITIVETYPE type, UINT primitive_count,
    UINT declared_min_vertex, UINT declared_num_vertices,
    const void *indices, D3DFORMAT index_format,
    const BYTE *vertices, UINT stride)
{
    const uint16_t *indices16 = (const uint16_t *)indices;
    D3D8Index16Range range;
    XgpuPlumeCachedIndexedDraw cached;
    XgpuPlumeRenderState render_state;
    UINT index_count;
    UINT vertex_count;
    UINT draw_stride = stride;
    DWORD draw_fvf = g_device_state.vertex_shader;
    uint32_t vb_data_va = 0;
    uint32_t ib_data_va = 0;
    uint64_t vb_generation = 1;
    uint64_t ib_generation = 1;
    uint64_t vertex_offset;
    void *transformed = NULL;
    const void *draw_vertices;
    HRESULT result = S_FALSE;

    if (!persistent_mesh_enabled() ||
        type != D3DPT_TRIANGLELIST || index_format != D3DFMT_INDEX16 ||
        xgpu_plume_ui_canvas_active())
        return S_FALSE;
    if (!indices16 || !vertices || !stride ||
        !primitive_vertex_count(type, primitive_count, &index_count))
        return E_INVALIDARG;
    if (!d3d_hle_guest_bound_mesh_identity(&vb_data_va, &vb_generation,
                                           &ib_data_va, &ib_generation))
        return S_FALSE;
    if (!d3d8_index16_range(indices16, index_count,
                            declared_min_vertex, declared_num_vertices,
                            &range))
        return E_INVALIDARG;
    vertex_count = range.maximum - range.minimum + 1u;
    if (vertex_count > index_count)
        return S_FALSE;
    vertex_offset =
        (uint64_t)(range.minimum - declared_min_vertex) * stride;
    if (vertex_offset > SIZE_MAX)
        return S_FALSE;
    draw_vertices = vertices + (size_t)vertex_offset;

    prepare_draw();
    if (!d3d8_vsh_is_programmable(g_device_state.vertex_shader)) {
        const DWORD position =
            g_device_state.vertex_shader & D3DFVF_POSITION_MASK;
        if (position == D3DFVF_XYZ) {
            if (FAILED(transform_fixed_xyz(
                    draw_vertices, vertex_count, draw_stride,
                    &transformed, &draw_stride)))
                goto done;
            draw_vertices = transformed;
            draw_fvf = (draw_fvf & ~D3DFVF_POSITION_MASK) | D3DFVF_XYZRHW;
            xgpu_plume_set_position_mode(2);
        } else if (position == D3DFVF_XYZRHW) {
            xgpu_plume_set_position_mode(1);
        } else {
            goto done;
        }
    } else {
        xgpu_plume_set_position_mode(0);
    }

    render_state = snapshot_render_state();
    memset(&cached, 0, sizeof(cached));
    cached.prim_type = (uint32_t)type;
    cached.prim_count = primitive_count;
    cached.vb_data_va = vb_data_va;
    cached.ib_data_va = ib_data_va;
    cached.index_byte_offset = 0;
    {
        uint32_t guest;

        if (xbox_guest_host_to_phys(indices16, &guest) &&
            guest >= ib_data_va) {
            cached.index_byte_offset = guest - ib_data_va;
        }
    }
    cached.index_count = index_count;
    cached.stride = draw_stride;
    cached.base_vertex = range.minimum;
    cached.fvf_or_vs = draw_fvf;
    cached.vb_generation = vb_generation;
    cached.ib_generation = ib_generation;
    cached.vertices = draw_vertices;
    cached.vertex_count = vertex_count;
    cached.indices = indices16;
    cached.render_state = &render_state;
    if (xgpu_plume_record_cached_indexed_draw(&cached))
        result = S_OK;

done:
    xgpu_plume_set_position_mode(0);
    free(transformed);
    return result;
}

static int d3d8_native_hle_indexed_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_NATIVE_HLE_INDEXED");
        enabled = value && value[0] && strcmp(value, "0") != 0 &&
                  strcmp(value, "false") != 0;
    }
    return enabled;
}

/* Record the common triangle-list UP case without expanding one vertex copy
 * per index. The observed contiguous range and normalized uint32 indices are
 * copied into Plume-owned frame storage before return. S_FALSE selects the
 * existing expansion fallback without changing externally visible behavior. */
static HRESULT submit_native_indexed_draw(
    D3DPRIMITIVETYPE type, UINT primitive_count,
    UINT declared_min_vertex, UINT declared_num_vertices,
    const void *indices, D3DFORMAT index_format,
    const BYTE *vertices, UINT stride)
{
    const uint16_t *indices16 = (const uint16_t *)indices;
    D3D8Index16Range range;
    uint32_t *normalized = NULL;
    const void *draw_vertices;
    void *transformed = NULL;
    UINT index_count;
    UINT vertex_count;
    UINT draw_stride = stride;
    DWORD draw_fvf = g_device_state.vertex_shader;
    uint64_t vertex_offset;
    uint64_t vertex_bytes;
    XgpuPlumeRenderState render_state;
    HRESULT result = S_FALSE;
    UINT i;

    if (!d3d8_native_hle_indexed_enabled() ||
        type != D3DPT_TRIANGLELIST || index_format != D3DFMT_INDEX16 ||
        xgpu_plume_ui_canvas_active())
        return S_FALSE;
    if (!indices16 || !vertices || !stride ||
        !primitive_vertex_count(type, primitive_count, &index_count))
        return E_INVALIDARG;
    if (!d3d8_index16_range(indices16, index_count,
                            declared_min_vertex, declared_num_vertices,
                            &range))
        return E_INVALIDARG;
    vertex_count = range.maximum - range.minimum + 1u;
    /* Sparse ranges would copy more vertex bytes than the proven fallback and
     * touch unreferenced guest data for no performance benefit. */
    if (vertex_count > index_count)
        return S_FALSE;
    vertex_offset =
        (uint64_t)(range.minimum - declared_min_vertex) * stride;
    vertex_bytes = (uint64_t)vertex_count * stride;
    if (vertex_offset > SIZE_MAX || vertex_bytes > UINT32_MAX ||
        vertex_bytes > SIZE_MAX)
        return S_FALSE;

    normalized = (uint32_t *)malloc((size_t)index_count * sizeof(*normalized));
    if (!normalized)
        return E_OUTOFMEMORY;
    for (i = 0; i < index_count; ++i)
        normalized[i] = (uint32_t)indices16[i] - range.minimum;
    draw_vertices = vertices + (size_t)vertex_offset;

    prepare_draw();
    if (!d3d8_vsh_is_programmable(g_device_state.vertex_shader)) {
        const DWORD position =
            g_device_state.vertex_shader & D3DFVF_POSITION_MASK;
        if (position == D3DFVF_XYZ) {
            if (FAILED(transform_fixed_xyz(
                    draw_vertices, vertex_count, draw_stride,
                    &transformed, &draw_stride)))
                goto done;
            draw_vertices = transformed;
            draw_fvf = (draw_fvf & ~D3DFVF_POSITION_MASK) | D3DFVF_XYZRHW;
            xgpu_plume_set_position_mode(2);
        } else if (position == D3DFVF_XYZRHW) {
            xgpu_plume_set_position_mode(1);
        } else {
            goto done;
        }
    } else {
        xgpu_plume_set_position_mode(0);
    }

    render_state = snapshot_render_state();
    if (xgpu_plume_record_indexed_draw(
            (uint32_t)type, primitive_count,
            draw_vertices, vertex_count, draw_stride, draw_fvf,
            normalized, index_count, &render_state)) {
        static int logged;

        result = S_OK;
        if (!logged) {
            fprintf(stderr,
                    "[D3D8] native HLE indexed path active "
                    "(indices=%u vertices=%u stride=%u)\n",
                    index_count, vertex_count, draw_stride);
            logged = 1;
        }
    }

done:
    xgpu_plume_set_position_mode(0);
    free(transformed);
    free(normalized);
    return result;
}

static HRESULT expand_indices(const void *indices, D3DFORMAT format,
                              UINT index_count, UINT min_vertex,
                              UINT num_vertices, UINT base_vertex,
                              const BYTE *vertices, UINT vertex_capacity,
                              UINT stride, BYTE **expanded)
{
    BYTE *output;
    UINT i;
    if (!indices || !vertices || !expanded || !stride || !index_count ||
        (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32) ||
        (uint64_t)index_count * stride > SIZE_MAX)
        return E_INVALIDARG;
    output = (BYTE *)malloc((size_t)index_count * stride);
    if (!output)
        return E_OUTOFMEMORY;
    for (i = 0; i < index_count; ++i) {
        uint64_t raw = format == D3DFMT_INDEX32
                           ? ((const uint32_t *)indices)[i]
                           : ((const uint16_t *)indices)[i];
        uint64_t local;
        uint64_t vertex;
        if (num_vertices) {
            if (raw < min_vertex || raw - min_vertex >= num_vertices) {
                free(output);
                return E_INVALIDARG;
            }
            local = raw - min_vertex;
        } else {
            local = raw;
        }
        vertex = local + base_vertex;
        if (vertex >= vertex_capacity) {
            free(output);
            return E_INVALIDARG;
        }
        memcpy(output + (size_t)i * stride,
               vertices + (size_t)vertex * stride, stride);
    }
    *expanded = output;
    return S_OK;
}

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self,
                                            const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    LONG ref;
    UINT i;
    (void)self;
    ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        for (i = 0; i < MAX_TEXTURE_STAGES; ++i) {
            if (g_cur_textures[i])
                ((IDirect3DTexture8 *)g_cur_textures[i])->lpVtbl->Release(
                    (IDirect3DTexture8 *)g_cur_textures[i]);
            g_cur_textures[i] = NULL;
        }
        if (g_cur_vb)
            g_cur_vb->lpVtbl->Release(g_cur_vb);
        if (g_cur_ib)
            g_cur_ib->lpVtbl->Release(g_cur_ib);
        g_cur_vb = NULL;
        g_cur_ib = NULL;
        d3d8_PgraphResourcesShutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        if (g_device_state.render_target)
            surface_Release(&g_device_state.render_target->iface);
        if (g_device_state.back_buffer)
            surface_Release(&g_device_state.back_buffer->iface);
        if (g_device_state.front_buffer)
            surface_Release(&g_device_state.front_buffer->iface);
        if (g_device_state.depth_surface)
            surface_Release(&g_device_state.depth_surface->iface);
        memset(&g_device_state, 0, sizeof(g_device_state));
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self,
                                         IDirect3D8 **factory)
{
    (void)self; (void)factory;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *caps)
{
    (void)self; (void)caps;
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *mode)
{
    (void)self; (void)mode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self,
                                                   void *parameters)
{
    (void)self; (void)parameters;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self,
                                   D3DPRESENT_PARAMETERS *parameters)
{
    (void)self;
    if (!parameters)
        return E_INVALIDARG;
    g_device_state.width = parameters->BackBufferWidth
                               ? parameters->BackBufferWidth
                               : D3D8_PANEL_WIDTH;
    g_device_state.height = parameters->BackBufferHeight
                                ? parameters->BackBufferHeight
                                : D3D8_PANEL_HEIGHT;
    return S_OK;
}

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src,
                                     const RECT *dst, HWND window, void *dirty)
{
    (void)self; (void)src; (void)dst; (void)window; (void)dirty;
    (void)xrecomp_host_pump_messages();
    present_device_backbuffer_with_reason(PLUME_PRESENT_DEVICE);
    return S_OK;
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self,
                                           INT index, DWORD type,
                                           IDirect3DSurface8 **surface)
{
    D3D8Surface *selected;
    (void)self; (void)type;
    if (!surface)
        return E_INVALIDARG;
    selected = index == -1 ? g_device_state.front_buffer
                           : (index == 0 ? g_device_state.back_buffer : NULL);
    if (!selected)
        return E_INVALIDARG;
    *surface = &selected->iface;
    surface_AddRef(*surface);
    return S_OK;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    ++g_begin_count;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    ++g_end_count;
    return S_OK;
}

static void clear_rects(DWORD count, const D3DRECT *rects, DWORD flags,
                        D3DCOLOR color, float depth, DWORD stencil,
                        uint32_t color_write_mask)
{
    DWORD rect_count = count && rects ? count : 1;
    DWORD i;
    float rgba[4] = {
        ((color >> 16) & 0xff) / 255.0f,
        ((color >> 8) & 0xff) / 255.0f,
        (color & 0xff) / 255.0f,
        ((color >> 24) & 0xff) / 255.0f,
    };
    color_write_mask &= 0xFu;
    if ((flags & D3DCLEAR_TARGET) && color_write_mask &&
        (color_write_mask != 0xFu || (count && rects))) {
        /* Preserve channels or pixels excluded by a partial clear. Binding a
         * target alone still does not seed recycled guest allocations. */
        sync_cpu_surface(g_pgraph_current_surface, 1);
    }
    for (i = 0; i < rect_count; ++i) {
        XgpuRect rect;
        const XgpuRect *rect_ptr = NULL;
        if (count && rects) {
            rect.x = (uint32_t)rects[i].x1;
            rect.y = (uint32_t)rects[i].y1;
            rect.width = (uint32_t)(rects[i].x2 - rects[i].x1);
            rect.height = (uint32_t)(rects[i].y2 - rects[i].y1);
            rect_ptr = &rect;
        }
        if ((flags & D3DCLEAR_TARGET) && color_write_mask)
            xgpu_plume_clear_target(rgba[0], rgba[1], rgba[2], rgba[3],
                                    color_write_mask, rect_ptr);
        if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
            xgpu_plume_clear_depth_stencil(
                (flags & D3DCLEAR_ZBUFFER) != 0,
                (flags & D3DCLEAR_STENCIL) != 0, depth, stencil, rect_ptr);
    }
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD count,
                                   const D3DRECT *rects, DWORD flags,
                                   D3DCOLOR color, float depth, DWORD stencil)
{
    uint32_t color_write_mask =
        (flags & D3DCLEAR_TARGET) >> 4;
    (void)self;
    ++g_clear_count;
    clear_rects(count, rects, flags, color, depth, stencil,
                color_write_mask);
    return S_OK;
}

void d3d8_PgraphClearSurface(const XgpuRect *rect, uint32_t color_write_mask,
                             DWORD flags, D3DCOLOR color, float depth,
                             DWORD stencil)
{
    D3DRECT converted;
    if (rect) {
        converted.x1 = (LONG)rect->x;
        converted.y1 = (LONG)rect->y;
        converted.x2 = (LONG)(rect->x + rect->width);
        converted.y2 = (LONG)(rect->y + rect->height);
    }
    ++g_clear_count;
    clear_rects(rect ? 1u : 0u, rect ? &converted : NULL, flags, color, depth,
                stencil, color_write_mask);
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self,
                                          D3DTRANSFORMSTATETYPE state,
                                          const D3DMATRIX *matrix)
{
    (void)self;
    ++g_transform_count;
    if (!matrix || (DWORD)state >= MAX_TRANSFORMS)
        return E_INVALIDARG;
    g_device_state.transforms[(DWORD)state] = *matrix;
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self,
                                          D3DTRANSFORMSTATETYPE state,
                                          D3DMATRIX *matrix)
{
    (void)self;
    if (!matrix || (DWORD)state >= MAX_TRANSFORMS)
        return E_INVALIDARG;
    *matrix = g_device_state.transforms[(DWORD)state];
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self,
                                            D3DRENDERSTATETYPE state,
                                            DWORD value)
{
    DWORD old_value;
    (void)self;
    ++g_state_count;
    if ((DWORD)state >= MAX_RENDER_STATES)
        return E_INVALIDARG;
    old_value = g_device_state.render_states[(DWORD)state];
    if (old_value == value)
        return S_OK;
    g_device_state.render_states[(DWORD)state] = value;
    if (((DWORD)state >= D3DRS_PSCONSTANT0_0 &&
         (DWORD)state <= D3DRS_PSCONSTANT1_7) ||
        state == D3DRS_FOGCOLOR || state == D3DRS_ALPHAREF ||
        state == D3DRS_ALPHAFUNC || state == D3DRS_ALPHATESTENABLE ||
        state == D3DRS_FOGENABLE || state == D3DRS_FOGTABLEMODE ||
        state == D3DRS_FOGSTART || state == D3DRS_FOGEND ||
        state == D3DRS_FOGDENSITY)
        d3d8_combiners_mark_constants_dirty();
    else if ((DWORD)state >= D3DRS_PSALPHAINPUTS0 &&
             (DWORD)state <= D3DRS_PSINPUTTEXTURE)
        d3d8_combiners_mark_dirty();
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self,
                                            D3DRENDERSTATETYPE state,
                                            DWORD *value)
{
    (void)self;
    if (!value || (DWORD)state >= MAX_RENDER_STATES)
        return E_INVALIDARG;
    *value = g_device_state.render_states[(DWORD)state];
    return S_OK;
}

static void update_sampler(DWORD stage)
{
    XgpuSamplerBinding binding;
    DWORD lod_bias_bits =
        g_device_state.tss[stage][D3DTSS_MIPMAPLODBIAS];

    memset(&binding, 0, sizeof(binding));
    binding.stage = stage;
    binding.address_u = g_device_state.tss[stage][D3DTSS_ADDRESSU];
    binding.address_v = g_device_state.tss[stage][D3DTSS_ADDRESSV];
    if (!binding.address_u)
        binding.address_u = XGPU_SAMPLER_ADDRESS_WRAP;
    if (!binding.address_v)
        binding.address_v = XGPU_SAMPLER_ADDRESS_WRAP;
    /* Xbox D3D8 exposes U/V stage state; use U for the volume/cube P axis. */
    binding.address_w = binding.address_u;
    binding.min_filter = g_device_state.tss[stage][D3DTSS_MINFILTER];
    binding.mag_filter = g_device_state.tss[stage][D3DTSS_MAGFILTER];
    binding.mip_filter = g_device_state.tss[stage][D3DTSS_MIPFILTER];
    if (!binding.min_filter)
        binding.min_filter = XGPU_SAMPLER_FILTER_LINEAR;
    if (!binding.mag_filter)
        binding.mag_filter = XGPU_SAMPLER_FILTER_LINEAR;
    memcpy(&binding.mip_lod_bias, &lod_bias_bits,
           sizeof(binding.mip_lod_bias));
    binding.max_mip_level =
        g_device_state.tss[stage][D3DTSS_MAXMIPLEVEL];
    binding.max_anisotropy =
        g_device_state.tss[stage][D3DTSS_MAXANISOTROPY];
    if (!binding.max_anisotropy)
        binding.max_anisotropy = 1;
    binding.border_color =
        g_device_state.tss[stage][D3DTSS_BORDERCOLOR];
    xgpu_plume_set_sampler_ex(&binding);
}

static HRESULT __stdcall dev_SetTextureStageState(
    IDirect3DDevice8 *self, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD value)
{
    (void)self;
    if (stage >= MAX_TEXTURE_STAGES || (DWORD)type >= MAX_TSS_STATES)
        return E_INVALIDARG;
    if (type == D3DTSS_COLORKEYOP &&
        !d3d8_combiners_set_color_key_mode(stage, value))
        return E_INVALIDARG;
    g_device_state.tss[stage][(DWORD)type] = value;
    if (type == D3DTSS_COLOROP || type == D3DTSS_COLORARG0 ||
        type == D3DTSS_COLORARG1 || type == D3DTSS_COLORARG2 ||
        type == D3DTSS_ALPHAOP || type == D3DTSS_ALPHAARG0 ||
        type == D3DTSS_ALPHAARG1 || type == D3DTSS_ALPHAARG2)
        g_device_state.fixed_state_valid = TRUE;
    update_sampler(stage);
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(
    IDirect3DDevice8 *self, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
    DWORD *value)
{
    (void)self;
    if (!value || stage >= MAX_TEXTURE_STAGES ||
        (DWORD)type >= MAX_TSS_STATES)
        return E_INVALIDARG;
    *value = g_device_state.tss[stage][(DWORD)type];
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD stage,
                                        IDirect3DBaseTexture8 *texture)
{
    IDirect3DBaseTexture8 *previous;
    (void)self;
    ++g_texture_count;
    if (stage >= MAX_TEXTURE_STAGES)
        return E_INVALIDARG;
    if (texture)
        ((IDirect3DTexture8 *)texture)->lpVtbl->AddRef(
            (IDirect3DTexture8 *)texture);
    previous = g_cur_textures[stage];
    g_cur_textures[stage] = texture;
    if (previous)
        ((IDirect3DTexture8 *)previous)->lpVtbl->Release(
            (IDirect3DTexture8 *)previous);
    mirror_texture(stage, texture);
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD stage,
                                        IDirect3DBaseTexture8 **texture)
{
    (void)self;
    if (!texture || stage >= MAX_TEXTURE_STAGES)
        return E_INVALIDARG;
    *texture = g_cur_textures[stage];
    if (*texture)
        ((IDirect3DTexture8 *)*texture)->lpVtbl->AddRef(
            (IDirect3DTexture8 *)*texture);
    return S_OK;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self,
                                             UINT stream,
                                             IDirect3DVertexBuffer8 *buffer,
                                             UINT stride)
{
    IDirect3DVertexBuffer8 *previous;
    (void)self;
    if (stream != 0)
        return E_INVALIDARG;
    if (buffer)
        buffer->lpVtbl->AddRef(buffer);
    previous = g_cur_vb;
    g_cur_vb = buffer;
    g_cur_vb_stride = stride;
    if (previous)
        previous->lpVtbl->Release(previous);
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self,
                                             UINT stream,
                                             IDirect3DVertexBuffer8 **buffer,
                                             UINT *stride)
{
    (void)self;
    if (stream != 0 || !buffer || !stride)
        return E_INVALIDARG;
    *buffer = g_cur_vb;
    *stride = g_cur_vb_stride;
    if (*buffer)
        (*buffer)->lpVtbl->AddRef(*buffer);
    return S_OK;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self,
                                        IDirect3DIndexBuffer8 *buffer,
                                        UINT base_vertex)
{
    IDirect3DIndexBuffer8 *previous;
    (void)self;
    if (buffer)
        buffer->lpVtbl->AddRef(buffer);
    previous = g_cur_ib;
    g_cur_ib = buffer;
    g_cur_ib_base_vertex = base_vertex;
    if (previous)
        previous->lpVtbl->Release(previous);
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self,
                                        IDirect3DIndexBuffer8 **buffer,
                                        UINT *base_vertex)
{
    (void)self;
    if (!buffer || !base_vertex)
        return E_INVALIDARG;
    *buffer = g_cur_ib;
    *base_vertex = g_cur_ib_base_vertex;
    if (*buffer)
        (*buffer)->lpVtbl->AddRef(*buffer);
    return S_OK;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self,
                                           D3DPRIMITIVETYPE type,
                                           UINT start_vertex,
                                           UINT primitive_count)
{
    D3D8VertexBuffer *buffer;
    UINT count;
    uint64_t start;
    uint64_t bytes;
    (void)self;
    InterlockedIncrement(&g_draw_count);
    if (!g_cur_vb || !g_cur_vb_stride ||
        !primitive_vertex_count(type, primitive_count, &count))
        return E_INVALIDARG;
    buffer = (D3D8VertexBuffer *)g_cur_vb;
    start = (uint64_t)start_vertex * g_cur_vb_stride;
    bytes = (uint64_t)count * g_cur_vb_stride;
    if (start > buffer->size || bytes > buffer->size - start)
        return E_INVALIDARG;
    return submit_draw(type, primitive_count, buffer->sys_mem + start,
                       g_cur_vb_stride);
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(
    IDirect3DDevice8 *self, D3DPRIMITIVETYPE type, UINT min_vertex,
    UINT num_vertices, UINT start_index, UINT primitive_count)
{
    D3D8VertexBuffer *vertex_buffer;
    D3D8IndexBuffer *index_buffer;
    UINT index_count;
    UINT index_size;
    uint64_t index_offset;
    BYTE *expanded = NULL;
    HRESULT result;
    (void)self;
    InterlockedIncrement(&g_draw_count);
    if (!g_cur_vb || !g_cur_ib || !g_cur_vb_stride ||
        !primitive_vertex_count(type, primitive_count, &index_count))
        return E_INVALIDARG;
    vertex_buffer = (D3D8VertexBuffer *)g_cur_vb;
    index_buffer = (D3D8IndexBuffer *)g_cur_ib;
    index_size = index_buffer->format == D3DFMT_INDEX32 ? 4u : 2u;
    index_offset = (uint64_t)start_index * index_size;
    if (index_offset > index_buffer->size ||
        (uint64_t)index_count * index_size > index_buffer->size - index_offset)
        return E_INVALIDARG;
    result = expand_indices(index_buffer->sys_mem + index_offset,
                            index_buffer->format, index_count, min_vertex,
                            num_vertices, g_cur_ib_base_vertex,
                            vertex_buffer->sys_mem,
                            vertex_buffer->size / g_cur_vb_stride,
                            g_cur_vb_stride, &expanded);
    if (FAILED(result))
        return result;
    result = submit_draw(type, primitive_count, expanded, g_cur_vb_stride);
    free(expanded);
    return result;
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self,
                                             D3DPRIMITIVETYPE type,
                                             UINT primitive_count,
                                             const void *vertices,
                                             UINT stride)
{
    HRESULT result;
    (void)self;
    InterlockedIncrement(&g_draw_count);
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_compat_draw_zone, "D3D Compat Record Draw");
    result = submit_draw(type, primitive_count, vertices, stride);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_draw_zone);
    return result;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(
    IDirect3DDevice8 *self, D3DPRIMITIVETYPE type, UINT min_vertex,
    UINT num_vertices, UINT primitive_count, const void *indices,
    D3DFORMAT index_format, const void *vertices, UINT stride)
{
    UINT index_count;
    BYTE *expanded = NULL;
    HRESULT result;
    (void)self;
    InterlockedIncrement(&g_draw_count);
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_compat_indexed_zone, "D3D Compat Indexed Draw");
    if (!primitive_vertex_count(type, primitive_count, &index_count)) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_indexed_zone);
        return E_INVALIDARG;
    }
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_compat_native_indexed_zone, "D3D Compat Native Indexed Try");
    result = submit_persistent_indexed_draw(
        type, primitive_count, min_vertex, num_vertices,
        indices, index_format, (const BYTE *)vertices, stride);
    if (result != S_FALSE) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_native_indexed_zone);
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_indexed_zone);
        return result;
    }
    result = submit_native_indexed_draw(
        type, primitive_count, min_vertex, num_vertices,
        indices, index_format, (const BYTE *)vertices, stride);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_native_indexed_zone);
    if (result != S_FALSE) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_indexed_zone);
        return result;
    }
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_compat_expand_zone, "D3D Compat Expand Indices");
    result = expand_indices(indices, index_format, index_count, min_vertex,
                            num_vertices, 0, (const BYTE *)vertices,
                            num_vertices, stride, &expanded);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_expand_zone);
    if (FAILED(result)) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_indexed_zone);
        return result;
    }
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_compat_record_zone, "D3D Compat Record Draw");
    result = submit_draw(type, primitive_count, expanded, stride);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_record_zone);
    free(expanded);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_compat_indexed_zone);
    return result;
}

static HRESULT __stdcall dev_CreateTexture(
    IDirect3DDevice8 *self, UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8 **texture)
{
    (void)self; (void)pool;
    return d3d8_CreateTextureImpl(width, height, levels, usage, format, texture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(
    IDirect3DDevice8 *self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer8 **buffer)
{
    (void)self; (void)pool;
    return d3d8_CreateVertexBufferImpl(length, usage, fvf, buffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(
    IDirect3DDevice8 *self, UINT length, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DIndexBuffer8 **buffer)
{
    (void)self; (void)pool;
    return d3d8_CreateIndexBufferImpl(length, usage, format, buffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(
    IDirect3DDevice8 *self, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, BOOL lockable,
    IDirect3DSurface8 **surface)
{
    (void)self; (void)multisample; (void)lockable;
    return create_surface(width, height, format, D3DUSAGE_RENDERTARGET,
                          surface);
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(
    IDirect3DDevice8 *self, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, IDirect3DSurface8 **surface)
{
    (void)self; (void)multisample;
    return create_surface(width, height, format, D3DUSAGE_DEPTHSTENCIL,
                          surface);
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self,
                                             IDirect3DSurface8 *render_target,
                                             IDirect3DSurface8 *depth_surface)
{
    D3D8Surface *old_render_target;
    D3D8Surface *old_depth_surface;
    (void)self;
    if (!render_target)
        return E_INVALIDARG;
    surface_AddRef(render_target);
    if (depth_surface)
        surface_AddRef(depth_surface);
    old_render_target = g_device_state.render_target;
    old_depth_surface = g_device_state.depth_surface;
    g_device_state.render_target = surface_from_iface(render_target);
    g_device_state.depth_surface = depth_surface
                                       ? surface_from_iface(depth_surface)
                                       : NULL;
    if (old_render_target)
        surface_Release(&old_render_target->iface);
    if (old_depth_surface)
        surface_Release(&old_depth_surface->iface);
    bind_direct_surfaces();
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self,
                                             IDirect3DSurface8 **surface)
{
    (void)self;
    if (!surface || !g_device_state.render_target)
        return E_INVALIDARG;
    *surface = &g_device_state.render_target->iface;
    surface_AddRef(*surface);
    return S_OK;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(
    IDirect3DDevice8 *self, IDirect3DSurface8 **surface)
{
    (void)self;
    if (!surface || !g_device_state.depth_surface)
        return E_INVALIDARG;
    *surface = &g_device_state.depth_surface->iface;
    surface_AddRef(*surface);
    return S_OK;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self,
                                         const D3DVIEWPORT8 *viewport)
{
    (void)self;
    if (!viewport || !viewport->Width || !viewport->Height)
        return E_INVALIDARG;
    g_device_state.viewport = *viewport;
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self,
                                         D3DVIEWPORT8 *viewport)
{
    (void)self;
    if (!viewport)
        return E_INVALIDARG;
    *viewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self,
                                         const D3DMATERIAL8 *material)
{
    (void)self;
    if (!material)
        return E_INVALIDARG;
    g_device_state.material = *material;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self,
                                         D3DMATERIAL8 *material)
{
    (void)self;
    if (!material)
        return E_INVALIDARG;
    *material = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD index,
                                      const D3DLIGHT8 *light)
{
    (void)self;
    if (!light || index >= MAX_LIGHTS)
        return E_INVALIDARG;
    g_device_state.lights[index] = *light;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD index,
                                      D3DLIGHT8 *light)
{
    (void)self;
    if (!light || index >= MAX_LIGHTS)
        return E_INVALIDARG;
    *light = g_device_state.lights[index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD index,
                                         BOOL enable)
{
    (void)self;
    if (index >= MAX_LIGHTS)
        return E_INVALIDARG;
    g_device_state.light_enable[index] = enable;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self,
                                             DWORD handle)
{
    (void)self;
    g_device_state.vertex_shader = handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self,
                                             DWORD *handle)
{
    (void)self;
    if (!handle)
        return E_INVALIDARG;
    *handle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(
    IDirect3DDevice8 *self, INT start, const void *data, DWORD count)
{
    (void)self;
    if (!data || start < 0)
        return E_INVALIDARG;
    d3d8_vsh_set_constant(start, (const float *)data, (int)count);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self,
                                            DWORD handle)
{
    (void)self;
    g_device_state.pixel_shader = handle;
    d3d8_combiners_set_pixel_shader(handle);
    xgpu_plume_set_active_ps(handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self,
                                            DWORD *handle)
{
    (void)self;
    if (!handle)
        return E_INVALIDARG;
    *handle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(
    IDirect3DDevice8 *self, INT start, const void *data, DWORD count)
{
    (void)self;
    if (!data || start < 0)
        return E_INVALIDARG;
    xgpu_plume_set_ps_const((uint32_t)start, (const float *)data, count);
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD flags,
                                       const D3DGAMMARAMP *ramp)
{
    (void)self; (void)flags; (void)ramp;
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self,
                                       D3DGAMMARAMP *ramp)
{
    (void)self;
    if (ramp)
        memset(ramp, 0, sizeof(*ramp));
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD number,
                                        const void *entries)
{
    (void)self; (void)number; (void)entries;
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD count,
                                       DWORD **push)
{
    (void)self; (void)count; (void)push;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *push)
{
    (void)self; (void)push;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD flags)
{
    (void)self; (void)flags;
    (void)xrecomp_host_pump_messages();
    present_device_backbuffer_with_reason(PLUME_PRESENT_SWAP);
    return S_OK;
}

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetDirect3D,
    dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters,
    dev_Reset, dev_Present, dev_GetBackBuffer, dev_BeginScene, dev_EndScene,
    dev_Clear, dev_SetTransform, dev_GetTransform, dev_SetRenderState,
    dev_GetRenderState, dev_SetTextureStageState, dev_GetTextureStageState,
    dev_SetTexture, dev_GetTexture, dev_SetStreamSource, dev_GetStreamSource,
    dev_SetIndices, dev_GetIndices, dev_DrawPrimitive,
    dev_DrawIndexedPrimitive, dev_DrawPrimitiveUP, dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture, dev_CreateVertexBuffer, dev_CreateIndexBuffer,
    dev_CreateRenderTarget, dev_CreateDepthStencilSurface, dev_SetRenderTarget,
    dev_GetRenderTarget, dev_GetDepthStencilSurface, dev_SetViewport,
    dev_GetViewport, dev_SetMaterial, dev_GetMaterial, dev_SetLight,
    dev_GetLight, dev_LightEnable, dev_SetVertexShader, dev_GetVertexShader,
    dev_SetVertexShaderConstant, dev_SetPixelShader, dev_GetPixelShader,
    dev_SetPixelShaderConstant, dev_SetGammaRamp, dev_GetGammaRamp,
    dev_SetPalette, dev_BeginPush, dev_EndPush, dev_Swap,
};

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

int d3d8_SetOutputExtent(UINT width, UINT height)
{
    D3D8Surface *back_buffer;
    D3D8Surface *front_buffer;
    D3D8Surface *depth_surface;
    BYTE *new_back_memory;
    BYTE *new_front_memory = NULL;
    BYTE *new_depth_memory = NULL;
    UINT new_back_pitch;
    UINT new_depth_pitch = 0;
    size_t back_bytes;
    size_t depth_bytes = 0;
    UINT old_width;
    UINT old_height;
    BOOL resize_viewport;

    if (!g_device_initialized || !width || !height)
        return 0;
    if (width == g_device_state.width && height == g_device_state.height)
        return 1;
    back_buffer = g_device_state.back_buffer;
    front_buffer = g_device_state.front_buffer;
    depth_surface = g_device_state.depth_surface;
    if (!back_buffer)
        return 0;

    new_back_pitch = d3d8_row_pitch(back_buffer->format, width);
    if (!new_back_pitch || height > SIZE_MAX / new_back_pitch)
        return 0;
    back_bytes = (size_t)new_back_pitch * height;
    new_back_memory = (BYTE *)calloc(1, back_bytes);
    if (!new_back_memory)
        return 0;
    if (front_buffer) {
        new_front_memory = (BYTE *)calloc(1, back_bytes);
        if (!new_front_memory) {
            free(new_back_memory);
            return 0;
        }
    }
    if (depth_surface) {
        new_depth_pitch = d3d8_row_pitch(depth_surface->format, width);
        if (!new_depth_pitch || height > SIZE_MAX / new_depth_pitch) {
            free(new_front_memory);
            free(new_back_memory);
            return 0;
        }
        depth_bytes = (size_t)new_depth_pitch * height;
        new_depth_memory = (BYTE *)calloc(1, depth_bytes);
        if (!new_depth_memory) {
            free(new_front_memory);
            free(new_back_memory);
            return 0;
        }
    }

    if (!xgpu_plume_set_output_extent(width, height)) {
        free(new_depth_memory);
        free(new_front_memory);
        free(new_back_memory);
        return 0;
    }

    old_width = g_device_state.width;
    old_height = g_device_state.height;
    resize_viewport = g_device_state.viewport.X == 0u &&
        g_device_state.viewport.Y == 0u &&
        g_device_state.viewport.Width == old_width &&
        g_device_state.viewport.Height == old_height;
    free(back_buffer->sys_mem);
    back_buffer->sys_mem = new_back_memory;
    back_buffer->pitch = new_back_pitch;
    back_buffer->width = width;
    back_buffer->height = height;
    back_buffer->dirty = TRUE;
    if (front_buffer) {
        free(front_buffer->sys_mem);
        front_buffer->sys_mem = new_front_memory;
        front_buffer->pitch = new_back_pitch;
        front_buffer->width = width;
        front_buffer->height = height;
        front_buffer->dirty = TRUE;
    }
    if (depth_surface) {
        free(depth_surface->sys_mem);
        depth_surface->sys_mem = new_depth_memory;
        depth_surface->pitch = new_depth_pitch;
        depth_surface->width = width;
        depth_surface->height = height;
        depth_surface->dirty = TRUE;
    }
    g_device_state.width = width;
    g_device_state.height = height;
    if (resize_viewport) {
        g_device_state.viewport.Width = width;
        g_device_state.viewport.Height = height;
    }

    if (g_device_state.render_target == back_buffer)
        bind_direct_surfaces();
    return 1;
}

void xbox_d3d8_set_window_title(const char *title)
{
    (void)title;
}

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self,
                                             const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(
    IDirect3D8 *self, UINT adapter, DWORD device_type, HWND focus_window,
    DWORD behavior_flags, D3DPRESENT_PARAMETERS *parameters,
    IDirect3DDevice8 **device)
{
#if defined(_WIN32)
    XgpuNativeWindow native_window;
#endif
    IDirect3DSurface8 *surface = NULL;
    HRESULT result;
    (void)self; (void)adapter; (void)device_type; (void)behavior_flags;
    if (!parameters || !device)
        return E_INVALIDARG;
    *device = NULL;
    if (g_device_initialized)
        return E_FAIL;
    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;
    g_device_state.native_window = parameters->hDeviceWindow
                                       ? parameters->hDeviceWindow
                                       : focus_window;
    g_device_state.width = parameters->BackBufferWidth
                               ? parameters->BackBufferWidth
                               : D3D8_PANEL_WIDTH;
    g_device_state.height = parameters->BackBufferHeight
                                ? parameters->BackBufferHeight
                                : D3D8_PANEL_HEIGHT;
    g_device_state.backbuffer_format = parameters->BackBufferFormat;
    init_default_states(&g_device_state);

    xgpu_plume_set_output_extent(g_device_state.width, g_device_state.height);

#if defined(_WIN32)
    memset(&native_window, 0, sizeof(native_window));
    native_window.kind = XGPU_NATIVE_WINDOW_WIN32;
    native_window.window = (uintptr_t)g_device_state.native_window;
    native_window.view = g_device_state.native_window;
    xgpu_plume_set_native_window(&native_window);
#endif

    result = create_surface(g_device_state.width, g_device_state.height,
                            g_device_state.backbuffer_format,
                            D3DUSAGE_RENDERTARGET, &surface);
    if (FAILED(result))
        return result;
    g_device_state.back_buffer = surface_from_iface(surface);
    g_device_state.render_target = g_device_state.back_buffer;
    surface_AddRef(surface);
    surface = NULL;
    result = create_surface(g_device_state.width, g_device_state.height,
                            g_device_state.backbuffer_format,
                            D3DUSAGE_RENDERTARGET, &surface);
    if (FAILED(result))
        goto fail;
    g_device_state.front_buffer = surface_from_iface(surface);
    if (parameters->EnableAutoDepthStencil) {
        surface = NULL;
        result = create_surface(g_device_state.width, g_device_state.height,
                                parameters->AutoDepthStencilFormat,
                                D3DUSAGE_DEPTHSTENCIL, &surface);
        if (FAILED(result)) {
            surface_Release(&g_device_state.render_target->iface);
            surface_Release(&g_device_state.back_buffer->iface);
            g_device_state.back_buffer = NULL;
            g_device_state.render_target = NULL;
            return result;
        }
        g_device_state.depth_surface = surface_from_iface(surface);
    }
    bind_direct_surfaces();
    result = d3d8_combiners_init();
    if (FAILED(result))
        goto fail;
    result = d3d8_vsh_init();
    if (FAILED(result)) {
        d3d8_combiners_shutdown();
        goto fail;
    }
    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;
    *device = &g_device;
    fprintf(stderr, "D3D8: Plume device created (%ux%u)\n",
            g_device_state.width, g_device_state.height);
    return S_OK;

fail:
    if (g_device_state.depth_surface)
        surface_Release(&g_device_state.depth_surface->iface);
    if (g_device_state.render_target)
        surface_Release(&g_device_state.render_target->iface);
    if (g_device_state.back_buffer)
        surface_Release(&g_device_state.back_buffer->iface);
    if (g_device_state.front_buffer)
        surface_Release(&g_device_state.front_buffer->iface);
    memset(&g_device_state, 0, sizeof(g_device_state));
    return result;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface, d3d8_AddRef, d3d8_Release, d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT sdk_version)
{
    (void)sdk_version;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
