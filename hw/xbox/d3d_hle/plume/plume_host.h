/*
 * plume_host.h — Game-agnostic Plume RHI host API for the xboxrecomp runtime.
 *
 * Implemented in C++ (plume_backend.cpp); consumed from C via this header.
 * Tied to Xbox D3D8 types (D3DPRIMITIVETYPE, D3DFORMAT, FVF) — not any one
 * recompiled title.
 */
#ifndef XGPU_PLUME_HOST_H
#define XGPU_PLUME_HOST_H

#include <stdint.h>
#include "plume_frametime.h"
#include "plume_perf.h"
#include "../xgpu_native_window.h"
#include "../xgpu_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default Xbox framebuffer dimensions live in xgpu_renderer.h. */

/* The host owns native-window creation and event processing. */
void xgpu_plume_set_native_window(const XgpuNativeWindow *window);
/* Retire guest work and reset draw/session state while retaining the process-
 * lifetime host device and swapchain. */
void xgpu_plume_reset_session(void);
/* Retire all Plume work and release the host RHI output at process teardown.
 * The overlay provider remains registered and becomes inert until the next
 * host device is initialized. */
void xgpu_plume_teardown_output(void);
/* Returns the live native API name only after Plume initialized it. */
const char *xgpu_plume_get_active_backend_name(void);
/* Set the logical output extent. After renderer startup this performs a
 * synchronized live resource transition. */
int xgpu_plume_set_output_extent(uint32_t width, uint32_t height);
/* Change the integer physical render-target scale immediately (1..10). */
int xgpu_plume_set_internal_resolution_scale(uint32_t scale);
uint32_t xgpu_plume_get_internal_resolution_scale(void);

/*
 * Bracket a title-owned UI phase that targets a fixed logical canvas. Centered
 * mode preserves the whole canvas at its native size; anchored mode preserves
 * small widget size while anchoring left/center/right draws and stretches only
 * near-full-width panels to cover the output.
 */
typedef enum XgpuPlumeUiCanvasMode {
    XGPU_PLUME_UI_CANVAS_ANCHORED = 0,
    XGPU_PLUME_UI_CANVAS_CENTERED = 1,
} XgpuPlumeUiCanvasMode;
void xgpu_plume_ui_canvas_begin(float logical_width, float logical_height,
                                XgpuPlumeUiCanvasMode mode);
void xgpu_plume_ui_canvas_end(void);
int xgpu_plume_ui_canvas_active(void);

/*
 * Named recording spans over the frame recording. A span brackets the draws
 * one caller-defined unit appends; a cached span can later be re-injected in
 * place of re-recording it. Keys are opaque to Plume; the caller owns their
 * packing and invalidation semantics.
 */
void xgpu_plume_span_begin(uint32_t key);
/* Returns the number of GeomDraws appended while the span was open. */
uint32_t xgpu_plume_span_end(uint32_t key);
/* Returns 1 when a cached packet was injected instead of drawing. */
int xgpu_plume_span_try_replay(uint32_t key);
void xgpu_plume_span_invalidate(uint32_t key);
/* Drop every packet whose (key & key_mask) == (key_bits & key_mask). */
void xgpu_plume_span_invalidate_mask(uint32_t key_mask, uint32_t key_bits);
void xgpu_plume_span_invalidate_all(void);
/* Number of GeomDraws recorded so far in the current frame recording. */
uint32_t xgpu_plume_recorded_draw_count(void);

/*
 * Optional host diagnostic composited after the guest frame and immediately
 * before swap-chain presentation. The provider is called only for accepted
 * host presents. Pixels are straight-alpha RGBA8 and remain owned by the
 * provider for the duration of the call. Logical overlays are drawn into the
 * Xbox-sized scene before output scaling; host overlays are drawn at native
 * swap-chain resolution after output scaling.
 */
typedef enum XgpuPlumeDebugOverlaySpace {
    XGPU_PLUME_DEBUG_OVERLAY_SPACE_LOGICAL = 0,
    XGPU_PLUME_DEBUG_OVERLAY_SPACE_HOST = 1
} XgpuPlumeDebugOverlaySpace;

