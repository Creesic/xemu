#include "d3d_hle_guest.h"

#include "d3d_frontend.h"
#include "d3d_hle_guest_driver.h"
#include "d3d8_cpu_surface_sync.h"
#include "d3d8_internal.h"
#include "d3d8_palette.h"
#include "d3d8_shader_constants.h"
#include "d3d8_swizzle.h"
#include "d3d8_tile_state.h"
#include "d3d8_texture_state.h"
#include "d3d8_vsh.h"
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
#include "plume/plume_f2_capture.h"
#endif
#include "plume/plume_host.h"
#include "platform/cpu_recorder.h"
#include "xemu_d3d_hle.h"
#include "xgpu_renderer.h"
#include "kernel/xbox_memory_layout.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
#define D3D_HLE_F2_LOG(...) ((void)0)
#else
#define D3D_HLE_F2_LOG(...) xgpu_plume_f2_log(__VA_ARGS__)
#endif

#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
static uint64_t d3d_hle_guest_f2_hash_bytes(const void *data, uint32_t bytes)
{
    const uint8_t *cursor = (const uint8_t *)data;
    uint64_t hash = UINT64_C(1469598103934665603);
    uint32_t i;

    for (i = 0; i < bytes; ++i) {
        hash ^= cursor[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
#endif

extern uint32_t g_eax;
extern uint32_t g_esp;
int d3d8_PgraphSetRenderTarget(const XgpuSurfaceBinding *binding);
int d3d8_PgraphBindSurfaceTextureStage(
    uint32_t stage, uint32_t offset, uint32_t unnormalized_coords,
    uint32_t texture_format);
int d3d8_PgraphDownloadSurfaceRange(uint32_t start, uint32_t bytes);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
void d3d_hle_guest_materialize_deferred_fixed_state(void);
void d3d_hle_guest_materialize_deferred_fog_state(void);
void d3d_hle_guest_mark_deferred_pixel_shader_dirty(void);
extern uint32_t xrecomp_d3d_hle_deferred_texture_state_va;
#endif
/* Push retarget is not a fallback path: it must resolve in test builds too. */
extern uint32_t xrecomp_d3d_hle_device_global_va;

enum {
    XBOX_D3DCOMMON_TYPE_MASK = 0x00070000u,
    XBOX_D3DCOMMON_TYPE_INDEXBUFFER = 0x00010000u,
    XBOX_D3DCOMMON_TYPE_PALETTE = 0x00020000u,
    XBOX_D3DCOMMON_TYPE_PUSHBUFFER = 0x00030000u,
    XBOX_D3DCOMMON_TYPE_SURFACE = 0x00050000u,
    XBOX_D3DCOMMON_TYPE_TEXTURE = 0x00040000u,
    XBOX_D3DCOMMON_TYPE_FIXUP = 0x00060000u,
    XBOX_D3DFORMAT_CUBEMAP = 0x00000004u,
    XBOX_D3DFORMAT_DIMENSION_MASK = 0x000000F0u,
    XBOX_D3DFORMAT_3D = 0x00000030u,
    XBOX_D3DCOMMON_BIND_REF = 0x00080000u,
    XBOX_D3DCOMMON_INTREFCOUNT_MASK = 0x00780000u,
    XBOX_D3DCOMMON_LIVE_MASK = 0x0078FFFFu,
    XBOX_D3D_MAX_STREAMS = 16,
    XBOX_D3D_MAX_TEXTURES = 4,
    XBOX_D3D_MAX_TILES = 8,
    XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE = 256,
    XBOX_D3D_HLE_MAX_STATES = 512,
    NV097_SET_ALPHA_TEST_ENABLE = 0x0300,
    NV097_SET_BLEND_ENABLE = 0x0304,
    NV097_SET_DEPTH_TEST_ENABLE = 0x030C,
    NV097_SET_DITHER_ENABLE = 0x0310,
    NV097_SET_LIGHTING_ENABLE = 0x0314,
    NV097_SET_STENCIL_TEST_ENABLE = 0x032C,
    NV097_SET_ALPHA_FUNC = 0x033C,
    NV097_SET_ALPHA_REF = 0x0340,
    NV097_SET_BLEND_FUNC_SFACTOR = 0x0344,
    NV097_SET_BLEND_FUNC_DFACTOR = 0x0348,
    NV097_SET_BLEND_EQUATION = 0x0350,
    NV097_SET_DEPTH_FUNC = 0x0354,
    NV097_SET_COLOR_MASK = 0x0358,
    NV097_SET_DEPTH_MASK = 0x035C,
    NV097_SET_STENCIL_MASK = 0x0360,
    NV097_SET_STENCIL_FUNC = 0x0364,
    NV097_SET_STENCIL_FUNC_REF = 0x0368,
    NV097_SET_STENCIL_FUNC_MASK = 0x036C,
    NV097_SET_STENCIL_OP_FAIL = 0x0370,
    NV097_SET_STENCIL_OP_ZFAIL = 0x0374,
    NV097_SET_STENCIL_OP_ZPASS = 0x0378,
    NV097_SET_POLYGON_OFFSET_SCALE_FACTOR = 0x0384,
    NV097_SET_POLYGON_OFFSET_BIAS = 0x0388,
    NV097_SET_TEXTURE_OFFSET = 0x1B00,
};

typedef struct D3DHleGuestBindings {
    uint32_t stream_resource[XBOX_D3D_MAX_STREAMS];
    uint32_t stream_stride[XBOX_D3D_MAX_STREAMS];
    uint32_t index_resource;
    uint32_t base_vertex;
    uint32_t texture_resource[XBOX_D3D_MAX_TEXTURES];
    uint32_t palette_resource[XBOX_D3D_MAX_TEXTURES];
    uint32_t render_target;
    uint32_t depth_stencil;
} D3DHleGuestBindings;

static _Noreturn void
d3d_hle_guest_fatal(const char *operation, HRESULT result);
static uint32_t d3d_hle_guest_level_bytes(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t format,
    uint32_t *pitch_out);
static uint32_t d3d_hle_guest_size_pitch(uint32_t size);
static uint32_t d3d_hle_guest_level_storage_bytes(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t format,
    uint32_t row_pitch, uint32_t *pitch_out);
static uint32_t d3d_hle_guest_read_u32(uint32_t va);
static bool d3d_hle_guest_try_read_u32(uint32_t va, uint32_t *value);
static uint32_t d3d_hle_guest_release_internal(uint32_t resource_va);
static void d3d_hle_guest_reset_immediate_state(void);

static D3DHleGuestBindings g_hle_bindings;
static D3DHleGuestFatalDiagnostic g_hle_fatal_diagnostic;
static D3DHleGuestReadRange g_hle_read_range;
static uint8_t *g_hle_up_scratch[2];
static size_t g_hle_up_scratch_capacity[2];

typedef enum D3DHleGuestResourceKind {
    D3D_HLE_RESOURCE_NONE,
    D3D_HLE_RESOURCE_TEXTURE,
    D3D_HLE_RESOURCE_SURFACE,
    D3D_HLE_RESOURCE_VERTEX_BUFFER,
    D3D_HLE_RESOURCE_INDEX_BUFFER,
    D3D_HLE_RESOURCE_PALETTE,
    D3D_HLE_RESOURCE_VERTEX_SHADER,
    D3D_HLE_RESOURCE_PIXEL_SHADER,
} D3DHleGuestResourceKind;

typedef struct D3DHleGuestResource {
    D3DHleGuestResourceKind kind;
    uint32_t object_va;
    uint32_t data_va;
    uint32_t data_bytes;
    uint32_t parent_va;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t levels;
    uint32_t level;
    uint32_t format;
    uint32_t size;
    uint32_t host_handle;
    uint32_t load_address;
    uint32_t face;
    uint32_t owned_object;
    uint32_t owned_data;
    void *host_object;
    uint64_t version;
    uint64_t uploaded_version;
    uint64_t content_serial;
    uint64_t cpu_scanout_hash;
    uint32_t cpu_scanout_hash_valid;
    uint32_t cpu_scanout_active;
    uint32_t cpu_scanout_published;
} D3DHleGuestResource;

typedef struct D3DHleGuestResourceChunk {
    struct D3DHleGuestResourceChunk *next;
    D3DHleGuestResource resources[XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE];
} D3DHleGuestResourceChunk;

static void d3d_hle_guest_destroy_resource(
    D3DHleGuestResource *resource);

static D3DHleGuestResourceChunk *g_hle_resource_chunks;
static D3DHleGuestResourceChunk *g_hle_resource_chunks_tail;
static uint32_t g_hle_resource_capacity;
static uint64_t g_hle_texture_version = 1;
static uint32_t g_hle_device_refs = 1;
static uint32_t g_hle_device_token;
static uint32_t g_hle_back_buffer;
static uint32_t g_hle_front_buffer;
static uint32_t g_hle_depth_buffer;
static uint32_t g_hle_vertex_shader;
static uint32_t g_hle_pixel_shader;
enum { D3D_HLE_PUSH_SCRATCH_BYTES = 16384 };
static uint32_t g_hle_push_scratch_va;
static uint32_t g_hle_push_scratch_bytes;
static uint32_t g_hle_push_constant_load;
typedef struct D3DHleGuestLoadedVertexProgram {
    uint32_t instruction_count;
    uint64_t generation;
    DWORD microcode[NV2A_VS_MAX_INSTRUCTIONS * 4];
} D3DHleGuestLoadedVertexProgram;
static D3DHleGuestLoadedVertexProgram
    g_hle_loaded_vertex_programs[NV2A_VS_MAX_INSTRUCTIONS];
static uint64_t g_hle_vertex_program_generation;
static DWORD g_hle_pixel_shader_constants[16];
static uint32_t g_hle_pixel_shader_constant_valid;
static DWORD g_hle_pixel_shader_effective[60];
static uint32_t g_hle_pixel_shader_effective_valid;
static uint32_t g_hle_fence;
static uint32_t g_hle_shader_constant_mode;
static XboxD3DTile g_hle_tiles[XBOX_D3D_MAX_TILES];
/* Host-side copy of the last successfully applied present parameters;
 * survives guest session resets so a same-title restart never rereads
 * freed guest stack memory. */
static D3DPRESENT_PARAMETERS g_hle_present_snapshot;
static int g_hle_present_snapshot_valid;
static uint64_t g_hle_palette_version[XBOX_D3D_MAX_TEXTURES];
static uint32_t g_hle_state_cache[XBOX_D3D_HLE_MAX_STATES];
static float g_hle_immediate_vertex[16][4];
static int g_hle_overlay_enabled;
static float g_hle_depth_bias;
static float g_hle_slope_bias;

/*
 * Registry lookups sit on the per-draw path: SwitchTexture resolves
 * textures by Data address and every indexed draw resolves the bound
 * vertex/index buffers by object address. Walking every chunk (and
 * refreshing every caller-owned texture header along the way) made each
 * lookup O(registry) and dominated frame time at a few hundred binds per
 * frame. These open-addressed indexes memoize key -> record. A hit is
 * never trusted blindly: records are reused after destruction and
 * caller-owned Data pointers move, so every hit is re-validated against
 * the live record and any mismatch falls back to the legacy scan, which
 * repairs the index. Chunk memory is never freed, so a stale entry is
 * always safe to dereference.
 */
typedef struct D3DHleGuestResourceIndex {
    uint32_t *keys;
    D3DHleGuestResource **records;
    uint32_t capacity;
    uint32_t used;
} D3DHleGuestResourceIndex;

static D3DHleGuestResourceIndex g_hle_object_index;
static D3DHleGuestResourceIndex g_hle_texture_data_index;

static void d3d_hle_guest_index_store(
    uint32_t *keys, D3DHleGuestResource **records, uint32_t capacity,
    uint32_t key, D3DHleGuestResource *record, uint32_t *used)
{
    uint32_t slot = (key * 2654435761u) & (capacity - 1u);
    while (keys[slot] && keys[slot] != key)
        slot = (slot + 1u) & (capacity - 1u);
    if (!keys[slot]) {
        keys[slot] = key;
        ++*used;
    }
    records[slot] = record;
}

static void d3d_hle_guest_index_put(
    D3DHleGuestResourceIndex *index, uint32_t key,
    D3DHleGuestResource *record)
{
    if (!key)
        return;
    if (!index->capacity ||
        (index->used + 1u) * 4u > index->capacity * 3u) {
        D3DHleGuestResourceIndex grown;
        uint32_t i;
        grown.capacity = index->capacity ? index->capacity * 2u : 1024u;
        grown.used = 0;
        grown.keys = (uint32_t *)calloc(
            grown.capacity, sizeof(*grown.keys));
        grown.records = (D3DHleGuestResource **)calloc(
            grown.capacity, sizeof(*grown.records));
        if (!grown.keys || !grown.records)
            d3d_hle_guest_fatal("HLE resource index allocation",
                                E_OUTOFMEMORY);
        for (i = 0; i < index->capacity; ++i) {
            if (index->keys[i])
                d3d_hle_guest_index_store(
                    grown.keys, grown.records, grown.capacity,
                    index->keys[i], index->records[i], &grown.used);
        }
        free(index->keys);
        free(index->records);
        *index = grown;
    }
    d3d_hle_guest_index_store(index->keys, index->records,
                              index->capacity, key, record,
                              &index->used);
}

static D3DHleGuestResource *d3d_hle_guest_index_get(
    const D3DHleGuestResourceIndex *index, uint32_t key)
{
    uint32_t slot;
    if (!index->capacity || !key)
        return NULL;
    slot = (key * 2654435761u) & (index->capacity - 1u);
    while (index->keys[slot]) {
        if (index->keys[slot] == key)
            return index->records[slot];
        slot = (slot + 1u) & (index->capacity - 1u);
    }
    return NULL;
}

static void d3d_hle_guest_refresh_external_resource(
    D3DHleGuestResource *resource)
{
    uint32_t data_va;
    uint32_t format_word;
    uint32_t size;
    uint32_t format;
    uint32_t levels;
    uint32_t width;
    uint32_t height;
    uint32_t depth = 1u;
    uint32_t level_width;
    uint32_t level_height;
    uint32_t level_depth;
    uint32_t bytes = 0;
    uint32_t level;
    uint32_t linear_pitch;

    if (!resource || resource->owned_object)
        return;

    /*
     * Caller-owned vertex and index buffer headers are live objects too.
     * Games may reuse the header with a new Data allocation while it remains
     * registered or even bound. The vertex draw path already reads Data from
     * the header directly; keep the registry and index-offset recovery in
     * lockstep with it. A header page unmapped by loader section churn means
     * there is nothing live to refresh.
     */
    if (resource->kind == D3D_HLE_RESOURCE_VERTEX_BUFFER ||
        resource->kind == D3D_HLE_RESOURCE_INDEX_BUFFER) {
        if (!d3d_hle_guest_try_read_u32(
                resource->object_va + 4u, &data_va))
            return;
        resource->data_va = data_va & 0x0FFFFFFFu;
        return;
    }

    /*
     * XGSetTextureHeader-style callers reuse a caller-owned
     * D3DPixelContainer object for unrelated allocations. Resource_Register
     * only relocates Data; Format and Size are ordinary guest fields and may
     * have changed since this object was first adopted. Treating the cached
     * descriptor as immutable can turn, for example, a 512x256 A8 mip chain
     * into a 640x480 LIN_A8R8G8B8 upload and consume the following texture.
     */
    if (resource->kind != D3D_HLE_RESOURCE_TEXTURE)
        return;

    if (!d3d_hle_guest_try_read_u32(
            resource->object_va + 4u, &data_va) ||
        !d3d_hle_guest_try_read_u32(
            resource->object_va + 12u, &format_word) ||
        !d3d_hle_guest_try_read_u32(
            resource->object_va + 16u, &size))
        return;
    data_va &= 0x0FFFFFFFu;
    format = (format_word >> 8) & 0xFFu;
    levels = (format_word >> 16) & 0xFu;
    if (!levels)
        levels = 1u;
    if (size) {
        width = (size & 0xFFFu) + 1u;
        height = ((size >> 12) & 0xFFFu) + 1u;
    } else {
        width = 1u << ((format_word >> 20) & 0xFu);
        height = 1u << ((format_word >> 24) & 0xFu);
        depth = 1u << ((format_word >> 28) & 0xFu);
    }
    linear_pitch = d3d_hle_guest_size_pitch(size);

    level_width = width;
    level_height = height;
    level_depth = depth;
    for (level = 0; level < levels; ++level) {
        uint32_t level_bytes = d3d_hle_guest_level_storage_bytes(
            level_width, level_height, level_depth, format,
            level == 0u ? linear_pitch : 0u, NULL);
        if (bytes > UINT32_MAX - level_bytes)
            d3d_hle_guest_fatal(
                "refreshed resource extent overflow", E_INVALIDARG);
        bytes += level_bytes;
        if (level_width > 1u) level_width >>= 1;
        if (level_height > 1u) level_height >>= 1;
        if (level_depth > 1u) level_depth >>= 1;
    }
    if (format_word & 0x4u) {
        bytes = (bytes + 127u) & ~127u;
        if (bytes > UINT32_MAX / 6u)
            d3d_hle_guest_fatal(
                "refreshed cube extent overflow", E_INVALIDARG);
        bytes *= 6u;
    }

    if (resource->data_va == data_va &&
        resource->data_bytes == bytes &&
        resource->width == width &&
        resource->height == height &&
        resource->depth == depth &&
        resource->levels == levels &&
        resource->format == format &&
        resource->size == size)
        return;

    resource->data_va = data_va;
    resource->data_bytes = bytes;
    resource->width = width;
    resource->height = height;
    resource->depth = depth;
    resource->levels = levels;
    resource->format = format;
    resource->size = size;
    resource->version = ++g_hle_texture_version;
    D3D_HLE_F2_LOG(
        "hle texture-header-refresh obj=%08X data=%08X "
        "%ux%u d=%u lv=%u fmt=%02X word=%08X size=%08X bytes=%u",
        resource->object_va, resource->data_va,
        resource->width, resource->height, resource->depth,
        resource->levels, resource->format, format_word,
        resource->size, resource->data_bytes);
}

static void d3d_hle_guest_prepare_fixed_state(void)
{
    /* Public XDK inline-push helpers write transform constants into the HLE
     * scratch window returned by MakeSpace. Consume them before the draw. */
    d3d_hle_guest_drain_inline_push();
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    /*
     * D3D::LazySetSpecFogCombiner's fog shadow feeds the final-combiner fog
     * term of programmable and fixed-function draws alike, so it cannot sit
     * behind the PS=0 gate below.
     */
    d3d_hle_guest_materialize_deferred_fog_state();
    /*
     * Xbox D3D's inline texture-stage setters update a guest-side shadow and
     * defer the actual combiner methods until LazySetState at draw time.  The
     * native draw hooks replace that XDK boundary. LazySetTextureState runs
     * for programmable and fixed-function draws alike, so sampler state cannot
     * sit behind a PS=0 gate. A previously observed host TSS value does not
     * prove that a later inline guest setter left the shadow unchanged.
     */
    d3d_hle_guest_materialize_deferred_fixed_state();
#endif
}

static bool d3d_hle_guest_resource_record_matches_live_type(
    const D3DHleGuestResource *resource)
{
    uint32_t common_type;

    if (!resource || resource->kind == D3D_HLE_RESOURCE_NONE)
        return false;
    if (resource->owned_object ||
        resource->kind == D3D_HLE_RESOURCE_VERTEX_SHADER ||
        resource->kind == D3D_HLE_RESOURCE_PIXEL_SHADER)
        return true;

    /*
     * Native D3D reuses caller-owned resource headers. An object-index entry
     * can therefore still point at a registry record whose guest address now
     * contains a different resource type. Revalidate the Common type before
     * returning the record; adopt_resource can then register the new live
     * object without mutating or dereferencing the stale descriptor. A header
     * page unmapped by loader section churn is equally "not this resource" —
     * never a fatal condition during a speculative registry walk.
     */
    if (!d3d_hle_guest_try_read_u32(resource->object_va, &common_type))
        return false;
    common_type &= XBOX_D3DCOMMON_TYPE_MASK;
    switch (resource->kind) {
    case D3D_HLE_RESOURCE_TEXTURE:
        return common_type == XBOX_D3DCOMMON_TYPE_TEXTURE;
    case D3D_HLE_RESOURCE_SURFACE:
        return common_type == XBOX_D3DCOMMON_TYPE_SURFACE;
    case D3D_HLE_RESOURCE_VERTEX_BUFFER:
        return common_type == 0;
    case D3D_HLE_RESOURCE_INDEX_BUFFER:
        return common_type == XBOX_D3DCOMMON_TYPE_INDEXBUFFER;
    default:
        return true;
    }
}

static D3DHleGuestResource *d3d_hle_guest_find_resource(uint32_t object_va)
{
    D3DHleGuestResourceChunk *chunk;
    unsigned i;
    D3DHleGuestResource *resource = d3d_hle_guest_index_get(
        &g_hle_object_index, object_va);
    if (resource && resource->object_va == object_va &&
        d3d_hle_guest_resource_record_matches_live_type(resource))
        return resource;
    for (chunk = g_hle_resource_chunks; chunk; chunk = chunk->next) {
        for (i = 0; i < XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE; ++i) {
            resource = &chunk->resources[i];
            if (resource->object_va == object_va &&
                d3d_hle_guest_resource_record_matches_live_type(resource)) {
                d3d_hle_guest_index_put(&g_hle_object_index, object_va,
                                        resource);
                return resource;
            }
        }
    }
    return NULL;
}

static bool d3d_hle_guest_texture_data_candidate(
    D3DHleGuestResource *resource, uint32_t data_va)
{
    uint32_t common;
    uint32_t live_data_va;

    if (!resource || resource->kind != D3D_HLE_RESOURCE_TEXTURE)
        return false;
    if (resource->owned_object)
        return (resource->data_va & 0x0FFFFFFFu) == data_va;

    /*
     * Caller-owned pixel-container headers are routinely freed and reused by
     * the native D3D runtime while their HLE registry slots remain allocated.
     * Do not parse Format/Size from every historical slot while searching by
     * Data: first prove that the live guest header is still a texture and is
     * the particular texture being requested. Besides avoiding stale garbage,
     * this is the revalidation required for a potentially stale index hit.
     * The loader can also unmap a historical header's page outright
     * (BINKDATA-style section churn); an unreadable header is simply not a
     * candidate.
     */
    if (!d3d_hle_guest_try_read_u32(resource->object_va, &common))
        return false;
    if ((common & XBOX_D3DCOMMON_TYPE_MASK) !=
            XBOX_D3DCOMMON_TYPE_TEXTURE ||
        !(common & XBOX_D3DCOMMON_LIVE_MASK))
        return false;
    if (!d3d_hle_guest_try_read_u32(
            resource->object_va + 4u, &live_data_va))
        return false;
    live_data_va &= 0x0FFFFFFFu;
    if (live_data_va != data_va)
        return false;

    d3d_hle_guest_refresh_external_resource(resource);
    return (resource->data_va & 0x0FFFFFFFu) == data_va;
}

static D3DHleGuestResource *d3d_hle_guest_find_texture_data(
    uint32_t data_va)
{
    D3DHleGuestResourceChunk *chunk;
    unsigned i;
    D3DHleGuestResource *resource;
    data_va &= 0x0FFFFFFFu;
    resource = d3d_hle_guest_index_get(
        &g_hle_texture_data_index, data_va);
    if (d3d_hle_guest_texture_data_candidate(resource, data_va))
        return resource;
    for (chunk = g_hle_resource_chunks; chunk; chunk = chunk->next) {
        for (i = 0; i < XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE; ++i) {
            resource = &chunk->resources[i];
            if (!d3d_hle_guest_texture_data_candidate(resource, data_va))
                continue;
            d3d_hle_guest_index_put(&g_hle_texture_data_index,
                                    data_va, resource);
            return resource;
        }
    }
    return NULL;
}

static uint32_t d3d_hle_guest_texture_surface_key(
    const D3DHleGuestResource *texture)
{
    D3DHleGuestResourceChunk *chunk;
    unsigned i;
    uint32_t memory_key = 0;
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_texture_surface_key_zone, "D3D HLE Texture Surface Lookup");
    if (!texture || texture->depth != 1u || texture->levels != 1u) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_texture_surface_key_zone);
        return 0;
    }
    for (chunk = g_hle_resource_chunks; chunk; chunk = chunk->next) {
        for (i = 0; i < XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE; ++i) {
            const D3DHleGuestResource *surface = &chunk->resources[i];
            if (surface->kind != D3D_HLE_RESOURCE_SURFACE ||
                surface->data_va != texture->data_va ||
                surface->format != texture->format)
                continue;
            /*
             * Memory render targets are keyed by their guest storage address.
             * Hosted proxy surfaces (the device backbuffer) are keyed by the
             * portable D3D8 resource handle, although their Xbox texture
             * aliases still point at the proxy's guest backing allocation.
             *
             * Do not require the surface and texture dimensions to match.
             * Xbox titles reuse one caller-owned surface header at several
             * sizes and can immediately sample a full-size render target
             * through a smaller linear texture at the same address. On the
             * native GPU this is one VRAM allocation; Plume must therefore
             * bind the latest compatible host generation and let normalized
             * sampling perform the size conversion.
             *
             * Keep scanning after a memory-backed match. Caller-owned
             * surface headers are transient and the registry may contain
             * several records for one Xbox allocation. A stale earlier
             * header must not hide the hosted device-backbuffer proxy, whose
             * renderer handle is the authoritative live image.
             */
            if (surface->host_handle) {
                XRECOMP_CPU_RECORDER_ZONE_END(
                    cpu_texture_surface_key_zone);
                return surface->host_handle;
            }
            if (!memory_key)
                memory_key = surface->data_va;
        }
    }
    if (memory_key) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_texture_surface_key_zone);
        return memory_key;
    }
    /*
     * Caller-owned surface headers are transient. The PGRAPH surface cache
     * outlives such a header and remains the authority for rendered storage,
     * so give it the address and let its format check decide whether the
     * texture can alias the latest generation.
     */
    memory_key = texture->data_va;
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_texture_surface_key_zone);
    return memory_key;
}

static void d3d_hle_guest_rebind_surface_textures(void)
{
    uint32_t stage;
    for (stage = 0; stage < XBOX_D3D_MAX_TEXTURES; ++stage) {
        D3DHleGuestResource *texture = d3d_hle_guest_find_resource(
            g_hle_bindings.texture_resource[stage]);
        uint32_t surface_key;
        if (!texture || texture->kind != D3D_HLE_RESOURCE_TEXTURE)
            continue;
        if (!texture->owned_object)
            d3d_hle_guest_refresh_external_resource(texture);
        if (d3d_hle_guest_resource_type(texture->object_va) !=
            XRECOMP_XBOX_D3DRTYPE_TEXTURE)
            continue;
        surface_key = d3d_hle_guest_texture_surface_key(texture);
        if (!surface_key ||
            !d3d8_PgraphBindSurfaceTextureStage(
                stage, surface_key, texture->size != 0u,
                surface_key == texture->data_va
                    ? texture->format : UINT32_MAX))
            continue;
        /*
         * Texture and render-target bindings are independent persistent Xbox
         * state. A title may bind a render-target alias as a texture before
         * rebinding that target for the next frame. The first lookup then has
         * no current host generation and temporarily falls back to guest RAM;
         * retrying here upgrades the still-bound texture to the generation
         * which SetRenderTarget just made available.
         */
        D3D_HLE_F2_LOG(
            "hle texture-surface-rebind stage=%u obj=%08X data=%08X "
            "key=%08X %ux%u fmt=%02X",
            stage, texture->object_va, texture->data_va, surface_key,
            texture->width, texture->height, texture->format);
    }
}

static void d3d_hle_guest_note_gpu_draw(void)
{
    D3DHleGuestResource *back_buffer =
        d3d_hle_guest_find_resource(g_hle_back_buffer);
    D3DHleGuestResource *target =
        d3d_hle_guest_find_resource(g_hle_bindings.render_target);
    if (back_buffer)
        back_buffer->cpu_scanout_active = 0;
    if (target && target->parent_va) {
        D3DHleGuestResource *parent =
            d3d_hle_guest_find_resource(target->parent_va);
        if (parent && parent->kind == D3D_HLE_RESOURCE_TEXTURE)
            parent->version = ++g_hle_texture_version;
    }
}

static D3DHleGuestResource *d3d_hle_guest_register_resource(
    D3DHleGuestResourceKind kind, uint32_t object_va)
{
    D3DHleGuestResourceChunk *chunk;
    unsigned i;
    D3DHleGuestResource *existing =
        d3d_hle_guest_find_resource(object_va);
    if (existing)
        return existing;
    for (chunk = g_hle_resource_chunks; chunk; chunk = chunk->next) {
        for (i = 0; i < XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE; ++i) {
            D3DHleGuestResource *resource = &chunk->resources[i];
            if (resource->kind == D3D_HLE_RESOURCE_NONE) {
                memset(resource, 0, sizeof(*resource));
                resource->kind = kind;
                resource->object_va = object_va;
                d3d_hle_guest_index_put(&g_hle_object_index, object_va,
                                        resource);
                return resource;
            }
        }
    }

    /*
     * Xbox D3D does not impose a 2,048-object ceiling. Keep records in
     * separately allocated chunks so registry growth cannot invalidate a
     * resource pointer held across nested operations such as GetSurfaceLevel.
     * Destroyed records remain in their chunk and are reused above.
     */
    chunk = (D3DHleGuestResourceChunk *)calloc(1, sizeof(*chunk));
    if (!chunk)
        d3d_hle_guest_fatal("HLE resource registry allocation",
                            E_OUTOFMEMORY);
    if (g_hle_resource_chunks_tail)
        g_hle_resource_chunks_tail->next = chunk;
    else
        g_hle_resource_chunks = chunk;
    g_hle_resource_chunks_tail = chunk;
    if (g_hle_resource_capacity >
        UINT32_MAX - XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE)
        d3d_hle_guest_fatal("HLE resource registry capacity",
                            E_OUTOFMEMORY);
    g_hle_resource_capacity += XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE;
    if (g_hle_resource_capacity > 2048u) {
        uint32_t counts[D3D_HLE_RESOURCE_PIXEL_SHADER + 1] = {0};
        D3DHleGuestResourceChunk *scan;
        unsigned slot;
        for (scan = g_hle_resource_chunks; scan; scan = scan->next) {
            for (slot = 0;
                 slot < XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE; ++slot) {
                D3DHleGuestResourceKind resource_kind =
                    scan->resources[slot].kind;
                if (resource_kind >= D3D_HLE_RESOURCE_TEXTURE &&
                    resource_kind <= D3D_HLE_RESOURCE_PIXEL_SHADER)
                    ++counts[resource_kind];
            }
        }
        fprintf(stderr,
                "[D3D-HLE] resource registry grew to %u slots "
                "(live tex=%u surf=%u vb=%u ib=%u vs=%u ps=%u)\n",
                g_hle_resource_capacity,
                counts[D3D_HLE_RESOURCE_TEXTURE],
                counts[D3D_HLE_RESOURCE_SURFACE],
                counts[D3D_HLE_RESOURCE_VERTEX_BUFFER],
                counts[D3D_HLE_RESOURCE_INDEX_BUFFER],
                counts[D3D_HLE_RESOURCE_VERTEX_SHADER],
                counts[D3D_HLE_RESOURCE_PIXEL_SHADER]);
    }

    chunk->resources[0].kind = kind;
    chunk->resources[0].object_va = object_va;
    d3d_hle_guest_index_put(&g_hle_object_index, object_va,
                            &chunk->resources[0]);
    return &chunk->resources[0];
}

