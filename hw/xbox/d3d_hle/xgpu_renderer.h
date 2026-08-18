#ifndef XGPU_RENDERER_H
#define XGPU_RENDERER_H

#include <stdint.h>

/* Standard Xbox framebuffer dimensions used when the guest has not supplied
 * a different active render-target size. */
#define XGPU_PANEL_WIDTH  640u
#define XGPU_PANEL_HEIGHT 480u

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t XgpuResourceId;

typedef struct XgpuRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} XgpuRect;

typedef enum XgpuZetaFormat {
    XGPU_ZETA_NONE = 0,
    XGPU_ZETA_Z16 = 1,
    XGPU_ZETA_Z24S8 = 2,
} XgpuZetaFormat;

typedef enum XgpuSurfaceLayout {
    XGPU_SURFACE_PITCH = 1,
    XGPU_SURFACE_SWIZZLE = 2,
} XgpuSurfaceLayout;

typedef struct XgpuSurfaceBinding {
    XgpuResourceId color_resource;
    XgpuResourceId zeta_resource;
    /* Optional guest-memory alias for a hosted depth resource. The D3D HLE
     * backbuffer/depth surfaces have stable renderer handles (F000....), while
     * Xbox texture aliases and CPU resolves address their guest allocations.
     * Both identities must select the same host zeta generation. */
    uint32_t zeta_guest_address;
    /* Clip rectangle: what the guest draws into. Drives the viewport and the
     * NDC transform -- do NOT substitute the allocation size here, or every
     * vertex is scaled against the wrong half-width. */
    uint32_t width;
    uint32_t height;
    /* Allocation the clip rectangle sits inside. The row stride comes from the
     * pitch and the clip has an origin, so the buffer can be larger than the
     * rectangle: MM3 clips 320x240 out of a pitch-2560 (640-wide) surface at
     * 0x0076C000. Sizing the host image from the clip alone leaves the rest of
     * the buffer unallocated, so a texture aliasing the same address reads
     * pixels that were never rendered. Used only for image allocation. */
    uint32_t image_width;
    uint32_t image_height;
    /* Depth allocation extent is independent of the active color target.
     * Xbox titles commonly reuse a full-size depth surface with smaller
     * offscreen color targets. Zero retains the legacy color-image fallback. */
    uint32_t zeta_width;
    uint32_t zeta_height;
    uint32_t color_pitch;
    uint32_t zeta_pitch;
    uint32_t color_format;
    uint32_t zeta_format;
    /* Guest-memory layouts are independent in the D3D HLE path. The raw
     * NV2A frontend supplies the same SET_SURFACE_FORMAT type for both. */
    uint32_t layout;
    uint32_t zeta_layout;
    uint32_t sample_count;
    uint32_t zeta_float;
} XgpuSurfaceBinding;

#ifdef __cplusplus
}
#endif

#endif