typedef struct XgpuPlumeDebugOverlayFrame {
    const void *pixels;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t version;
    uint32_t space;
} XgpuPlumeDebugOverlayFrame;

typedef int (*XgpuPlumeDebugOverlayProvider)(
    XgpuPlumeDebugOverlayFrame *frame);

enum {
    XGPU_PLUME_DEBUG_OVERLAY_LAYER_DIAGNOSTIC = 0,
    XGPU_PLUME_DEBUG_OVERLAY_LAYER_MENU = 100,
    XGPU_PLUME_MAX_DEBUG_OVERLAY_PROVIDERS = 8
};

/* Providers are ordered by layer and retain a stable texture slot. */
int xgpu_plume_register_debug_overlay_provider(
    XgpuPlumeDebugOverlayProvider provider,
    int32_t layer);

/* Frame loop driven by the generic D3D8 / NV2A path. */
/* Change the live swap-chain synchronization policy. The setting is retained
 * until Plume initializes, so title/runtime configuration may arrive before
 * the first host-visible frame. */
void xgpu_plume_set_vsync_enabled(int enabled);
void xgpu_plume_present_set_clear(float r, float g, float b, float a);
void xgpu_plume_present_frame(void);
void xgpu_plume_set_render_target(const XgpuSurfaceBinding *binding);
void xgpu_plume_clear_target(float r, float g, float b, float a,
                             uint32_t color_write_mask,
                             const XgpuRect *rect);
void xgpu_plume_set_surface_texture(uint32_t stage, uint32_t guest_ptr,
                                    uint32_t unnormalized_coords);
void xgpu_plume_set_present_surface(uint32_t guest_ptr);
int xgpu_plume_wait_for_idle(uint32_t surface_sync_flags);

typedef struct XgpuPlumeRenderState {
    uint32_t depth_enable;
    uint32_t z_perspective;
    uint32_t depth_write;
    uint32_t depth_func;
    uint32_t blend_enable;
    uint32_t src_blend;
    uint32_t dst_blend;
    uint32_t blend_op;
    uint32_t blend_color;
    uint32_t alpha_test_enable;
    uint32_t alpha_func;
    uint32_t alpha_ref;
    uint32_t cull_mode;
    uint32_t color_write_mask;
    uint32_t zeta_format;
    uint32_t zeta_float;
    uint32_t stencil_enable;
    uint32_t stencil_func;
    uint32_t stencil_ref;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_fail;
    uint32_t stencil_zfail;
    uint32_t stencil_pass;
    uint32_t depth_bias_bits;
    uint32_t slope_scaled_depth_bias_bits;
    uint32_t position_mode;
    uint32_t ui_canvas_active;
    uint32_t ui_canvas_mode;
    float ui_canvas_width;
    float ui_canvas_height;
    uint32_t programmable_position_bounds_valid;
    float programmable_position_min_x;
    float programmable_position_max_x;
    /*
     * Four-stage fixed-function texture combiner state. Programmable pixel
     * shaders ignore these fields; the shared PS=0 fallback snapshots them
     * with each draw so later SetTextureStageState calls cannot mutate it.
     */
    uint32_t fixed_color_op[4];
    uint32_t fixed_color_arg0[4];
    uint32_t fixed_color_arg1[4];
    uint32_t fixed_color_arg2[4];
    uint32_t fixed_alpha_op[4];
    uint32_t fixed_alpha_arg0[4];
    uint32_t fixed_alpha_arg1[4];
    uint32_t fixed_alpha_arg2[4];
    uint32_t fixed_texture_factor;
    uint32_t fixed_state_valid;
    /*
     * Raw Xbox D3D viewport.  The Plume draw recorder combines this with the
     * currently bound zeta format to reproduce the XDK-managed c[-38]/c[-37]
     * viewport constants and matching host depth normalization.
     */
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    float viewport_min_z;
    float viewport_max_z;
    uint32_t scissor_enable;
    uint32_t scissor_x;
    uint32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    uint32_t stipple_enable;
    uint32_t stipple_pattern[32];
} XgpuPlumeRenderState;