static uint32_t d3d_hle_guest_alloc(uint32_t bytes, uint32_t alignment,
                                    uint32_t high)
{
    uint32_t low = 0x1000u;
    uint32_t va;

    if (xbox_HeapSyntheticAvailable()) {
        low = XBOX_HEAP_EXT_BASE;
        high = XBOX_HEAP_EXT_BASE + XBOX_HEAP_EXT_SIZE - 1u;
    }
    va = xbox_HeapAllocRange(bytes, alignment, low, high);
    if (!va)
        d3d_hle_guest_fatal("guest resource allocation", E_OUTOFMEMORY);
    memset(xbox_guest_ptr(va), 0, bytes);
    return va;
}

bool d3d_hle_guest_synthetic_allocator_available(void)
{
    return xbox_HeapSyntheticAvailable();
}

static _Noreturn void
d3d_hle_guest_fatal(const char *operation, HRESULT result)
{
    fprintf(stderr,
            "[D3D-HLE] fatal: %s failed (HRESULT=0x%08X); "
            "the native frontend cannot safely continue\n",
            operation, (unsigned)result);
    if (g_hle_fatal_diagnostic)
        g_hle_fatal_diagnostic();
    abort();
}

void d3d_hle_guest_set_fatal_diagnostic(
    D3DHleGuestFatalDiagnostic diagnostic)
{
    g_hle_fatal_diagnostic = diagnostic;
}

void d3d_hle_guest_set_read_range(D3DHleGuestReadRange reader)
{
    g_hle_read_range = reader;
}

static const void *d3d_hle_guest_snapshot_range(
    unsigned slot, uint32_t va, size_t bytes)
{
    uint8_t *scratch;

    /* Static-recompiler guests expose one flat host mapping, so retain the
     * zero-copy path unless the host explicitly supplies a range reader.
     * Xemu's virtual pages may map to discontiguous physical RAM, and a
     * single xbox_guest_ptr() translation is only valid within its page. */
    if (!g_hle_read_range)
        return xbox_guest_ptr(va);
    if (slot >= sizeof(g_hle_up_scratch) / sizeof(g_hle_up_scratch[0]))
        d3d_hle_guest_fatal("guest range scratch slot", E_INVALIDARG);
    if (!bytes)
        d3d_hle_guest_fatal("guest range snapshot size", E_INVALIDARG);
    if (g_hle_up_scratch_capacity[slot] < bytes) {
        scratch = (uint8_t *)realloc(g_hle_up_scratch[slot], bytes);
        if (!scratch)
            d3d_hle_guest_fatal("guest range snapshot", E_OUTOFMEMORY);
        g_hle_up_scratch[slot] = scratch;
        g_hle_up_scratch_capacity[slot] = bytes;
    }
    if (!g_hle_read_range(va, g_hle_up_scratch[slot], bytes))
        d3d_hle_guest_fatal("guest range read", E_INVALIDARG);
    return g_hle_up_scratch[slot];
}

static bool d3d_hle_guest_try_read_u32(uint32_t va, uint32_t *value)
{
    return value && g_hle_read_range &&
           g_hle_read_range(va, value, sizeof(*value));
}

int d3d_hle_guest_native_active(void)
{
    return xrecomp_d3d_frontend_active() == XRECOMP_D3D_FRONTEND_HLE;
}

uint32_t d3d_hle_guest_stack_u32(unsigned argument_index)
{
    uint32_t va = g_esp + 4u + argument_index * 4u;
    return *(volatile uint32_t *)xbox_guest_ptr(va);
}

void d3d_hle_guest_stdcall_return(unsigned argument_bytes)
{
    g_esp += 4u + argument_bytes;
}

void d3d_hle_guest_return_u32(uint32_t value)
{
    g_eax = value;
}

void d3d_hle_guest_write_u32(uint32_t va, uint32_t value)
{
    memcpy(xbox_guest_ptr(va), &value, sizeof(value));
}

static uint32_t d3d_hle_guest_read_u32(uint32_t va)
{
    uint32_t value;
    memcpy(&value, xbox_guest_ptr(va), sizeof(value));
    return value;
}

static IDirect3DDevice8 *d3d_hle_guest_require_device(
    const char *operation)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    if (!device || !device->lpVtbl)
        d3d_hle_guest_fatal(operation, E_FAIL);
    return device;
}

static D3DHleGuestResourceKind d3d_hle_guest_kind_from_type(
    uint32_t type)
{
    switch (type) {
    case XRECOMP_XBOX_D3DRTYPE_SURFACE:
    case XRECOMP_XBOX_D3DRTYPE_VOLUME:
        return D3D_HLE_RESOURCE_SURFACE;
    case XRECOMP_XBOX_D3DRTYPE_TEXTURE:
    case XRECOMP_XBOX_D3DRTYPE_VOLUMETEXTURE:
    case XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE:
        return D3D_HLE_RESOURCE_TEXTURE;
    case XRECOMP_XBOX_D3DRTYPE_VERTEXBUFFER:
        return D3D_HLE_RESOURCE_VERTEX_BUFFER;
    case XRECOMP_XBOX_D3DRTYPE_INDEXBUFFER:
        return D3D_HLE_RESOURCE_INDEX_BUFFER;
    case XRECOMP_XBOX_D3DRTYPE_PALETTE:
        return D3D_HLE_RESOURCE_PALETTE;
    default:
        return D3D_HLE_RESOURCE_NONE;
    }
}

static D3DHleGuestResource *d3d_hle_guest_adopt_resource(
    uint32_t object_va)
{
    D3DHleGuestResource *resource;
    D3DHleGuestResourceKind kind;
    uint32_t type;
    uint32_t format;
    uint32_t size;
    if (!object_va)
        return NULL;
    resource = d3d_hle_guest_find_resource(object_va);
    if (resource) {
        d3d_hle_guest_refresh_external_resource(resource);
        return resource;
    }
    type = d3d_hle_guest_resource_type(object_va);
    kind = d3d_hle_guest_kind_from_type(type);
    if (kind == D3D_HLE_RESOURCE_NONE)
        return NULL;
    resource = d3d_hle_guest_register_resource(kind, object_va);
    resource->data_va =
        d3d_hle_guest_read_u32(object_va + 4u) & 0x0FFFFFFFu;
    resource->version = ++g_hle_texture_version;
    if (kind == D3D_HLE_RESOURCE_TEXTURE ||
        kind == D3D_HLE_RESOURCE_SURFACE) {
        uint32_t width;
        uint32_t height;
        uint32_t depth;
        uint32_t level;
        uint32_t bytes = 0;
        uint32_t linear_pitch;
        format = d3d_hle_guest_read_u32(object_va + 12u);
        size = d3d_hle_guest_read_u32(object_va + 16u);
        resource->format = (format >> 8) & 0xFFu;
        resource->levels = (format >> 16) & 0xFu;
        if (!resource->levels)
            resource->levels = 1;
        resource->size = size;
        resource->depth = 1;
        if (size) {
            resource->width = (size & 0xFFFu) + 1u;
            resource->height = ((size >> 12) & 0xFFFu) + 1u;
        } else {
            resource->width = 1u << ((format >> 20) & 0xFu);
            resource->height = 1u << ((format >> 24) & 0xFu);
            resource->depth = 1u << ((format >> 28) & 0xFu);
        }
        linear_pitch = d3d_hle_guest_size_pitch(size);
        if (kind == D3D_HLE_RESOURCE_SURFACE) {
            resource->parent_va =
                d3d_hle_guest_read_u32(object_va + 20u);
            resource->levels = 1;
        }
        width = resource->width;
        height = resource->height;
        depth = resource->depth;
        for (level = 0; level < resource->levels; ++level) {
            uint32_t level_bytes = d3d_hle_guest_level_storage_bytes(
                width, height, depth, resource->format,
                level == 0u ? linear_pitch : 0u, NULL);
            if (bytes > UINT32_MAX - level_bytes)
                d3d_hle_guest_fatal(
                    "adopted resource extent overflow", E_INVALIDARG);
            bytes += level_bytes;
            if (width > 1u) width >>= 1;
            if (height > 1u) height >>= 1;
            if (depth > 1u) depth >>= 1;
        }
        if (type == XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE) {
            bytes = (bytes + 127u) & ~127u;
            if (bytes > UINT32_MAX / 6u)
                d3d_hle_guest_fatal(
                    "adopted cube extent overflow", E_INVALIDARG);
            bytes *= 6u;
        }
        resource->data_bytes = bytes;
    }
    return resource;
}

static HRESULT d3d_hle_guest_register_buffer(
    uint32_t object_va, uint32_t length, D3DHleGuestResourceKind kind)
{
    D3DHleGuestResource *resource;

    if (!object_va || !length)
        return E_INVALIDARG;
    resource = d3d_hle_guest_adopt_resource(object_va);
    if (!resource || resource->kind != kind)
        return E_INVALIDARG;

    /* Native XDK buffer headers carry their Data address but not their
     * allocation extent. Mirror the requested creation length so indexed
     * draws cannot treat arbitrary 16-bit values as valid vertex offsets. */
    resource->data_bytes = length;
    if (!resource->content_serial)
        resource->content_serial = 1;
    return S_OK;
}

HRESULT d3d_hle_guest_register_vertex_buffer(
    uint32_t object_va, uint32_t length)
{
    return d3d_hle_guest_register_buffer(
        object_va, length, D3D_HLE_RESOURCE_VERTEX_BUFFER);
}

HRESULT d3d_hle_guest_register_index_buffer(
    uint32_t object_va, uint32_t length)
{
    return d3d_hle_guest_register_buffer(
        object_va, length, D3D_HLE_RESOURCE_INDEX_BUFFER);
}

HRESULT d3d_hle_guest_register_native_resource(uint32_t object_va)
{
    return d3d_hle_guest_adopt_resource(object_va) ? S_OK : E_INVALIDARG;
}

int d3d_hle_guest_adopt_switch_texture(
    uint32_t texture_va, uint32_t data, uint32_t format)
{
    D3DHleGuestResource *texture;
    uint32_t common;
    uint32_t live_data;
    uint32_t live_format;
    uint32_t live_size;

    /* ESI is caller context, not part of SwitchTexture's ABI. It can be an
     * unrelated value for callers that do not retain a PixelContainer there;
     * validate the complete header through xemu's non-fatal range reader
     * before the normal adopter dereferences or registers it. */
    if (!texture_va ||
        !d3d_hle_guest_try_read_u32(texture_va, &common) ||
        (common & XBOX_D3DCOMMON_TYPE_MASK) !=
            XBOX_D3DCOMMON_TYPE_TEXTURE ||
        !(common & XBOX_D3DCOMMON_LIVE_MASK) ||
        !d3d_hle_guest_try_read_u32(texture_va + 4u, &live_data) ||
        !d3d_hle_guest_try_read_u32(texture_va + 12u, &live_format) ||
        !d3d_hle_guest_try_read_u32(texture_va + 16u, &live_size) ||
        (live_data & 0x0FFFFFFFu) != (data & 0x0FFFFFFFu) ||
        live_format != format)
        return 0;

    (void)live_size;
    texture = d3d_hle_guest_adopt_resource(texture_va);
    if (!texture || texture->kind != D3D_HLE_RESOURCE_TEXTURE ||
        (texture->data_va & 0x0FFFFFFFu) != (data & 0x0FFFFFFFu))
        return 0;
    d3d_hle_guest_index_put(
        &g_hle_texture_data_index, data & 0x0FFFFFFFu, texture);
    return 1;
}

static void d3d_hle_guest_resource_add_bind_ref(uint32_t resource_va)
{
    uint32_t common;
    if (!resource_va)
        return;
    common = d3d_hle_guest_read_u32(resource_va);
    if (common > UINT32_MAX - XBOX_D3DCOMMON_BIND_REF)
        d3d_hle_guest_fatal("resource bind-reference overflow",
                            E_INVALIDARG);
    d3d_hle_guest_write_u32(
        resource_va, common + XBOX_D3DCOMMON_BIND_REF);
}

static void d3d_hle_guest_resource_release_bind_ref(uint32_t resource_va)
{
    uint32_t common;
    if (!resource_va)
        return;
    common = d3d_hle_guest_read_u32(resource_va);
    if ((common & XBOX_D3DCOMMON_INTREFCOUNT_MASK) == 0)
        d3d_hle_guest_fatal("resource bind-reference underflow",
                            E_INVALIDARG);

    /*
     * DrawPrimitiveUP/DrawIndexedPrimitiveUP synchronously copy the referenced
     * guest vertices into Plume's frame arena. No queued host draw retains the
     * guest allocation after the native draw helper returns.
     */
    d3d_hle_guest_write_u32(resource_va + 8u, 0);
    common -= XBOX_D3DCOMMON_BIND_REF;
    d3d_hle_guest_write_u32(resource_va, common);
    if ((common & XBOX_D3DCOMMON_LIVE_MASK) == 0) {
        D3DHleGuestResource *resource =
            d3d_hle_guest_find_resource(resource_va);
        if (!resource)
            d3d_hle_guest_fatal(
                "resource unbind missing registry entry", E_INVALIDARG);
        d3d_hle_guest_destroy_resource(resource);
    }

}

/* Direct-HLE resources normally store a physical offset and expose it through
 * the cached 0x80000000 alias. The runtime's overflow heap is already a real
 * guest-VA window, however; OR-ing that VA with the physical alias bit folds
 * it onto unrelated low RAM. Preserve HEAP_EXT addresses verbatim so
 * CPU/archive-backed registered resources can live outside scarce physical
 * memory. */
static uint32_t d3d_hle_guest_data_address(uint32_t data_va)
{
    if (data_va >= XBOX_HEAP_EXT_BASE
        && data_va < XBOX_HEAP_EXT_BASE + XBOX_HEAP_EXT_SIZE)
        return data_va;
    return data_va | 0x80000000u;
}

static uint32_t d3d_hle_guest_data_offset(uint32_t data_va)
{
    if (data_va >= XBOX_HEAP_EXT_BASE &&
        data_va < XBOX_HEAP_EXT_BASE + XBOX_HEAP_EXT_SIZE) {
        return data_va & 0x0FFFFFFFu;
    }
    return data_va;
}

static uint8_t *d3d_hle_guest_data_ptr(uint32_t data_va)
{
    return xbox_guest_ptr(d3d_hle_guest_data_address(data_va));
}

static const uint8_t *d3d_hle_guest_bound_vertex_data(
    uint32_t *stride_out)
{
    uint32_t resource_va = g_hle_bindings.stream_resource[0];
    uint32_t stride = g_hle_bindings.stream_stride[0];
    uint32_t data_va;
    if (!resource_va || !stride)
        d3d_hle_guest_fatal("draw without stream zero", E_INVALIDARG);
    data_va = d3d_hle_guest_read_u32(resource_va + 4u);
    if (!data_va)
        d3d_hle_guest_fatal("draw with null vertex-buffer data",
                            E_INVALIDARG);
    *stride_out = stride;
    return d3d_hle_guest_data_ptr(data_va);
}

static uint32_t d3d_hle_guest_primitive_count(
    D3DPRIMITIVETYPE type, uint32_t element_count)
{
    switch (type) {
    case D3DPT_POINTLIST:
        return element_count;
    case D3DPT_LINELIST:
        return element_count / 2u;
    case D3DPT_LINELOOP:
    case D3DPT_LINESTRIP:
        return element_count >= 2u ? element_count - 1u : 0u;
    case D3DPT_TRIANGLELIST:
        return element_count / 3u;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
    case D3DPT_POLYGON:
        return element_count >= 3u ? element_count - 2u : 0u;
    case D3DPT_QUADLIST:
        return element_count / 4u;
    case D3DPT_QUADSTRIP:
        return element_count >= 4u ? (element_count - 2u) / 2u : 0u;
    default:
        d3d_hle_guest_fatal("draw with unsupported primitive type",
                            E_INVALIDARG);
    }
}

void d3d_hle_guest_get_device_caps(uint32_t caps_va)
{
    /*
     * The Xbox has one fixed graphics device. XDK builds construct the same
     * 53-DWORD D3DCAPS8 record during CRT startup; keeping it here removes a
     * title-global dependency from the signature-resolved hook.
     */
    static const uint32_t xbox_caps[53] = {
        0x00000001u, 0x00000000u, 0x00000000u, 0x00000002u,
        0x00000000u, 0x00000000u, 0x80000007u, 0x00000000u,
        0x007BBEF0u, 0x00000CF2u, 0x00377101u, 0x000000FFu,
        0x00001FFFu, 0x00001FFFu, 0x000000FFu, 0x00084208u,
        0x0007EC87u, 0x1F030700u, 0x1F030700u, 0x17030300u,
        0x17000000u, 0x1F000000u, 0x00100000u, 0x00100000u,
        0x00020000u, 0x00200000u, 0x00000000u, 0x00000004u,
        0x501502F9u, 0xCCBEBC20u, 0xCCBEBC20u, 0x4CBEBC20u,
        0x4CBEBC20u, 0x00000000u, 0x000000FFu, 0x00080004u,
        0x00FFFFFFu, 0x00000004u, 0x00000004u, 0x000000BBu,
        0x00000008u, 0x00000000u, 0x00000004u, 0x00000000u,
        0x42800000u, 0x0000FFFFu, 0x0000FFFFu, 0x00000010u,
        0x000000FFu, 0xFFFF0101u, 0x00000060u, 0xFFFF0101u,
        0x3F800000u,
    };
    memcpy(xbox_guest_ptr(caps_va), xbox_caps, sizeof(xbox_caps));
}

uint32_t d3d_hle_guest_resource_type(uint32_t resource_va)
{
    uint32_t common = d3d_hle_guest_read_u32(resource_va);
    uint32_t format;

    switch (common & XBOX_D3DCOMMON_TYPE_MASK) {
    case 0:
        return XRECOMP_XBOX_D3DRTYPE_VERTEXBUFFER;
    case XBOX_D3DCOMMON_TYPE_INDEXBUFFER:
        return XRECOMP_XBOX_D3DRTYPE_INDEXBUFFER;
    case XBOX_D3DCOMMON_TYPE_PALETTE:
        return XRECOMP_XBOX_D3DRTYPE_PALETTE;
    case XBOX_D3DCOMMON_TYPE_PUSHBUFFER:
        return XRECOMP_XBOX_D3DRTYPE_PUSHBUFFER;
    case XBOX_D3DCOMMON_TYPE_TEXTURE:
        format = d3d_hle_guest_read_u32(resource_va + 12u);
        if (format & XBOX_D3DFORMAT_CUBEMAP)
            return XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE;
        return (format & XBOX_D3DFORMAT_DIMENSION_MASK) >=
                       XBOX_D3DFORMAT_3D
                   ? XRECOMP_XBOX_D3DRTYPE_VOLUMETEXTURE
                   : XRECOMP_XBOX_D3DRTYPE_TEXTURE;
    case XBOX_D3DCOMMON_TYPE_SURFACE:
        format = d3d_hle_guest_read_u32(resource_va + 12u);
        return (format & XBOX_D3DFORMAT_DIMENSION_MASK) >=
                       XBOX_D3DFORMAT_3D
                   ? XRECOMP_XBOX_D3DRTYPE_VOLUME
                   : XRECOMP_XBOX_D3DRTYPE_SURFACE;
    case XBOX_D3DCOMMON_TYPE_FIXUP:
        return XRECOMP_XBOX_D3DRTYPE_FIXUP;
    default:
        return XRECOMP_XBOX_D3DRTYPE_NONE;
    }
}

void d3d_hle_guest_surface_desc(
    uint32_t resource_va, uint32_t level, uint32_t desc_va)
{
    /*
     * g_TextureFormat from the retail XDK. Bits 2..5 carry bits-per-pixel,
     * bit 6 identifies depth formats, and the sign bit identifies render
     * targets. Unspecified table entries are zero, as in the XDK image.
     */
    static const uint8_t format_info[256] = {
        0x09, 0x09, 0x11, 0x91, 0x11, 0x91, 0xA1, 0xA1,
        0x00, 0x00, 0x00, 0x09, 0x04, 0x00, 0x08, 0x08,
        0x12, 0x92, 0xA2, 0x8A, 0x00, 0x00, 0x12, 0x92,
        0x00, 0x09, 0x11, 0x0A, 0x92, 0x12, 0xA2, 0x0A,
        0x12, 0x00, 0x00, 0x00, 0x12, 0x12, 0x00, 0x11,
        0x11, 0x11, 0x61, 0x61, 0x51, 0x51, 0x62, 0x62,
        0x52, 0x52, 0x11, 0x21, 0x00, 0x12, 0x00, 0x12,
        0x11, 0x11, 0x21, 0x21, 0x21, 0x12, 0x12, 0x22,
        0x22, 0x22,
    };
    enum {
        XBOX_D3DUSAGE_RENDERTARGET = 1,
        XBOX_D3DUSAGE_DEPTHSTENCIL = 2,
        XBOX_D3DMULTISAMPLE_NONE = 17,
    };
    uint32_t format = d3d_hle_guest_read_u32(resource_va + 12u);
    uint32_t size = d3d_hle_guest_read_u32(resource_va + 16u);
    uint32_t mip_count = (format >> 16) & 0xFu;
    uint32_t format_code = (format >> 8) & 0xFFu;
    uint32_t info = format_info[format_code];
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t slice;
    uint32_t usage = 0;

    if (level >= mip_count)
        d3d_hle_guest_fatal("Get2DSurfaceDesc (invalid mip level)",
                            E_INVALIDARG);

    if (size != 0) {
        width = (size & 0xFFFu) + 1u;
        height = ((size >> 12) & 0xFFFu) + 1u;
        pitch = (((size >> 24) & 0xFFu) + 1u) << 6;
        slice = pitch * height;
    } else {
        int width_log2 = (int)((format >> 20) & 0xFu) - (int)level;
        int height_log2 = (int)((format >> 24) & 0xFu) - (int)level;
        int stored_width_log2;
        int stored_height_log2;
        uint32_t bits_per_pixel = info & 0x3Cu;
        int compressed = format_code == 12u ||
                         (format_code >= 14u && format_code <= 15u);

        width = 1u << (width_log2 > 0 ? width_log2 : 0);
        height = 1u << (height_log2 > 0 ? height_log2 : 0);
        stored_width_log2 =
            compressed && width_log2 < 2 ? 2 : width_log2;
        stored_height_log2 =
            compressed && height_log2 < 2 ? 2 : height_log2;
        if (stored_width_log2 < 0)
            stored_width_log2 = 0;
        if (stored_height_log2 < 0)
            stored_height_log2 = 0;
        if (format_code == 12u)
            pitch = 2u << stored_width_log2;
        else if (format_code >= 14u && format_code <= 15u)
            pitch = 4u << stored_width_log2;
        else
            pitch = (bits_per_pixel << stored_width_log2) >> 3;
        slice = (bits_per_pixel << stored_width_log2) *
                (1u << stored_height_log2) >> 3;
    }

    if (level == 0) {
        if ((int8_t)info < 0)
            usage = XBOX_D3DUSAGE_RENDERTARGET;
        else if (info & 0x40u)
            usage = XBOX_D3DUSAGE_DEPTHSTENCIL;
    }

    /* Xbox D3DSURFACE_DESC is seven DWORDs; unlike the PC shape it has no Pool. */
    d3d_hle_guest_write_u32(desc_va + 0u, format_code);
    d3d_hle_guest_write_u32(
        desc_va + 4u, d3d_hle_guest_resource_type(resource_va));
    d3d_hle_guest_write_u32(desc_va + 8u, usage);
    d3d_hle_guest_write_u32(desc_va + 12u, slice);
    d3d_hle_guest_write_u32(
        desc_va + 16u, XBOX_D3DMULTISAMPLE_NONE);
    d3d_hle_guest_write_u32(desc_va + 20u, width);
    d3d_hle_guest_write_u32(desc_va + 24u, height);
}

/* Xbox culling is the pair (CullMode winding, FrontFace winding). The XDK
 * combines both into NV097_SET_CULL_FACE; PC D3D8 has no front-face state,
 * so a non-default FrontFace inverts the effective D3DCULL instead. */
#define XRECOMP_XBOX_D3DFRONT_CW 0x900u
#define XRECOMP_XBOX_D3DFRONT_CCW 0x901u
static uint32_t g_hle_cull_winding = XRECOMP_XBOX_D3DCULL_CCW;
static uint32_t g_hle_front_face = XRECOMP_XBOX_D3DFRONT_CW;

static void d3d_hle_guest_apply_cull_mode(void)
{
    uint32_t winding = g_hle_cull_winding;
    uint32_t plume_cull_mode;

    if (g_hle_front_face == XRECOMP_XBOX_D3DFRONT_CCW &&
        winding != XRECOMP_XBOX_D3DCULL_NONE) {
        winding = winding == XRECOMP_XBOX_D3DCULL_CW
                      ? XRECOMP_XBOX_D3DCULL_CCW
                      : XRECOMP_XBOX_D3DCULL_CW;
    }
    switch (winding) {
    case XRECOMP_XBOX_D3DCULL_NONE:
        plume_cull_mode = D3DCULL_NONE;
        break;
    case XRECOMP_XBOX_D3DCULL_CW:
        plume_cull_mode = D3DCULL_CW;
        break;
    case XRECOMP_XBOX_D3DCULL_CCW:
        plume_cull_mode = D3DCULL_CCW;
        break;
    default:
        plume_cull_mode = D3DCULL_CCW;
        break;
    }
    d3d_hle_guest_set_render_state(D3DRS_CULLMODE, plume_cull_mode);
}

void d3d_hle_guest_set_cull_mode(uint32_t xbox_cull_mode)
{
    if (xbox_cull_mode != XRECOMP_XBOX_D3DCULL_NONE &&
        xbox_cull_mode != XRECOMP_XBOX_D3DCULL_CW &&
        xbox_cull_mode != XRECOMP_XBOX_D3DCULL_CCW)
        d3d_hle_guest_fatal("SetRenderState_CullMode (invalid winding)",
                            E_INVALIDARG);
    g_hle_cull_winding = xbox_cull_mode;
    d3d_hle_guest_apply_cull_mode();
}

void d3d_hle_guest_set_front_face(uint32_t xbox_front_face)
{
    if (xbox_front_face != XRECOMP_XBOX_D3DFRONT_CW &&
        xbox_front_face != XRECOMP_XBOX_D3DFRONT_CCW)
        d3d_hle_guest_fatal("SetRenderState_FrontFace (invalid winding)",
                            E_INVALIDARG);
    g_hle_front_face = xbox_front_face;
    d3d_hle_guest_apply_cull_mode();
}

void d3d_hle_guest_set_fill_mode(uint32_t xbox_fill_mode)
{
    uint32_t plume_fill_mode;

    switch (xbox_fill_mode) {
    case 0x1B00u:
        plume_fill_mode = 1u;
        break;
    case 0x1B01u:
        plume_fill_mode = 2u;
        break;
    case 0x1B02u:
        plume_fill_mode = 3u;
        break;
    default:
        d3d_hle_guest_fatal("SetRenderState_FillMode (invalid mode)",
                            E_INVALIDARG);
    }
    d3d_hle_guest_set_render_state(D3DRS_FILLMODE, plume_fill_mode);
}

static int d3d_hle_guest_ascii_equal_ignore_case(const char *left,
                                                  const char *right)
{
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;

        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
    }
    return *left == *right;
}

static int d3d_hle_guest_fog_enabled(void)
{
    const char *value = getenv("XRECOMP_FOG_ENABLED");

    if (!value || !value[0])
        return 1;
    return strcmp(value, "0") != 0
        && !d3d_hle_guest_ascii_equal_ignore_case(value, "false")
        && !d3d_hle_guest_ascii_equal_ignore_case(value, "no")
        && !d3d_hle_guest_ascii_equal_ignore_case(value, "off");
}

void d3d_hle_guest_set_render_state(D3DRENDERSTATETYPE state,
                                    uint32_t value)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    HRESULT result;

    /* Keep this override at the shared D3D state boundary. MM3's sky-owned
     * flag also gates sky rendering, while D3DRS_FOGENABLE controls only the
     * final fog blend for both direct and deferred XDK state paths. */
    if (state == D3DRS_FOGENABLE && !d3d_hle_guest_fog_enabled())
        value = 0;
    if ((uint32_t)state < XBOX_D3D_HLE_MAX_STATES)
        g_hle_state_cache[(uint32_t)state] = value;
    if ((uint32_t)state >= 256u)
        return;
    if (!device)
        d3d_hle_guest_fatal("SetRenderState (device unavailable)",
                            E_FAIL);
    result = device->lpVtbl->SetRenderState(device, state, value);
    if (result != S_OK)
        d3d_hle_guest_fatal("SetRenderState", result);
}

void d3d_hle_guest_set_texture_stage_state(
    uint32_t stage, D3DTEXTURESTAGESTATETYPE state, uint32_t value)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    HRESULT result;
    if (!device)
        d3d_hle_guest_fatal("SetTextureStageState (device unavailable)",
                            E_FAIL);
    result = device->lpVtbl->SetTextureStageState(
        device, stage, state, value);
    if (result != S_OK)
        d3d_hle_guest_fatal("SetTextureStageState", result);
}

/* Guest-controlled constant writes are never fatal: the hardware oracle
 * accepts or ignores them and keeps running. ponytail: single latch —
 * first dropped write wins the diagnostic, later ones stay silent. */
static void d3d_hle_guest_drop_vertex_shader_constant(
    const char *operation, int32_t start_register, uint32_t count)
{
    static bool logged;

    if (logged)
        return;
    logged = true;
    fprintf(stderr,
            "[D3D-HLE] dropped SetVertexShaderConstant %s "
            "(start=%d count=%u mode=%08X); continuing without this "
            "write\n",
            operation, (int)start_register, (unsigned)count,
            (unsigned)g_hle_shader_constant_mode);
}

void d3d_hle_guest_set_vertex_shader_constant_hardware(
    uint32_t start_register, uint32_t constant_data_va, uint32_t count)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    const void *constant_data;
    HRESULT result;
    if (!device)
        d3d_hle_guest_fatal(
            "SetVertexShaderConstant (device unavailable)", E_FAIL);
    if (!count || start_register >= 192u || count > 192u - start_register) {
        d3d_hle_guest_drop_vertex_shader_constant(
            "hardware range", (int32_t)start_register, count);
        return;
    }
    constant_data = xbox_guest_ptr(constant_data_va);
    result = device->lpVtbl->SetVertexShaderConstant(
        device, (INT)start_register, constant_data, count);
    if (result != S_OK)
        d3d_hle_guest_fatal("SetVertexShaderConstant", result);
}

