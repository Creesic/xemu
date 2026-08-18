/**
 * D3D8 Compatibility Layer - Internal Header
 *
 * Shared types and declarations for the guest D3D8 compatibility layer.
 * Not part of the public API - only included by d3d8_*.c files.
 */

#ifndef D3D8_INTERNAL_H
#define D3D8_INTERNAL_H

#define COBJMACROS
#include <stddef.h>
#include "d3d8_xbox.h"
#include "xgpu_renderer.h"

typedef int (*D3D8VblankScanoutCallback)(void);

/* Portable D3D8 device accessor used by recompiled code on every host. */
IDirect3DDevice8 *d3d8_GetDevice(void);

void                *d3d8_GetNativeWindow(void);
UINT                 d3d8_GetBackbufferWidth(void);
UINT                 d3d8_GetBackbufferHeight(void);

/* Add the hosted device's initial depth attachment to a guest-memory color
 * binding. This is used only while xemu mirrors the original Xbox
 * backbuffers; ordinary SetRenderTarget(NULL depth) still detaches depth. */
int                  d3d8_PgraphAttachDefaultZeta(
                         XgpuSurfaceBinding *binding);

/* Select and present a guest-memory PGRAPH surface with Swap cadence. */
int                  d3d8_PgraphPresentSurface(uint32_t offset);
int                  d3d8_PgraphRefreshSurface(uint32_t offset);
int                  d3d8_PgraphPresentSurfaceForSwap(uint32_t offset);
uint32_t             d3d8_HlePresentCount(void);
uint32_t             d3d8_HleDrawCount(void);
void                 d3d8_SetVblankScanoutCallback(
                         D3D8VblankScanoutCallback callback);
int                  d3d8_VblankScanout(void);

/* Current render state array accessor */
const DWORD         *d3d8_GetRenderStates(void);
const DWORD         *d3d8_GetTSS(DWORD stage);
BOOL                 d3d8_FixedFunctionStateKnown(void);

/* Transform accessors */
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type);

/* Lighting accessors (d3d8_device.c) */
const D3DLIGHT8     *d3d8_GetLight(DWORD index);
BOOL                 d3d8_GetLightEnable(DWORD index);
const D3DMATERIAL8  *d3d8_GetMaterial(void);
UINT                 d3d8_GetNumLights(void);

/* ================================================================
 * Resource wrapper structures
 * ================================================================ */

typedef struct D3D8VertexBuffer {
    IDirect3DVertexBuffer8  iface;      /* COM interface (must be first) */
    LONG                    ref_count;
    UINT                    size;
    DWORD                   fvf;
    DWORD                   usage;
    BYTE                   *sys_mem;    /* System memory for Lock */
    BOOL                    locked;
    BOOL                    dirty;
} D3D8VertexBuffer;

typedef struct D3D8IndexBuffer {
    IDirect3DIndexBuffer8   iface;
    LONG                    ref_count;
    UINT                    size;
    D3DFORMAT               format;     /* INDEX16 or INDEX32 */
    DWORD                   usage;
    BYTE                   *sys_mem;
    BOOL                    locked;
    BOOL                    dirty;
} D3D8IndexBuffer;

typedef struct D3D8Texture {
    IDirect3DTexture8       iface;
    LONG                    ref_count;
    UINT                    width;
    UINT                    height;
    UINT                    levels;
    D3DFORMAT               d3d8_format;
    BYTE                   *sys_mem;
    size_t                  data_size;
    size_t                 *level_offsets;
    UINT                   *level_pitches;
    UINT                   *level_rows;
    UINT                    pitch;      /* Row pitch of level 0 */
    UINT                    locked_level;
    BOOL                    locked;
    BOOL                    dirty;
    uint64_t                plume_version;
} D3D8Texture;

typedef struct D3D8Surface {
    IDirect3DSurface8       iface;
    LONG                    ref_count;
    BYTE                   *sys_mem;
    UINT                    pitch;
    uint32_t                resource_id;
    uint32_t                guest_address;
    UINT                    width;
    UINT                    height;
    D3DFORMAT               format;
    DWORD                   usage;
    DWORD                   lock_flags;
    BOOL                    locked;
    BOOL                    dirty;
} D3D8Surface;

/* ================================================================
 * Format conversion (d3d8_resources.c)
 * ================================================================ */

UINT        d3d8_format_bpp(D3DFORMAT fmt);
BOOL        d3d8_format_is_compressed(D3DFORMAT fmt);
UINT        d3d8_row_pitch(D3DFORMAT fmt, UINT width);
BOOL        d3d8_surface_to_bgra(D3DFORMAT fmt, const void *source,
                                 UINT source_pitch, void *bgra,
                                 UINT bgra_pitch, UINT width, UINT height);
BOOL        d3d8_surface_from_bgra(D3DFORMAT fmt, const void *bgra,
                                   UINT bgra_pitch, void *destination,
                                   UINT destination_pitch, UINT width,
                                   UINT height);

/* Resource creation (d3d8_resources.c) */
HRESULT d3d8_CreateVertexBufferImpl(UINT Length, DWORD Usage, DWORD FVF, IDirect3DVertexBuffer8 **ppVB);
HRESULT d3d8_CreateIndexBufferImpl(UINT Length, DWORD Usage, D3DFORMAT Format, IDirect3DIndexBuffer8 **ppIB);
HRESULT d3d8_CreateTextureImpl(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, IDirect3DTexture8 **ppTex);
BYTE *d3d8_texture_make_upload_snapshot(const D3D8Texture *texture);

/* ================================================================
 * NV2A Register Combiner pixel shaders (d3d8_combiners.c)
 * ================================================================ */

#include "d3d8_combiners.h"

/* ================================================================
 * NV2A Programmable Vertex Shaders (d3d8_vsh.c)
 * ================================================================ */

#include "d3d8_vsh.h"

/* Called after a guest texture unlock; pixels are linearized for Plume. */
void d3d8_plume_on_texture_unlock(IDirect3DTexture8 *tex);
uint64_t d3d8_plume_next_texture_version(void);

#endif /* D3D8_INTERNAL_H */