typedef struct XgpuTextureBinding {
    uint32_t stage;
    uint32_t guest_ptr;
    const void *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t levels;
    uint32_t dimensionality;
    uint32_t pitch;
    uint32_t bytes;
    uint32_t format;
    uint64_t version;
    uint32_t cube;      /* 1 = 6 consecutive faces (NV2A cubemap layout) */
    /*
     * Xbox linear (pitch) textures use texel-space coordinates. Host APIs
     * sample normalized coordinates, so Plume must apply 1/width, 1/height
     * (and 1/depth) in the programmable pixel path.
     */
    uint32_t unnormalized_coords;
} XgpuTextureBinding;

typedef enum XgpuSamplerAddressMode {
    XGPU_SAMPLER_ADDRESS_WRAP = 1,
    XGPU_SAMPLER_ADDRESS_MIRROR = 2,
    XGPU_SAMPLER_ADDRESS_CLAMP = 3,
    XGPU_SAMPLER_ADDRESS_BORDER = 4,
    XGPU_SAMPLER_ADDRESS_MIRROR_ONCE = 5,
} XgpuSamplerAddressMode;

typedef enum XgpuSamplerFilter {
    XGPU_SAMPLER_FILTER_NONE = 0,
    XGPU_SAMPLER_FILTER_POINT = 1,
    XGPU_SAMPLER_FILTER_LINEAR = 2,
    XGPU_SAMPLER_FILTER_ANISOTROPIC = 3,
} XgpuSamplerFilter;

/*
 * Explicit sampler state shared by the direct D3D HLE and packed-NV2A
 * frontends. Values use the XGPU enums above, not either frontend's register
 * encoding.
 */
typedef struct XgpuSamplerBinding {
    uint32_t stage;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mip_filter;
    float mip_lod_bias;
    uint32_t max_mip_level;
    uint32_t max_anisotropy;
    /* Guest D3DCOLOR / NV097 A8R8G8B8 border value. */
    uint32_t border_color;
} XgpuSamplerBinding;

#define XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT 16

/*
 * Exact Xbox D3D8 vertex-declaration mapping retained with a translated
 * programmable vertex shader. Formats are X_D3DVSDT_* / NV2A attribute type
 * bytes, not host RenderFormat values.
 */
typedef struct XgpuPlumeVertexDeclaration {
    uint16_t attributes_present;
    uint8_t stream[XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT];
    uint8_t format[XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT];
    uint16_t offset[XGPU_PLUME_VERTEX_ATTRIBUTE_COUNT];
} XgpuPlumeVertexDeclaration;

void xgpu_plume_record_draw(uint32_t prim_type, uint32_t prim_count,
                            const void *verts, uint32_t stride, uint32_t fvf,
                            const XgpuPlumeRenderState *render_state);
/* Record an ordinary compatibility draw with owned uint32 index data. Vertex
 * and index bytes are copied into the frame recording before return. */
int xgpu_plume_record_indexed_draw(
    uint32_t prim_type, uint32_t prim_count,
    const void *verts, uint32_t vertex_count,
    uint32_t stride, uint32_t fvf,
    const uint32_t *indices, uint32_t index_count,
    const XgpuPlumeRenderState *render_state);

typedef struct XgpuPlumeCachedIndexedDraw {
    uint32_t prim_type;
    uint32_t prim_count;
    uint32_t vb_data_va;
    uint32_t ib_data_va;
    uint32_t index_byte_offset;
    uint32_t index_count;
    uint32_t stride;
    uint32_t base_vertex;
    uint32_t fvf_or_vs;
    uint64_t vb_generation;
    uint64_t ib_generation;
    const void *vertices;
    uint32_t vertex_count;
    const uint16_t *indices;
    const XgpuPlumeRenderState *render_state;
} XgpuPlumeCachedIndexedDraw;

/* 1 = recorded against a persistent GPU mesh (hit or first upload).
 * 0 = caller must use the existing expand/record_draw path. */
int xgpu_plume_record_cached_indexed_draw(
    const XgpuPlumeCachedIndexedDraw *draw);