void d3d_hle_guest_set_vertex_shader_constant(
    int32_t start_register, uint32_t constant_data_va, uint32_t count)
{
    /* Raw pass-through: the XDK helper bodies already hand us hardware
     * constant slots (PGR2 writes start=96 through this path), and the
     * committed, MM3-validated implementation did no translation here.
     * ponytail: the ±96 mode translator is unproven against any title;
     * reintroduce only with a live-validated mode-switching title. */
    d3d_hle_guest_set_vertex_shader_constant_hardware(
        (uint32_t)start_register, constant_data_va, count);
}

void d3d_hle_guest_set_shader_constant_mode(uint32_t mode)
{
    uint32_t base_mode = mode & ~XBOX_D3DSCM_NORESERVEDCONSTANTS;

    if ((mode & ~(XBOX_D3DSCM_NORESERVEDCONSTANTS | 3u)) ||
        base_mode > XBOX_D3DSCM_192CONSTANTSANDFIXEDPIPELINE) {
        static bool logged;

        if (!logged) {
            logged = true;
            fprintf(stderr,
                    "[D3D-HLE] dropped SetShaderConstantMode(%08X); "
                    "keeping current mode %08X\n",
                    (unsigned)mode,
                    (unsigned)g_hle_shader_constant_mode);
        }
        return;
    }
    g_hle_shader_constant_mode = mode;
}

void d3d_hle_guest_set_stream_source(
    uint32_t stream, uint32_t resource_va, uint32_t stride)
{
    uint32_t previous;
    if (stream >= XBOX_D3D_MAX_STREAMS)
        d3d_hle_guest_fatal("SetStreamSource (invalid stream)",
                            E_INVALIDARG);
    if (resource_va) {
        if (!d3d_hle_guest_adopt_resource(resource_va))
            d3d_hle_guest_fatal(
                "SetStreamSource invalid resource", E_INVALIDARG);
        d3d_hle_guest_resource_add_bind_ref(resource_va);
    }
    previous = g_hle_bindings.stream_resource[stream];
    g_hle_bindings.stream_resource[stream] = resource_va;
    g_hle_bindings.stream_stride[stream] = stride;
    d3d_hle_guest_resource_release_bind_ref(previous);
}

void d3d_hle_guest_set_indices(
    uint32_t resource_va, uint32_t base_vertex)
{
    uint32_t previous;
    if (resource_va) {
        if (!d3d_hle_guest_adopt_resource(resource_va))
            d3d_hle_guest_fatal(
                "SetIndices invalid resource", E_INVALIDARG);
        d3d_hle_guest_resource_add_bind_ref(resource_va);
    }
    previous = g_hle_bindings.index_resource;
    g_hle_bindings.index_resource = resource_va;
    g_hle_bindings.base_vertex = base_vertex;
    d3d_hle_guest_resource_release_bind_ref(previous);
}

void d3d_hle_guest_draw_vertices(
    D3DPRIMITIVETYPE primitive_type, uint32_t start_vertex,
    uint32_t vertex_count)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    D3DHleGuestResource *vertex_resource;
    const uint8_t *vertices;
    uint32_t stride;
    uint32_t raw_data_va;
    uint32_t primitive_count =
        d3d_hle_guest_primitive_count(primitive_type, vertex_count);
    uint64_t byte_offset;
    uint64_t byte_count;
    uint64_t byte_end;
    HRESULT result;
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_draw_vertices_zone, "D3D HLE Draw Vertices");
    if (!device || !device->lpVtbl || !device->lpVtbl->DrawPrimitiveUP)
        d3d_hle_guest_fatal("DrawVertices (device unavailable)", E_FAIL);
    if (!primitive_count)
        d3d_hle_guest_fatal("DrawVertices (too few vertices)",
                            E_INVALIDARG);
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_draw_vertices_state_zone, "D3D HLE Draw State");
    vertex_resource =
        d3d_hle_guest_find_resource(g_hle_bindings.stream_resource[0]);
    d3d_hle_guest_refresh_external_resource(vertex_resource);
    d3d_hle_guest_prepare_fixed_state();
    vertices = d3d_hle_guest_bound_vertex_data(&stride);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_vertices_state_zone);
    byte_offset = (uint64_t)start_vertex * stride;
    byte_count = (uint64_t)vertex_count * stride;
    if (byte_count > UINT64_MAX - byte_offset)
        d3d_hle_guest_fatal("DrawVertices (vertex range overflow)",
                            E_INVALIDARG);
    byte_end = byte_offset + byte_count;
    if (byte_offset > UINT32_MAX)
        d3d_hle_guest_fatal("DrawVertices (vertex offset overflow)",
                            E_INVALIDARG);
    raw_data_va = d3d_hle_guest_read_u32(
        g_hle_bindings.stream_resource[0] + 4u);
    D3D_HLE_F2_LOG(
        "hle draw prim=%u count=%u pc=%u vb=%08X raw=%08X "
        "data=%08X vbbytes=%u start=%u stride=%u range=%llu:%llu "
        "over=%u",
        (uint32_t)primitive_type, vertex_count, primitive_count,
        g_hle_bindings.stream_resource[0], raw_data_va,
        vertex_resource ? vertex_resource->data_va : 0u,
        vertex_resource ? vertex_resource->data_bytes : 0u,
        start_vertex, stride,
        (unsigned long long)byte_offset,
        (unsigned long long)byte_end,
        vertex_resource && vertex_resource->data_bytes &&
            byte_end > vertex_resource->data_bytes ? 1u : 0u);
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_draw_vertices_submit_zone, "D3D HLE Device Draw");
    result = device->lpVtbl->DrawPrimitiveUP(
        device, primitive_type, primitive_count,
        vertices + (size_t)byte_offset, stride);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_vertices_submit_zone);
    if (result != S_OK)
        d3d_hle_guest_fatal("DrawVertices", result);
    d3d_hle_guest_note_gpu_draw();
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_vertices_zone);
}

void d3d_hle_guest_draw_indexed_vertices(
    D3DPRIMITIVETYPE primitive_type, uint32_t index_count,
    uint32_t indices_va)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    D3DHleGuestResource *vertex_resource;
    D3DHleGuestResource *index_resource;
    uint32_t resolved_indices_va = indices_va;
    const uint16_t *indices;
    const uint16_t *draw_indices;
    uint16_t *sanitized_indices = NULL;
    const uint8_t *vertices;
    uint32_t stride;
    uint32_t primitive_count =
        d3d_hle_guest_primitive_count(primitive_type, index_count);
    uint64_t first_vertex;
    uint64_t byte_offset;
    uint32_t vertex_capacity = 0x10000u;
    uint32_t invalid_indices = 0;
    uint32_t invalid_primitives = 0;
    uint32_t first_invalid = UINT32_MAX;
    uint32_t i;
    HRESULT result;
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_draw_indexed_zone, "D3D HLE Draw Indexed");
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_draw_indexed_state_zone, "D3D HLE Draw State");
    vertex_resource =
        d3d_hle_guest_find_resource(g_hle_bindings.stream_resource[0]);
    index_resource =
        d3d_hle_guest_find_resource(g_hle_bindings.index_resource);
    if (!device || !device->lpVtbl ||
        !device->lpVtbl->DrawIndexedPrimitiveUP)
        d3d_hle_guest_fatal(
            "DrawIndexedVertices (device unavailable)", E_FAIL);
    if (!primitive_count || !index_count)
        d3d_hle_guest_fatal(
            "DrawIndexedVertices (too few indices)", E_INVALIDARG);
    d3d_hle_guest_refresh_external_resource(vertex_resource);
    d3d_hle_guest_refresh_external_resource(index_resource);
    d3d_hle_guest_prepare_fixed_state();
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_indexed_state_zone);
    if (index_resource &&
        index_resource->kind == D3D_HLE_RESOURCE_INDEX_BUFFER &&
        index_resource->data_va &&
        indices_va < index_resource->data_va) {
        uint64_t index_bytes =
            (uint64_t)index_count * sizeof(*indices);
        uint64_t resolved =
            (uint64_t)index_resource->data_va + indices_va;

        /*
         * The XDK's SetIndices leaf caches the bound index-buffer Data
         * pointer in a module global. DrawIndexedVertices callers commonly
         * add their byte offset to that cached pointer before entering the
         * public draw leaf. A native SetIndices bridge cannot update an
         * unknown, title-local XDK global, so lifted callers instead arrive
         * with the byte offset alone. Recover the pointer from the bound
         * resource. Adopted Xbox vertex/index resources do not encode their
         * extent, so data_bytes is legitimately zero for those objects.
         *
         * An absolute pointer into the bound index buffer cannot precede its
         * Data address. Use that invariant to distinguish offsets without a
         * title-specific address threshold. Already-absolute pointers pass
         * through unchanged.
         */
        if (index_resource->data_bytes &&
            (indices_va >= index_resource->data_bytes ||
             index_bytes >
                 index_resource->data_bytes - indices_va))
            d3d_hle_guest_fatal(
                "DrawIndexedVertices (index range overflow)",
                E_INVALIDARG);
        if (resolved > UINT32_MAX)
            d3d_hle_guest_fatal(
                "DrawIndexedVertices (index pointer overflow)",
                E_INVALIDARG);
        resolved_indices_va = (uint32_t)resolved;
    }
    /*
     * Faithful floor for a bad index pointer: real hardware DMA-reads
     * whatever the caller pushed and draws garbage for a frame — it never
     * halts the console. The HLE cannot read unmapped memory, so probe the
     * resolved range (first and last index word) with tolerant reads and
     * DROP the draw when unreadable (RSC2 intermittently issues offset
     * 000001C0 with no usable bound index buffer during race transitions;
     * the previous xbox_guest_ptr fail-fast killed the whole process).
     */
    {
        uint32_t probe_word;
        uint64_t index_bytes = (uint64_t)index_count * sizeof(*indices);
        uint32_t last_va =
            resolved_indices_va + (index_count - 1u) * sizeof(*indices);

        if (resolved_indices_va > UINT32_MAX - index_bytes ||
            !d3d_hle_guest_try_read_u32(resolved_indices_va, &probe_word) ||
            !d3d_hle_guest_try_read_u32(last_va & ~3u, &probe_word)) {
            D3D_HLE_F2_LOG(
                "hle dropped indexed draw: unreadable indices "
                "prim=%u index_count=%u arg=%08X resolved=%08X "
                "ib=%08X ibdata=%08X ibbytes=%u",
                (uint32_t)primitive_type, index_count, indices_va,
                resolved_indices_va, g_hle_bindings.index_resource,
                index_resource ? index_resource->data_va : 0u,
                index_resource ? index_resource->data_bytes : 0u);
            fprintf(stderr,
                    "[D3D-HLE] dropped indexed draw: unreadable indices "
                    "arg=%08X resolved=%08X index_count=%u ib=%08X\n",
                    indices_va, resolved_indices_va, index_count,
                    g_hle_bindings.index_resource);
            XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_indexed_zone);
            return;
        }
    }
    indices =
        (const uint16_t *)xbox_guest_ptr(resolved_indices_va);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    if (xgpu_plume_f2_active()) {
        uint32_t min_index = UINT32_MAX;
        uint32_t max_index = 0;
        uint32_t sample_index;
        uint16_t sample[12] = {0};
        uint32_t sample_count = index_count < 12u ? index_count : 12u;

        for (sample_index = 0; sample_index < index_count; ++sample_index) {
            uint32_t index = indices[sample_index];
            if (index < min_index)
                min_index = index;
            if (index > max_index)
                max_index = index;
        }
        memcpy(sample, indices, sample_count * sizeof(sample[0]));
        D3D_HLE_F2_LOG(
            "hle indexed prim=%u count=%u pc=%u ib=%08X "
            "ibdata=%08X ibbytes=%u arg=%08X resolved=%08X "
            "vb=%08X vbdata=%08X vbbytes=%u base=%u stride=%u "
            "range=%u:%u idx=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            (uint32_t)primitive_type, index_count, primitive_count,
            g_hle_bindings.index_resource,
            index_resource ? index_resource->data_va : 0u,
            index_resource ? index_resource->data_bytes : 0u,
            indices_va, resolved_indices_va,
            g_hle_bindings.stream_resource[0],
            vertex_resource ? vertex_resource->data_va : 0u,
            vertex_resource ? vertex_resource->data_bytes : 0u,
            g_hle_bindings.base_vertex,
            g_hle_bindings.stream_stride[0],
            min_index, max_index,
            sample[0], sample[1], sample[2], sample[3],
            sample[4], sample[5], sample[6], sample[7],
            sample[8], sample[9], sample[10], sample[11]);
    }
#endif
    vertices = d3d_hle_guest_bound_vertex_data(&stride);
    first_vertex = g_hle_bindings.base_vertex;
    byte_offset = first_vertex * stride;
    if (first_vertex > UINT32_MAX || byte_offset > UINT32_MAX)
        d3d_hle_guest_fatal(
            "DrawIndexedVertices (base vertex overflow)",
            E_INVALIDARG);
    if (vertex_resource && vertex_resource->data_bytes) {
        if (byte_offset >= vertex_resource->data_bytes) {
            D3D_HLE_F2_LOG(
                "hle indexed dropped: base byte=%llu vbbytes=%u stride=%u",
                (unsigned long long)byte_offset,
                vertex_resource->data_bytes, stride);
            XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_indexed_zone);
            return;
        }
        vertex_capacity =
            (vertex_resource->data_bytes - (uint32_t)byte_offset) / stride;
        if (!vertex_capacity) {
            D3D_HLE_F2_LOG(
                "hle indexed dropped: no complete vertices vbbytes=%u "
                "base=%u stride=%u",
                vertex_resource->data_bytes,
                g_hle_bindings.base_vertex, stride);
            XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_indexed_zone);
            return;
        }
    }

    for (i = 0; i < index_count; ++i) {
        if (indices[i] >= vertex_capacity) {
            if (first_invalid == UINT32_MAX)
                first_invalid = i;
            ++invalid_indices;
        }
    }
    draw_indices = indices;
    if (invalid_indices) {
        sanitized_indices = (uint16_t *)malloc(
            (size_t)index_count * sizeof(*sanitized_indices));
        if (!sanitized_indices)
            d3d_hle_guest_fatal(
                "DrawIndexedVertices index sanitization", E_OUTOFMEMORY);
        memcpy(sanitized_indices, indices,
               (size_t)index_count * sizeof(*sanitized_indices));

        if (primitive_type == D3DPT_TRIANGLELIST) {
            for (i = 0; i + 2u < index_count; i += 3u) {
                if (indices[i] >= vertex_capacity ||
                    indices[i + 1u] >= vertex_capacity ||
                    indices[i + 2u] >= vertex_capacity) {
                    sanitized_indices[i] = 0;
                    sanitized_indices[i + 1u] = 0;
                    sanitized_indices[i + 2u] = 0;
                    ++invalid_primitives;
                }
            }
        } else {
            /* A corrupt strip/fan index can contaminate several neighboring
             * primitives. Degenerate the draw instead of inventing geometry
             * or allowing an out-of-bounds guest vertex read. */
            memset(sanitized_indices, 0,
                   (size_t)index_count * sizeof(*sanitized_indices));
            invalid_primitives = primitive_count;
        }
        draw_indices = sanitized_indices;
        D3D_HLE_F2_LOG(
            "hle indexed sanitized invalid_indices=%u "
            "invalid_primitives=%u first=%u capacity=%u "
            "vbbytes=%u base=%u stride=%u",
            invalid_indices, invalid_primitives, first_invalid,
            vertex_capacity, vertex_resource ? vertex_resource->data_bytes : 0u,
            g_hle_bindings.base_vertex, stride);
    }
    /*
     * Native XDK-created buffers now retain their exact requested extent.
     * Pass that capacity through so the compatibility path independently
     * enforces the same vertex bounds after the malformed-primitive guard.
     */
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_draw_indexed_submit_zone, "D3D HLE Device Draw");
    result = device->lpVtbl->DrawIndexedPrimitiveUP(
        device, primitive_type, 0, vertex_capacity, primitive_count,
        draw_indices,
        D3DFMT_INDEX16, vertices + (size_t)byte_offset, stride);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_indexed_submit_zone);
    free(sanitized_indices);
    if (result != S_OK)
        d3d_hle_guest_fatal("DrawIndexedVertices", result);
    d3d_hle_guest_note_gpu_draw();
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_draw_indexed_zone);
}

uint32_t d3d_hle_guest_resource_is_busy(uint32_t resource_va)
{
    uint32_t common;
    uint32_t target_va = resource_va;
    uint32_t parent_va;
    uint32_t lock;
    if (!resource_va)
        d3d_hle_guest_fatal("D3DResource_IsBusy (null resource)",
                            E_INVALIDARG);
    common = d3d_hle_guest_read_u32(resource_va);
    if ((common & XBOX_D3DCOMMON_TYPE_MASK) ==
        XBOX_D3DCOMMON_TYPE_SURFACE) {
        parent_va = d3d_hle_guest_read_u32(resource_va + 20u);
        if (parent_va) {
            if ((common & XBOX_D3DCOMMON_INTREFCOUNT_MASK) ||
                (d3d_hle_guest_read_u32(parent_va) &
                 XBOX_D3DCOMMON_INTREFCOUNT_MASK))
                return 1;
            target_va = parent_va;
        }
    }
    common = d3d_hle_guest_read_u32(target_va);
    if (common & XBOX_D3DCOMMON_INTREFCOUNT_MASK)
        return 1;
    if ((common & XBOX_D3DCOMMON_TYPE_MASK) ==
        XBOX_D3DCOMMON_TYPE_SURFACE) {
        parent_va = d3d_hle_guest_read_u32(target_va + 20u);
        if (parent_va &&
            (d3d_hle_guest_read_u32(parent_va) &
             XBOX_D3DCOMMON_INTREFCOUNT_MASK))
            return 1;
    }
    lock = d3d_hle_guest_read_u32(target_va + 8u);
    if (lock && !xgpu_plume_wait_for_idle(0))
        d3d_hle_guest_fatal("D3DResource_IsBusy wait", E_FAIL);
    d3d_hle_guest_write_u32(target_va + 8u, 0);
    return 0;
}

void d3d_hle_guest_block_on_resource(uint32_t resource_va)
{
    uint32_t common;
    uint32_t parent_va = 0;

    if (!resource_va)
        d3d_hle_guest_fatal("BlockOnResource (null resource)",
                            E_INVALIDARG);
    if (!xgpu_plume_wait_for_idle(0))
        d3d_hle_guest_fatal("BlockOnResource", E_FAIL);
    common = d3d_hle_guest_read_u32(resource_va);
    if ((common & XBOX_D3DCOMMON_TYPE_MASK) ==
        XBOX_D3DCOMMON_TYPE_SURFACE) {
        parent_va = d3d_hle_guest_read_u32(resource_va + 20u);
    }
    d3d_hle_guest_write_u32(resource_va + 8u, 0);
    if (parent_va)
        d3d_hle_guest_write_u32(parent_va + 8u, 0);
}

/* Storage widths from the XDK's PixelJar g_TextureFormat table
 * (windows/directx/dxg/d3d8/se/pixeljar.hpp); the linear-pitch and upload
 * layout math is only as fail-closed as this table is faithful.
 * Contract: tests/d3d_hle_format_bpp_oracle_contract.py. */
static uint32_t d3d_hle_guest_format_bytes_per_pixel(uint32_t format)
{
    switch (format & 0xFFu) {
    case 0x00: /* L8 */
    case 0x01: /* AL8 */
    case 0x0B: /* P8 */
    case 0x13: /* LIN_L8 */
    case 0x19: /* A8 */
    case 0x1B: /* LIN_AL8 */
    case 0x1F: /* LIN_A8 */
        return 1;
    case 0x02: /* A1R5G5B5 */
    case 0x03: /* X1R5G5B5 */
    case 0x04: /* A4R4G4B4 */
    case 0x05: /* R5G6B5 */
    case 0x10: /* LIN_A1R5G5B5 */
    case 0x11: /* LIN_R5G6B5 */
    case 0x16: /* LIN_R8B8 */
    case 0x17: /* LIN_G8B8 / LIN_V8U8 */
    case 0x1A: /* A8L8 */
    case 0x1C: /* LIN_X1R5G5B5 */
    case 0x1D: /* LIN_A4R4G4B4 */
    case 0x20: /* LIN_A8L8 */
    case 0x24: /* UYVY */
    case 0x25: /* YUY2 */
    case 0x27: /* R6G5B5 / L6V5U5 */
    case 0x28: /* G8B8 / V8U8 */
    case 0x29: /* R8B8 */
    case 0x2C: /* D16_LOCKABLE / D16 */
    case 0x2D: /* F16 */
    case 0x30: /* LIN_D16 */
    case 0x31: /* LIN_F16 */
    case 0x32: /* L16 */
    case 0x35: /* LIN_L16 */
    case 0x37: /* LIN_R6G5B5 / LIN_L6V5U5 */
    case 0x38: /* R5G5B5A1 */
    case 0x39: /* R4G4B4A4 */
    case 0x3D: /* LIN_R5G5B5A1 */
    case 0x3E: /* LIN_R4G4B4A4 */
        return 2;
    default:
        return 4;
    }
}

static int d3d_hle_guest_format_is_compressed(uint32_t format)
{
    format &= 0xFFu;
    return format == 0x0Cu || format == 0x0Eu || format == 0x0Fu;
}

static uint32_t d3d_hle_guest_level_bytes(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t format,
    uint32_t *pitch_out)
{
    uint64_t pitch;
    uint64_t rows;
    uint64_t bytes;
    if (d3d_hle_guest_format_is_compressed(format)) {
        uint32_t block_bytes = (format & 0xFFu) == 0x0Cu ? 8u : 16u;
        pitch = ((width + 3u) / 4u) * block_bytes;
        rows = (height + 3u) / 4u;
    } else {
        pitch = (uint64_t)width *
                d3d_hle_guest_format_bytes_per_pixel(format);
        rows = height;
    }
    bytes = pitch * rows * depth;
    if (!bytes || pitch > UINT32_MAX || bytes > UINT32_MAX)
        d3d_hle_guest_fatal("texture extent overflow", E_INVALIDARG);
    if (pitch_out)
        *pitch_out = (uint32_t)pitch;
    return (uint32_t)bytes;
}

static uint32_t d3d_hle_guest_size_pitch(uint32_t size)
{
    return size ? ((((size >> 24) & 0xFFu) + 1u) << 6) : 0u;
}

/* PixelContainer Size stores a pitch for level zero only. Lower mip levels
 * remain contiguous after that padded level, matching Plume's upload layout. */
static uint32_t d3d_hle_guest_level_storage_bytes(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t format,
    uint32_t row_pitch, uint32_t *pitch_out)
{
    uint32_t tight_pitch;
    uint32_t tight_bytes = d3d_hle_guest_level_bytes(
        width, height, depth, format, &tight_pitch);
    uint64_t bytes;
    if (!row_pitch) {
        if (pitch_out)
            *pitch_out = tight_pitch;
        return tight_bytes;
    }
    if (row_pitch < tight_pitch) {
        fprintf(stderr,
                "[D3D-HLE] rejected linear storage: %ux%ux%u fmt=%02X "
                "row-pitch=%u tight-pitch=%u tight-bytes=%u\n",
                width, height, depth, format, row_pitch, tight_pitch,
                tight_bytes);
        d3d_hle_guest_fatal("linear texture pitch", E_INVALIDARG);
    }
    bytes = (uint64_t)row_pitch * (tight_bytes / tight_pitch);
    if (!bytes || bytes > UINT32_MAX)
        d3d_hle_guest_fatal("linear texture extent", E_INVALIDARG);
    if (pitch_out)
        *pitch_out = row_pitch;
    return (uint32_t)bytes;
}

static uint32_t d3d_hle_guest_level_offset(
    const D3DHleGuestResource *resource, uint32_t level,
    uint32_t *width_out, uint32_t *height_out, uint32_t *pitch_out)
{
    uint32_t width = resource->width;
    uint32_t height = resource->height;
    uint32_t depth = resource->depth;
    uint32_t offset = 0;
    uint32_t i;
    const uint32_t linear_pitch =
        d3d_hle_guest_size_pitch(resource->size);
    if (level >= resource->levels)
        d3d_hle_guest_fatal("texture mip level", E_INVALIDARG);
    for (i = 0; i < level; ++i) {
        uint32_t level_bytes = d3d_hle_guest_level_storage_bytes(
            width, height, depth, resource->format,
            i == 0u ? linear_pitch : 0u, NULL);
        if (offset > UINT32_MAX - level_bytes)
            d3d_hle_guest_fatal("texture mip offset", E_INVALIDARG);
        offset += level_bytes;
        if (width > 1)
            width >>= 1;
        if (height > 1)
            height >>= 1;
        if (depth > 1)
            depth >>= 1;
    }
    if (width_out)
        *width_out = width;
    if (height_out)
        *height_out = height;
    (void)d3d_hle_guest_level_storage_bytes(
        width, height, depth, resource->format,
        level == 0u ? linear_pitch : 0u, pitch_out);
    return offset;
}

static uint32_t d3d_hle_guest_log2(uint32_t value)
{
    uint32_t result = 0;
    while (value > 1u) {
        value >>= 1;
        ++result;
    }
    return result;
}

static uint32_t d3d_hle_guest_full_mip_count(
    uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t largest = width;
    uint32_t levels = 1;
    if (height > largest)
        largest = height;
    if (depth > largest)
        largest = depth;
    while (largest > 1u && levels < 15u) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

uint32_t d3d_hle_guest_create_texture2(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t levels,
    uint32_t usage, uint32_t format, uint32_t resource_type)
{
    uint32_t object_va;
    uint32_t data_va;
    uint32_t format_word;
    uint32_t size_word = 0;
    uint32_t bytes = 0;
    uint32_t level_width;
    uint32_t level_height;
    uint32_t level_depth;
    uint32_t level_pitch;
    uint32_t linear_pitch = 0;
    uint32_t i;
    int power_of_two;
    D3DHleGuestResource *resource;

    if (!width || !height || !depth ||
        (resource_type != XRECOMP_XBOX_D3DRTYPE_TEXTURE &&
         resource_type != XRECOMP_XBOX_D3DRTYPE_VOLUMETEXTURE &&
         resource_type != XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE))
        return 0;
    if (!levels)
        levels = d3d_hle_guest_full_mip_count(width, height, depth);
    if (levels > 15u)
        return 0;

    power_of_two = (width & (width - 1u)) == 0 &&
                   (height & (height - 1u)) == 0 &&
                   (depth & (depth - 1u)) == 0;
    if (!power_of_two) {
        (void)d3d_hle_guest_level_bytes(
            width, height, depth, format, &level_pitch);
        if (level_pitch > UINT32_MAX - 63u)
            return 0;
        linear_pitch = (level_pitch + 63u) & ~63u;
        size_word = ((width - 1u) & 0xFFFu) |
                    (((height - 1u) & 0xFFFu) << 12) |
                    ((((linear_pitch >> 6) - 1u) & 0xFFu) << 24);
    }

    level_width = width;
    level_height = height;
    level_depth = depth;
    for (i = 0; i < levels; ++i) {
        uint32_t level_bytes = d3d_hle_guest_level_storage_bytes(
            level_width, level_height, level_depth, format,
            i == 0u ? linear_pitch : 0u, &level_pitch);
        if (bytes > UINT32_MAX - level_bytes)
            return 0;
        bytes += level_bytes;
        if (level_width > 1)
            level_width >>= 1;
        if (level_height > 1)
            level_height >>= 1;
        if (level_depth > 1)
            level_depth >>= 1;
    }
    if (resource_type == XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE) {
        bytes = (bytes + 127u) & ~127u;
        if (bytes > UINT32_MAX / 6u)
            return 0;
        bytes *= 6u;
    }

    object_va = d3d_hle_guest_alloc(20u, 16u, 0x07FFFFFFu);
    data_va = d3d_hle_guest_alloc(bytes, 128u, 0x03FFFFFFu);
    format_word = ((format & 0xFFu) << 8) | ((levels & 0xFu) << 16) |
                  ((resource_type ==
                    XRECOMP_XBOX_D3DRTYPE_VOLUMETEXTURE ? 3u : 2u) << 4);
    if (resource_type == XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE)
        format_word |= 4u;
    if (power_of_two) {
        format_word |= d3d_hle_guest_log2(width) << 20;
        format_word |= d3d_hle_guest_log2(height) << 24;
        format_word |= d3d_hle_guest_log2(depth) << 28;
    }
    if (usage & 0x10000u)
        format_word &= ~8u;

    d3d_hle_guest_write_u32(object_va, 0x01040001u);
    d3d_hle_guest_write_u32(object_va + 4u, data_va & 0x0FFFFFFFu);
    d3d_hle_guest_write_u32(object_va + 8u, 0);
    d3d_hle_guest_write_u32(object_va + 12u, format_word);
    d3d_hle_guest_write_u32(object_va + 16u, size_word);

    resource = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_TEXTURE, object_va);
    resource->data_va = d3d_hle_guest_data_offset(data_va);
    resource->data_bytes = bytes;
    resource->width = width;
    resource->height = height;
    resource->depth = depth;
    resource->levels = levels;
    resource->format = format & 0xFFu;
    resource->size = size_word;
    resource->owned_object = 1;
    resource->owned_data = 1;
    resource->version = ++g_hle_texture_version;
    return object_va;
}

static uint32_t d3d_hle_guest_get_surface_level(
    uint32_t texture_va, uint32_t face, uint32_t level)
{
    D3DHleGuestResource *texture =
        d3d_hle_guest_adopt_resource(texture_va);
    D3DHleGuestResource *surface;
    uint32_t object_va;
    uint32_t offset;
    uint32_t face_stride = 0;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t size_word = 0;
    uint32_t common;
    if (!texture || texture->kind != D3D_HLE_RESOURCE_TEXTURE)
        d3d_hle_guest_fatal("GetSurfaceLevel on non-HLE texture",
                            E_INVALIDARG);
    if (d3d_hle_guest_resource_type(texture_va) ==
        XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE) {
        uint32_t i;
        uint32_t width0 = texture->width;
        uint32_t height0 = texture->height;
        uint32_t depth0 = texture->depth;
        if (face >= 6u)
            d3d_hle_guest_fatal("cube face", E_INVALIDARG);
        for (i = 0; i < texture->levels; ++i) {
            face_stride += d3d_hle_guest_level_bytes(
                width0, height0, depth0, texture->format, NULL);
            if (width0 > 1u) width0 >>= 1;
            if (height0 > 1u) height0 >>= 1;
            if (depth0 > 1u) depth0 >>= 1;
        }
        face_stride = (face_stride + 127u) & ~127u;
    } else if (face) {
        d3d_hle_guest_fatal("face on non-cube texture", E_INVALIDARG);
    }
    offset = d3d_hle_guest_level_offset(
        texture, level, &width, &height, &pitch);
    offset += face * face_stride;
    object_va = d3d_hle_guest_alloc(24u, 16u, 0x07FFFFFFu);
    if (texture->size)
        size_word = ((width - 1u) & 0xFFFu) |
                    (((height - 1u) & 0xFFFu) << 12) |
                    (((((pitch + 63u) & ~63u) >> 6) - 1u) << 24);
    d3d_hle_guest_write_u32(object_va, 0x01050001u);
    d3d_hle_guest_write_u32(
        object_va + 4u, (texture->data_va + offset) & 0x0FFFFFFFu);
    d3d_hle_guest_write_u32(object_va + 8u, 0);
    d3d_hle_guest_write_u32(
        object_va + 12u, d3d_hle_guest_read_u32(texture_va + 12u));
    d3d_hle_guest_write_u32(object_va + 16u, size_word);
    d3d_hle_guest_write_u32(object_va + 20u, texture_va);
    common = d3d_hle_guest_read_u32(texture_va);
    d3d_hle_guest_write_u32(texture_va, common + 1u);

    surface = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_SURFACE, object_va);
    surface->data_va = texture->data_va + offset;
    surface->parent_va = texture_va;
    surface->width = width;
    surface->height = height;
    surface->depth = 1;
    surface->levels = 1;
    surface->level = level;
    surface->face = face;
    surface->format = texture->format;
    surface->size = size_word;
    surface->owned_object = 1;
    surface->version = texture->version;
    return object_va;
}

