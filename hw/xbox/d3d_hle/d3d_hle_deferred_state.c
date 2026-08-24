#include "d3d_hle_guest.h"

#include "plume/plume_f2_capture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint8_t *xbox_guest_ptr(uint32_t va);

#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
/*
 * Emitted by tools.recomp from the signature-bound D3D::LazySetState and
 * structurally verified LazySetCombiners bodies. Zero addresses mean that
 * the current title did not resolve the optional deferred-state bridge and
 * therefore cannot enter native HLE readiness.
 */
extern uint32_t xrecomp_d3d_hle_dirty_flags_va;
extern uint32_t xrecomp_d3d_hle_deferred_texture_state_va;
/*
 * The contiguous six-DWORD D3D::LazySetSpecFogCombiner shadow (fog enable,
 * table mode, start, end, density, range-fog enable).  Zero means the
 * structural review did not resolve it; the fog bridge then stays inert.
 */
extern uint32_t xrecomp_d3d_hle_fog_state_va;
#endif

enum {
    XRECOMP_D3D_DEFERRED_STAGE_COUNT = 4,
    XRECOMP_D3D_DEFERRED_STAGE_DWORDS = 32,
    XRECOMP_D3D_DEFERRED_UNKNOWN = 0x7FFFFFFF,
    XRECOMP_D3D_DEFERRED_FIXED_DIRTY = 0x0000480F,
    XRECOMP_D3D_DEFERRED_SPEC_FOG_DIRTY = 0x00002000,
};

typedef struct D3DHleDeferredTextureState {
    uint8_t deferred_index;
    D3DTEXTURESTAGESTATETYPE host_state;
} D3DHleDeferredTextureState;

/*
 * XDK 4039+ texture-state order, verified structurally by
 * tools.recomp.d3d_deferred_state. Unsupported NV2A-only state is left in the
 * guest shadow; this bridge carries the sampler and fixed-function state that
 * the Plume compatibility device consumes.
 */
static const D3DHleDeferredTextureState g_deferred_texture_states[] = {
    { 0, D3DTSS_ADDRESSU },
    { 1, D3DTSS_ADDRESSV },
    { 3, D3DTSS_MAGFILTER },
    { 4, D3DTSS_MINFILTER },
    { 5, D3DTSS_MIPFILTER },
    { 6, D3DTSS_MIPMAPLODBIAS },
    { 7, D3DTSS_MAXMIPLEVEL },
    { 8, D3DTSS_MAXANISOTROPY },
    { 9, D3DTSS_COLORKEYOP },
    { 12, D3DTSS_COLOROP },
    { 13, D3DTSS_COLORARG0 },
    { 14, D3DTSS_COLORARG1 },
    { 15, D3DTSS_COLORARG2 },
    { 16, D3DTSS_ALPHAOP },
    { 17, D3DTSS_ALPHAARG0 },
    { 18, D3DTSS_ALPHAARG1 },
    { 19, D3DTSS_ALPHAARG2 },
    { 20, D3DTSS_RESULTARG },
    { 28, D3DTSS_TEXCOORDINDEX },
    { 29, D3DTSS_BORDERCOLOR },
};

static uint32_t guest_read_u32(uint32_t va)
{
    return *(const uint32_t *)xbox_guest_ptr(va);
}

static void guest_write_u32(uint32_t va, uint32_t value)
{
    *(uint32_t *)xbox_guest_ptr(va) = value;
}

void d3d_hle_guest_mark_deferred_pixel_shader_dirty(void)
{
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    uint32_t dirty;
    if (!xrecomp_d3d_hle_dirty_flags_va)
        return;
    dirty = guest_read_u32(xrecomp_d3d_hle_dirty_flags_va);
    /*
     * D3DDevice_SetPixelShader(0) sets both LazySetCombiners (0x0800) and
     * LazySetShaderStageProgram (0x4000).  Preserve exactly that XDK shadow
     * transition while the host retires its programmable shader separately.
     */
    guest_write_u32(
        xrecomp_d3d_hle_dirty_flags_va, dirty | 0x00004800u);
#endif
}