void xgpu_plume_mesh_cache_invalidate_va(uint32_t resource_data_va);
void xgpu_plume_set_position_mode(uint32_t mode);
void xgpu_plume_clear_depth_stencil(uint32_t clear_depth,
                                    uint32_t clear_stencil,
                                    float depth, uint32_t stencil,
                                    const XgpuRect *rect);
int xgpu_plume_present_host_frame(const void *pixels, uint32_t w, uint32_t h,
                                  uint32_t pitch);
int xgpu_plume_present_host_frame_format(const void *pixels, uint32_t w,
                                         uint32_t h, uint32_t pitch,
                                         uint32_t format);
/* Stop retaining the last host frame across otherwise-empty guest
 * presents. Overlay frontends call this when their host-frame sequence ends
 * so an already-rendered guest scanout can become visible immediately. */
void xgpu_plume_retire_host_frame(void);

void xgpu_plume_set_texture(uint32_t stage, uint32_t guest_ptr,
                            const void *pixels, uint32_t w, uint32_t h,
                            uint32_t pitch, uint32_t bytes, uint32_t format,
                            uint64_t version);
void xgpu_plume_set_texture_ex(const XgpuTextureBinding *binding);
/*
 * Bind an existing exact guest/version/shape cache entry. Frontends can use
 * this before converting guest pixels; zero means an upload is still needed.
 */
int xgpu_plume_bind_texture_if_cached(const XgpuTextureBinding *binding);
/*
 * Monotonic ownership serial for a host texture slot. Both the retained D3D8
 * frontend and the direct NV2A frontend write these slots; a frontend-side
 * binding cache must compare this serial before assuming its last binding is
 * still active.
 */
uint64_t xgpu_plume_texture_binding_serial(uint32_t stage);
void xgpu_plume_set_sampler_ex(const XgpuSamplerBinding *binding);
/* Packed NV2A compatibility entry point. New frontends should use _ex. */
void xgpu_plume_set_sampler(uint32_t stage, uint32_t address, uint32_t filter,
                            uint32_t border_color);

/* Programmable pixel shaders (Xbox assembly or direct portable HLSL). */
uint32_t xgpu_plume_create_pixel_shader(const char *text,
                                        uint32_t cube_texture_mask);
/* GPU vertex-transform path: translate an NV2A vertex program (transform
 * microcode, 4 dwords/instruction) to a Plume vertex shader (cached). Returns
 * nonzero if a GPU VS is available for this program, 0 if the caller must fall
 * back to the CPU interpreter. */
int xgpu_plume_set_vertex_program(const uint32_t *microcode, uint32_t length,
                                  const uint32_t *vertex_format);

/* Record a deferred indexed programmable draw: the GPU fetches raw interleaved
 * vertex attributes and transforms them with the vertex shader last set via
 * xgpu_plume_set_vertex_program, feeding the active combiner pixel shader. This
 * replaces the CPU vertex interpreter for supported programs/layouts. Textures,
 * combiner constants, and the active PS come from the current bind state (as for
 * xgpu_plume_record_draw). Returns nonzero if recorded, 0 if unsupported (caller
 * must fall back to the CPU transform path). */
typedef struct XgpuProgIndexedDraw {
    const void *vertex_data;      /* raw interleaved vertex array, host pointer */
    uint32_t    vertex_bytes;     /* bytes to upload (covers referenced verts) */
    uint32_t    stride;           /* per-vertex byte stride */
    const uint32_t *indices;      /* index list (vertex numbers) */
    uint32_t    index_count;
    const float *vs_constants;    /* 192 float4 values for the VS constant stream */
    float       viewport_z_offset;
    float       viewport_z_scale;
    uint16_t    attrs_used;       /* bitmask of array-backed v0-v15 inputs */
    const float *latched_inputs;  /* 16 float4 persistent immediate v# latches */
    const uint32_t *attr_format;  /* [16] NV2A vertex-array format per attribute */
    const uint32_t *attr_offset;  /* [16] byte offset of each attr within a vertex */
    uint32_t    prim_type;        /* D3DPRIMITIVETYPE */
    const XgpuPlumeRenderState *render_state;
} XgpuProgIndexedDraw;
XgpuPlumeGpuDrawResult
xgpu_plume_record_prog_indexed_draw(const XgpuProgIndexedDraw *desc);