uint32_t d3d_hle_guest_texture_get_surface_level2(
    uint32_t texture_va, uint32_t level)
{
    return d3d_hle_guest_get_surface_level(texture_va, 0, level);
}

uint32_t d3d_hle_guest_cube_get_surface(
    uint32_t texture_va, uint32_t face, uint32_t level)
{
    return d3d_hle_guest_get_surface_level(texture_va, face, level);
}

static void d3d_hle_guest_destroy_resource(
    D3DHleGuestResource *resource)
{
    uint32_t object_va;
    uint32_t parent_va;
    if (!resource)
        return;
    object_va = resource->object_va;
    parent_va = resource->parent_va;
    if (resource->host_object &&
        resource->kind == D3D_HLE_RESOURCE_SURFACE) {
        IDirect3DSurface8 *surface =
            (IDirect3DSurface8 *)resource->host_object;
        surface->lpVtbl->Release(surface);
    }
    if (resource->host_object &&
        resource->kind == D3D_HLE_RESOURCE_PIXEL_SHADER)
        free(resource->host_object);
    if (resource->owned_data && resource->data_va)
        xbox_HeapFree(resource->data_va);
    if (resource->owned_object)
        xbox_HeapFree(object_va);
    memset(resource, 0, sizeof(*resource));
    if (parent_va)
        (void)d3d_hle_guest_release_internal(parent_va);
}

static void d3d_hle_guest_destroy_resource_for_reset(
    D3DHleGuestResource *resource)
{
    if (!resource)
        return;
    if (resource->kind == D3D_HLE_RESOURCE_VERTEX_SHADER &&
        resource->host_handle)
        d3d8_vsh_delete_shader(resource->host_handle);
    if (resource->host_object &&
        resource->kind == D3D_HLE_RESOURCE_SURFACE) {
        IDirect3DSurface8 *surface =
            (IDirect3DSurface8 *)resource->host_object;
        surface->lpVtbl->Release(surface);
    }
    if (resource->host_object &&
        resource->kind == D3D_HLE_RESOURCE_PIXEL_SHADER)
        free(resource->host_object);
    if (resource->owned_data && resource->data_va)
        xbox_HeapFree(resource->data_va);
    if (resource->owned_object)
        xbox_HeapFree(resource->object_va);
    memset(resource, 0, sizeof(*resource));
}

static uint32_t d3d_hle_guest_release_internal(uint32_t resource_va)
{
    D3DHleGuestResource *resource =
        d3d_hle_guest_adopt_resource(resource_va);
    uint32_t common = d3d_hle_guest_read_u32(resource_va);
    uint32_t refs = common & 0xFFFFu;
    if (!refs)
        d3d_hle_guest_fatal("Release with zero public references",
                            E_INVALIDARG);
    if (refs > 1u || (common & XBOX_D3DCOMMON_INTREFCOUNT_MASK)) {
        --refs;
        d3d_hle_guest_write_u32(resource_va, (common & ~0xFFFFu) | refs);
        return refs;
    }
    if (!resource)
        d3d_hle_guest_fatal("Release unknown guest resource",
                            E_INVALIDARG);
    d3d_hle_guest_destroy_resource(resource);
    return 0;
}

uint32_t d3d_hle_guest_resource_release(uint32_t resource_va)
{
    /* GetTexture2 returns zero for an unbound stage. Some Xbox titles sweep
     * every stage and release each returned handle without a separate null
     * check, so a null handle carries no reference and is already released. */
    if (!resource_va)
        return 0;
    return d3d_hle_guest_release_internal(resource_va);
}

void d3d_hle_guest_note_native_resource_release(uint32_t resource_va)
{
    D3DHleGuestResource *resource;
    uint32_t common;

    if (!resource_va ||
        (resource_va != g_hle_bindings.render_target &&
         resource_va != g_hle_bindings.depth_stencil))
        return;

    common = d3d_hle_guest_read_u32(resource_va);
    if ((common & XBOX_D3DCOMMON_TYPE_MASK) !=
            XBOX_D3DCOMMON_TYPE_SURFACE ||
        (common & 0xFFFFu) != 1u ||
        !(common & XBOX_D3DCOMMON_INTREFCOUNT_MASK))
        return;

    resource = d3d_hle_guest_find_resource(resource_va);
    if (!resource || resource->kind != D3D_HLE_RESOURCE_SURFACE)
        d3d_hle_guest_fatal(
            "native Release missing bound surface", E_INVALIDARG);

    /*
     * The native Xbox Release body drops a surface's parent immediately
     * when the last public reference is released, before it checks whether
     * an internal device bind keeps that surface alive. The direct-HLE
     * binding record later destroys the zero-public-ref surface when it is
     * unbound. Do not let that deferred destruction release the parent a
     * second time: native Release has already discharged the surface's
     * parent ownership.
     */
    resource->parent_va = 0;
}

void d3d_hle_guest_prepare_surface_cpu_lock(uint32_t surface_va)
{
    D3DHleGuestResource *surface =
        d3d_hle_guest_adopt_resource(surface_va);
    uint32_t pitch;
    if (!surface || surface->kind != D3D_HLE_RESOURCE_SURFACE)
        d3d_hle_guest_fatal("LockRect invalid surface", E_INVALIDARG);
    if (surface->size)
        pitch = (((surface->size >> 24) & 0xFFu) + 1u) << 6;
    else
        (void)d3d_hle_guest_level_bytes(
            surface->width, surface->height, 1, surface->format, &pitch);
    /* Memory render targets have no hosted COM object to download below.
     * Synchronize their content serial before exposing guest memory to the
     * CPU. The backend reclaims in-flight WAIT downloads first and submits
     * recorded work only if this surface remains stale. This also prevents a
     * writable lock from racing GPU work that references the allocation. */
    if (!surface->host_object && surface->data_va &&
        xgpu_plume_surface_known(surface->data_va) &&
        !xgpu_plume_color_surface_sync(surface->data_va))
        d3d_hle_guest_fatal("LockRect memory-surface wait", E_FAIL);
    if (surface->host_object && surface->data_va)
        (void)xgpu_plume_download_color_surface(
            surface->host_handle,
            d3d_hle_guest_data_ptr(surface->data_va),
            surface->width, surface->height, pitch);
}

static void d3d_hle_guest_finish_surface_cpu_lock(
    uint32_t surface_va, uint32_t flags, bool update_guest_lock)
{
    D3DHleGuestResource *surface =
        d3d_hle_guest_adopt_resource(surface_va);
    D3DHleGuestResource *texture;
    uint32_t lock_value;
    if (!surface || surface->kind != D3D_HLE_RESOURCE_SURFACE)
        d3d_hle_guest_fatal("LockRect invalid surface", E_INVALIDARG);
    texture = surface->parent_va ?
        d3d_hle_guest_adopt_resource(surface->parent_va) : surface;
    if (!texture)
        d3d_hle_guest_fatal("LockRect missing resource", E_FAIL);
    if (!(flags & D3DLOCK_READONLY)) {
        ++g_hle_texture_version;
        lock_value = (uint32_t)g_hle_texture_version;
        if (!lock_value)
            lock_value = 1;
        if (update_guest_lock)
            d3d_hle_guest_write_u32(surface_va + 8u, lock_value);
        surface->version = g_hle_texture_version;
        texture->version = g_hle_texture_version;
        if (!surface->host_object && surface->data_va)
            d3d8_PgraphMarkCpuSurfaceLock(surface->data_va, flags);
        if (surface->host_object &&
            surface->object_va == g_hle_back_buffer) {
            if (!surface->cpu_scanout_active)
                surface->cpu_scanout_published = 0;
            surface->cpu_scanout_active = 1;
        }
    }
    D3D_HLE_F2_LOG(
        "hle surface-lock obj=%08X data=%08X host=%08X %ux%u "
        "fmt=%02X flags=%08X",
        surface->object_va, surface->data_va, surface->host_handle,
        surface->width, surface->height, surface->format, flags);
}

void d3d_hle_guest_note_surface_cpu_write(
    uint32_t surface_va, uint32_t flags)
{
    /* The native XDK lock has already updated the guest object's lock word;
     * only advance Plume's host-side content tracking here. */
    d3d_hle_guest_finish_surface_cpu_lock(surface_va, flags, false);
}

void d3d_hle_guest_surface_lock_rect(
    uint32_t surface_va, uint32_t locked_rect_va, uint32_t rect_va,
    uint32_t flags)
{
    D3DHleGuestResource *surface;
    uint32_t pitch;
    uint32_t data_va;
    uint32_t left = 0;
    uint32_t top = 0;
    if (!locked_rect_va)
        d3d_hle_guest_fatal("LockRect invalid output", E_INVALIDARG);
    d3d_hle_guest_prepare_surface_cpu_lock(surface_va);
    surface = d3d_hle_guest_adopt_resource(surface_va);
    if (surface->size)
        pitch = (((surface->size >> 24) & 0xFFu) + 1u) << 6;
    else
        (void)d3d_hle_guest_level_bytes(
            surface->width, surface->height, 1, surface->format, &pitch);
    data_va = surface->data_va;
    if (rect_va) {
        left = d3d_hle_guest_read_u32(rect_va);
        top = d3d_hle_guest_read_u32(rect_va + 4u);
        if (left >= surface->width || top >= surface->height)
            d3d_hle_guest_fatal("LockRect rectangle", E_INVALIDARG);
        data_va += top * pitch +
                   left * d3d_hle_guest_format_bytes_per_pixel(
                       surface->format);
    }
    if (flags & 0x40u) {
        if (data_va < XBOX_PHYS_RAM) {
            data_va |= 0xF0000000u;
        } else {
            /* The hardware tile window only aliases the first 64 MiB of
             * RAM. A synthetic-heap allocation has no F-window alias, so
             * hand back its cached guest VA — the synthetic PDEs make it
             * directly writable and it decodes to the same storage. */
            data_va = d3d_hle_guest_data_address(data_va);
        }
    }
    d3d_hle_guest_write_u32(locked_rect_va, pitch);
    d3d_hle_guest_write_u32(locked_rect_va + 4u, data_va);
    d3d_hle_guest_finish_surface_cpu_lock(surface_va, flags, true);
}

/* Opt-in gate for the on-GPU zeta->Y16 alias conversion; deliberately off
 * by default until its validation gate passes (design doc). */
static int d3d_hle_guest_gpu_zeta_alias_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_GPU_ZETA_ALIAS");
        enabled = value && value[0] && strcmp(value, "0") != 0 &&
                  !d3d_hle_guest_ascii_equal_ignore_case(value, "false");
    }
    return enabled;
}

static int d3d_hle_guest_legacy_buffer_lock_sync_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("XRECOMP_D3D_HLE_BUFFER_LOCK_SYNC");
        enabled = value && value[0] && strcmp(value, "0") != 0 &&
                  !d3d_hle_guest_ascii_equal_ignore_case(value, "false");
    }
    return enabled;
}

uint32_t d3d_hle_guest_vertex_buffer_lock2(
    uint32_t vertex_buffer_va, uint32_t flags)
{
    D3DHleGuestResource *resource;
    uint32_t data_va;
    int synchronized = 0;
    if (!vertex_buffer_va)
        d3d_hle_guest_fatal("VertexBuffer_Lock2 null buffer",
                            E_INVALIDARG);
    resource = d3d_hle_guest_adopt_resource(vertex_buffer_va);
    if (!resource ||
        (resource->kind != D3D_HLE_RESOURCE_VERTEX_BUFFER &&
         resource->kind != D3D_HLE_RESOURCE_INDEX_BUFFER))
        d3d_hle_guest_fatal(
            "VertexBuffer_Lock2 invalid buffer", E_INVALIDARG);
    /* Diagnostic A/B control for the former device-wide synchronization.
     * Production defaults to the resource-local snapshot behavior below. */
    if (d3d_hle_guest_legacy_buffer_lock_sync_enabled()) {
        if (!(flags & 0x10u)) {
            if (!xgpu_plume_wait_for_idle(0))
                d3d_hle_guest_fatal("VertexBuffer_Lock2 flush", E_FAIL);
            synchronized = 1;
        }
        if (!(flags & 0xA0u) && !synchronized &&
            ((d3d_hle_guest_read_u32(vertex_buffer_va) &
              XBOX_D3DCOMMON_INTREFCOUNT_MASK) ||
             d3d_hle_guest_read_u32(vertex_buffer_va + 8u))) {
            if (!xgpu_plume_wait_for_idle(0))
                d3d_hle_guest_fatal("VertexBuffer_Lock2 wait", E_FAIL);
        }
    }
    if (!(flags & 0xA0u)) {
        /*
         * Direct HLE submits these buffers through DrawPrimitiveUP or
         * DrawIndexedPrimitiveUP. Both paths synchronously copy every
         * referenced byte into Plume's frame storage before returning, so no
         * queued host draw can still read the guest allocation here. Retire
         * the resource-local fence without flushing unrelated render work.
         *
         * The packed internal reference count remains binding ownership, not
         * evidence of an outstanding GPU read. NOOVERWRITE/DISCARD-style
         * locks retain the title-managed fence value just as before.
         */
        d3d_hle_guest_write_u32(vertex_buffer_va + 8u, 0);
    }
    data_va = d3d_hle_guest_read_u32(vertex_buffer_va + 4u);
    if (resource->content_serial == 0)
        resource->content_serial = 1;
    resource->content_serial++;
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    xgpu_plume_mesh_cache_invalidate_va(
        resource->data_va ? resource->data_va : data_va);
#endif
    return d3d_hle_guest_data_address(data_va);
}

int d3d_hle_guest_bound_mesh_identity(
    uint32_t *vb_data_va, uint64_t *vb_generation,
    uint32_t *ib_data_va, uint64_t *ib_generation)
{
    D3DHleGuestResource *vertex =
        d3d_hle_guest_find_resource(g_hle_bindings.stream_resource[0]);
    D3DHleGuestResource *index =
        d3d_hle_guest_find_resource(g_hle_bindings.index_resource);

    if (!vb_data_va || !vb_generation || !ib_data_va || !ib_generation)
        return 0;
    if (!vertex || !index || !vertex->data_va || !index->data_va)
        return 0;
    if (vertex->kind != D3D_HLE_RESOURCE_VERTEX_BUFFER &&
        vertex->kind != D3D_HLE_RESOURCE_INDEX_BUFFER)
        return 0;
    if (index->kind != D3D_HLE_RESOURCE_INDEX_BUFFER &&
        index->kind != D3D_HLE_RESOURCE_VERTEX_BUFFER)
        return 0;
    *vb_data_va = vertex->data_va;
    *ib_data_va = index->data_va;
    *vb_generation = vertex->content_serial ? vertex->content_serial : 1;
    *ib_generation = index->content_serial ? index->content_serial : 1;
    return 1;
}

void d3d_hle_guest_switch_texture(
    uint32_t method_header, uint32_t data, uint32_t format)
{
    uint32_t method = method_header & 0x1FFCu;
    uint32_t stage;
    D3DHleGuestResource *texture;
    XgpuTextureBinding binding;
    uint32_t pitch;
    uint8_t *snapshot = NULL;
    const void *pixels;
    uint32_t bytes;
    uint32_t face_count;
    uint32_t face_stride;
    uint32_t surface_key;
    uint32_t type;
    uint32_t encoded_pitch;
    uint32_t resolved_color_mips = 0;
    int has_host_color_mips = 0;
    int resolved_surfaces = 0;
    uint32_t resolved_nonzero_bytes = 0;
    int surface_bound = 0;
    int texture_cache_hit = 0;
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_switch_texture_zone, "D3D HLE Switch Texture");
    if (method < NV097_SET_TEXTURE_OFFSET ||
        method > NV097_SET_TEXTURE_OFFSET + 3u * 0x40u ||
        ((method - NV097_SET_TEXTURE_OFFSET) & 0x3Fu))
        d3d_hle_guest_fatal("SwitchTexture method header", E_INVALIDARG);
    stage = (method - NV097_SET_TEXTURE_OFFSET) >> 6;
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_texture_find_zone, "D3D HLE Texture Data Lookup");
    texture = d3d_hle_guest_find_texture_data(data);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_texture_find_zone);
    if (!texture)
        d3d_hle_guest_fatal("SwitchTexture unowned texture", E_NOTIMPL);
    if (format != d3d_hle_guest_read_u32(texture->object_va + 12u))
        d3d_hle_guest_fatal("SwitchTexture format mismatch",
                            E_INVALIDARG);
    encoded_pitch = d3d_hle_guest_size_pitch(texture->size);
    /*
     * A pitch-linear PixelContainer's Size word owns the guest row stride.
     * It can be wider than width * bytes-per-pixel because Xbox encodes pitch
     * in 64-byte units.  Keep that padding when the allocation is aliased
     * (notably Z24S8 rebound as Y16); otherwise every uploaded row advances by
     * the tight width and walks diagonally through the rendered surface.
     */
    (void)d3d_hle_guest_level_storage_bytes(
        texture->width, texture->height, texture->depth,
        texture->format, encoded_pitch, &pitch);
    type = d3d_hle_guest_resource_type(texture->object_va);
    face_count =
        type == XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE ? 6u : 1u;
    d3d8_combiners_set_texture_binding(
        stage, texture->depth > 1u ? 3u : 2u, face_count == 6u,
        texture->format == 0x35u, texture->format);
    surface_key = face_count == 1u
        ? d3d_hle_guest_texture_surface_key(texture) : 0;
    if (surface_key) {
        XRECOMP_CPU_RECORDER_ZONE_BEGIN(
            cpu_hle_texture_surface_bind_zone,
            "D3D HLE Texture Surface Bind");
        surface_bound = d3d8_PgraphBindSurfaceTextureStage(
            stage, surface_key, texture->size != 0u,
            surface_key == texture->data_va
                ? texture->format : UINT32_MAX);
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_texture_surface_bind_zone);
    }
    if (surface_bound) {
        D3D_HLE_F2_LOG(
            "hle texture-surface stage=%u obj=%08X data=%08X raw=%08X "
            "key=%08X %ux%u fmt=%02X word=%08X size=%08X pitch=%u:%u",
            stage, texture->object_va, texture->data_va,
            d3d_hle_guest_read_u32(texture->object_va + 4u), surface_key,
            texture->width, texture->height, texture->format, format,
            texture->size, pitch, encoded_pitch);
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_switch_texture_zone);
        return;
    }
    face_stride = 0;
    {
        uint32_t width = texture->width;
        uint32_t height = texture->height;
        uint32_t depth = texture->depth;
        uint32_t level;
        for (level = 0; level < texture->levels; ++level) {
            uint32_t level_bytes = d3d_hle_guest_level_storage_bytes(
                width, height, depth, texture->format,
                level == 0u ? encoded_pitch : 0u, NULL);
            if (face_stride > UINT32_MAX - level_bytes)
                d3d_hle_guest_fatal("SwitchTexture mip extent",
                                    E_INVALIDARG);
            face_stride += level_bytes;
            if (width > 1u) width >>= 1;
            if (height > 1u) height >>= 1;
            if (depth > 1u) depth >>= 1;
        }
    }
    if (face_count == 6u)
        face_stride = (face_stride + 127u) & ~127u;
    if (face_stride > UINT32_MAX / face_count)
        d3d_hle_guest_fatal("SwitchTexture extent", E_INVALIDARG);
    bytes = face_stride * face_count;
    texture->data_bytes = bytes;
    /*
     * Render-to-texture mip chains may exist only as separate live Plume
     * surfaces; the guest allocation is not necessarily synchronized. Detect
     * those hosted levels before accepting the ordinary texture-cache path.
     */
    if (!texture->size && face_count == 1u && texture->depth == 1u &&
        texture->levels > 1u &&
        d3d_hle_guest_format_bytes_per_pixel(texture->format) == 4u) {
        uint32_t level;
        uint32_t level_offset = 0;
        uint32_t level_width = texture->width;
        uint32_t level_height = texture->height;
        for (level = 0; level < texture->levels; ++level) {
            uint32_t level_bytes = d3d_hle_guest_level_bytes(
                level_width, level_height, 1u, texture->format, NULL);
            if (xgpu_plume_surface_known(texture->data_va + level_offset)) {
                has_host_color_mips = 1;
                break;
            }
            level_offset += level_bytes;
            if (level_width > 1u) level_width >>= 1;
            if (level_height > 1u) level_height >>= 1;
        }
    }
    /*
     * Xbox titles can reinterpret the active Z24S8 allocation as a
     * pitch-linear Y16 texture.  The PGRAPH texture frontend resolves that
     * rendered zeta image back into guest memory before uploading it; direct
     * D3D HLE must preserve the same ownership boundary.  A successful
     * resolve also needs a fresh version so Plume cannot reuse the prior
     * all-zero guest upload.
     */
    if (texture->format == 0x35u) {
        /*
         * GPU-side alias conversion (see
         * docs/technical/plume-gpu-zeta-alias-conversion.md): when the
         * opt-in gate is set and the render owner fully satisfies the
         * stage from an on-GPU depth conversion, skip the CPU resolve,
         * the version bump, and the guest-byte re-upload entirely. Any
         * refusal falls through to the legacy path below unchanged.
         */
        if (d3d_hle_guest_gpu_zeta_alias_enabled() &&
            xgpu_plume_bind_zeta_alias(
                stage, texture->data_va, texture->width,
                texture->height, pitch, texture->size != 0u)) {
            XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_switch_texture_zone);
            return;
        }
        resolved_surfaces =
            d3d8_PgraphDownloadSurfaceRange(texture->data_va, bytes);
        if (resolved_surfaces > 0)
            texture->version = ++g_hle_texture_version;
    }
    pixels = d3d_hle_guest_data_ptr(texture->data_va);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    if (texture->format == 0x35u && xgpu_plume_f2_active()) {
        const uint8_t *resolved = (const uint8_t *)pixels;
        uint32_t i;
        for (i = 0; i < bytes; ++i)
            resolved_nonzero_bytes += resolved[i] != 0;
    }
#endif
    memset(&binding, 0, sizeof(binding));
    binding.stage = stage;
    binding.guest_ptr = texture->object_va;
    binding.pixels = pixels;
    binding.width = texture->width;
    binding.height = texture->height;
    binding.depth = texture->depth;
    binding.levels = texture->levels;
    binding.dimensionality = texture->depth > 1u ? 3u : 2u;
    binding.pitch = pitch;
    binding.bytes = bytes;
    binding.format = texture->format;
    binding.version = texture->version > g_hle_palette_version[stage]
        ? texture->version : g_hle_palette_version[stage];
    binding.cube = face_count == 6u;
    /*
     * A nonzero PixelContainer Size denotes pitch-linear storage. Its texture
     * coordinates are in texels on Xbox, unlike swizzled/compressed textures.
     */
    binding.unnormalized_coords = texture->size != 0u;
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    if (xgpu_plume_f2_active()) {
        D3D_HLE_F2_LOG(
            "hle texture-content stage=%u obj=%08X data=%08X bytes=%u "
            "rawhash=%016llX ver=%llu",
            stage, texture->object_va, texture->data_va, bytes,
            (unsigned long long)d3d_hle_guest_f2_hash_bytes(pixels, bytes),
            (unsigned long long)texture->version);
    }
#endif
    /*
     * Repeated direct-HLE SetTexture calls used to allocate and unswizzle the
     * complete guest mip chain before Plume's cache could reject the duplicate
     * upload. Version changes represent guest writes, so an exact cache hit is
     * safe to bind before touching the pixel data.
     */
    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_texture_cache_zone, "D3D HLE Texture Cache Bind");
    texture_cache_hit =
        !xgpu_plume_f2_active() &&
        texture->format != D3DFMT_P8 &&
        (!has_host_color_mips ||
         texture->uploaded_version == binding.version) &&
        xgpu_plume_bind_texture_if_cached(&binding);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_texture_cache_zone);
    if (texture_cache_hit) {
        XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_switch_texture_zone);
        return;
    }

    XRECOMP_CPU_RECORDER_ZONE_BEGIN(
        cpu_hle_texture_upload_zone, "D3D HLE Texture Upload Miss");
    if (!texture->size &&
        !d3d_hle_guest_format_is_compressed(texture->format) &&
        texture->depth == 1u) {
        uint32_t face;
        snapshot = (uint8_t *)malloc(bytes);
        if (!snapshot)
            d3d_hle_guest_fatal("SwitchTexture snapshot",
                                E_OUTOFMEMORY);
        memset(snapshot, 0, bytes);
        for (face = 0; face < face_count; ++face) {
            uint32_t level;
            uint32_t offset = face * face_stride;
            uint32_t width = texture->width;
            uint32_t height = texture->height;
            for (level = 0; level < texture->levels; ++level) {
                uint32_t level_bytes = d3d_hle_guest_level_bytes(
                    width, height, 1u, texture->format, NULL);
                uint32_t bytes_per_pixel =
                    d3d_hle_guest_format_bytes_per_pixel(texture->format);
                int downloaded = 0;
                if (face_count == 1u && bytes_per_pixel == 4u &&
                    xgpu_plume_surface_known(texture->data_va + offset)) {
                    /* The snapshot consumed by Plume stores linear mip data. */
                    downloaded = xgpu_plume_download_color_surface(
                        texture->data_va + offset, snapshot + offset,
                        width, height, width * bytes_per_pixel);
                    if (downloaded)
                        ++resolved_color_mips;
                }
                if (!downloaded) {
                    xbox_unswizzle_rect(
                        snapshot + offset,
                        (const uint8_t *)pixels + offset,
                        width, height, bytes_per_pixel);
                }
                offset += level_bytes;
                if (width > 1u) width >>= 1;
                if (height > 1u) height >>= 1;
            }
        }
        pixels = snapshot;
    }
    if (texture->format == D3DFMT_P8) {
        D3DHleGuestResource *palette = d3d_hle_guest_find_resource(
            g_hle_bindings.palette_resource[stage]);
        const uint32_t *entries;
        const uint8_t *indices = (const uint8_t *)pixels;
        uint8_t *expanded;
        uint32_t entry_count;
        uint32_t expanded_bytes;
        uint32_t pixel;

        if (!palette || palette->kind != D3D_HLE_RESOURCE_PALETTE)
            d3d_hle_guest_fatal("P8 texture without bound palette",
                                E_INVALIDARG);
        entry_count = xbox_d3d8_palette_entry_count(
            d3d_hle_guest_read_u32(palette->object_va));
        entries = (const uint32_t *)d3d_hle_guest_data_ptr(palette->data_va);
        if (bytes > UINT32_MAX / 4u)
            d3d_hle_guest_fatal("P8 texture expansion overflow",
                                E_INVALIDARG);
        expanded_bytes = bytes * 4u;
        expanded = (uint8_t *)malloc(expanded_bytes);
        if (!expanded)
            d3d_hle_guest_fatal("P8 texture expansion", E_OUTOFMEMORY);
        for (pixel = 0; pixel < bytes; ++pixel) {
            uint32_t color = xbox_d3d8_palette_lookup(
                entries, entry_count, indices[pixel]);
            memcpy(expanded + pixel * 4u, &color, sizeof(color));
        }
        free(snapshot);
        snapshot = expanded;
        pixels = expanded;
        bytes = expanded_bytes;
        pitch *= 4u;
        binding.format = D3DFMT_A8R8G8B8;
        binding.bytes = bytes;
        binding.pitch = pitch;
    }
    binding.pixels = pixels;
    xgpu_plume_set_texture_ex(&binding);
    texture->uploaded_version = binding.version;
    {
        uint32_t head[4] = {0};
        uint32_t head_bytes = bytes < sizeof(head) ? bytes : sizeof(head);
        memcpy(head, d3d_hle_guest_data_ptr(texture->data_va),
               head_bytes);
        D3D_HLE_F2_LOG(
            "hle texture-upload stage=%u obj=%08X data=%08X raw=%08X "
            "%ux%u d=%u lv=%u fmt=%02X word=%08X size=%08X "
            "pitch=%u:%u bytes=%u ver=%llu resolve=%d mips=%u nz=%u "
            "head=%08X,%08X,%08X,%08X",
            stage, texture->object_va, texture->data_va,
            d3d_hle_guest_read_u32(texture->object_va + 4u),
            texture->width, texture->height, texture->depth,
            texture->levels, texture->format, format, texture->size,
            pitch, encoded_pitch, bytes,
            (unsigned long long)binding.version,
            resolved_surfaces, resolved_color_mips,
            resolved_nonzero_bytes,
            head[0], head[1], head[2], head[3]);
    }
    free(snapshot);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_texture_upload_zone);
    XRECOMP_CPU_RECORDER_ZONE_END(cpu_hle_switch_texture_zone);
}

static uint32_t d3d_hle_guest_compare_func(uint32_t value)
{
    if (value >= 0x200u && value <= 0x207u)
        return value - 0x1FFu;
    d3d_hle_guest_fatal("NV097 compare function", E_INVALIDARG);
}

static uint32_t d3d_hle_guest_stencil_op(uint32_t value)
{
    switch (value) {
    case 0x1E00u: return 1;
    case 0x0000u: return 2;
    case 0x1E01u: return 3;
    case 0x1E02u: return 4;
    case 0x1E03u: return 5;
    case 0x150Au: return 6;
    case 0x8507u: return 7;
    case 0x8508u: return 8;
    default:
        d3d_hle_guest_fatal("NV097 stencil operation", E_INVALIDARG);
    }
}

static uint32_t d3d_hle_guest_blend_factor(uint32_t value)
{
    switch (value) {
    case 0x0000u: return D3DBLEND_ZERO;
    case 0x0001u: return D3DBLEND_ONE;
    case 0x0300u: return D3DBLEND_SRCCOLOR;
    case 0x0301u: return D3DBLEND_INVSRCCOLOR;
    case 0x0302u: return D3DBLEND_SRCALPHA;
    case 0x0303u: return D3DBLEND_INVSRCALPHA;
    case 0x0304u: return D3DBLEND_DESTALPHA;
    case 0x0305u: return D3DBLEND_INVDESTALPHA;
    case 0x0306u: return D3DBLEND_DESTCOLOR;
    case 0x0307u: return D3DBLEND_INVDESTCOLOR;
    case 0x0308u: return D3DBLEND_SRCALPHASAT;
    default:
        d3d_hle_guest_fatal("NV097 blend factor", E_INVALIDARG);
    }
}

