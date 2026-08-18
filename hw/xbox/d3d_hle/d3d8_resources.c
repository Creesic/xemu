/**
 * D3D8 Resource Management - Vertex Buffers, Index Buffers, Textures
 *
 * Implements CPU-owned Xbox D3D8 resources. The portable renderer consumes
 * immutable snapshots of this guest-visible memory at bind/draw boundaries.
 */

#include "d3d8_internal.h"
#include "d3d8_swizzle.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

UINT d3d8_format_bpp(D3DFORMAT fmt)
{
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_LIN_A8R8G8B8:
    case D3DFMT_LIN_X8R8G8B8:
    case D3DFMT_INDEX32:
        return 32;
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_LIN_R5G6B5:
    case D3DFMT_LIN_A1R5G5B5:
    case D3DFMT_LIN_A4R4G4B4:
    case D3DFMT_D16:
    case D3DFMT_INDEX16:
        return 16;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
        return 8;
    case D3DFMT_DXT1:  return 4;   /* 4 bits per pixel (BC1) */
    case D3DFMT_DXT3:
    case D3DFMT_DXT5:  return 8;   /* 8 bits per pixel (BC2/BC3) */
    case D3DFMT_D24S8: return 32;
    default: return 32;
    }
}

BOOL d3d8_format_is_compressed(D3DFORMAT fmt)
{
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT5;
}

UINT d3d8_row_pitch(D3DFORMAT fmt, UINT width)
{
    if (d3d8_format_is_compressed(fmt)) {
        UINT block_width = (width + 3) / 4;
        UINT block_bytes = (fmt == D3DFMT_DXT1) ? 8 : 16;
        return block_width * block_bytes;
    }
    return (width * d3d8_format_bpp(fmt)) / 8;
}

static BOOL surface_format_is_bgra32(D3DFORMAT fmt)
{
    return fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8 ||
           fmt == D3DFMT_LIN_A8R8G8B8 || fmt == D3DFMT_LIN_X8R8G8B8;
}

static BOOL surface_format_is_565(D3DFORMAT fmt)
{
    return fmt == D3DFMT_R5G6B5 || fmt == D3DFMT_LIN_R5G6B5;
}

static BOOL surface_format_is_1555(D3DFORMAT fmt)
{
    return fmt == D3DFMT_A1R5G5B5 || fmt == D3DFMT_LIN_A1R5G5B5;
}

static BOOL surface_format_is_4444(D3DFORMAT fmt)
{
    return fmt == D3DFMT_A4R4G4B4 || fmt == D3DFMT_LIN_A4R4G4B4;
}