void d3d_hle_guest_materialize_deferred_fixed_state(void)
{
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    static uint32_t last_hash = UINT32_MAX;
    uint32_t dirty_before;
    uint32_t dirty_after;
    uint32_t hash = 2166136261u;
    unsigned stage;
    size_t state_index;

    if (!xrecomp_d3d_hle_dirty_flags_va ||
        !xrecomp_d3d_hle_deferred_texture_state_va)
        return;

    dirty_before = guest_read_u32(xrecomp_d3d_hle_dirty_flags_va);
    if (!(dirty_before & XRECOMP_D3D_DEFERRED_FIXED_DIRTY))
        return;

    for (stage = 0; stage < XRECOMP_D3D_DEFERRED_STAGE_COUNT; ++stage) {
        uint32_t stage_va =
            xrecomp_d3d_hle_deferred_texture_state_va +
            stage * XRECOMP_D3D_DEFERRED_STAGE_DWORDS * sizeof(uint32_t);
        for (state_index = 0;
             state_index < sizeof(g_deferred_texture_states) /
                               sizeof(g_deferred_texture_states[0]);
             ++state_index) {
            const D3DHleDeferredTextureState *mapping =
                &g_deferred_texture_states[state_index];
            uint32_t value = guest_read_u32(
                stage_va + mapping->deferred_index * sizeof(uint32_t));
            if (value == XRECOMP_D3D_DEFERRED_UNKNOWN)
                continue;
            hash = (hash ^ value) * 16777619u;
            d3d_hle_guest_set_texture_stage_state(
                stage, mapping->host_state, value);
        }
    }

    dirty_after = dirty_before & ~XRECOMP_D3D_DEFERRED_FIXED_DIRTY;
    guest_write_u32(xrecomp_d3d_hle_dirty_flags_va, dirty_after);
    if (hash != last_hash) {
        xgpu_plume_f2_log(
            "hle deferred-fixed shadow=%08X dirty=%08X->%08X hash=%08X",
            xrecomp_d3d_hle_deferred_texture_state_va,
            dirty_before, dirty_after, hash);
        last_hash = hash;
    }
#endif
}

void d3d_hle_guest_materialize_deferred_fog_state(void)
{
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    static uint32_t last_logged[6] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX,
    };
    uint32_t dirty;
    uint32_t fog[6];
    unsigned i;

    if (!xrecomp_d3d_hle_dirty_flags_va || !xrecomp_d3d_hle_fog_state_va)
        return;

    dirty = guest_read_u32(xrecomp_d3d_hle_dirty_flags_va);
    if (!(dirty & XRECOMP_D3D_DEFERRED_SPEC_FOG_DIRTY))
        return;

    /*
     * D3D::LazySetSpecFogCombiner's shadow, in guest order: enable, table
     * mode, start, end, density, range-fog enable.  Apply the parameters
     * before the enable so an enabling transition never draws with stale
     * curve state, exactly as the XDK emits SET_FOG_MODE/PARAMS with the
     * enable in one method run.
     */
    for (i = 0; i < 6; ++i)
        fog[i] = guest_read_u32(
            xrecomp_d3d_hle_fog_state_va + i * sizeof(uint32_t));
    d3d_hle_guest_set_render_state(D3DRS_FOGTABLEMODE, fog[1]);
    d3d_hle_guest_set_render_state(D3DRS_FOGSTART, fog[2]);
    d3d_hle_guest_set_render_state(D3DRS_FOGEND, fog[3]);
    d3d_hle_guest_set_render_state(D3DRS_FOGDENSITY, fog[4]);
    d3d_hle_guest_set_render_state(D3DRS_RANGEFOGENABLE, fog[5] != 0);
    d3d_hle_guest_set_render_state(D3DRS_FOGENABLE, fog[0] != 0);

    guest_write_u32(xrecomp_d3d_hle_dirty_flags_va,
                    dirty & ~(uint32_t)XRECOMP_D3D_DEFERRED_SPEC_FOG_DIRTY);
    if (memcmp(last_logged, fog, sizeof(fog)) != 0) {
        xgpu_plume_f2_log(
            "hle deferred-fog shadow=%08X enable=%u mode=%u "
            "start=%08X end=%08X density=%08X range=%u",
            xrecomp_d3d_hle_fog_state_va, fog[0], fog[1],
            fog[2], fog[3], fog[4], fog[5]);
        memcpy(last_logged, fog, sizeof(fog));
    }
#endif
}

#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
extern void d3d_hle_lazy_set_state_gen_unused(void);
#endif

/*
 * Preserve ordinary guest calls to this internal XDK boundary.  Native draw
 * hooks never enter this retained body: true HLE has a lightweight device
 * token, not the full XDK CDevice that LazySetState dereferences.
 */
void d3d_hle_lazy_set_state(void)
{
#if !defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
    d3d_hle_lazy_set_state_gen_unused();
#endif
}