static uint32_t d3d_hle_guest_blend_op(uint32_t value)
{
    switch (value) {
    case 0x8006u: return 1;
    case 0x800Au: return 2;
    case 0x800Bu: return 3;
    case 0x8007u: return 4;
    case 0x8008u: return 5;
    default:
        d3d_hle_guest_fatal("NV097 blend equation", E_INVALIDARG);
    }
}

static uint32_t d3d_hle_guest_color_mask(uint32_t value)
{
    return ((value >> 16) & 1u) |
           (((value >> 8) & 1u) << 1) |
           ((value & 1u) << 2) |
           (((value >> 24) & 1u) << 3);
}

static void d3d_hle_guest_set_float_state(uint32_t state, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)state, bits);
}

void d3d_hle_guest_set_z_enable(uint32_t value)
{
    d3d8_combiners_set_z_perspective(value == 2u);
    /*
     * Xbox D3DRS_ZENABLE is a D3DZBUFFERTYPE, not a boolean:
     * 0 disables depth, 1 selects Z buffering, and 2 selects W buffering.
     * Preserve the raw mode so deferred PS=0 draws can select the same
     * explicit W-depth output as generated combiner shaders.
     */
    d3d_hle_guest_set_render_state(D3DRS_ZENABLE, value);
}

void d3d_hle_guest_set_stencil_fail(uint32_t value)
{
    d3d_hle_guest_set_render_state(
        D3DRS_STENCILFAIL, d3d_hle_guest_stencil_op(value));
}

void d3d_hle_guest_set_z_bias(uint32_t value)
{
    g_hle_slope_bias = -0.25f * (float)(int32_t)value;
    g_hle_depth_bias = -(float)(int32_t)value;
    d3d_hle_guest_set_float_state(
        XRECOMP_D3DRS_DEPTH_BIAS_SLOPE, g_hle_slope_bias);
    d3d_hle_guest_set_float_state(
        XRECOMP_D3DRS_DEPTH_BIAS_CONSTANT, g_hle_depth_bias);
}

void d3d_hle_guest_set_multisample_antialias(uint32_t value)
{
    d3d_hle_guest_set_render_state(
        D3DRS_MULTISAMPLEANTIALIAS, value != 0);
}

void d3d_hle_guest_set_texcoord_index(uint32_t stage, uint32_t value)
{
    if (stage >= 4u)
        d3d_hle_guest_fatal("TexCoordIndex stage", E_INVALIDARG);
    d3d_hle_guest_set_texture_stage_state(
        stage, D3DTSS_TEXCOORDINDEX, value);
}

void d3d_hle_guest_set_bump_env(
    uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage >= XBOX_D3D_MAX_TEXTURES ||
        xbox_d3d8_bump_env_component(type) < 0 ||
        !d3d8_combiners_set_bump_env(stage, type, value)) {
        d3d_hle_guest_fatal("SetTextureState_BumpEnv", E_INVALIDARG);
    }
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    if (xrecomp_d3d_hle_deferred_texture_state_va) {
        d3d_hle_guest_write_u32(
            xrecomp_d3d_hle_deferred_texture_state_va +
                (stage * 32u + type) * sizeof(uint32_t),
            value);
    }
#endif
}

void d3d_hle_guest_set_color_key(uint32_t stage, uint32_t value)
{
    if (stage >= XBOX_D3D_MAX_TEXTURES ||
        !d3d8_combiners_set_color_key(stage, value)) {
        d3d_hle_guest_fatal("SetTextureState_ColorKeyColor", E_INVALIDARG);
    }
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    if (xrecomp_d3d_hle_deferred_texture_state_va) {
        d3d_hle_guest_write_u32(
            xrecomp_d3d_hle_deferred_texture_state_va +
                (stage * 32u + XBOX_D3DTSS_COLORKEYCOLOR) * sizeof(uint32_t),
            value);
    }
#endif
}

void d3d_hle_guest_set_xbox_texture_stage_state(
    uint32_t stage, uint32_t type, uint32_t value)
{
    D3DTEXTURESTAGESTATETYPE host_state;

    switch (type) {
    case XBOX_D3DTSS_ADDRESSU: host_state = D3DTSS_ADDRESSU; break;
    case XBOX_D3DTSS_ADDRESSV: host_state = D3DTSS_ADDRESSV; break;
    case XBOX_D3DTSS_MAGFILTER: host_state = D3DTSS_MAGFILTER; break;
    case XBOX_D3DTSS_MINFILTER: host_state = D3DTSS_MINFILTER; break;
    case XBOX_D3DTSS_MIPFILTER: host_state = D3DTSS_MIPFILTER; break;
    case XBOX_D3DTSS_MIPMAPLODBIAS: host_state = D3DTSS_MIPMAPLODBIAS; break;
    case XBOX_D3DTSS_MAXMIPLEVEL: host_state = D3DTSS_MAXMIPLEVEL; break;
    case XBOX_D3DTSS_MAXANISOTROPY: host_state = D3DTSS_MAXANISOTROPY; break;
    case XBOX_D3DTSS_COLORKEYOP: host_state = D3DTSS_COLORKEYOP; break;
    case XBOX_D3DTSS_COLOROP: host_state = D3DTSS_COLOROP; break;
    case XBOX_D3DTSS_COLORARG0: host_state = D3DTSS_COLORARG0; break;
    case XBOX_D3DTSS_COLORARG1: host_state = D3DTSS_COLORARG1; break;
    case XBOX_D3DTSS_COLORARG2: host_state = D3DTSS_COLORARG2; break;
    case XBOX_D3DTSS_ALPHAOP: host_state = D3DTSS_ALPHAOP; break;
    case XBOX_D3DTSS_ALPHAARG0: host_state = D3DTSS_ALPHAARG0; break;
    case XBOX_D3DTSS_ALPHAARG1: host_state = D3DTSS_ALPHAARG1; break;
    case XBOX_D3DTSS_ALPHAARG2: host_state = D3DTSS_ALPHAARG2; break;
    case XBOX_D3DTSS_RESULTARG: host_state = D3DTSS_RESULTARG; break;
    case XBOX_D3DTSS_TEXCOORDINDEX: host_state = D3DTSS_TEXCOORDINDEX; break;
    case XBOX_D3DTSS_BORDERCOLOR: host_state = D3DTSS_BORDERCOLOR; break;
    case XBOX_D3DTSS_BUMPENVMAT00:
    case XBOX_D3DTSS_BUMPENVMAT01:
    case XBOX_D3DTSS_BUMPENVMAT11:
    case XBOX_D3DTSS_BUMPENVMAT10:
    case XBOX_D3DTSS_BUMPENVLSCALE:
    case XBOX_D3DTSS_BUMPENVLOFFSET:
        d3d_hle_guest_set_bump_env(stage, type, value);
        return;
    case XBOX_D3DTSS_COLORKEYCOLOR:
        d3d_hle_guest_set_color_key(stage, value);
        return;
    default:
        d3d_hle_guest_fatal(
            "SetTextureStageState unsupported Xbox state", E_NOTIMPL);
    }
    d3d_hle_guest_set_texture_stage_state(stage, host_state, value);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    if (xrecomp_d3d_hle_deferred_texture_state_va) {
        d3d_hle_guest_write_u32(
            xrecomp_d3d_hle_deferred_texture_state_va +
                (stage * 32u + type) * sizeof(uint32_t),
            value);
    }
#endif
}

static const XboxD3DTile *d3d_hle_guest_find_tile(uint32_t data_va)
{
    unsigned i;

    for (i = 0; i < XBOX_D3D_MAX_TILES; ++i) {
        if (xbox_d3d8_tile_contains(&g_hle_tiles[i], data_va))
            return &g_hle_tiles[i];
    }
    return NULL;
}

void d3d_hle_guest_set_tile(uint32_t index, uint32_t tile_va)
{
    XboxD3DTile tile = { 0 };

    if (index >= XBOX_D3D_MAX_TILES)
        d3d_hle_guest_fatal("SetTile index", E_INVALIDARG);
    if (tile_va && (!g_hle_read_range ||
                    !g_hle_read_range(tile_va, &tile, sizeof(tile)))) {
        d3d_hle_guest_fatal("SetTile descriptor", E_INVALIDARG);
    }
    if (tile.memory && (!tile.size || !tile.pitch))
        d3d_hle_guest_fatal("SetTile extent", E_INVALIDARG);
    g_hle_tiles[index] = tile;
}

void d3d_hle_guest_get_tile(uint32_t index, uint32_t tile_va)
{
    if (index >= XBOX_D3D_MAX_TILES || !tile_va)
        d3d_hle_guest_fatal("GetTile", E_INVALIDARG);
    memcpy(xbox_guest_ptr(tile_va), &g_hle_tiles[index],
           sizeof(g_hle_tiles[index]));
}

void d3d_hle_guest_set_simple(uint32_t method_header, uint32_t value)
{
    uint32_t method = method_header & 0x1FFCu;
    switch (method) {
    case NV097_SET_ALPHA_TEST_ENABLE:
        d3d_hle_guest_set_render_state(D3DRS_ALPHATESTENABLE, value);
        break;
    case NV097_SET_BLEND_ENABLE:
        d3d_hle_guest_set_render_state(D3DRS_ALPHABLENDENABLE, value);
        break;
    case NV097_SET_DEPTH_TEST_ENABLE:
        d3d_hle_guest_set_render_state(D3DRS_ZENABLE, value);
        break;
    case NV097_SET_DITHER_ENABLE:
        d3d_hle_guest_set_render_state(D3DRS_DITHERENABLE, value);
        break;
    case NV097_SET_LIGHTING_ENABLE:
        d3d_hle_guest_set_render_state(D3DRS_LIGHTING, value);
        break;
    case NV097_SET_STENCIL_TEST_ENABLE:
        d3d_hle_guest_set_render_state(D3DRS_STENCILENABLE, value);
        break;
    case NV097_SET_ALPHA_FUNC:
        d3d_hle_guest_set_render_state(
            D3DRS_ALPHAFUNC, d3d_hle_guest_compare_func(value));
        break;
    case NV097_SET_ALPHA_REF:
        d3d_hle_guest_set_render_state(D3DRS_ALPHAREF, value);
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        d3d_hle_guest_set_render_state(
            D3DRS_SRCBLEND, d3d_hle_guest_blend_factor(value));
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        d3d_hle_guest_set_render_state(
            D3DRS_DESTBLEND, d3d_hle_guest_blend_factor(value));
        break;
    case NV097_SET_BLEND_EQUATION:
        d3d_hle_guest_set_render_state(
            D3DRS_BLENDOP, d3d_hle_guest_blend_op(value));
        break;
    case NV097_SET_DEPTH_FUNC:
        d3d_hle_guest_set_render_state(
            D3DRS_ZFUNC, d3d_hle_guest_compare_func(value));
        break;
    case NV097_SET_COLOR_MASK:
        d3d_hle_guest_set_render_state(
            D3DRS_COLORWRITEENABLE, d3d_hle_guest_color_mask(value));
        break;
    case NV097_SET_DEPTH_MASK:
        d3d_hle_guest_set_render_state(D3DRS_ZWRITEENABLE, value);
        break;
    case NV097_SET_STENCIL_MASK:
        d3d_hle_guest_set_render_state(D3DRS_STENCILWRITEMASK, value);
        break;
    case NV097_SET_STENCIL_FUNC:
        d3d_hle_guest_set_render_state(
            D3DRS_STENCILFUNC, d3d_hle_guest_compare_func(value));
        break;
    case NV097_SET_STENCIL_FUNC_REF:
        d3d_hle_guest_set_render_state(D3DRS_STENCILREF, value);
        break;
    case NV097_SET_STENCIL_FUNC_MASK:
        d3d_hle_guest_set_render_state(D3DRS_STENCILMASK, value);
        break;
    case NV097_SET_STENCIL_OP_FAIL:
        d3d_hle_guest_set_render_state(
            D3DRS_STENCILFAIL, d3d_hle_guest_stencil_op(value));
        break;
    case NV097_SET_STENCIL_OP_ZFAIL:
        d3d_hle_guest_set_render_state(
            D3DRS_STENCILZFAIL, d3d_hle_guest_stencil_op(value));
        break;
    case NV097_SET_STENCIL_OP_ZPASS:
        d3d_hle_guest_set_render_state(
            D3DRS_STENCILPASS, d3d_hle_guest_stencil_op(value));
        break;
    case NV097_SET_POLYGON_OFFSET_SCALE_FACTOR:
        memcpy(&g_hle_slope_bias, &value, sizeof(value));
        d3d_hle_guest_set_float_state(
            XRECOMP_D3DRS_DEPTH_BIAS_SLOPE, g_hle_slope_bias);
        break;
    case NV097_SET_POLYGON_OFFSET_BIAS:
        memcpy(&g_hle_depth_bias, &value, sizeof(value));
        d3d_hle_guest_set_float_state(
            XRECOMP_D3DRS_DEPTH_BIAS_CONSTANT, g_hle_depth_bias);
        break;
    default:
        /*
         * The XDK Simple body also carries cached NV-only policy bits which
         * have no host-rasterizer equivalent. Preserve their last value
         * instead of re-entering the pushbuffer or aborting.
         */
        g_hle_state_cache[(method >> 2) %
                          XBOX_D3D_HLE_MAX_STATES] = value;
        break;
    }
}

static HRESULT d3d_hle_guest_adopt_native_vertex_shader_layout(
    uint32_t shader_handle, uint32_t address, uint32_t attribute_offset,
    uint32_t count_offset, uint32_t program_offset)
{
    enum {
        XBOX_PUSH_METHOD_MASK = 0x00001FFCu,
        XBOX_PUSH_COUNT_MASK = 0x1FFC0000u,
        XBOX_PUSH_COUNT_SHIFT = 18,
        XBOX_NV097_SET_TRANSFORM_PROGRAM = 0x00000B00u,
        XBOX_VSH_MAX_PUSH_DWORDS = 4096,
    };
    uint32_t object_va = shader_handle & ~1u;
    uint32_t push_dwords = d3d_hle_guest_read_u32(
        object_va + count_offset);
    DWORD microcode[NV2A_VS_MAX_INSTRUCTIONS * 4];
    uint32_t microcode_dwords = 0;
    uint32_t cursor = 0;
    uint32_t data_va;
    uint32_t data_bytes;
    DWORD host_handle;
    NV2AVshDeclaration declaration;
    D3DHleGuestResource *shader;
    uint64_t content_serial = UINT64_C(1469598103934665603);
    unsigned attribute;
    HRESULT result;

    if (push_dwords > XBOX_VSH_MAX_PUSH_DWORDS ||
        object_va > UINT32_MAX - program_offset - push_dwords * 4u)
        return E_INVALIDARG;

    memset(&declaration, 0, sizeof(declaration));
    memset(declaration.format, 0x02, sizeof(declaration.format));
    for (attribute = 0; attribute < NV2A_VS_MAX_INPUTS; ++attribute) {
        uint32_t slot = object_va + attribute_offset + attribute * 16u;
        uint32_t stream = d3d_hle_guest_read_u32(slot + 0u);
        uint32_t offset = d3d_hle_guest_read_u32(slot + 4u);
        uint32_t format = d3d_hle_guest_read_u32(slot + 8u);
        uint32_t tessellation = d3d_hle_guest_read_u32(slot + 12u);
        uint32_t fields[] = { stream, offset, format, tessellation };
        unsigned field;

        if (stream >= XBOX_D3D_MAX_STREAMS || offset > UINT16_MAX ||
            format > UINT8_MAX)
            return E_INVALIDARG;
        for (field = 0; field < sizeof(fields) / sizeof(fields[0]); ++field) {
            content_serial ^= fields[field];
            content_serial *= UINT64_C(1099511628211);
        }
        declaration.stream[attribute] = (uint8_t)stream;
        declaration.offset[attribute] = (uint16_t)offset;
        declaration.format[attribute] = (uint8_t)format;
        if (format != 0x02u)
            declaration.attributes_present |= (uint16_t)(1u << attribute);
    }
    declaration.valid = 1;

    if (push_dwords) {
        while (cursor < push_dwords) {
            uint32_t command = d3d_hle_guest_read_u32(
                object_va + program_offset + cursor++ * 4u);
            uint32_t method = command & XBOX_PUSH_METHOD_MASK;
            uint32_t count =
                (command & XBOX_PUSH_COUNT_MASK) >> XBOX_PUSH_COUNT_SHIFT;
            uint32_t i;

            if (!count || count > push_dwords - cursor)
                return E_INVALIDARG;
            if (method == XBOX_NV097_SET_TRANSFORM_PROGRAM) {
                if ((count & 3u) ||
                    count > sizeof(microcode) / sizeof(microcode[0]) -
                                microcode_dwords)
                    return E_INVALIDARG;
                for (i = 0; i < count; ++i) {
                    microcode[microcode_dwords++] = d3d_hle_guest_read_u32(
                        object_va + program_offset + (cursor + i) * 4u);
                }
            }
            cursor += count;
        }
        data_va = object_va + program_offset;
        data_bytes = push_dwords * 4u;
    } else {
        const D3DHleGuestLoadedVertexProgram *loaded;

        if (address >= sizeof(g_hle_loaded_vertex_programs) /
                           sizeof(g_hle_loaded_vertex_programs[0]))
            return E_INVALIDARG;
        loaded = &g_hle_loaded_vertex_programs[address];
        if (!loaded->instruction_count)
            return E_INVALIDARG;
        microcode_dwords = loaded->instruction_count * 4u;
        memcpy(microcode, loaded->microcode,
               microcode_dwords * sizeof(microcode[0]));
        content_serial ^= address;
        content_serial *= UINT64_C(1099511628211);
        content_serial ^= loaded->generation;
        content_serial *= UINT64_C(1099511628211);
        shader = d3d_hle_guest_find_resource(object_va);
        if (shader && shader->kind == D3D_HLE_RESOURCE_VERTEX_SHADER &&
            shader->data_va == 0 && shader->load_address == address &&
            shader->content_serial == content_serial)
            return S_OK;
        data_va = 0;
        data_bytes = microcode_dwords * sizeof(microcode[0]);
    }
    if (!microcode_dwords || (microcode_dwords & 3u))
        return E_INVALIDARG;

    result = d3d8_vsh_create_shader(
        microcode, (int)(microcode_dwords / 4u), &declaration,
        &host_handle);
    if (result != S_OK)
        return result;
    shader = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_VERTEX_SHADER, object_va);
    if (shader->kind != D3D_HLE_RESOURCE_VERTEX_SHADER) {
        d3d8_vsh_delete_shader(host_handle);
        return E_INVALIDARG;
    }
    if (shader->host_handle)
        d3d8_vsh_delete_shader(shader->host_handle);
    shader->data_va = data_va;
    shader->data_bytes = data_bytes;
    shader->host_handle = (uint32_t)host_handle;
    shader->load_address = address;
    shader->content_serial = content_serial;
    shader->owned_object = 0;
    fprintf(stderr,
            "[D3D-HLE] adopted native XDK vertex shader %08X "
            "(%u instructions)\n",
            shader_handle, microcode_dwords / 4u);
    return S_OK;
}

static HRESULT d3d_hle_guest_adopt_native_vertex_shader(
    uint32_t shader_handle, uint32_t address)
{
    uint32_t object_va = shader_handle & ~1u;
    D3DHleGuestResource *shader;
    HRESULT result;

    if (!(shader_handle & 1u) || object_va < 0x1000u)
        return E_INVALIDARG;
    shader = d3d_hle_guest_find_resource(object_va);
    if (shader && (shader->kind != D3D_HLE_RESOURCE_VERTEX_SHADER ||
                   shader->data_va != 0))
        return shader->kind == D3D_HLE_RESOURCE_VERTEX_SHADER
                   ? S_OK : E_INVALIDARG;

    /* XDK 4034 and later: RefCount, Flags, ProgramSize,
     * ProgramAndConstantsDwords, Dimensionality[4], 16 input slots, then
     * the native push stream.  Very early XDKs use a larger pre-4034 header;
     * accept that documented layout only if the modern one does not parse. */
    result = d3d_hle_guest_adopt_native_vertex_shader_layout(
        shader_handle, address, 0x14u, 0x0Cu, 0x114u);
    if (result == S_OK)
        return S_OK;
    return d3d_hle_guest_adopt_native_vertex_shader_layout(
        shader_handle, address, 0x28u, 0x14u, 0x168u);
}

HRESULT d3d_hle_guest_load_vertex_shader_program(
    uint32_t function_va, uint32_t address)
{
    uint32_t header;
    uint32_t instruction_count;
    uint32_t bytes;
    const void *source;
    D3DHleGuestLoadedVertexProgram *loaded;

    if (!function_va ||
        address >= sizeof(g_hle_loaded_vertex_programs) /
                       sizeof(g_hle_loaded_vertex_programs[0]))
        return E_INVALIDARG;
    header = d3d_hle_guest_read_u32(function_va);
    instruction_count = (header >> 16) & 0x1FFu;
    if (!instruction_count ||
        instruction_count > NV2A_VS_MAX_INSTRUCTIONS - address)
        return E_INVALIDARG;
    bytes = instruction_count * 16u;
    if (function_va > UINT32_MAX - 4u - bytes)
        return E_INVALIDARG;
    source = d3d_hle_guest_snapshot_range(0, function_va + 4u, bytes);
    loaded = &g_hle_loaded_vertex_programs[address];
    memcpy(loaded->microcode, source, bytes);
    loaded->instruction_count = instruction_count;
    loaded->generation = ++g_hle_vertex_program_generation;
    return S_OK;
}

void d3d_hle_guest_load_vertex_shader(
    uint32_t shader_handle, uint32_t address)
{
    uint32_t object_va = shader_handle - 1u;
    D3DHleGuestResource *shader =
        d3d_hle_guest_find_resource(object_va);
    if (!shader &&
        d3d_hle_guest_adopt_native_vertex_shader(
            shader_handle, address) == S_OK)
        shader = d3d_hle_guest_find_resource(object_va);
    else if (shader && shader->data_va == 0 &&
             d3d_hle_guest_adopt_native_vertex_shader(
                 shader_handle, address) == S_OK)
        shader = d3d_hle_guest_find_resource(object_va);
    if (!shader || shader->kind != D3D_HLE_RESOURCE_VERTEX_SHADER)
        d3d_hle_guest_fatal("LoadVertexShader handle", E_INVALIDARG);
    shader->load_address = address;
}

HRESULT d3d_hle_guest_create_vertex_shader(
    uint32_t declaration_va, uint32_t program_va, uint32_t output_va,
    uint32_t usage)
{
    uint32_t program_header;
    uint32_t instruction_count;
    uint32_t object_va;
    DWORD host_handle;
    uint32_t bytes;
    HRESULT result;
    NV2AVshDeclaration declaration;
    const NV2AVshDeclaration *declaration_ptr = NULL;
    D3DHleGuestResource *shader;
    if (!program_va || !output_va)
        return E_INVALIDARG;
    if (declaration_va) {
        if (!d3d8_vsh_parse_declaration(
                (const DWORD *)xbox_guest_ptr(declaration_va),
                NV2A_VS_MAX_DECLARATION_DWORDS, &declaration))
            return E_INVALIDARG;
        declaration_ptr = &declaration;
    }
    program_header = d3d_hle_guest_read_u32(program_va);
    instruction_count = program_header >> 16;
    if (!instruction_count || instruction_count > NV2A_VS_MAX_INSTRUCTIONS)
        return E_INVALIDARG;
    bytes = 0x114u + instruction_count * 16u;
    object_va = d3d_hle_guest_alloc(bytes, 16u, 0x07FFFFFFu);
    d3d_hle_guest_write_u32(object_va, 1u);
    d3d_hle_guest_write_u32(object_va + 4u, declaration_va);
    d3d_hle_guest_write_u32(object_va + 8u, usage);
    d3d_hle_guest_write_u32(object_va + 12u, instruction_count);
    memcpy(xbox_guest_ptr(object_va + 0x114u),
           xbox_guest_ptr(program_va + 4u), instruction_count * 16u);
    result = d3d8_vsh_create_shader(
        (const DWORD *)xbox_guest_ptr(program_va + 4u),
        (int)instruction_count, declaration_ptr, &host_handle);
    if (result != S_OK) {
        xbox_HeapFree(object_va);
        return result;
    }
    shader = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_VERTEX_SHADER, object_va);
    shader->data_va = object_va + 0x114u;
    shader->data_bytes = instruction_count * 16u;
    shader->host_handle = (uint32_t)host_handle;
    shader->owned_object = 1;
    d3d_hle_guest_write_u32(output_va, object_va | 1u);
    return S_OK;
}

HRESULT d3d_hle_guest_create_pixel_shader(
    uint32_t definition_va, uint32_t output_va)
{
    uint32_t object_va;
    D3DHleGuestResource *shader;
    if (!definition_va || !output_va)
        return E_INVALIDARG;
    object_va = d3d_hle_guest_alloc(252u, 16u, 0x07FFFFFFu);
    d3d_hle_guest_write_u32(object_va, 1u);
    d3d_hle_guest_write_u32(object_va + 4u, 1u);
    d3d_hle_guest_write_u32(object_va + 8u, object_va + 12u);
    memcpy(xbox_guest_ptr(object_va + 12u),
           xbox_guest_ptr(definition_va), 0xF0u);
    shader = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_PIXEL_SHADER, object_va);
    shader->data_va = object_va + 12u;
    shader->data_bytes = 0xF0u;
    shader->owned_object = 1;
    d3d_hle_guest_write_u32(output_va, object_va);
    return S_OK;
}

static uint32_t d3d_hle_guest_surface_pitch(
    const D3DHleGuestResource *surface)
{
    const XboxD3DTile *tile =
        d3d_hle_guest_find_tile(surface->data_va);
    uint32_t pitch;
    if (tile && tile->pitch)
        return tile->pitch;
    if (surface->size)
        return (((surface->size >> 24) & 0xFFu) + 1u) << 6;
    (void)d3d_hle_guest_level_bytes(
        surface->width, surface->height, 1u, surface->format, &pitch);
    return pitch;
}

static int d3d_hle_guest_host_surface_fingerprint(
    D3DHleGuestResource *surface, D3D8CpuSurfaceFingerprint *fingerprint)
{
    uint32_t pitch;
    if (!surface || !fingerprint || !surface->data_va ||
        !surface->width || !surface->height ||
        d3d_hle_guest_format_bytes_per_pixel(surface->format) != 4u)
        return 0;
    pitch = d3d_hle_guest_surface_pitch(surface);
    if (pitch < surface->width * 4u)
        return 0;
    *fingerprint = d3d8_cpu_surface_fingerprint(
        d3d_hle_guest_data_ptr(surface->data_va),
        surface->width, surface->height, pitch);
    return 1;
}

static HRESULT d3d_hle_guest_upload_host_surface(
    D3DHleGuestResource *surface, int force)
{
    uint32_t source_pitch;
    uint32_t bgra_pitch;
    size_t bgra_bytes;
    uint8_t *bgra;
    BOOL converted;
    int uploaded;
    D3D8CpuSurfaceFingerprint fingerprint;
    int fingerprint_valid;
    if (!surface || !surface->host_object || !surface->host_handle ||
        (!force && surface->version == surface->uploaded_version))
        return S_OK;
    if (!surface->data_va || !surface->width || !surface->height ||
        surface->width > UINT32_MAX / 4u)
        return E_FAIL;
    source_pitch = d3d_hle_guest_surface_pitch(surface);
    bgra_pitch = surface->width * 4u;
    bgra_bytes = (size_t)bgra_pitch * surface->height;
    if (surface->height &&
        bgra_bytes / surface->height != bgra_pitch)
        return E_FAIL;
    bgra = (uint8_t *)malloc(bgra_bytes);
    if (!bgra)
        return E_OUTOFMEMORY;
    converted = d3d8_surface_to_bgra(
        (D3DFORMAT)surface->format,
        d3d_hle_guest_data_ptr(surface->data_va), source_pitch,
        bgra, bgra_pitch, surface->width, surface->height);
    uploaded = converted && xgpu_plume_upload_color_surface(
        surface->host_handle, bgra, surface->width, surface->height,
        bgra_pitch);
    free(bgra);
    if (!uploaded)
        return E_FAIL;
    surface->uploaded_version = surface->version;
    fingerprint_valid =
        d3d_hle_guest_host_surface_fingerprint(surface, &fingerprint);
    if (fingerprint_valid) {
        surface->cpu_scanout_hash = fingerprint.hash;
        surface->cpu_scanout_hash_valid = 1;
    }
    D3D_HLE_F2_LOG(
        "hle cpu-surface upload obj=%08X host=%08X %ux%u fmt=%02X v=%llu",
        surface->object_va, surface->host_handle, surface->width,
        surface->height, surface->format,
        (unsigned long long)surface->version);
    return S_OK;
}

static HRESULT d3d_hle_guest_upload_dirty_host_surface(
    D3DHleGuestResource *surface)
{
    return d3d_hle_guest_upload_host_surface(surface, 0);
}

static uint32_t d3d_hle_guest_add_public_ref(uint32_t object_va)
{
    uint32_t common;
    uint32_t refs;
    if (!object_va)
        return 0;
    common = d3d_hle_guest_read_u32(object_va);
    refs = common & 0xFFFFu;
    if (refs == 0xFFFFu)
        d3d_hle_guest_fatal("public reference overflow", E_INVALIDARG);
    ++refs;
    d3d_hle_guest_write_u32(
        object_va, (common & ~0xFFFFu) | refs);
    return object_va;
}

static uint32_t d3d_hle_guest_proxy_format(
    D3DFORMAT host_format, uint32_t usage)
{
    /*
     * The Plume D3D8 facade stores PC-style format values on its hosted
     * surfaces. Xbox GetBackBuffer/GetDesc exposes a linear render-target
     * format instead; retail XDK code relies on that distinction before
     * writing directly through LockRect. In particular, the color
     * backbuffer is exposed as LIN_A8R8G8B8 even when the presentation
     * parameters requested opaque X8R8G8B8.
     */
    if (usage & D3DUSAGE_RENDERTARGET) {
        switch (host_format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_LIN_A8R8G8B8:
        case D3DFMT_LIN_X8R8G8B8:
            return D3DFMT_LIN_A8R8G8B8;
        case D3DFMT_R5G6B5:
        case D3DFMT_LIN_R5G6B5:
            return D3DFMT_LIN_R5G6B5;
        case D3DFMT_A1R5G5B5:
        case D3DFMT_LIN_A1R5G5B5:
            return D3DFMT_LIN_A1R5G5B5;
        case D3DFMT_A4R4G4B4:
        case D3DFMT_LIN_A4R4G4B4:
            return D3DFMT_LIN_A4R4G4B4;
        default:
            break;
        }
    }
    return (uint32_t)host_format & 0xFFu;
}