BOOL d3d8_surface_to_bgra(D3DFORMAT fmt, const void *source,
                          UINT source_pitch, void *bgra, UINT bgra_pitch,
                          UINT width, UINT height)
{
    UINT y;
    if (!source || !bgra || !width || !height || width > UINT_MAX / 4u ||
        bgra_pitch < width * 4u)
        return FALSE;
    if (surface_format_is_bgra32(fmt)) {
        if (source_pitch < width * 4u)
            return FALSE;
        for (y = 0; y < height; ++y)
            memcpy((BYTE *)bgra + (size_t)y * bgra_pitch,
                   (const BYTE *)source + (size_t)y * source_pitch,
                   (size_t)width * 4u);
        return TRUE;
    }
    if (!surface_format_is_565(fmt) && !surface_format_is_1555(fmt) &&
        !surface_format_is_4444(fmt))
        return FALSE;
    if (width > UINT_MAX / 2u || source_pitch < width * 2u)
        return FALSE;
    for (y = 0; y < height; ++y) {
        const WORD *src = (const WORD *)((const BYTE *)source +
                                         (size_t)y * source_pitch);
        BYTE *dst = (BYTE *)bgra + (size_t)y * bgra_pitch;
        UINT x;
        for (x = 0; x < width; ++x) {
            const WORD value = src[x];
            if (surface_format_is_565(fmt)) {
                const BYTE b = (BYTE)(value & 0x1fu);
                const BYTE g = (BYTE)((value >> 5) & 0x3fu);
                const BYTE r = (BYTE)((value >> 11) & 0x1fu);
                dst[x * 4u + 0u] = (BYTE)((b << 3) | (b >> 2));
                dst[x * 4u + 1u] = (BYTE)((g << 2) | (g >> 4));
                dst[x * 4u + 2u] = (BYTE)((r << 3) | (r >> 2));
                dst[x * 4u + 3u] = 0xffu;
            } else if (surface_format_is_1555(fmt)) {
                const BYTE b = (BYTE)(value & 0x1fu);
                const BYTE g = (BYTE)((value >> 5) & 0x1fu);
                const BYTE r = (BYTE)((value >> 10) & 0x1fu);
                dst[x * 4u + 0u] = (BYTE)((b << 3) | (b >> 2));
                dst[x * 4u + 1u] = (BYTE)((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = (BYTE)((r << 3) | (r >> 2));
                dst[x * 4u + 3u] = (value & 0x8000u) ? 0xffu : 0u;
            } else {
                dst[x * 4u + 0u] = (BYTE)((value & 0x0fu) * 17u);
                dst[x * 4u + 1u] = (BYTE)(((value >> 4) & 0x0fu) * 17u);
                dst[x * 4u + 2u] = (BYTE)(((value >> 8) & 0x0fu) * 17u);
                dst[x * 4u + 3u] = (BYTE)(((value >> 12) & 0x0fu) * 17u);
            }
        }
    }
    return TRUE;
}

BOOL d3d8_surface_from_bgra(D3DFORMAT fmt, const void *bgra,
                            UINT bgra_pitch, void *destination,
                            UINT destination_pitch, UINT width, UINT height)
{
    UINT y;
    if (!bgra || !destination || !width || !height || width > UINT_MAX / 4u ||
        bgra_pitch < width * 4u)
        return FALSE;
    if (surface_format_is_bgra32(fmt)) {
        if (destination_pitch < width * 4u)
            return FALSE;
        for (y = 0; y < height; ++y)
            memcpy((BYTE *)destination + (size_t)y * destination_pitch,
                   (const BYTE *)bgra + (size_t)y * bgra_pitch,
                   (size_t)width * 4u);
        return TRUE;
    }
    if (!surface_format_is_565(fmt) && !surface_format_is_1555(fmt) &&
        !surface_format_is_4444(fmt))
        return FALSE;
    if (width > UINT_MAX / 2u || destination_pitch < width * 2u)
        return FALSE;
    for (y = 0; y < height; ++y) {
        const BYTE *src = (const BYTE *)bgra + (size_t)y * bgra_pitch;
        WORD *dst = (WORD *)((BYTE *)destination +
                            (size_t)y * destination_pitch);
        UINT x;
        for (x = 0; x < width; ++x) {
            const BYTE b = src[x * 4u + 0u];
            const BYTE g = src[x * 4u + 1u];
            const BYTE r = src[x * 4u + 2u];
            const BYTE a = src[x * 4u + 3u];
            if (surface_format_is_565(fmt))
                dst[x] = (WORD)(((WORD)(r >> 3) << 11) |
                                ((WORD)(g >> 2) << 5) | (b >> 3));
            else if (surface_format_is_1555(fmt))
                dst[x] = (WORD)(((a >= 0x80u) ? 0x8000u : 0u) |
                                ((WORD)(r >> 3) << 10) |
                                ((WORD)(g >> 3) << 5) | (b >> 3));
            else
                dst[x] = (WORD)(((WORD)(a >> 4) << 12) |
                                ((WORD)(r >> 4) << 8) |
                                ((WORD)(g >> 4) << 4) | (b >> 4));
        }
    }
    return TRUE;
}

/* ================================================================
 * Vertex Buffer Implementation
 * ================================================================ */

static D3D8VertexBuffer *vb_from_iface(IDirect3DVertexBuffer8 *iface)
{
    return (D3D8VertexBuffer *)iface;
}

static HRESULT __stdcall vb_QueryInterface(IDirect3DVertexBuffer8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall vb_AddRef(IDirect3DVertexBuffer8 *self)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    return (ULONG)InterlockedIncrement(&vb->ref_count);
}

static ULONG __stdcall vb_Release(IDirect3DVertexBuffer8 *self)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    LONG ref = InterlockedDecrement(&vb->ref_count);
    if (ref <= 0) {
        free(vb->sys_mem);
        free(vb);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall vb_GetDevice(IDirect3DVertexBuffer8 *self, IDirect3DDevice8 **ppDevice)
{
    (void)self;
    *ppDevice = xbox_GetD3DDevice();
    return S_OK;
}

static DWORD __stdcall vb_SetPriority(IDirect3DVertexBuffer8 *self, DWORD Priority)
{
    (void)self; (void)Priority;
    return 0;
}

static DWORD __stdcall vb_GetPriority(IDirect3DVertexBuffer8 *self)
{
    (void)self;
    return 0;
}

static void __stdcall vb_PreLoad(IDirect3DVertexBuffer8 *self)
{
    (void)self;
}

static DWORD __stdcall vb_GetType(IDirect3DVertexBuffer8 *self)
{
    (void)self;
    return 3; /* D3DRTYPE_VERTEXBUFFER */
}

static HRESULT __stdcall vb_Lock(IDirect3DVertexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    (void)Flags;

    if (!ppbData) return E_INVALIDARG;
    if (vb->locked) return E_FAIL;
    if (OffsetToLock > vb->size) return E_INVALIDARG;
    if (SizeToLock && SizeToLock > vb->size - OffsetToLock)
        return E_INVALIDARG;

    *ppbData = vb->sys_mem + OffsetToLock;
    vb->locked = TRUE;
    return S_OK;
}

static HRESULT __stdcall vb_Unlock(IDirect3DVertexBuffer8 *self)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    if (!vb->locked) return E_FAIL;

    vb->locked = FALSE;
    vb->dirty = TRUE;
    return S_OK;
}

static HRESULT __stdcall vb_GetDesc(IDirect3DVertexBuffer8 *self, void *pDesc)
{
    (void)self; (void)pDesc;
    return E_NOTIMPL;
}

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    vb_QueryInterface,
    vb_AddRef,
    vb_Release,
    vb_GetDevice,
    vb_SetPriority,
    vb_GetPriority,
    vb_PreLoad,
    vb_GetType,
    vb_Lock,
    vb_Unlock,
    vb_GetDesc,
};

HRESULT d3d8_CreateVertexBufferImpl(UINT Length, DWORD Usage, DWORD FVF, IDirect3DVertexBuffer8 **ppVB)
{
    D3D8VertexBuffer *vb;
    if (!ppVB || !Length) return E_INVALIDARG;

    vb = (D3D8VertexBuffer *)calloc(1, sizeof(*vb));
    if (!vb) return E_OUTOFMEMORY;

    vb->sys_mem = (BYTE *)calloc(1, Length);
    if (!vb->sys_mem) { free(vb); return E_OUTOFMEMORY; }

    vb->iface.lpVtbl = &g_vb_vtbl;
    vb->ref_count = 1;
    vb->size = Length;
    vb->fvf = FVF;
    vb->usage = Usage;

    *ppVB = &vb->iface;
    return S_OK;
}

/* ================================================================
 * Index Buffer Implementation
 * ================================================================ */

static D3D8IndexBuffer *ib_from_iface(IDirect3DIndexBuffer8 *iface)
{
    return (D3D8IndexBuffer *)iface;
}

static HRESULT __stdcall ib_QueryInterface(IDirect3DIndexBuffer8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall ib_AddRef(IDirect3DIndexBuffer8 *self)
{
    return (ULONG)InterlockedIncrement(&ib_from_iface(self)->ref_count);
}

static ULONG __stdcall ib_Release(IDirect3DIndexBuffer8 *self)
{
    D3D8IndexBuffer *ib = ib_from_iface(self);
    LONG ref = InterlockedDecrement(&ib->ref_count);
    if (ref <= 0) {
        free(ib->sys_mem);
        free(ib);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall ib_GetDevice(IDirect3DIndexBuffer8 *self, IDirect3DDevice8 **ppDevice)
{
    (void)self;
    *ppDevice = xbox_GetD3DDevice();
    return S_OK;
}

static DWORD __stdcall ib_SetPriority(IDirect3DIndexBuffer8 *self, DWORD Priority) { (void)self; (void)Priority; return 0; }
static DWORD __stdcall ib_GetPriority(IDirect3DIndexBuffer8 *self) { (void)self; return 0; }
static void  __stdcall ib_PreLoad(IDirect3DIndexBuffer8 *self) { (void)self; }
static DWORD __stdcall ib_GetType(IDirect3DIndexBuffer8 *self) { (void)self; return 4; /* D3DRTYPE_INDEXBUFFER */ }

static HRESULT __stdcall ib_Lock(IDirect3DIndexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags)
{
    D3D8IndexBuffer *ib = ib_from_iface(self);
    (void)Flags;
    if (!ppbData) return E_INVALIDARG;
    if (ib->locked) return E_FAIL;
    if (OffsetToLock > ib->size) return E_INVALIDARG;
    if (SizeToLock && SizeToLock > ib->size - OffsetToLock)
        return E_INVALIDARG;
    *ppbData = ib->sys_mem + OffsetToLock;
    ib->locked = TRUE;
    return S_OK;
}

static HRESULT __stdcall ib_Unlock(IDirect3DIndexBuffer8 *self)
{
    D3D8IndexBuffer *ib = ib_from_iface(self);
    if (!ib->locked) return E_FAIL;
    ib->locked = FALSE;
    ib->dirty = TRUE;
    return S_OK;
}

static HRESULT __stdcall ib_GetDesc(IDirect3DIndexBuffer8 *self, void *pDesc)
{
    (void)self; (void)pDesc;
    return E_NOTIMPL;
}

static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    ib_QueryInterface, ib_AddRef, ib_Release,
    ib_GetDevice, ib_SetPriority, ib_GetPriority, ib_PreLoad, ib_GetType,
    ib_Lock, ib_Unlock, ib_GetDesc,
};

HRESULT d3d8_CreateIndexBufferImpl(UINT Length, DWORD Usage, D3DFORMAT Format, IDirect3DIndexBuffer8 **ppIB)
{
    D3D8IndexBuffer *ib;
    if (!ppIB || !Length ||
        (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32))
        return E_INVALIDARG;

    ib = (D3D8IndexBuffer *)calloc(1, sizeof(*ib));
    if (!ib) return E_OUTOFMEMORY;

    ib->sys_mem = (BYTE *)calloc(1, Length);
    if (!ib->sys_mem) { free(ib); return E_OUTOFMEMORY; }

    ib->iface.lpVtbl = &g_ib_vtbl;
    ib->ref_count = 1;
    ib->size = Length;
    ib->format = Format;
    ib->usage = Usage;

    *ppIB = &ib->iface;
    return S_OK;
}

/* ================================================================
 * Texture Implementation
 * ================================================================ */

static D3D8Texture *tex_from_iface(IDirect3DTexture8 *iface)
{
    return (D3D8Texture *)iface;
}

static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *self)
{
    return (ULONG)InterlockedIncrement(&tex_from_iface(self)->ref_count);
}

static ULONG __stdcall tex_Release(IDirect3DTexture8 *self)
{
    D3D8Texture *tex = tex_from_iface(self);
    LONG ref = InterlockedDecrement(&tex->ref_count);
    if (ref <= 0) {
        free(tex->sys_mem);
        free(tex->level_offsets);
        free(tex->level_pitches);
        free(tex->level_rows);
        free(tex);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall tex_GetDevice(IDirect3DTexture8 *self, IDirect3DDevice8 **ppDevice)
{
    (void)self;
    *ppDevice = xbox_GetD3DDevice();
    return S_OK;
}

static DWORD __stdcall tex_SetPriority(IDirect3DTexture8 *self, DWORD Priority) { (void)self; (void)Priority; return 0; }
static DWORD __stdcall tex_GetPriority(IDirect3DTexture8 *self) { (void)self; return 0; }
static void  __stdcall tex_PreLoad(IDirect3DTexture8 *self) { (void)self; }
static DWORD __stdcall tex_GetType(IDirect3DTexture8 *self) { (void)self; return 5; /* D3DRTYPE_TEXTURE */ }

static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *self)
{
    return tex_from_iface(self)->levels;
}

static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *self, UINT Level, D3DSURFACE_DESC *pDesc)
{
    D3D8Texture *tex = tex_from_iface(self);
    if (!pDesc || Level >= tex->levels) return E_INVALIDARG;
    pDesc->Format = tex->d3d8_format;
    pDesc->Width = tex->width >> Level;
    pDesc->Height = tex->height >> Level;
    if (pDesc->Width < 1) pDesc->Width = 1;
    if (pDesc->Height < 1) pDesc->Height = 1;
    pDesc->Pool = D3DPOOL_DEFAULT;
    pDesc->Type = 5; /* D3DRTYPE_TEXTURE */
    pDesc->Usage = 0;
    pDesc->Size = tex->level_pitches[Level] * tex->level_rows[Level];
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    return S_OK;
}

static HRESULT __stdcall tex_GetSurfaceLevel(IDirect3DTexture8 *self, UINT Level, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Level; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall tex_LockRect(IDirect3DTexture8 *self, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    D3D8Texture *tex = tex_from_iface(self);
    UINT width, height, x, y, right, bottom;
    size_t byte_offset;
    (void)Flags;

    if (!pLockedRect || Level >= tex->levels) return E_INVALIDARG;
    if (tex->locked) return E_FAIL;

    width = tex->width >> Level;
    height = tex->height >> Level;
    if (!width) width = 1;
    if (!height) height = 1;
    x = pRect ? (UINT)pRect->left : 0;
    y = pRect ? (UINT)pRect->top : 0;
    right = pRect ? (UINT)pRect->right : width;
    bottom = pRect ? (UINT)pRect->bottom : height;
    if (x >= right || y >= bottom || right > width || bottom > height)
        return E_INVALIDARG;

    if (d3d8_format_is_compressed(tex->d3d8_format)) {
        UINT block_bytes = tex->d3d8_format == D3DFMT_DXT1 ? 8u : 16u;
        if ((x & 3u) || (y & 3u) ||
            ((right & 3u) && right != width) ||
            ((bottom & 3u) && bottom != height))
            return E_INVALIDARG;
        byte_offset = tex->level_offsets[Level] +
                      (size_t)(y / 4u) * tex->level_pitches[Level] +
                      (size_t)(x / 4u) * block_bytes;
    } else if (d3d8_format_is_swizzled(tex->d3d8_format) && pRect) {
        /* A rectangular pointer cannot describe Morton-order storage. */
        return E_INVALIDARG;
    } else {
        byte_offset = tex->level_offsets[Level] +
                      (size_t)y * tex->level_pitches[Level] +
                      (size_t)x * (d3d8_format_bpp(tex->d3d8_format) / 8u);
    }

    pLockedRect->Pitch = (INT)tex->level_pitches[Level];
    pLockedRect->pBits = tex->sys_mem + byte_offset;
    tex->locked = TRUE;
    tex->locked_level = Level;
    return S_OK;
}

static HRESULT __stdcall tex_UnlockRect(IDirect3DTexture8 *self, UINT Level)
{
    D3D8Texture *tex = tex_from_iface(self);
    if (Level >= tex->levels || !tex->locked || Level != tex->locked_level)
        return E_FAIL;

    tex->locked = FALSE;
    tex->dirty = TRUE;
    tex->plume_version = d3d8_plume_next_texture_version();

    d3d8_plume_on_texture_unlock(&tex->iface);
    return S_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

HRESULT d3d8_CreateTextureImpl(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, IDirect3DTexture8 **ppTex)
{
    D3D8Texture *tex;
    UINT level;
    UINT level_width;
    UINT level_height;
    (void)Usage;

    if (!ppTex || !Width || !Height) return E_INVALIDARG;

    tex = (D3D8Texture *)calloc(1, sizeof(*tex));
    if (!tex) return E_OUTOFMEMORY;

    tex->d3d8_format = Format;
    tex->width = Width;
    tex->height = Height;
    tex->levels = Levels ? Levels : 1;
    tex->level_offsets = (size_t *)calloc(tex->levels, sizeof(*tex->level_offsets));
    tex->level_pitches = (UINT *)calloc(tex->levels, sizeof(*tex->level_pitches));
    tex->level_rows = (UINT *)calloc(tex->levels, sizeof(*tex->level_rows));
    if (!tex->level_offsets || !tex->level_pitches || !tex->level_rows) {
        free(tex->level_offsets);
        free(tex->level_pitches);
        free(tex->level_rows);
        free(tex);
        return E_OUTOFMEMORY;
    }
    level_width = Width;
    level_height = Height;
    for (level = 0; level < tex->levels; ++level) {
        UINT rows = d3d8_format_is_compressed(Format)
                        ? (level_height + 3u) / 4u
                        : level_height;
        tex->level_offsets[level] = tex->data_size;
        tex->level_pitches[level] = d3d8_row_pitch(Format, level_width);
        tex->level_rows[level] = rows;
        tex->data_size += (size_t)tex->level_pitches[level] * rows;
        if (level_width > 1) level_width >>= 1;
        if (level_height > 1) level_height >>= 1;
    }
    tex->pitch = tex->level_pitches[0];
    tex->sys_mem = (BYTE *)calloc(1, tex->data_size);
    if (!tex->sys_mem) {
        free(tex->level_offsets);
        free(tex->level_pitches);
        free(tex->level_rows);
        free(tex);
        return E_OUTOFMEMORY;
    }
    tex->plume_version = d3d8_plume_next_texture_version();

    tex->iface.lpVtbl = &g_tex_vtbl;
    tex->ref_count = 1;

    *ppTex = &tex->iface;
    return S_OK;
}

BYTE *d3d8_texture_make_upload_snapshot(const D3D8Texture *texture)
{
    BYTE *snapshot;
    UINT level;
    UINT width;
    UINT height;
    UINT bpp;
    if (!texture || !texture->sys_mem || !texture->data_size)
        return NULL;
    snapshot = (BYTE *)malloc(texture->data_size);
    if (!snapshot)
        return NULL;
    if (d3d8_format_is_compressed(texture->d3d8_format) ||
        !d3d8_format_is_swizzled(texture->d3d8_format)) {
        memcpy(snapshot, texture->sys_mem, texture->data_size);
        return snapshot;
    }

    bpp = d3d8_format_bpp(texture->d3d8_format) / 8u;
    if (!bpp) {
        free(snapshot);
        return NULL;
    }
    width = texture->width;
    height = texture->height;
    for (level = 0; level < texture->levels; ++level) {
        xbox_unswizzle_rect(snapshot + texture->level_offsets[level],
                            texture->sys_mem + texture->level_offsets[level],
                            width, height, bpp);
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
    }
    return snapshot;
}