/* 2D-engine blit: copy the current contents of src_resource into the distinct
 * destination surface, ordered with the draw queue. Resource identity and
 * guest-memory aliases deliberately remain separate. */
int xgpu_plume_blit_surface(const XgpuSurfaceBinding *destination,
                            uint32_t dst_guest, uint32_t src_resource,
                            uint32_t src_guest);
/* Nonzero when the guest offset has a hosted Plume surface generation. */
int xgpu_plume_surface_known(uint32_t guest);
/* Nonzero when recorded or in-flight color work could leave guest memory
 * stale for this surface, so a CPU lock must still drain the device. Zero
 * only when the WAIT-download pipeline already packed every recorded write
 * back into guest RAM. XRECOMP_D3D_HLE_SURFACE_LOCK_SYNC=1 forces nonzero
 * for a legacy-synchronization A/B. */
int xgpu_plume_color_surface_pending(uint32_t guest);
/* Guarantee guest RAM holds this surface's current pixels before a CPU read.
 * The current implementation may block while draining all in-flight ring
 * slots. The content-serial recheck is an early-out between drain stages; a
 * serial that can never resolve still returns success after all required
 * drains complete, matching the existing synchronization contract. Returns
 * zero only when a required wait fails. */
int xgpu_plume_color_surface_sync(uint32_t guest);
/* Synchronous BGRA8 bridge for CPU-visible IDirect3DSurface8 locks. */
int xgpu_plume_download_color_surface(uint32_t resource_id, void *pixels,
                                      uint32_t width, uint32_t height,
                                      uint32_t pitch);
int xgpu_plume_upload_color_surface(uint32_t resource_id, const void *pixels,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch);
/* Resolve a rendered DEPTH surface to guest memory as packed Z24S8. A guest
 * texture may alias the depth buffer rather than a colour surface, and
 * resolving colour alone leaves such a stage reading zero. */
int xgpu_plume_download_zeta_surface(uint32_t resource_id, void *pixels,
                                     uint32_t width, uint32_t height,
                                     uint32_t pitch);
/* Bind a pitch-linear Y16 view of the live zeta allocation at `zeta_guest`
 * directly from an on-GPU conversion, skipping the CPU resolve+re-upload
 * round trip. Returns 1 only when the stage was fully satisfied; 0 means
 * the caller must take the legacy download+upload path (fail-closed). See
 * docs/technical/plume-gpu-zeta-alias-conversion.md. */
int xgpu_plume_bind_zeta_alias(uint32_t stage, uint32_t zeta_guest,
                               uint32_t alias_width, uint32_t alias_height,
                               uint32_t alias_pitch,
                               uint32_t unnormalized_coords);
void xgpu_plume_set_active_ps(uint32_t handle);
void xgpu_plume_set_ps_const(uint32_t start, const float *values, uint32_t count);
/* 16 float4 combiner constants for the per-draw CB combiner path. */
void xgpu_plume_set_combiner_consts(const float *values);
/* Extended constant snapshot for generated D3D8 register-combiner programs. */
void xgpu_plume_set_combiner_consts_ex(const float *values,
                                       uint32_t float4_count);

/* Runtime-translated NV2A vertex programs and their b9 constants. */
int xgpu_plume_register_vertex_shader(uint32_t handle, const char *hlsl,
                                      uint16_t inputs_read,
                                      uint16_t outputs_written,
                                      const XgpuPlumeVertexDeclaration *declaration);
void xgpu_plume_set_active_vertex_shader(uint32_t handle);
void xgpu_plume_set_vertex_shader_constants(const float *values,
                                             uint32_t float4_count);
/* Persistent Xbox v0-v15 values consumed when a declaration omits a register. */
void xgpu_plume_set_vertex_data4f(uint32_t reg, const float *value);

#ifdef __cplusplus
}
#endif

#endif /* XGPU_PLUME_HOST_H */