static uint32_t d3d_hle_guest_proxy_surface(
    IDirect3DSurface8 *host_surface, uint32_t usage)
{
    D3D8Surface *host = (D3D8Surface *)host_surface;
    D3DHleGuestResource *resource;
    uint32_t object_va;
    uint32_t data_va;
    uint32_t pitch;
    uint32_t bytes;
    uint32_t size_word;
    uint32_t guest_format;
    if (!host_surface)
        return 0;
    guest_format = d3d_hle_guest_proxy_format(host->format, usage);
    if (!host->width || !host->height ||
        (!host->pitch && host->width > UINT32_MAX / 4u)) {
        host_surface->lpVtbl->Release(host_surface);
        return 0;
    }
    pitch = host->pitch ? host->pitch : host->width * 4u;
    if (pitch > UINT32_MAX - 63u) {
        host_surface->lpVtbl->Release(host_surface);
        return 0;
    }
    /*
     * Xbox linear-surface headers encode pitch in 64-byte units. The hosted
     * D3D surface may use a tightly packed row (for example, 854 * 4 bytes),
     * so give its guest-visible shadow allocation an encodable pitch instead
     * of truncating the host pitch in the size word.
     */
    pitch = (pitch + 63u) & ~63u;
    if (pitch > UINT32_MAX / host->height) {
        host_surface->lpVtbl->Release(host_surface);
        return 0;
    }
    bytes = pitch * host->height;
    object_va = d3d_hle_guest_alloc(24u, 16u, 0x07FFFFFFu);
    data_va = d3d_hle_guest_alloc(bytes, 128u, 0x03FFFFFFu);
    size_word = ((host->width - 1u) & 0xFFFu) |
                (((host->height - 1u) & 0xFFFu) << 12) |
                (((pitch >> 6) - 1u) << 24);
    /*
     * One public reference is retained by the device cache and one is
     * returned to the guest. This keeps GetBackBuffer/GetDepthStencil stable
     * after the caller releases its reference.
     */
    d3d_hle_guest_write_u32(object_va, 0x01050002u);
    d3d_hle_guest_write_u32(
        object_va + 4u, d3d_hle_guest_data_offset(data_va));
    d3d_hle_guest_write_u32(object_va + 8u, 0);
    d3d_hle_guest_write_u32(
        object_va + 12u,
        ((guest_format & 0xFFu) << 8) | (1u << 16) |
        (2u << 4));
    d3d_hle_guest_write_u32(object_va + 16u, size_word);
    d3d_hle_guest_write_u32(object_va + 20u, 0);
    resource = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_SURFACE, object_va);
    resource->data_va = d3d_hle_guest_data_offset(data_va);
    resource->data_bytes = bytes;
    resource->width = host->width;
    resource->height = host->height;
    resource->depth = 1;
    resource->levels = 1;
    resource->format = guest_format;
    resource->size = size_word;
    resource->host_handle = host->resource_id;
    resource->host_object = host_surface;
    host->guest_address = resource->data_va;
    resource->owned_object = 1;
    resource->owned_data = 1;
    resource->version = ++g_hle_texture_version;
    resource->uploaded_version = resource->version;
    return object_va;
}

void d3d_hle_guest_get_display_mode(uint32_t mode_va)
{
    if (!mode_va)
        d3d_hle_guest_fatal("GetDisplayMode output", E_INVALIDARG);
    d3d_hle_guest_write_u32(mode_va + 0u, d3d8_GetBackbufferWidth());
    d3d_hle_guest_write_u32(mode_va + 4u, d3d8_GetBackbufferHeight());
    d3d_hle_guest_write_u32(mode_va + 8u, 60u);
    d3d_hle_guest_write_u32(mode_va + 12u, 0u);
    d3d_hle_guest_write_u32(mode_va + 16u, D3DFMT_LIN_X8R8G8B8);
}

static void d3d_hle_guest_read_present_parameters(
    uint32_t parameters_va, D3DPRESENT_PARAMETERS *parameters)
{
    memset(parameters, 0, sizeof(*parameters));
    parameters->BackBufferWidth =
        d3d_hle_guest_read_u32(parameters_va + 0u);
    parameters->BackBufferHeight =
        d3d_hle_guest_read_u32(parameters_va + 4u);
    parameters->BackBufferFormat = (D3DFORMAT)
        d3d_hle_guest_read_u32(parameters_va + 8u);
    parameters->BackBufferCount =
        d3d_hle_guest_read_u32(parameters_va + 12u);
    parameters->MultiSampleType = (D3DMULTISAMPLE_TYPE)
        d3d_hle_guest_read_u32(parameters_va + 16u);
    parameters->SwapEffect = (D3DSWAPEFFECT)
        d3d_hle_guest_read_u32(parameters_va + 20u);
    parameters->hDeviceWindow = NULL;
    parameters->Windowed =
        d3d_hle_guest_read_u32(parameters_va + 28u) != 0;
    parameters->EnableAutoDepthStencil =
        d3d_hle_guest_read_u32(parameters_va + 32u) != 0;
    parameters->AutoDepthStencilFormat = (D3DFORMAT)
        d3d_hle_guest_read_u32(parameters_va + 36u);
    parameters->Flags = d3d_hle_guest_read_u32(parameters_va + 40u);
    parameters->FullScreen_RefreshRateInHz =
        d3d_hle_guest_read_u32(parameters_va + 44u);
    parameters->FullScreen_PresentationInterval =
        d3d_hle_guest_read_u32(parameters_va + 48u);
}

HRESULT d3d_hle_guest_register_vertex_shader_snapshot(
    const uint32_t *declaration_source, size_t declaration_dwords,
    const uint32_t *program_source, size_t program_dwords,
    uint32_t source_program_va, uint32_t shader_handle, uint32_t usage)
{
    uint32_t program_header;
    uint32_t instruction_count;
    uint32_t object_va = shader_handle & ~1u;
    DWORD host_handle;
    HRESULT result;
    NV2AVshDeclaration declaration;
    const NV2AVshDeclaration *declaration_ptr = NULL;
    D3DHleGuestResource *shader;

    (void)usage;
    if (!program_source || !program_dwords || !object_va)
        return E_INVALIDARG;
    if (declaration_source) {
        if (!d3d8_vsh_parse_declaration(
                (const DWORD *)declaration_source,
                declaration_dwords, &declaration))
            return E_INVALIDARG;
        declaration_ptr = &declaration;
    }
    program_header = program_source[0];
    instruction_count = program_header >> 16;
    if (!instruction_count || instruction_count > NV2A_VS_MAX_INSTRUCTIONS ||
        1u + instruction_count * 4u > program_dwords)
        return E_INVALIDARG;
    result = d3d8_vsh_create_shader(
        (const DWORD *)(program_source + 1u),
        (int)instruction_count, declaration_ptr, &host_handle);
    if (result != S_OK)
        return result;
    shader = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_VERTEX_SHADER, object_va);
    shader->data_va = source_program_va ? source_program_va + 4u : 0u;
    shader->data_bytes = instruction_count * 16u;
    shader->host_handle = (uint32_t)host_handle;
    shader->owned_object = 0;
    return S_OK;
}

HRESULT d3d_hle_guest_register_vertex_shader(
    uint32_t declaration_va, uint32_t program_va, uint32_t shader_handle,
    uint32_t usage)
{
    uint32_t program_header;
    size_t program_dwords;

    if (!program_va)
        return E_INVALIDARG;
    program_header = d3d_hle_guest_read_u32(program_va);
    program_dwords = 1u + (size_t)(program_header >> 16) * 4u;
    return d3d_hle_guest_register_vertex_shader_snapshot(
        declaration_va
            ? (const uint32_t *)xbox_guest_ptr(declaration_va) : NULL,
        declaration_va ? NV2A_VS_MAX_DECLARATION_DWORDS : 0u,
        (const uint32_t *)xbox_guest_ptr(program_va), program_dwords,
        program_va, shader_handle, usage);
}

HRESULT d3d_hle_guest_register_pixel_shader(
    const uint32_t source_definition[60], uint32_t shader_handle)
{
    D3DHleGuestResource *shader;
    void *definition;
    if (!source_definition || !shader_handle)
        return E_INVALIDARG;
    definition = malloc(0xF0u);
    if (!definition)
        return E_OUTOFMEMORY;
    memcpy(definition, source_definition, 0xF0u);
    shader = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_PIXEL_SHADER, shader_handle);
    if (shader->host_object)
        free(shader->host_object);
    shader->host_object = definition;
    shader->data_va = 0;
    shader->data_bytes = 0xF0u;
    shader->owned_object = 0;
    return S_OK;
}

void d3d_hle_guest_unregister_vertex_shader(uint32_t shader_handle)
{
    D3DHleGuestResource *shader =
        d3d_hle_guest_find_resource(shader_handle & ~1u);
    if (!shader || shader->kind != D3D_HLE_RESOURCE_VERTEX_SHADER)
        return;
    if (g_hle_vertex_shader == shader_handle)
        (void)d3d_hle_guest_set_vertex_shader(0);
    d3d8_vsh_delete_shader(shader->host_handle);
    memset(shader, 0, sizeof(*shader));
}

void d3d_hle_guest_unregister_pixel_shader(uint32_t shader_handle)
{
    D3DHleGuestResource *shader =
        d3d_hle_guest_find_resource(shader_handle);
    if (!shader || shader->kind != D3D_HLE_RESOURCE_PIXEL_SHADER)
        return;
    if (g_hle_pixel_shader == shader_handle)
        (void)d3d_hle_guest_set_pixel_shader(0);
    free(shader->host_object);
    memset(shader, 0, sizeof(*shader));
}

HRESULT d3d_hle_guest_start_host_device(
    uint32_t parameters_va, uintptr_t native_window)
{
    D3DPRESENT_PARAMETERS parameters;
    IDirect3D8 *factory;
    IDirect3DDevice8 *device = NULL;
    HRESULT result;

    if (!parameters_va || !native_window)
        return E_INVALIDARG;
    if (xbox_GetD3DDevice())
        return S_OK;

    d3d_hle_guest_read_present_parameters(parameters_va, &parameters);
    parameters.hDeviceWindow = (HWND)native_window;
    parameters.Windowed = TRUE;
    factory = xbox_Direct3DCreate8(0);
    if (!factory)
        return E_FAIL;
    result = factory->lpVtbl->CreateDevice(
        factory, 0, 1, (HWND)native_window, 0, &parameters, &device);
    if (result == S_OK && device) {
        d3d8_SetVblankScanoutCallback(d3d_hle_guest_vblank_scanout);
        /* The guest's D3DPRESENT_PARAMETERS usually live on its stack and
         * are dead after this call returns. Snapshot the decoded copy so a
         * same-title session reset can restart the host device without
         * rereading freed guest memory. */
        g_hle_present_snapshot = parameters;
        g_hle_present_snapshot_valid = 1;
    }
    return result == S_OK && device ? S_OK : E_FAIL;
}

HRESULT d3d_hle_guest_restart_host_device(uintptr_t native_window)
{
    D3DPRESENT_PARAMETERS parameters;
    IDirect3D8 *factory;
    IDirect3DDevice8 *device = NULL;
    HRESULT result;

    if (!native_window)
        return E_INVALIDARG;
    if (!g_hle_present_snapshot_valid)
        return E_FAIL;
    if (xbox_GetD3DDevice())
        return S_OK;

    parameters = g_hle_present_snapshot;
    parameters.hDeviceWindow = (HWND)native_window;
    parameters.Windowed = TRUE;
    factory = xbox_Direct3DCreate8(0);
    if (!factory)
        return E_FAIL;
    result = factory->lpVtbl->CreateDevice(
        factory, 0, 1, (HWND)native_window, 0, &parameters, &device);
    if (result == S_OK && device)
        d3d8_SetVblankScanoutCallback(d3d_hle_guest_vblank_scanout);
    return result == S_OK && device ? S_OK : E_FAIL;
}

HRESULT d3d_hle_guest_reset(uint32_t parameters_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("Reset device");
    D3DPRESENT_PARAMETERS parameters;
    if (!parameters_va)
        return E_INVALIDARG;
    d3d_hle_guest_read_present_parameters(parameters_va, &parameters);
    return device->lpVtbl->Reset(device, &parameters);
}

static HRESULT d3d_hle_guest_bind_memory_render_target(
    D3DHleGuestResource *color, D3DHleGuestResource *depth,
    int attach_default_depth)
{
    XgpuSurfaceBinding binding;
    memset(&binding, 0, sizeof(binding));
    binding.color_resource = color->data_va;
    binding.width = color->width;
    binding.height = color->height;
    binding.image_width = color->width;
    binding.image_height = color->height;
    binding.color_pitch = d3d_hle_guest_surface_pitch(color);
    binding.color_format = color->format;
    binding.layout = (color->size ||
                      d3d_hle_guest_find_tile(color->data_va)) ?
        XGPU_SURFACE_PITCH : XGPU_SURFACE_SWIZZLE;
    binding.sample_count = 1;
    if (depth) {
        binding.zeta_resource = depth->host_handle
            ? depth->host_handle : depth->data_va;
        binding.zeta_guest_address = depth->data_va;
        binding.zeta_width = depth->width;
        binding.zeta_height = depth->height;
        binding.zeta_pitch = d3d_hle_guest_surface_pitch(depth);
        binding.zeta_format =
            (depth->format == D3DFMT_D16 ||
             depth->format == D3DFMT_F16)
                ? XGPU_ZETA_Z16
                : XGPU_ZETA_Z24S8;
        binding.zeta_float =
            depth->format == D3DFMT_F16 ||
            depth->format == D3DFMT_F24S8;
        binding.zeta_layout = depth->size ?
            XGPU_SURFACE_PITCH : XGPU_SURFACE_SWIZZLE;
    }
    else if (attach_default_depth) {
        (void)d3d8_PgraphAttachDefaultZeta(&binding);
    }
    return d3d8_PgraphSetRenderTarget(&binding) ? S_OK : E_FAIL;
}

HRESULT d3d_hle_guest_set_render_target(
    uint32_t color_va, uint32_t depth_va, uint32_t viewport_va)
{
    D3DHleGuestResource *color;
    D3DHleGuestResource *depth = NULL;
    uint32_t previous_color;
    uint32_t previous_depth;
    HRESULT result;
    if (!color_va)
        color_va = g_hle_back_buffer;
    color = d3d_hle_guest_adopt_resource(color_va);
    if (depth_va)
        depth = d3d_hle_guest_adopt_resource(depth_va);
    if (!color || color->kind != D3D_HLE_RESOURCE_SURFACE)
        return E_INVALIDARG;
    if (depth_va && (!depth ||
        depth->kind != D3D_HLE_RESOURCE_SURFACE))
        return E_INVALIDARG;
    d3d_hle_guest_resource_add_bind_ref(color_va);
    d3d_hle_guest_resource_add_bind_ref(depth_va);
    if (color->host_object) {
        IDirect3DDevice8 *device =
            d3d_hle_guest_require_device("SetRenderTarget device");
        result = device->lpVtbl->SetRenderTarget(
            device, (IDirect3DSurface8 *)color->host_object,
            depth ? (IDirect3DSurface8 *)depth->host_object : NULL);
    } else {
        result = d3d_hle_guest_bind_memory_render_target(color, depth, 0);
    }
    if (result != S_OK) {
        d3d_hle_guest_resource_release_bind_ref(depth_va);
        d3d_hle_guest_resource_release_bind_ref(color_va);
        return result;
    }
    previous_color = g_hle_bindings.render_target;
    previous_depth = g_hle_bindings.depth_stencil;
    g_hle_bindings.render_target = color_va;
    g_hle_bindings.depth_stencil = depth_va;
    d3d_hle_guest_resource_release_bind_ref(previous_depth);
    d3d_hle_guest_resource_release_bind_ref(previous_color);
    D3D_HLE_F2_LOG(
        "hle set-rt color=%08X data=%08X host=%08X dims=%ux%u "
        "depth=%08X depth-data=%08X depth-host=%08X",
        color_va, color->data_va, color->host_handle,
        color->width, color->height, depth_va,
        depth ? depth->data_va : 0, depth ? depth->host_handle : 0);
    d3d_hle_guest_rebind_surface_textures();
    if (viewport_va)
        result = d3d_hle_guest_set_viewport(viewport_va);
    return result;
}

uint32_t d3d_hle_guest_get_back_buffer(uint32_t index)
{
    IDirect3DSurface8 *surface = NULL;
    IDirect3DDevice8 *device;
    uint32_t *cached_surface;
    INT host_index;

    /* Xbox extends GetBackBuffer with index -1 for the current front buffer.
     * Keep it distinct from back buffer 0: collapsing both aliases makes a
     * title alternately sample/render the wrong half of its flip chain. */
    if (index == UINT32_MAX) {
        cached_surface = &g_hle_front_buffer;
        host_index = -1;
    } else {
        if (index > 0u)
            return 0;
        cached_surface = &g_hle_back_buffer;
        host_index = (INT)index;
    }
    if (*cached_surface)
        return d3d_hle_guest_add_public_ref(*cached_surface);
    device = d3d_hle_guest_require_device("GetBackBuffer device");
    if (device->lpVtbl->GetBackBuffer(
            device, host_index, 0, &surface) != S_OK)
        return 0;
    *cached_surface = d3d_hle_guest_proxy_surface(
        surface, D3DUSAGE_RENDERTARGET);
    if (host_index == 0 && !g_hle_bindings.render_target) {
        g_hle_bindings.render_target = g_hle_back_buffer;
        d3d_hle_guest_resource_add_bind_ref(g_hle_back_buffer);
    }
    return *cached_surface;
}

HRESULT d3d_hle_guest_mirror_back_buffer(uint32_t object_va)
{
    D3DHleGuestResource *resource;
    uint32_t previous_color;
    HRESULT result;

    if (!object_va)
        return E_INVALIDARG;
    resource = d3d_hle_guest_adopt_resource(object_va);
    if (!resource || resource->kind != D3D_HLE_RESOURCE_SURFACE)
        return E_INVALIDARG;
    if (g_hle_back_buffer == object_va)
        return S_OK;

    /* The value returned by xemu's original GetBackBuffer is a real Xbox
     * surface over guest VRAM, not storage owned by the hosted D3D8 facade.
     * Keep that memory authoritative and register it with the shared PGRAPH
     * surface path. Treating it as a hosted proxy causes LockRect to download
     * an uninitialized host image over MM3's loading buffer. */
    d3d_hle_guest_resource_add_bind_ref(object_va);
    result = d3d_hle_guest_bind_memory_render_target(resource, NULL, 1);
    if (result != S_OK) {
        d3d_hle_guest_resource_release_bind_ref(object_va);
        return result;
    }

    previous_color = g_hle_bindings.render_target;
    g_hle_back_buffer = object_va;
    g_hle_bindings.render_target = object_va;
    d3d_hle_guest_resource_release_bind_ref(previous_color);
    fprintf(stderr,
            "[D3D-HLE] mirrored guest backbuffer %08X data=%08X "
            "as memory target %ux%u pitch=%u fmt=%02X\n",
            object_va, resource->data_va, resource->width, resource->height,
            d3d_hle_guest_surface_pitch(resource), resource->format);
    return S_OK;
}

uint32_t d3d_hle_guest_get_render_target(void)
{
    uint32_t target = g_hle_bindings.render_target;
    if (!target)
        target = d3d_hle_guest_get_back_buffer(0);
    else
        d3d_hle_guest_add_public_ref(target);
    return target;
}

uint32_t d3d_hle_guest_get_depth_stencil(void)
{
    IDirect3DSurface8 *surface = NULL;
    IDirect3DDevice8 *device;
    uint32_t target = g_hle_bindings.depth_stencil;
    if (target)
        return d3d_hle_guest_add_public_ref(target);
    if (g_hle_depth_buffer)
        return d3d_hle_guest_add_public_ref(g_hle_depth_buffer);
    device = d3d_hle_guest_require_device("GetDepthStencil device");
    if (device->lpVtbl->GetDepthStencilSurface(
            device, &surface) != S_OK || !surface)
        return 0;
    g_hle_depth_buffer = d3d_hle_guest_proxy_surface(
        surface, D3DUSAGE_DEPTHSTENCIL);
    g_hle_bindings.depth_stencil = g_hle_depth_buffer;
    d3d_hle_guest_resource_add_bind_ref(g_hle_depth_buffer);
    return g_hle_depth_buffer;
}

void d3d_hle_guest_set_gamma_ramp(uint32_t flags, uint32_t ramp_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("SetGammaRamp device");
    device->lpVtbl->SetGammaRamp(
        device, flags, (const D3DGAMMARAMP *)xbox_guest_ptr(ramp_va));
}

void d3d_hle_guest_get_gamma_ramp(uint32_t ramp_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("GetGammaRamp device");
    device->lpVtbl->GetGammaRamp(
        device, (D3DGAMMARAMP *)xbox_guest_ptr(ramp_va));
}

static uint32_t d3d_hle_guest_surface_offset(
    const D3DHleGuestResource *surface, uint32_t x, uint32_t y)
{
    uint32_t bpp =
        d3d_hle_guest_format_bytes_per_pixel(surface->format);
    if (surface->size)
        return y * d3d_hle_guest_surface_pitch(surface) + x * bpp;
    return swizzle_offset(x, y, surface->width, surface->height) * bpp;
}

HRESULT d3d_hle_guest_copy_rects(
    uint32_t source_va, uint32_t rects_va, uint32_t count,
    uint32_t destination_va, uint32_t points_va)
{
    D3DHleGuestResource *source =
        d3d_hle_guest_adopt_resource(source_va);
    D3DHleGuestResource *destination =
        d3d_hle_guest_adopt_resource(destination_va);
    uint32_t copy_count = count ? count : 1u;
    uint32_t bpp;
    uint32_t i;
    if (!source || !destination ||
        source->kind != D3D_HLE_RESOURCE_SURFACE ||
        destination->kind != D3D_HLE_RESOURCE_SURFACE ||
        source->format != destination->format)
        return E_INVALIDARG;
    if (d3d_hle_guest_format_is_compressed(source->format)) {
        if (count || source->width != destination->width ||
            source->height != destination->height)
            return E_NOTIMPL;
        memcpy(d3d_hle_guest_data_ptr(destination->data_va),
               d3d_hle_guest_data_ptr(source->data_va),
               d3d_hle_guest_level_bytes(
                   source->width, source->height, 1u,
                   source->format, NULL));
        destination->version = ++g_hle_texture_version;
        return S_OK;
    }
    bpp = d3d_hle_guest_format_bytes_per_pixel(source->format);
    (void)d3d8_PgraphDownloadSurfaceRange(
        source->data_va, source->data_bytes);
    for (i = 0; i < copy_count; ++i) {
        uint32_t left = 0, top = 0;
        uint32_t right = source->width, bottom = source->height;
        uint32_t dst_x = 0, dst_y = 0;
        uint32_t x, y;
        if (count) {
            uint32_t rect = rects_va + i * 16u;
            left = d3d_hle_guest_read_u32(rect + 0u);
            top = d3d_hle_guest_read_u32(rect + 4u);
            right = d3d_hle_guest_read_u32(rect + 8u);
            bottom = d3d_hle_guest_read_u32(rect + 12u);
            if (points_va) {
                dst_x = d3d_hle_guest_read_u32(points_va + i * 8u);
                dst_y = d3d_hle_guest_read_u32(
                    points_va + i * 8u + 4u);
            }
        }
        if (right < left || bottom < top ||
            right > source->width || bottom > source->height ||
            dst_x + right - left > destination->width ||
            dst_y + bottom - top > destination->height)
            return E_INVALIDARG;
        for (y = 0; y < bottom - top; ++y) {
            for (x = 0; x < right - left; ++x) {
                uint8_t *dst = d3d_hle_guest_data_ptr(
                    destination->data_va) +
                    d3d_hle_guest_surface_offset(
                        destination, dst_x + x, dst_y + y);
                const uint8_t *src = d3d_hle_guest_data_ptr(
                    source->data_va) +
                    d3d_hle_guest_surface_offset(
                        source, left + x, top + y);
                memcpy(dst, src, bpp);
            }
        }
    }
    destination->version = ++g_hle_texture_version;
    return S_OK;
}

HRESULT d3d_hle_guest_set_transform(uint32_t state, uint32_t matrix_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("SetTransform device");
    return device->lpVtbl->SetTransform(
        device, (D3DTRANSFORMSTATETYPE)state,
        (const D3DMATRIX *)xbox_guest_ptr(matrix_va));
}

HRESULT d3d_hle_guest_get_transform(uint32_t state, uint32_t matrix_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("GetTransform device");
    return device->lpVtbl->GetTransform(
        device, (D3DTRANSFORMSTATETYPE)state,
        (D3DMATRIX *)xbox_guest_ptr(matrix_va));
}

HRESULT d3d_hle_guest_set_viewport(uint32_t viewport_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("SetViewport device");
    return device->lpVtbl->SetViewport(
        device, (const D3DVIEWPORT8 *)xbox_guest_ptr(viewport_va));
}

HRESULT d3d_hle_guest_get_viewport(uint32_t viewport_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("GetViewport device");
    return device->lpVtbl->GetViewport(
        device, (D3DVIEWPORT8 *)xbox_guest_ptr(viewport_va));
}

uint32_t d3d_hle_guest_get_texture(uint32_t stage)
{
    if (stage >= XBOX_D3D_MAX_TEXTURES)
        return 0;
    return d3d_hle_guest_add_public_ref(
        g_hle_bindings.texture_resource[stage]);
}

HRESULT d3d_hle_guest_set_texture(uint32_t stage, uint32_t texture_va)
{
    uint32_t previous;
    D3DHleGuestResource *texture;
    if (stage >= XBOX_D3D_MAX_TEXTURES)
        return E_INVALIDARG;
    if (!texture_va) {
        previous = g_hle_bindings.texture_resource[stage];
        g_hle_bindings.texture_resource[stage] = 0;
        d3d8_combiners_set_texture_binding(
            stage, 2, FALSE, FALSE, 0);
        xgpu_plume_set_texture(
            stage, 0, NULL, 0, 0, 0, 0, 0, ++g_hle_texture_version);
        d3d_hle_guest_resource_release_bind_ref(previous);
        return S_OK;
    }
    texture = d3d_hle_guest_adopt_resource(texture_va);
    if (!texture || texture->kind != D3D_HLE_RESOURCE_TEXTURE)
        return E_INVALIDARG;
    d3d_hle_guest_resource_add_bind_ref(texture_va);
    previous = g_hle_bindings.texture_resource[stage];
    g_hle_bindings.texture_resource[stage] = texture_va;
    d3d_hle_guest_switch_texture(
        NV097_SET_TEXTURE_OFFSET + stage * 0x40u,
        d3d_hle_guest_read_u32(texture_va + 4u),
        d3d_hle_guest_read_u32(texture_va + 12u));
    d3d_hle_guest_resource_release_bind_ref(previous);
    return S_OK;
}

HRESULT d3d_hle_guest_set_palette(uint32_t stage, uint32_t palette_va)
{
    D3DHleGuestResource *palette = NULL;
    uint32_t previous;

    if (stage >= XBOX_D3D_MAX_TEXTURES)
        return E_INVALIDARG;
    if (palette_va) {
        palette = d3d_hle_guest_adopt_resource(palette_va);
        if (!palette || palette->kind != D3D_HLE_RESOURCE_PALETTE)
            return E_INVALIDARG;
        d3d_hle_guest_resource_add_bind_ref(palette_va);
    }
    previous = g_hle_bindings.palette_resource[stage];
    g_hle_bindings.palette_resource[stage] = palette_va;
    g_hle_palette_version[stage] = ++g_hle_texture_version;
    d3d_hle_guest_resource_release_bind_ref(previous);

    if (g_hle_bindings.texture_resource[stage]) {
        uint32_t texture_va = g_hle_bindings.texture_resource[stage];
        d3d_hle_guest_switch_texture(
            NV097_SET_TEXTURE_OFFSET + stage * 0x40u,
            d3d_hle_guest_read_u32(texture_va + 4u),
            d3d_hle_guest_read_u32(texture_va + 12u));
    }
    return S_OK;
}

uint32_t d3d_hle_guest_get_indices(uint32_t base_vertex_va)
{
    if (base_vertex_va)
        d3d_hle_guest_write_u32(
            base_vertex_va, g_hle_bindings.base_vertex);
    return d3d_hle_guest_add_public_ref(g_hle_bindings.index_resource);
}

uint32_t d3d_hle_guest_get_stream_source(
    uint32_t stream, uint32_t stride_va)
{
    if (stream >= XBOX_D3D_MAX_STREAMS)
        return 0;
    if (stride_va)
        d3d_hle_guest_write_u32(
            stride_va, g_hle_bindings.stream_stride[stream]);
    return d3d_hle_guest_add_public_ref(
        g_hle_bindings.stream_resource[stream]);
}

uint32_t d3d_hle_guest_device_add_ref(void)
{
    return ++g_hle_device_refs;
}

uint32_t d3d_hle_guest_device_release(void)
{
    uint32_t stream;
    uint32_t stage;
    uint32_t back_buffer;
    uint32_t front_buffer;
    uint32_t depth_buffer;
    uint32_t device_token;
    if (!g_hle_device_refs)
        return 0;
    --g_hle_device_refs;
    if (g_hle_device_refs)
        return g_hle_device_refs;

    for (stream = 0; stream < XBOX_D3D_MAX_STREAMS; ++stream) {
        uint32_t resource = g_hle_bindings.stream_resource[stream];
        g_hle_bindings.stream_resource[stream] = 0;
        g_hle_bindings.stream_stride[stream] = 0;
        d3d_hle_guest_resource_release_bind_ref(resource);
    }
    for (stage = 0; stage < XBOX_D3D_MAX_TEXTURES; ++stage) {
        uint32_t resource = g_hle_bindings.texture_resource[stage];
        uint32_t palette = g_hle_bindings.palette_resource[stage];
        g_hle_bindings.texture_resource[stage] = 0;
        g_hle_bindings.palette_resource[stage] = 0;
        d3d_hle_guest_resource_release_bind_ref(resource);
        d3d_hle_guest_resource_release_bind_ref(palette);
    }
    {
        uint32_t resource = g_hle_bindings.index_resource;
        g_hle_bindings.index_resource = 0;
        g_hle_bindings.base_vertex = 0;
        d3d_hle_guest_resource_release_bind_ref(resource);
    }
    {
        uint32_t color = g_hle_bindings.render_target;
        uint32_t depth = g_hle_bindings.depth_stencil;
        g_hle_bindings.render_target = 0;
        g_hle_bindings.depth_stencil = 0;
        d3d_hle_guest_resource_release_bind_ref(depth);
        d3d_hle_guest_resource_release_bind_ref(color);
    }
    back_buffer = g_hle_back_buffer;
    front_buffer = g_hle_front_buffer;
    depth_buffer = g_hle_depth_buffer;
    g_hle_back_buffer = 0;
    g_hle_front_buffer = 0;
    g_hle_depth_buffer = 0;
    if (back_buffer)
        (void)d3d_hle_guest_release_internal(back_buffer);
    if (front_buffer && front_buffer != back_buffer)
        (void)d3d_hle_guest_release_internal(front_buffer);
    if (depth_buffer && depth_buffer != back_buffer &&
        depth_buffer != front_buffer)
        (void)d3d_hle_guest_release_internal(depth_buffer);
    device_token = g_hle_device_token;
    g_hle_device_token = 0;
    d3d8_SetVblankScanoutCallback(NULL);
    if (device_token)
        xbox_HeapFree(device_token);
    return g_hle_device_refs;
}

void d3d_hle_guest_reset_registry(void)
{
    D3DHleGuestResourceChunk *chunk = g_hle_resource_chunks;
    unsigned i;

    memset(&g_hle_bindings, 0, sizeof(g_hle_bindings));
    for (chunk = g_hle_resource_chunks; chunk;) {
        D3DHleGuestResourceChunk *next = chunk->next;
        for (i = 0; i < XBOX_D3D_HLE_RESOURCE_CHUNK_SIZE; ++i)
            d3d_hle_guest_destroy_resource_for_reset(&chunk->resources[i]);
        free(chunk);
        chunk = next;
    }
    g_hle_resource_chunks = NULL;
    g_hle_resource_chunks_tail = NULL;
    g_hle_resource_capacity = 0;

    free(g_hle_object_index.keys);
    free(g_hle_object_index.records);
    free(g_hle_texture_data_index.keys);
    free(g_hle_texture_data_index.records);
    memset(&g_hle_object_index, 0, sizeof(g_hle_object_index));
    memset(&g_hle_texture_data_index, 0,
           sizeof(g_hle_texture_data_index));

    for (i = 0; i < sizeof(g_hle_up_scratch) / sizeof(g_hle_up_scratch[0]);
         ++i) {
        free(g_hle_up_scratch[i]);
        g_hle_up_scratch[i] = NULL;
        g_hle_up_scratch_capacity[i] = 0;
    }
    if (g_hle_push_scratch_va)
        xbox_HeapFree(g_hle_push_scratch_va);
    g_hle_push_scratch_va = 0;
    g_hle_push_scratch_bytes = 0;
    g_hle_push_constant_load = 0;
    if (g_hle_device_token)
        xbox_HeapFree(g_hle_device_token);
    g_hle_device_token = 0;
    g_hle_device_refs = 1;
    g_hle_texture_version = 1;
    g_hle_back_buffer = 0;
    g_hle_front_buffer = 0;
    g_hle_depth_buffer = 0;
    g_hle_vertex_shader = 0;
    g_hle_pixel_shader = 0;
    g_hle_vertex_program_generation = 0;
    memset(g_hle_loaded_vertex_programs, 0,
           sizeof(g_hle_loaded_vertex_programs));
    memset(g_hle_pixel_shader_constants, 0,
           sizeof(g_hle_pixel_shader_constants));
    g_hle_pixel_shader_constant_valid = 0;
    memset(g_hle_pixel_shader_effective, 0,
           sizeof(g_hle_pixel_shader_effective));
    g_hle_pixel_shader_effective_valid = 0;
    g_hle_fence = 0;
    g_hle_shader_constant_mode = XBOX_D3DSCM_96CONSTANTS;
    memset(g_hle_tiles, 0, sizeof(g_hle_tiles));
    memset(g_hle_palette_version, 0, sizeof(g_hle_palette_version));
    memset(g_hle_state_cache, 0, sizeof(g_hle_state_cache));
    memset(g_hle_immediate_vertex, 0, sizeof(g_hle_immediate_vertex));
    g_hle_overlay_enabled = 0;
    g_hle_depth_bias = 0.0f;
    g_hle_slope_bias = 0.0f;
    d3d_hle_guest_reset_immediate_state();
    d3d8_SetVblankScanoutCallback(NULL);
}

static void d3d_hle_guest_release_device(void)
{
    IDirect3DDevice8 *device;
    d3d_hle_guest_reset_registry();
    device = xbox_GetD3DDevice();
    if (device && device->lpVtbl) {
        ULONG refs;
        do {
            refs = device->lpVtbl->Release(device);
        } while (refs);
    }
}

void d3d_hle_guest_reset_session(void)
{
    d3d_hle_guest_release_device();
    xgpu_plume_reset_session();
}

void d3d_hle_guest_teardown_host_device(void)
{
    d3d_hle_guest_release_device();
    xgpu_plume_teardown_output();
}

void d3d_hle_guest_block_until_vertical_blank(void)
{
    D3DHleGuestResource *target =
        d3d_hle_guest_find_resource(g_hle_bindings.render_target);

    if (!xgpu_plume_wait_for_idle(0))
        d3d_hle_guest_fatal("BlockUntilVerticalBlank", E_FAIL);
    D3D_HLE_F2_LOG(
        "hle vblank-wait back=%08X target=%08X data=%08X host=%08X "
        "dims=%ux%u",
        g_hle_back_buffer, g_hle_bindings.render_target,
        target ? target->data_va : 0, target ? target->host_handle : 0,
        target ? target->width : 0, target ? target->height : 0);
}

int d3d_hle_guest_vblank_scanout(void)
{
    D3DHleGuestResource *back_buffer =
        d3d_hle_guest_find_resource(g_hle_back_buffer);
    D3D8CpuSurfaceFingerprint fingerprint;
    IDirect3DDevice8 *device;

    /*
     * A writable Xbox backbuffer lock may remain live across many refreshes.
     * The loading meter updates its small .raw rectangle through that pointer
     * after the one establishing Swap, with no later D3D call. Model coherent
     * scanout at VBlank, but only for an explicitly published CPU backbuffer.
     * The first normal GPU draw retires this mode.
     */
    if (!back_buffer || !back_buffer->host_object ||
        !back_buffer->cpu_scanout_active ||
        !back_buffer->cpu_scanout_published ||
        !d3d_hle_guest_host_surface_fingerprint(
            back_buffer, &fingerprint) ||
        (back_buffer->cpu_scanout_hash_valid &&
         back_buffer->cpu_scanout_hash == fingerprint.hash))
        return 0;

    if (d3d_hle_guest_upload_host_surface(back_buffer, 1) != S_OK)
        d3d_hle_guest_fatal("VBlank CPU scanout upload", E_FAIL);
    device = d3d_hle_guest_require_device("VBlank CPU scanout device");
    if (device->lpVtbl->Swap(device, 0) != S_OK)
        d3d_hle_guest_fatal("VBlank CPU scanout Swap", E_FAIL);
    D3D_HLE_F2_LOG(
        "hle vblank-scanout back=%08X data=%08X host=%08X hash=%016llX",
        g_hle_back_buffer, back_buffer->data_va, back_buffer->host_handle,
        (unsigned long long)fingerprint.hash);
    return 1;
}

uint32_t d3d_hle_guest_base_texture_get_level_count(uint32_t texture_va)
{
    D3DHleGuestResource *texture =
        d3d_hle_guest_adopt_resource(texture_va);
    return texture ? texture->levels : 0;
}

void d3d_hle_guest_volume_get_level_desc(
    uint32_t texture_va, uint32_t level, uint32_t desc_va)
{
    D3DHleGuestResource *texture =
        d3d_hle_guest_adopt_resource(texture_va);
    uint32_t width, height, depth, offset;
    uint32_t i;
    if (!texture || level >= texture->levels || !desc_va)
        d3d_hle_guest_fatal("VolumeTexture_GetLevelDesc", E_INVALIDARG);
    width = texture->width;
    height = texture->height;
    depth = texture->depth;
    offset = 0;
    for (i = 0; i < level; ++i) {
        offset += d3d_hle_guest_level_bytes(
            width, height, depth, texture->format, NULL);
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
        if (depth > 1u) depth >>= 1;
    }
    (void)offset;
    d3d_hle_guest_write_u32(desc_va + 0u, texture->format);
    d3d_hle_guest_write_u32(
        desc_va + 4u, XRECOMP_XBOX_D3DRTYPE_VOLUME);
    d3d_hle_guest_write_u32(desc_va + 8u, 0);
    d3d_hle_guest_write_u32(
        desc_va + 12u, d3d_hle_guest_level_bytes(
            width, height, depth, texture->format, NULL));
    d3d_hle_guest_write_u32(desc_va + 16u, width);
    d3d_hle_guest_write_u32(desc_va + 20u, height);
    d3d_hle_guest_write_u32(desc_va + 24u, depth);
}

HRESULT d3d_hle_guest_resource_register(
    uint32_t resource_va, uint32_t base_va)
{
    uint32_t data;
    uint32_t type;
    D3DHleGuestResource *resource;
    if (!resource_va)
        return E_INVALIDARG;
    data = d3d_hle_guest_read_u32(resource_va + 4u) + base_va;
    type = d3d_hle_guest_resource_type(resource_va);
    if (type != XRECOMP_XBOX_D3DRTYPE_PALETTE)
        data &= 0x0FFFFFFFu;
    d3d_hle_guest_write_u32(resource_va + 4u, data);
    resource = d3d_hle_guest_adopt_resource(resource_va);
    if (resource)
        resource->data_va = data & 0x0FFFFFFFu;
    return S_OK;
}

uint32_t d3d_hle_guest_create_surface(
    uint32_t width, uint32_t height, uint32_t format)
{
    uint32_t object_va;
    uint32_t data_va;
    uint32_t pitch;
    uint32_t bytes;
    uint32_t size_word;
    D3DHleGuestResource *resource;
    if (!width || !height)
        return 0;
    pitch = width * d3d_hle_guest_format_bytes_per_pixel(format);
    pitch = (pitch + 63u) & ~63u;
    if (!pitch || pitch > UINT32_MAX / height)
        return 0;
    bytes = pitch * height;
    object_va = d3d_hle_guest_alloc(24u, 16u, 0x07FFFFFFu);
    data_va = d3d_hle_guest_alloc(bytes, 64u, 0x03FFFFFFu);
    size_word = ((width - 1u) & 0xFFFu) |
                (((height - 1u) & 0xFFFu) << 12) |
                (((pitch >> 6) - 1u) << 24);
    d3d_hle_guest_write_u32(object_va, 0x81050001u);
    d3d_hle_guest_write_u32(
        object_va + 4u, d3d_hle_guest_data_offset(data_va));
    d3d_hle_guest_write_u32(object_va + 8u, 0);
    d3d_hle_guest_write_u32(
        object_va + 12u,
        ((format & 0xFFu) << 8) | (1u << 16) | (2u << 4));
    d3d_hle_guest_write_u32(object_va + 16u, size_word);
    d3d_hle_guest_write_u32(object_va + 20u, 0);
    resource = d3d_hle_guest_register_resource(
        D3D_HLE_RESOURCE_SURFACE, object_va);
    resource->data_va = d3d_hle_guest_data_offset(data_va);
    resource->data_bytes = bytes;
    resource->width = width;
    resource->height = height;
    resource->depth = 1;
    resource->levels = 1;
    resource->format = format & 0xFFu;
    resource->size = size_word;
    resource->owned_object = 1;
    resource->owned_data = 1;
    resource->version = ++g_hle_texture_version;
    return object_va;
}

HRESULT d3d_hle_guest_clear(
    uint32_t count, uint32_t rects_va, uint32_t flags, uint32_t color,
    uint32_t depth_bits, uint32_t stencil)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("Clear device");
    HRESULT result;
    float depth;
    memcpy(&depth, &depth_bits, sizeof(depth));
    result = device->lpVtbl->Clear(
        device, count,
        count ? (const D3DRECT *)xbox_guest_ptr(rects_va) : NULL,
        flags, color, depth, stencil);
    if (result == S_OK && (flags & D3DCLEAR_TARGET))
        d3d_hle_guest_note_gpu_draw();
    return result;
}

static uint8_t d3d_hle_guest_clamp_byte(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

HRESULT d3d_hle_guest_update_overlay(
    uint32_t surface_va, uint32_t source_rect_va,
    uint32_t destination_surface_va, uint32_t destination_rect_va,
    uint32_t color_key, uint32_t flags)
{
    D3DHleGuestResource *surface =
        d3d_hle_guest_adopt_resource(surface_va);
    uint32_t width, height, pitch, x, y;
    uint8_t *rgba;
    const uint8_t *source;
    (void)source_rect_va;
    (void)destination_surface_va;
    (void)destination_rect_va;
    (void)color_key;
    (void)flags;
    if (!surface ||
        (surface->format != D3DFMT_YUY2 &&
         surface->format != D3DFMT_UYVY))
        return E_INVALIDARG;
    if (!g_hle_overlay_enabled)
        return S_OK;
    width = surface->width;
    height = surface->height;
    pitch = d3d_hle_guest_surface_pitch(surface);
    rgba = (uint8_t *)malloc((size_t)width * height * 4u);
    if (!rgba)
        return E_OUTOFMEMORY;
    source = d3d_hle_guest_data_ptr(surface->data_va);
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; x += 2u) {
            const uint8_t *pair = source + y * pitch + x * 2u;
            int y0, y1, u, v;
            uint32_t p;
            if (surface->format == D3DFMT_YUY2) {
                y0 = pair[0]; u = pair[1]; y1 = pair[2]; v = pair[3];
            } else {
                u = pair[0]; y0 = pair[1]; v = pair[2]; y1 = pair[3];
            }
            for (p = 0; p < 2u && x + p < width; ++p) {
                int yy = (p ? y1 : y0) - 16;
                int uu = u - 128;
                int vv = v - 128;
                uint8_t *out = rgba + ((size_t)y * width + x + p) * 4u;
                out[0] = d3d_hle_guest_clamp_byte(
                    (298 * yy + 409 * vv + 128) >> 8);
                out[1] = d3d_hle_guest_clamp_byte(
                    (298 * yy - 100 * uu - 208 * vv + 128) >> 8);
                out[2] = d3d_hle_guest_clamp_byte(
                    (298 * yy + 516 * uu + 128) >> 8);
                out[3] = 255;
            }
        }
    }
    if (!xgpu_plume_present_host_frame(
            rgba, width, height, width * 4u)) {
        free(rgba);
        return E_FAIL;
    }
    free(rgba);
    return S_OK;
}

void d3d_hle_guest_enable_overlay(uint32_t enable)
{
    const int was_enabled = g_hle_overlay_enabled;
    g_hle_overlay_enabled = enable != 0;
    if (was_enabled != g_hle_overlay_enabled)
        xemu_d3d_hle_overlay_state_changed(g_hle_overlay_enabled);
    if (was_enabled && !g_hle_overlay_enabled) {
        xgpu_plume_retire_host_frame();
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
        if (getenv("XEMU_D3D_HLE_CAPTURE_AFTER_FMV"))
            xgpu_plume_f2_request();
#endif
    }
}

HRESULT d3d_hle_guest_swap(uint32_t flags)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("Swap device");
    D3DHleGuestResource *back_buffer =
        d3d_hle_guest_find_resource(g_hle_back_buffer);
    D3DHleGuestResource *target =
        d3d_hle_guest_find_resource(g_hle_bindings.render_target);
    HRESULT result =
        d3d_hle_guest_upload_dirty_host_surface(back_buffer);
    D3D_HLE_F2_LOG(
        "hle swap back=%08X target=%08X data=%08X host=%08X "
        "dims=%ux%u flags=%08X",
        g_hle_back_buffer, g_hle_bindings.render_target,
        target ? target->data_va : 0,
        target ? target->host_handle : 0,
        target ? target->width : 0,
        target ? target->height : 0, flags);
    if (result != S_OK)
        return result;
    /*
     * Xbox titles may construct the scanout surface directly over physical
     * memory rather than obtaining the device proxy backbuffer. The shared
     * PGRAPH surface registry already owns CPU-write synchronization for that
     * memory. Present a full-panel memory target through it; offscreen targets
     * keep normal swap-chain semantics.
     */
    /* A widescreen host backbuffer does not change the Xbox scanout panel.
     * Accept both the host-extended device extent and the native 640x480
     * memory surface; Plume composites the latter to the output extent. */
    const int target_is_scanout = target &&
        ((target->width == d3d8_GetBackbufferWidth() &&
          target->height == d3d8_GetBackbufferHeight()) ||
         (target->width == XGPU_PANEL_WIDTH &&
          target->height == XGPU_PANEL_HEIGHT));
    if (target_is_scanout && !target->host_object && target->data_va &&
        d3d8_PgraphPresentSurfaceForSwap(target->data_va))
        return S_OK;
    result = device->lpVtbl->Swap(device, flags);
    if (result == S_OK && back_buffer &&
        back_buffer->cpu_scanout_active)
        back_buffer->cpu_scanout_published = 1;
    return result;
}

HRESULT d3d_hle_guest_set_back_buffer_scale(
    uint32_t horizontal_bits, uint32_t vertical_bits)
{
    float horizontal;
    float vertical;
    memcpy(&horizontal, &horizontal_bits, sizeof(horizontal));
    memcpy(&vertical, &vertical_bits, sizeof(vertical));
    if (!isfinite(horizontal) || !isfinite(vertical) ||
        horizontal <= 0.0f || vertical <= 0.0f)
        return E_INVALIDARG;
    memcpy(&g_hle_state_cache[510], &horizontal, sizeof(horizontal));
    memcpy(&g_hle_state_cache[511], &vertical, sizeof(vertical));
    return S_OK;
}

HRESULT d3d_hle_guest_check_device_format(
    uint32_t adapter, uint32_t device_type, uint32_t adapter_format,
    uint32_t usage, uint32_t resource_type, uint32_t check_format)
{
    (void)adapter_format;
    (void)usage;
    if (adapter != 0 || device_type != 1)
        return E_INVALIDARG;
    if (resource_type < XRECOMP_XBOX_D3DRTYPE_SURFACE ||
        resource_type > XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE)
        return E_INVALIDARG;
    switch (check_format & 0xFFu) {
    case D3DFMT_L8: case D3DFMT_A8L8: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_R5G6B5:
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_DXT1: case D3DFMT_DXT3: case D3DFMT_DXT5:
    case D3DFMT_LIN_A1R5G5B5: case D3DFMT_LIN_R5G6B5:
    case D3DFMT_LIN_A8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
    case D3DFMT_D24S8: case D3DFMT_D16:
    case D3DFMT_YUY2: case D3DFMT_UYVY:
        return S_OK;
    default:
        return E_INVALIDARG;
    }
}

HRESULT d3d_hle_guest_create_device(
    uint32_t adapter, uint32_t device_type, uint32_t focus_window,
    uint32_t behavior_flags, uint32_t parameters_va, uint32_t output_va)
{
    IDirect3DDevice8 *device = xbox_GetD3DDevice();
    D3DPRESENT_PARAMETERS parameters;
    HRESULT result = S_OK;
    (void)focus_window;
    if (!parameters_va)
        return E_INVALIDARG;
    if (!d3d_hle_guest_driver_prepare()) {
        if (output_va)
            d3d_hle_guest_write_u32(output_va, 0);
        return E_FAIL;
    }
    d3d_hle_guest_read_present_parameters(parameters_va, &parameters);
    if (!device) {
        IDirect3D8 *factory = xbox_Direct3DCreate8(0);
        if (!factory)
            return E_FAIL;
        result = factory->lpVtbl->CreateDevice(
            factory, adapter, device_type, NULL, behavior_flags,
            &parameters, &device);
    }
    if (result != S_OK || !device) {
        if (output_va)
            d3d_hle_guest_write_u32(output_va, 0);
        return result != S_OK ? result : E_FAIL;
    }
    if (!g_hle_device_token) {
        g_hle_device_token =
            d3d_hle_guest_alloc(4u, 4u, 0x07FFFFFFu);
        d3d_hle_guest_write_u32(g_hle_device_token, 1u);
    }
    g_hle_device_refs = 1;
    d3d8_SetVblankScanoutCallback(d3d_hle_guest_vblank_scanout);
    if (output_va)
        d3d_hle_guest_write_u32(output_va, g_hle_device_token);
    return S_OK;
}

static uint32_t d3d_hle_guest_create_buffer(
    uint32_t length, D3DHleGuestResourceKind kind)
{
    uint32_t object_va;
    uint32_t data_va;
    D3DHleGuestResource *resource;
    if (!length)
        return 0;
    object_va = d3d_hle_guest_alloc(12u, 16u, 0x07FFFFFFu);
    data_va = d3d_hle_guest_alloc(length, 64u, 0x03FFFFFFu);
    d3d_hle_guest_write_u32(
        object_va, kind == D3D_HLE_RESOURCE_INDEX_BUFFER ?
        0x01010001u : 0x01000001u);
    d3d_hle_guest_write_u32(
        object_va + 4u, d3d_hle_guest_data_offset(data_va));
    d3d_hle_guest_write_u32(object_va + 8u, 0);
    resource = d3d_hle_guest_register_resource(kind, object_va);
    resource->data_va = d3d_hle_guest_data_offset(data_va);
    resource->data_bytes = length;
    resource->owned_object = 1;
    resource->owned_data = 1;
    resource->version = ++g_hle_texture_version;
    return object_va;
}

uint32_t d3d_hle_guest_create_vertex_buffer(uint32_t length)
{
    return d3d_hle_guest_create_buffer(
        length, D3D_HLE_RESOURCE_VERTEX_BUFFER);
}

uint32_t d3d_hle_guest_create_index_buffer(uint32_t length)
{
    return d3d_hle_guest_create_buffer(
        length, D3D_HLE_RESOURCE_INDEX_BUFFER);
}

uint32_t d3d_hle_guest_set_fence(uint32_t flags)
{
    (void)flags;
    if (++g_hle_fence == 0)
        ++g_hle_fence;
    return g_hle_fence;
}

void d3d_hle_guest_block_on_time(uint32_t fence, uint32_t flags)
{
    (void)fence;
    (void)flags;
    if (!xgpu_plume_wait_for_idle(0))
        d3d_hle_guest_fatal("BlockOnTime", E_FAIL);
}

void d3d_hle_guest_lock_2d_surface(
    uint32_t texture_va, uint32_t face, uint32_t level,
    uint32_t locked_rect_va, uint32_t rect_va, uint32_t flags)
{
    uint32_t surface_va =
        d3d_hle_guest_get_surface_level(texture_va, face, level);
    d3d_hle_guest_surface_lock_rect(
        surface_va, locked_rect_va, rect_va, flags);
    (void)d3d_hle_guest_resource_release(surface_va);
}

void d3d_hle_guest_lock_3d_surface(
    uint32_t texture_va, uint32_t level, uint32_t locked_box_va,
    uint32_t box_va, uint32_t flags)
{
    D3DHleGuestResource *texture =
        d3d_hle_guest_adopt_resource(texture_va);
    uint32_t width, height, depth, pitch, offset;
    uint32_t left = 0, top = 0, front = 0;
    uint32_t slice_pitch;
    (void)flags;
    if (!texture || !locked_box_va)
        d3d_hle_guest_fatal("Lock3DSurface", E_INVALIDARG);
    offset = d3d_hle_guest_level_offset(
        texture, level, &width, &height, &pitch);
    depth = texture->depth;
    while (level && depth > 1u) {
        depth >>= 1;
        --level;
    }
    slice_pitch = pitch * height;
    if (box_va) {
        left = d3d_hle_guest_read_u32(box_va + 0u);
        top = d3d_hle_guest_read_u32(box_va + 4u);
        front = d3d_hle_guest_read_u32(box_va + 16u);
    }
    if (left >= width || top >= height || front >= depth)
        d3d_hle_guest_fatal("Lock3DSurface box", E_INVALIDARG);
    d3d_hle_guest_write_u32(locked_box_va + 0u, pitch);
    d3d_hle_guest_write_u32(locked_box_va + 4u, slice_pitch);
    d3d_hle_guest_write_u32(
        locked_box_va + 8u,
        texture->data_va + offset + front * slice_pitch +
        top * pitch + left *
        d3d_hle_guest_format_bytes_per_pixel(texture->format));
    texture->version = ++g_hle_texture_version;
}

void d3d_hle_guest_note_resource_cpu_write(uint32_t resource_va)
{
    D3DHleGuestResource *resource =
        d3d_hle_guest_adopt_resource(resource_va);

    if (resource)
        resource->version = ++g_hle_texture_version;
}

void d3d_hle_guest_select_vertex_shader(
    uint32_t shader_handle, uint32_t address)
{
    if (!shader_handle)
        shader_handle = g_hle_vertex_shader;
    if (!shader_handle)
        d3d_hle_guest_fatal("SelectVertexShader handle", E_INVALIDARG);
    d3d_hle_guest_load_vertex_shader(shader_handle, address);
    (void)d3d_hle_guest_set_vertex_shader(shader_handle);
}

void d3d_hle_guest_delete_vertex_shader(uint32_t shader_handle)
{
    uint32_t object_va = shader_handle & 1u ?
        shader_handle - 1u : shader_handle;
    D3DHleGuestResource *shader =
        d3d_hle_guest_find_resource(object_va);
    if (!shader || shader->kind != D3D_HLE_RESOURCE_VERTEX_SHADER)
        return;
    if (g_hle_vertex_shader == shader_handle)
        (void)d3d_hle_guest_set_vertex_shader(0);
    d3d8_vsh_delete_shader(shader->host_handle);
    (void)d3d_hle_guest_resource_release(object_va);
}

HRESULT d3d_hle_guest_set_vertex_shader(uint32_t shader_handle)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("SetVertexShader device");
    DWORD host_handle = shader_handle;
    if (shader_handle & 1u) {
        D3DHleGuestResource *shader =
            d3d_hle_guest_find_resource(shader_handle - 1u);
        if (!shader &&
            d3d_hle_guest_adopt_native_vertex_shader(
                shader_handle, 0) == S_OK)
            shader = d3d_hle_guest_find_resource(shader_handle - 1u);
        if (!shader || shader->kind != D3D_HLE_RESOURCE_VERTEX_SHADER)
            return E_INVALIDARG;
        host_handle = shader->host_handle;
    }
    if (device->lpVtbl->SetVertexShader(
            device, host_handle) != S_OK)
        return E_FAIL;
    g_hle_vertex_shader = shader_handle;
    return S_OK;
}

void d3d_hle_guest_draw_vertices_up(
    uint32_t primitive_type, uint32_t vertex_count,
    uint32_t vertices_va, uint32_t stride)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("DrawVerticesUP device");
    uint32_t primitive_count = d3d_hle_guest_primitive_count(
        (D3DPRIMITIVETYPE)primitive_type, vertex_count);
    uint64_t vertex_bytes = (uint64_t)vertex_count * stride;
    const void *vertices;
    HRESULT result;

    if (!primitive_count)
        return;
    if (!vertices_va || !stride || vertex_bytes > SIZE_MAX ||
        (uint64_t)vertices_va + vertex_bytes >
            (uint64_t)UINT32_MAX + 1u)
        d3d_hle_guest_fatal("DrawVerticesUP vertex range", E_INVALIDARG);
    vertices = d3d_hle_guest_snapshot_range(
        0, vertices_va, (size_t)vertex_bytes);
    D3D_HLE_F2_LOG(
        "hle draw-up prim=%u count=%u pc=%u va=%08X stride=%u bytes=%llu",
        primitive_type, vertex_count, primitive_count, vertices_va, stride,
        (unsigned long long)vertex_bytes);
    d3d_hle_guest_prepare_fixed_state();
    result = device->lpVtbl->DrawPrimitiveUP(
        device, (D3DPRIMITIVETYPE)primitive_type, primitive_count,
        vertices, stride);
    if (result != S_OK)
        d3d_hle_guest_fatal("DrawVerticesUP", result);
    d3d_hle_guest_note_gpu_draw();
}

void d3d_hle_guest_draw_indexed_vertices_up(
    uint32_t primitive_type, uint32_t vertex_count,
    uint32_t indices_va, uint32_t vertices_va, uint32_t stride)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("DrawIndexedVerticesUP device");
    uint32_t index_count;
    uint32_t primitive_count;
    uint32_t min_index = UINT32_MAX;
    uint32_t max_index = 0;
    uint32_t i;
    uint64_t index_bytes;
    uint64_t vertex_bytes;
    const uint16_t *indices;
    const void *vertices;
    switch ((D3DPRIMITIVETYPE)primitive_type) {
    case D3DPT_POINTLIST: index_count = vertex_count; break;
    case D3DPT_LINELIST: index_count = vertex_count; break;
    case D3DPT_LINELOOP:
    case D3DPT_LINESTRIP: index_count = vertex_count; break;
    case D3DPT_TRIANGLELIST: index_count = vertex_count; break;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: index_count = vertex_count; break;
    case D3DPT_QUADLIST: index_count = vertex_count; break;
    case D3DPT_QUADSTRIP:
    case D3DPT_POLYGON: index_count = vertex_count; break;
    default: d3d_hle_guest_fatal(
        "DrawIndexedVerticesUP primitive", E_INVALIDARG);
    }
    primitive_count = d3d_hle_guest_primitive_count(
        (D3DPRIMITIVETYPE)primitive_type, index_count);
    if (!primitive_count)
        return;
    index_bytes = (uint64_t)index_count * sizeof(*indices);
    if (!indices_va || index_bytes > SIZE_MAX ||
        (uint64_t)indices_va + index_bytes >
            (uint64_t)UINT32_MAX + 1u)
        d3d_hle_guest_fatal(
            "DrawIndexedVerticesUP index range", E_INVALIDARG);
    indices = (const uint16_t *)d3d_hle_guest_snapshot_range(
        0, indices_va, (size_t)index_bytes);
    d3d_hle_guest_prepare_fixed_state();
    for (i = 0; i < index_count; ++i) {
        if (indices[i] < min_index) min_index = indices[i];
        if (indices[i] > max_index) max_index = indices[i];
    }
    vertex_bytes = ((uint64_t)max_index + 1u) * stride;
    if (!vertices_va || !stride || vertex_bytes > SIZE_MAX ||
        (uint64_t)vertices_va + vertex_bytes >
            (uint64_t)UINT32_MAX + 1u)
        d3d_hle_guest_fatal(
            "DrawIndexedVerticesUP vertex range", E_INVALIDARG);
    vertices = d3d_hle_guest_snapshot_range(
        1, vertices_va, (size_t)vertex_bytes);
    D3D_HLE_F2_LOG(
        "hle indexed-up prim=%u count=%u pc=%u iva=%08X ibytes=%llu "
        "vva=%08X stride=%u vbytes=%llu range=%u:%u",
        primitive_type, index_count, primitive_count, indices_va,
        (unsigned long long)index_bytes, vertices_va, stride,
        (unsigned long long)vertex_bytes, min_index, max_index);
    if (device->lpVtbl->DrawIndexedPrimitiveUP(
            device, (D3DPRIMITIVETYPE)primitive_type,
            min_index, max_index - min_index + 1u, primitive_count,
            indices, D3DFMT_INDEX16, vertices,
            stride) != S_OK)
        d3d_hle_guest_fatal("DrawIndexedVerticesUP", E_FAIL);
    d3d_hle_guest_note_gpu_draw();
}

/* Begin/End immediate mode snapshots the current attribute latches whenever
 * position (attribute zero) is written, matching NV2A method semantics. */
enum { D3D_HLE_IMM_MAX_VERTS = 2048 };
static int g_hle_imm_active;
static uint32_t g_hle_imm_primitive;
static uint32_t g_hle_imm_attr_mask;
static uint32_t g_hle_imm_vert_count;
static float g_hle_imm_verts[D3D_HLE_IMM_MAX_VERTS][16][4];

static void d3d_hle_guest_reset_immediate_state(void)
{
    g_hle_imm_active = 0;
    g_hle_imm_primitive = 0;
    g_hle_imm_attr_mask = 0;
    g_hle_imm_vert_count = 0;
    memset(g_hle_imm_verts, 0, sizeof(g_hle_imm_verts));
}

static void d3d_hle_imm_note_attribute(uint32_t reg)
{
    if (!g_hle_imm_active || reg >= 16u)
        return;
    g_hle_imm_attr_mask |= 1u << reg;
    if (reg != 0u)
        return;
    if (g_hle_imm_vert_count >= D3D_HLE_IMM_MAX_VERTS)
        d3d_hle_guest_fatal("Begin/End vertex overflow", E_OUTOFMEMORY);
    memcpy(g_hle_imm_verts[g_hle_imm_vert_count], g_hle_immediate_vertex,
           sizeof(g_hle_immediate_vertex));
    ++g_hle_imm_vert_count;
}

void d3d_hle_guest_set_vertex_data4f(
    uint32_t reg, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    if (reg == UINT32_MAX)
        reg = 0;
    if (reg >= 16u)
        return;
    memcpy(&g_hle_immediate_vertex[reg][0], &x, sizeof(x));
    memcpy(&g_hle_immediate_vertex[reg][1], &y, sizeof(y));
    memcpy(&g_hle_immediate_vertex[reg][2], &z, sizeof(z));
    memcpy(&g_hle_immediate_vertex[reg][3], &w, sizeof(w));
    d3d8_vsh_set_vertex_data4f(reg, &g_hle_immediate_vertex[reg][0]);
    xgpu_plume_set_vertex_data4f(reg, &g_hle_immediate_vertex[reg][0]);
    d3d_hle_imm_note_attribute(reg);
}

void d3d_hle_guest_set_vertex_data2f(uint32_t reg, uint32_t x, uint32_t y)
{
    float zero = 0.0f;
    float one = 1.0f;
    uint32_t z;
    uint32_t w;

    memcpy(&z, &zero, sizeof(z));
    memcpy(&w, &one, sizeof(w));
    d3d_hle_guest_set_vertex_data4f(reg, x, y, z, w);
}

void d3d_hle_guest_set_vertex_data2s(uint32_t reg, uint32_t a, uint32_t b)
{
    float fa = (float)(int32_t)a;
    float fb = (float)(int32_t)b;
    uint32_t x;
    uint32_t y;

    memcpy(&x, &fa, sizeof(x));
    memcpy(&y, &fb, sizeof(y));
    d3d_hle_guest_set_vertex_data2f(reg, x, y);
}

void d3d_hle_guest_set_vertex_data_color(uint32_t reg, uint32_t color)
{
    float components[4];
    uint32_t bits[4];
    unsigned i;

    components[0] = (float)(color & 0xFFu) / 255.0f;
    components[1] = (float)((color >> 8) & 0xFFu) / 255.0f;
    components[2] = (float)((color >> 16) & 0xFFu) / 255.0f;
    components[3] = (float)((color >> 24) & 0xFFu) / 255.0f;
    for (i = 0; i < 4; ++i)
        memcpy(&bits[i], &components[i], sizeof(bits[i]));
    d3d_hle_guest_set_vertex_data4f(
        reg, bits[0], bits[1], bits[2], bits[3]);
}

void d3d_hle_guest_set_vertex_data4ub(
    uint32_t reg, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t packed = (a & 0xFFu) | ((b & 0xFFu) << 8) |
                      ((c & 0xFFu) << 16) | ((d & 0xFFu) << 24);
    d3d_hle_guest_set_vertex_data_color(reg, packed);
}

void d3d_hle_guest_begin(uint32_t primitive_type)
{
    if (g_hle_imm_active)
        d3d_hle_guest_fatal("Begin inside Begin/End", E_FAIL);
    g_hle_imm_active = 1;
    g_hle_imm_primitive = primitive_type;
    g_hle_imm_attr_mask = 0;
    g_hle_imm_vert_count = 0;
}

static uint32_t d3d_hle_guest_pack_immediate_color(const float color[4])
{
    return ((uint32_t)(color[3] * 255.0f + 0.5f) << 24) |
           ((uint32_t)(color[2] * 255.0f + 0.5f) << 16) |
           ((uint32_t)(color[1] * 255.0f + 0.5f) << 8) |
           (uint32_t)(color[0] * 255.0f + 0.5f);
}

void d3d_hle_guest_end(void)
{
    /* XYZRHW + diffuse + specular + four float2 texture coordinates. */
    static float packed[D3D_HLE_IMM_MAX_VERTS * 14];
    static int warned_unmatched_end;
    IDirect3DDevice8 *device;
    const uint32_t supported_mask = 0x1u | 0x8u | 0x10u | 0x1E00u;
    uint32_t fvf;
    uint32_t tex_count = 0;
    uint32_t floats_per_vertex;
    uint32_t out = 0;
    uint32_t v;
    int transformed = 0;
    HRESULT result;

    if (!g_hle_imm_active) {
        /* Automatic HLE can become active between a native Begin and End.
         * There is no complete primitive to submit in that case; resync at
         * the boundary instead of terminating the title. */
        if (!warned_unmatched_end) {
            fprintf(stderr,
                    "[D3D-HLE] warning: ignored End without intercepted Begin\n");
            warned_unmatched_end = 1;
        }
        return;
    }
    g_hle_imm_active = 0;
    if (g_hle_imm_vert_count == 0)
        return;
    if (!(g_hle_imm_attr_mask & 0x1u) ||
        (g_hle_imm_attr_mask & ~supported_mask))
        d3d_hle_guest_fatal("Begin/End unsupported attribute mask",
                            E_NOTIMPL);
    device = d3d_hle_guest_require_device("Begin/End device");
    if (!d3d8_vsh_is_programmable(g_hle_vertex_shader)) {
        transformed = (g_hle_vertex_shader & 0x00Eu) == D3DFVF_XYZRHW;
    } else {
        for (v = 0; v < g_hle_imm_vert_count; ++v) {
            if (g_hle_imm_verts[v][0][3] != 1.0f) {
                transformed = 1;
                break;
            }
        }
    }
    while (tex_count < 4u &&
           (g_hle_imm_attr_mask & (0x200u << tex_count)))
        ++tex_count;
    if ((g_hle_imm_attr_mask >> 9) != ((1u << tex_count) - 1u))
        d3d_hle_guest_fatal("Begin/End non-contiguous texture attributes",
                            E_NOTIMPL);
    fvf = (transformed ? D3DFVF_XYZRHW : D3DFVF_XYZ) |
          ((g_hle_imm_attr_mask & 0x8u) ? D3DFVF_DIFFUSE : 0u) |
          ((g_hle_imm_attr_mask & 0x10u) ? D3DFVF_SPECULAR : 0u) |
          (tex_count << D3DFVF_TEXCOUNT_SHIFT);
    floats_per_vertex = (transformed ? 4u : 3u) +
                         ((g_hle_imm_attr_mask & 0x8u) ? 1u : 0u) +
                         ((g_hle_imm_attr_mask & 0x10u) ? 1u : 0u) +
                         tex_count * 2u;
    for (v = 0; v < g_hle_imm_vert_count; ++v) {
        const float (*attr)[4] = g_hle_imm_verts[v];
        uint32_t t;

        packed[out++] = attr[0][0];
        packed[out++] = attr[0][1];
        packed[out++] = attr[0][2];
        if (transformed)
            packed[out++] = attr[0][3];
        if (g_hle_imm_attr_mask & 0x8u) {
            uint32_t color = d3d_hle_guest_pack_immediate_color(attr[3]);
            memcpy(&packed[out++], &color, sizeof(color));
        }
        if (g_hle_imm_attr_mask & 0x10u) {
            uint32_t color = d3d_hle_guest_pack_immediate_color(attr[4]);
            memcpy(&packed[out++], &color, sizeof(color));
        }
        for (t = 0; t < tex_count; ++t) {
            packed[out++] = attr[9 + t][0];
            packed[out++] = attr[9 + t][1];
        }
    }
    d3d_hle_guest_prepare_fixed_state();
    result = device->lpVtbl->SetVertexShader(device, fvf);
    if (result == S_OK) {
        result = device->lpVtbl->DrawPrimitiveUP(
            device, (D3DPRIMITIVETYPE)g_hle_imm_primitive,
            d3d_hle_guest_primitive_count(
                (D3DPRIMITIVETYPE)g_hle_imm_primitive,
                g_hle_imm_vert_count),
            packed, floats_per_vertex * 4u);
    }
    if (result == S_OK)
        result = device->lpVtbl->SetVertexShader(device, g_hle_vertex_shader);
    if (result != S_OK)
        d3d_hle_guest_fatal("Begin/End draw", result);
    d3d_hle_guest_note_gpu_draw();
    g_hle_imm_vert_count = 0;
    g_hle_imm_attr_mask = 0;
}

static void d3d_hle_guest_note_synthetic_visibility(void)
{
    static int logged;

    if (!logged) {
        logged = 1;
        fprintf(stderr, "[D3D-HLE] visibility tests report synthetic "
                        "always-visible results\n");
    }
}

void d3d_hle_guest_begin_visibility_test(void)
{
    d3d_hle_guest_note_synthetic_visibility();
}

HRESULT d3d_hle_guest_end_visibility_test(uint32_t index)
{
    (void)index;
    return S_OK;
}

HRESULT d3d_hle_guest_get_visibility_test_result(
    uint32_t index, uint32_t result_va, uint32_t timestamp_va)
{
    static uint32_t timestamp;

    (void)index;
    if (result_va)
        d3d_hle_guest_write_u32(result_va, XGPU_PANEL_WIDTH *
                                           XGPU_PANEL_HEIGHT);
    if (timestamp_va) {
        ++timestamp;
        d3d_hle_guest_write_u32(timestamp_va, timestamp);
        d3d_hle_guest_write_u32(timestamp_va + 4u, 0);
    }
    return S_OK;
}

void d3d_hle_guest_get_display_field_status(uint32_t status_va)
{
    static uint32_t vblank_count;

    if (!status_va)
        return;
    ++vblank_count;
    d3d_hle_guest_write_u32(status_va, 3u);
    d3d_hle_guest_write_u32(status_va + 4u, vblank_count);
}

extern void d3d8_PgraphSetWindowClip(int enabled, uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height);

void d3d_hle_guest_set_scissors(
    uint32_t count, uint32_t exclusive, uint32_t rects_va)
{
    if (count == 0u) {
        d3d8_PgraphSetWindowClip(0, 0, 0, 0, 0);
        return;
    }
    if (count != 1u || exclusive)
        d3d_hle_guest_fatal("SetScissors unsupported configuration",
                            E_NOTIMPL);
    {
        uint32_t x1 = d3d_hle_guest_read_u32(rects_va);
        uint32_t y1 = d3d_hle_guest_read_u32(rects_va + 4u);
        uint32_t x2 = d3d_hle_guest_read_u32(rects_va + 8u);
        uint32_t y2 = d3d_hle_guest_read_u32(rects_va + 12u);

        if (x2 < x1 || y2 < y1)
            d3d_hle_guest_fatal("SetScissors invalid rectangle",
                                E_INVALIDARG);
        d3d8_PgraphSetWindowClip(1, x1, y1, x2 - x1, y2 - y1);
    }
}

void d3d_hle_guest_set_screen_space_offset(uint32_t x_bits, uint32_t y_bits)
{
    float x;
    float y;

    memcpy(&x, &x_bits, sizeof(x));
    memcpy(&y, &y_bits, sizeof(y));
    if (x > 1.0f || x < -1.0f || y > 1.0f || y < -1.0f)
        d3d_hle_guest_fatal("SetScreenSpaceOffset beyond subpixel range",
                            E_NOTIMPL);
}

HRESULT d3d_hle_guest_set_vertex_shader_input(
    uint32_t handle, uint32_t stream_count, uint32_t inputs_va)
{
    uint32_t i;

    (void)handle;
    for (i = 0; i < stream_count; ++i) {
        uint32_t entry = inputs_va + i * 12u;
        uint32_t buffer_va = d3d_hle_guest_read_u32(entry);
        uint32_t stride = d3d_hle_guest_read_u32(entry + 4u);
        uint32_t offset = d3d_hle_guest_read_u32(entry + 8u);

        if (offset)
            d3d_hle_guest_fatal("SetVertexShaderInput nonzero offset",
                                E_NOTIMPL);
        d3d_hle_guest_set_stream_source(i, buffer_va, stride);
    }
    return S_OK;
}

HRESULT d3d_hle_guest_set_vertex_shader_input_direct(
    uint32_t stream_count, uint32_t inputs_va)
{
    static int logged;

    if (!logged) {
        logged = 1;
        fprintf(stderr,
                "[D3D-HLE] SetVertexShaderInputDirect: VAF override "
                "ignored (streams bound, layout from current shader)\n");
    }
    return d3d_hle_guest_set_vertex_shader_input(0, stream_count, inputs_va);
}

void d3d_hle_guest_set_logic_op(uint32_t value)
{
    if (value != 0u)
        d3d_hle_guest_fatal("SetRenderState_LogicOp (active logic op)",
                            E_NOTIMPL);
}

void d3d_hle_guest_set_vertex_blend(uint32_t value)
{
    if (value != 0u && value != 1u && value != 3u && value != 5u)
        d3d_hle_guest_fatal("SetRenderState_VertexBlend (Xbox-only mode)",
                            E_NOTIMPL);
    d3d_hle_guest_set_render_state(D3DRS_VERTEXBLEND, value);
}

/* Arm the unmodeled-method work list: one line per distinct method id, not
 * one per process, so a first attach of Forza/SM2 enumerates everything the
 * decoder still has to learn instead of hiding it behind the first hit. */
static void d3d_hle_guest_log_unmodeled_push_method(
    uint32_t method, uint32_t count)
{
    enum { SEEN_MAX = 64 };
    static uint32_t seen[SEEN_MAX];
    static unsigned seen_count;
    unsigned i;

    for (i = 0; i < seen_count; ++i) {
        if (seen[i] == method)
            return;
    }
    if (seen_count < SEEN_MAX)
        seen[seen_count++] = method;
    fprintf(stderr,
            "[D3D-HLE] inline push method 0x%04X (count %u) not modeled; "
            "skipped\n",
            method, count);
}

/*
 * Decode the NV2A method stream the guest wrote into the scratch window.
 * `end_va` bounds the walk: BeginPush/EndPush know exactly how far the
 * caller wrote, which is stricter than trusting a zero header to terminate.
 * Zero means "use the whole buffer" for the legacy MakeSpace callers.
 */
static void d3d_hle_guest_drain_push_range(uint32_t end_va)
{
    uint32_t at = g_hle_push_scratch_va;
    uint32_t end;

    if (!at)
        return;
    end = at + g_hle_push_scratch_bytes;
    if (end_va && end_va < end)
        end = end_va;
    while (at + 4u <= end) {
        uint32_t header = d3d_hle_guest_read_u32(at);
        uint32_t count = header >> 18;
        uint32_t method = header & 0x1FFCu;

        if (header == 0u || at + 4u + count * 4u > end)
            break;
        if (method == 0x1EA4u && count == 1u) {
            g_hle_push_constant_load = d3d_hle_guest_read_u32(at + 4u);
        } else if (method == 0x0B80u && count && (count & 3u) == 0u) {
            /* NV097_SET_TRANSFORM_CONSTANT_LOAD carries the hardware slot
             * (0..191) directly; the public-mode bias must not reapply. */
            d3d_hle_guest_set_vertex_shader_constant_hardware(
                g_hle_push_constant_load, at + 4u, count / 4u);
            g_hle_push_constant_load += count / 4u;
        } else {
            d3d_hle_guest_log_unmodeled_push_method(method, count);
        }
        d3d_hle_guest_write_u32(at, 0);
        at += 4u + count * 4u;
    }
}

void d3d_hle_guest_drain_inline_push(void)
{
    d3d_hle_guest_drain_push_range(0);
}

/*
 * Reserve a scratch window of at least `count` dwords and hand back its VA.
 * The XDK grow path asks for Count*4 + 0x204, so match that headroom rather
 * than a fixed size: BeginStateBig callers write Count dwords straight at
 * the cursor and must never run past the window.
 */
static uint32_t d3d_hle_guest_reserve_push(uint32_t count)
{
    uint64_t needed = (uint64_t)count * 4u + 0x204u;
    uint32_t bytes;

    if (needed < D3D_HLE_PUSH_SCRATCH_BYTES)
        needed = D3D_HLE_PUSH_SCRATCH_BYTES;
    if (needed > 0x01000000u) {
        d3d_hle_guest_log_unmodeled_push_method(0xFFFFu, count);
        return 0;
    }
    bytes = (uint32_t)((needed + 63u) & ~63ull);
    if (g_hle_push_scratch_va && bytes <= g_hle_push_scratch_bytes) {
        /* Reuse: retire whatever the previous window still holds first. */
        d3d_hle_guest_drain_push_range(0);
    } else {
        if (g_hle_push_scratch_va) {
            d3d_hle_guest_drain_push_range(0);
            xbox_HeapFree(g_hle_push_scratch_va);
        }
        g_hle_push_scratch_va =
            d3d_hle_guest_alloc(bytes, 64u, 0x03FFFFFFu);
        g_hle_push_scratch_bytes = bytes;
    }
    memset(d3d_hle_guest_data_ptr(g_hle_push_scratch_va), 0,
           g_hle_push_scratch_bytes);
    return g_hle_push_scratch_va;
}

/*
 * Point the guest's own push cursor at the scratch window.
 *
 * BeginStateBig and MakeRequestedSpace return nothing: the caller writes its
 * dwords through g_pDevice->pPut ([device+0], limit at [device+4]). Handing
 * back a VA the caller never reads would leave those writes on the native
 * cursor and split the renderer, so the REPLACE body has to retarget the
 * device fields themselves.
 */
static void d3d_hle_guest_retarget_push_cursor(uint32_t va, uint32_t bytes)
{
    uint32_t device;

    if (!va || !xrecomp_d3d_hle_device_global_va)
        return;
    if (!d3d_hle_guest_try_read_u32(
            xrecomp_d3d_hle_device_global_va, &device) || !device)
        return;
    d3d_hle_guest_write_u32(device, va);
    d3d_hle_guest_write_u32(device + 4u, va + bytes);
}

uint32_t d3d_hle_guest_begin_push(uint32_t count)
{
    return d3d_hle_guest_reserve_push(count);
}

void d3d_hle_guest_end_push(uint32_t push_va)
{
    d3d_hle_guest_drain_push_range(push_va);
}

void d3d_hle_guest_kick_push_buffer(void)
{
    d3d_hle_guest_drain_push_range(0);
}

void d3d_hle_guest_begin_state_big(uint32_t count)
{
    uint32_t va = d3d_hle_guest_reserve_push(count);

    /* ponytail: this drains and rewinds to the window start, where native
     * appends at the existing pPut. Only correct because reserve_push drains
     * first, so nothing pending is dropped. Forza calls this ~10M times a
     * run and will feel the rewind; revisit once the unmodeled-method log
     * says what actually has to be replayed. */
    d3d_hle_guest_retarget_push_cursor(va, g_hle_push_scratch_bytes);
}

void d3d_hle_guest_make_requested_space(
    uint32_t requested, uint32_t maximum)
{
    uint32_t va;

    (void)maximum;
    /* ponytail: MakeRequestedSpace's arguments are BYTES (the recon body
     * passes Count*4 + 0x204), but reserve_push takes a dword count and
     * scales by 4 again, so this over-allocates by ~4x. Harmless headroom.
     * Do NOT shrink it without a byte-vs-dword test proving which unit each
     * caller actually passes. */
    va = d3d_hle_guest_reserve_push(requested);
    d3d_hle_guest_retarget_push_cursor(va, g_hle_push_scratch_bytes);
}

uint32_t d3d_hle_guest_make_space(void)
{
    if (!g_hle_push_scratch_va)
        return d3d_hle_guest_reserve_push(0);
    d3d_hle_guest_drain_push_range(0);
    return g_hle_push_scratch_va;
}

uint32_t d3d_hle_guest_palette_size(uint32_t palette_va)
{
    return d3d_hle_guest_read_u32(palette_va) >> 30;
}

uint32_t d3d_hle_guest_palette_lock2(uint32_t palette_va, uint32_t flags)
{
    (void)flags;
    return d3d_hle_guest_data_address(
        d3d_hle_guest_read_u32(palette_va + 4u));
}

uint32_t d3d_hle_guest_resource_add_ref(uint32_t resource_va)
{
    uint32_t common;
    uint32_t refs;

    if (!resource_va)
        return 0;
    common = d3d_hle_guest_read_u32(resource_va);
    refs = (common & 0xFFFFu) + 1u;
    if (refs > 0xFFFFu)
        d3d_hle_guest_fatal("AddRef reference overflow", E_FAIL);
    d3d_hle_guest_write_u32(resource_va, (common & ~0xFFFFu) | refs);
    return refs;
}

uint32_t d3d_hle_guest_create_surface2(
    uint32_t width, uint32_t height, uint32_t usage, uint32_t format)
{
    (void)usage;
    return d3d_hle_guest_create_surface(width, height, format);
}

void d3d_hle_guest_delete_pixel_shader(uint32_t shader_handle)
{
    D3DHleGuestResource *shader =
        d3d_hle_guest_find_resource(shader_handle);
    if (!shader || shader->kind != D3D_HLE_RESOURCE_PIXEL_SHADER)
        return;
    if (g_hle_pixel_shader == shader_handle)
        (void)d3d_hle_guest_set_pixel_shader(0);
    (void)d3d_hle_guest_resource_release(shader_handle);
}

static void d3d_hle_guest_map_pixel_shader_constant(
    DWORD *effective, const DWORD *definition,
    uint32_t reg, DWORD color)
{
    uint32_t stage;

    for (stage = 0; stage < 8u; ++stage) {
        if (((definition[57] >> (stage * 4u)) & 0xFu) == reg)
            effective[10 + stage] = color;
        if (((definition[58] >> (stage * 4u)) & 0xFu) == reg)
            effective[18 + stage] = color;
    }
    if ((definition[59] & 0xFu) == reg)
        effective[43] = color;
    if (((definition[59] >> 4u) & 0xFu) == reg)
        effective[44] = color;
}

static void d3d_hle_guest_apply_pixel_shader(
    const D3DHleGuestResource *shader)
{
    const DWORD *definition;

    if (!shader || shader->kind != D3D_HLE_RESOURCE_PIXEL_SHADER)
        return;
    definition = shader->host_object
        ? (const DWORD *)shader->host_object
        : (const DWORD *)xbox_guest_ptr(shader->data_va);
    memcpy(g_hle_pixel_shader_effective, definition,
           sizeof(g_hle_pixel_shader_effective));
    /*
     * The retained Xbox D3D SetPixelShader body dumps the shader's frozen
     * definition, including its combiner constants. SetPixelShaderConstant
     * updates the currently active mapped constants immediately, but the
     * device-side API register bank is not replayed through a later shader
     * bind.
     */
    g_hle_pixel_shader_effective_valid = 1;
    D3D_HLE_F2_LOG(
        "hleps bind guest=%08X data=%08X "
        "a0=%08X fabcd=%08X fefg=%08X rgb0=%08X "
        "aout0=%08X rgbout0=%08X count=%08X tex=%08X",
        shader->object_va, shader->data_va,
        definition[0], definition[8], definition[9], definition[34],
        definition[26], definition[45], definition[53], definition[54]);

    d3d8_combiners_set_definition(g_hle_pixel_shader_effective);
}

HRESULT d3d_hle_guest_set_pixel_shader(uint32_t shader_handle)
{
    if (!shader_handle) {
        g_hle_pixel_shader = 0;
        g_hle_pixel_shader_effective_valid = 0;
        D3D_HLE_F2_LOG("hleps bind guest=00000000");
        d3d8_combiners_set_definition(NULL);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
        /*
         * The retained XDK SetPixelShader(0) path sets the combiner and
         * texture-shader dirty bits consumed by LazySetState.  Its native HLE
         * replacement must preserve that shadow-state contract.
         */
        d3d_hle_guest_mark_deferred_pixel_shader_dirty();
#endif
        /*
         * prepare_draw() intentionally leaves Plume's active shader alone
         * when no D3D8 combiner is selected, because the native PGRAPH path
         * may have supplied one.  This HLE frontend owns the current D3D8
         * draw state, however, so SetPixelShader(0) must explicitly retire
         * its previous programmable shader before a fixed-function draw is
         * queued.
         */
        xgpu_plume_set_active_ps(0);
        return S_OK;
    }
    {
        D3DHleGuestResource *shader =
            d3d_hle_guest_find_resource(shader_handle);
        if (!shader || shader->kind != D3D_HLE_RESOURCE_PIXEL_SHADER)
            return E_INVALIDARG;
        g_hle_pixel_shader = shader_handle;
        d3d_hle_guest_apply_pixel_shader(shader);
    }
    return S_OK;
}

static uint32_t d3d_hle_guest_float4_color(const float *value)
{
    uint32_t r = (uint32_t)(fmaxf(0.0f, fminf(1.0f, value[0])) *
                            255.0f + 0.5f);
    uint32_t g = (uint32_t)(fmaxf(0.0f, fminf(1.0f, value[1])) *
                            255.0f + 0.5f);
    uint32_t b = (uint32_t)(fmaxf(0.0f, fminf(1.0f, value[2])) *
                            255.0f + 0.5f);
    uint32_t a = (uint32_t)(fmaxf(0.0f, fminf(1.0f, value[3])) *
                            255.0f + 0.5f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void d3d_hle_guest_set_pixel_shader_constant(
    uint32_t start_register, uint32_t data_va, uint32_t count)
{
    D3DHleGuestResource *shader =
        d3d_hle_guest_find_resource(g_hle_pixel_shader);
    const DWORD *definition =
        shader && shader->kind == D3D_HLE_RESOURCE_PIXEL_SHADER
            ? (shader->host_object
                   ? (const DWORD *)shader->host_object
                   : (const DWORD *)xbox_guest_ptr(shader->data_va))
            : NULL;
    const float *values = (const float *)xbox_guest_ptr(data_va);
    uint32_t i;

    for (i = 0; i < count; ++i) {
        uint32_t reg = start_register + i;
        uint32_t color;
        if (reg >= 16u)
            continue;
        color = d3d_hle_guest_float4_color(values + i * 4u);
        g_hle_pixel_shader_constants[reg] = color;
        g_hle_pixel_shader_constant_valid |= 1u << reg;
        if (!definition || !g_hle_pixel_shader_effective_valid)
            continue;
        d3d_hle_guest_map_pixel_shader_constant(
            g_hle_pixel_shader_effective, definition, reg, color);
    }
    if (definition && g_hle_pixel_shader_effective_valid)
        d3d8_combiners_set_definition(g_hle_pixel_shader_effective);
}

void d3d_hle_guest_kickoff_and_wait_for_idle(void)
{
    d3d_hle_guest_block_on_time(g_hle_fence, 2u);
}

/*
 * Plume owns the GPU for an attached title, so the Xbox synchronization
 * surface is answered from host state. Falling through to the native XDK
 * bodies here would poke NV2A behind Plume's back — the split-renderer case
 * UNIVERSAL-SETUP refuses.
 */
uint32_t d3d_hle_guest_insert_fence(void)
{
    return d3d_hle_guest_set_fence(0);
}

void d3d_hle_guest_block_on_fence(uint32_t fence)
{
    d3d_hle_guest_block_on_time(fence, 0);
}

uint32_t d3d_hle_guest_is_busy(void)
{
    /* ponytail: Plume retires work synchronously at these boundaries, so
     * "busy" is never observable from the guest. Track real outstanding
     * submissions here if a title ever spins on a true busy answer. */
    return 0;
}

void d3d_hle_guest_block_until_not_busy(uint32_t resource_va)
{
    if (resource_va)
        d3d_hle_guest_block_on_resource(resource_va);
    else
        d3d_hle_guest_block_on_time(g_hle_fence, 0);
}

/*
 * Fixed-function lighting state lives in the hosted D3D8 device; route the
 * guest stdcall entries through the same vtable the COM path uses instead of
 * shadowing it here.
 */
HRESULT d3d_hle_guest_set_material(uint32_t material_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("SetMaterial device");
    if (!material_va)
        return E_INVALIDARG;
    return device->lpVtbl->SetMaterial(
        device, (const D3DMATERIAL8 *)xbox_guest_ptr(material_va));
}

HRESULT d3d_hle_guest_set_light(uint32_t index, uint32_t light_va)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("SetLight device");
    if (!light_va)
        return E_INVALIDARG;
    return device->lpVtbl->SetLight(
        device, index, (const D3DLIGHT8 *)xbox_guest_ptr(light_va));
}

HRESULT d3d_hle_guest_light_enable(uint32_t index, uint32_t enable)
{
    IDirect3DDevice8 *device =
        d3d_hle_guest_require_device("LightEnable device");
    return device->lpVtbl->LightEnable(device, index, (BOOL)enable);
}

/*
 * SetPixelShaderProgram binds an inline pixel-shader definition without
 * creating a handle. Feed the same effective-definition path a handle bind
 * uses so the combiner state stays single-owner.
 */
HRESULT d3d_hle_guest_set_pixel_shader_program(uint32_t definition_va)
{
    if (!definition_va) {
        g_hle_pixel_shader = 0;
        g_hle_pixel_shader_effective_valid = 0;
        d3d8_combiners_set_definition(NULL);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
        d3d_hle_guest_mark_deferred_pixel_shader_dirty();
#endif
        return S_OK;
    }
    memcpy(g_hle_pixel_shader_effective,
           xbox_guest_ptr(definition_va),
           sizeof(g_hle_pixel_shader_effective));
    g_hle_pixel_shader_effective_valid = 1;
    d3d8_combiners_set_definition(g_hle_pixel_shader_effective);
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    d3d_hle_guest_mark_deferred_pixel_shader_dirty();
#endif
    return S_OK;
}

/*
 * Callbacks are recorded only. Plume never calls back into guest code from a
 * host thread, and no existing path does either; a title that depends on the
 * callback firing keeps its own polling behaviour.
 */
static uint32_t g_hle_swap_callback;
static uint32_t g_hle_vblank_callback;
static uint32_t g_hle_insert_callback;
static uint32_t g_hle_insert_callback_context;

void d3d_hle_guest_set_swap_callback(uint32_t callback_va)
{
    g_hle_swap_callback = callback_va;
}

void d3d_hle_guest_set_vertical_blank_callback(uint32_t callback_va)
{
    g_hle_vblank_callback = callback_va;
}

void d3d_hle_guest_insert_callback(
    uint32_t type, uint32_t callback_va, uint32_t context)
{
    (void)type;
    g_hle_insert_callback = callback_va;
    g_hle_insert_callback_context = context;
}

/*
 * D3D_LazySetPointParams(Device) recomputes the NV2A point-size registers
 * from the D3DRS_POINTSIZE/POINTSCALE render states the guest already set.
 * Those render states are owned by the shared render-state path, and the
 * hosted device recomputes point size at draw time from them, so the flush
 * is satisfied by the state already tracked. The Device argument is the
 * caller's XDK CDevice, which HLE deliberately does not model (see
 * d3d_hle_lazy_set_state) — it is validated, never dereferenced.
 *
 * ponytail: no separate point-param shadow exists to materialize; add one
 * only if a title is observed setting point state through a path that
 * bypasses SetRenderState.
 */
void d3d_hle_guest_lazy_set_point_params(uint32_t device_va)
{
    (void)device_va;
}
