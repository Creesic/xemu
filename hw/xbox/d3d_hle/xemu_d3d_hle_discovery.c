/*
 * Runtime Xbox D3D8 discovery for the Plume frontend.
 *
 * XbSymbolDatabase identifies the XDK copy linked into the currently loaded
 * XBE.  Its per-symbol parameter metadata lets this bridge normalize both
 * ordinary stdcall builds and LTCG register variants into the already
 * reviewed, title-neutral D3D HLE wrappers.
 */
#include "qemu/osdep.h"

#include <ctype.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include <libXbSymbolDatabase.h>
#include <Xbe.h>

#include "d3d_hle_guest.h"
#include "xemu_d3d_hle_discovery.h"

extern uint32_t g_eax, g_ebx, g_ecx, g_edx, g_ebp, g_esi, g_edi, g_esp;
uint8_t *xbox_guest_ptr(uint32_t va);

typedef struct XemuD3DHleBinding {
    const char *name;
    XemuD3DHleEntry entry;
    uint8_t param_count;
    uint8_t params[XEMU_D3D_HLE_MAX_ABI_ARGS];
} XemuD3DHleBinding;

/* Later LTCG builds fold the fixed Adapter/DeviceType/FocusWindow arguments
 * out of Direct3D_CreateDevice and expose only behavior, parameters, output. */
static void automatic_create_device_compact(void)
{
    uint32_t behavior_flags = d3d_hle_guest_stack_u32(0);
    uint32_t parameters = d3d_hle_guest_stack_u32(1);
    uint32_t output = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    d3d_hle_guest_return_u32((uint32_t)d3d_hle_guest_create_device(
        0, 1, 0, behavior_flags, parameters, output));
}

#define ST XEMU_D3D_ABI_STACK
#define AX XEMU_D3D_ABI_EAX
#define BX XEMU_D3D_ABI_EBX
#define CX XEMU_D3D_ABI_ECX
#define DX XEMU_D3D_ABI_EDX
#define BP XEMU_D3D_ABI_EBP
#define SI XEMU_D3D_ABI_ESI
#define DI XEMU_D3D_ABI_EDI
#define B0(api, entry_) { #api, (entry_), 0, { 0 } }
#define B1(api, entry_) { #api, (entry_), 1, { ST } }
#define B2(api, entry_) { #api, (entry_), 2, { ST, ST } }
#define B3(api, entry_) { #api, (entry_), 3, { ST, ST, ST } }
#define B4(api, entry_) { #api, (entry_), 4, { ST, ST, ST, ST } }
#define B5(api, entry_) { #api, (entry_), 5, { ST, ST, ST, ST, ST } }
#define B6(api, entry_) { #api, (entry_), 6, { ST, ST, ST, ST, ST, ST } }
#define B7(api, entry_) { #api, (entry_), 7, { ST, ST, ST, ST, ST, ST, ST } }
#define A1(api, entry_, a_) { #api, (entry_), 1, { (a_) } }
#define A2(api, entry_, a_, b_) { #api, (entry_), 2, { (a_), (b_) } }
#define A3(api, entry_, a_, b_, c_) \
    { #api, (entry_), 3, { (a_), (b_), (c_) } }
#define A5(api, entry_, a_, b_, c_, d_, e_) \
    { #api, (entry_), 5, { (a_), (b_), (c_), (d_), (e_) } }
#define A6(api, entry_, a_, b_, c_, d_, e_, f_) \
    { #api, (entry_), 6, { (a_), (b_), (c_), (d_), (e_), (f_) } }

/*
 * Canonical API -> reviewed wrapper ABI.  Most targets are ordinary stack
 * wrappers.  The small explicit set records the historical register ABI of
 * a wrapper imported before universal runtime discovery existed.
 */
static const XemuD3DHleBinding bindings[] = {
    B1(D3DDevice_GetDeviceCaps, d3d_hle_device_get_device_caps),
    B1(D3DDevice_GetDisplayMode, d3d_hle_device_get_display_mode),
    B1(D3DDevice_Reset, d3d_hle_device_reset_std),
    B2(D3DDevice_SetRenderTarget, d3d_hle_device_set_render_target_std),
    B1(D3DDevice_GetBackBuffer2, d3d_hle_device_get_back_buffer2_std),
    B2(D3DDevice_SetGammaRamp, d3d_hle_device_set_gamma_ramp),
    A1(D3DDevice_GetGammaRamp, d3d_hle_device_get_gamma_ramp, DX),
    B5(D3DDevice_CopyRects, d3d_hle_device_copy_rects),
    B0(D3DDevice_GetRenderTarget2, d3d_hle_device_get_render_target2),
    B0(D3DDevice_GetDepthStencilSurface2,
       d3d_hle_device_get_depth_stencil_surface2),
    B2(D3DDevice_SetTransform, d3d_hle_device_set_transform_std),
    A2(D3DDevice_GetTransform, d3d_hle_device_get_transform, AX, DX),
    B1(D3DDevice_SetViewport, d3d_hle_device_set_viewport),
    A1(D3DDevice_GetViewport, d3d_hle_device_get_viewport, DX),
    A1(D3DDevice_GetTexture2, d3d_hle_device_get_texture2, SI),
    B2(D3DDevice_SetTexture, d3d_hle_device_set_texture_std),
    A3(D3DDevice_SwitchTexture, d3d_hle_device_switch_texture, CX, DX, ST),
    B2(D3DDevice_SetIndices, d3d_hle_device_set_indices_std),
    A1(D3DDevice_GetIndices2, d3d_hle_device_get_indices2, DI),
    B0(D3DDevice_AddRef, d3d_hle_device_add_ref),
    B0(D3DDevice_Release, d3d_hle_device_release),
    B0(D3D_KickOffAndWaitForIdle, d3d_hle_kickoff_and_wait_for_idle),
    B0(D3DDevice_BlockUntilVerticalBlank,
       d3d_hle_device_block_until_vertical_blank),
    B1(D3DDevice_SetRenderState_FogColor,
       d3d_hle_device_set_render_state_fog_color),
    B1(D3DDevice_SetRenderState_CullMode,
       d3d_hle_device_set_render_state_cull_mode),
    B1(D3DDevice_SetRenderState_ZEnable,
       d3d_hle_device_set_render_state_z_enable),
    B1(D3DDevice_SetRenderState_StencilEnable,
       d3d_hle_device_set_render_state_stencil_enable),
    B1(D3DDevice_SetRenderState_StencilFail,
       d3d_hle_device_set_render_state_stencil_fail),
    B1(D3DDevice_SetRenderState_TextureFactor,
       d3d_hle_device_set_render_state_texture_factor),
    B1(D3DDevice_SetRenderState_Dxt1NoiseEnable,
       d3d_hle_device_set_render_state_dxt1_noise_enable),
    B1(D3DDevice_SetRenderState_OcclusionCullEnable,
       d3d_hle_device_set_render_state_occlusion_cull_enable),
    B1(D3DDevice_SetRenderState_StencilCullEnable,
       d3d_hle_device_set_render_state_stencil_cull_enable),
    B1(D3DDevice_SetRenderState_RopZCmpAlwaysRead,
       d3d_hle_device_set_render_state_rop_z_cmp_always_read),
    B1(D3DDevice_SetRenderState_RopZRead,
       d3d_hle_device_set_render_state_rop_z_read),
    B1(D3DDevice_SetRenderState_DoNotCullUncompressed,
       d3d_hle_device_set_render_state_do_not_cull_uncompressed),
    B1(D3DDevice_SetRenderState_ZBias,
       d3d_hle_device_set_render_state_z_bias),
    B1(D3DDevice_SetRenderState_MultiSampleRenderTargetMode,
       d3d_hle_device_set_render_state_multisample_render_target_mode),
    B1(D3DDevice_SetRenderState_MultiSampleAntiAlias,
       d3d_hle_device_set_render_state_multisample_antialias),
    B1(D3DDevice_SetRenderState_TwoSidedLighting,
       d3d_hle_device_set_render_state_two_sided_lighting),
    A2(D3DDevice_SetRenderState_Simple,
       d3d_hle_device_set_render_state_simple, CX, DX),
    A2(D3DDevice_SetTextureState_TexCoordIndex,
       d3d_hle_device_set_texture_state_texcoord_index, SI, ST),
    B2(D3DDevice_SetTextureState_BorderColor,
       d3d_hle_device_set_texture_state_border_color_std),
    A3(D3DDevice_SetTextureStageStateNotInline,
       d3d_hle_device_set_texture_stage_state_not_inline, AX, DX, CX),
    B3(D3D_CommonSetRenderTarget, d3d_hle_common_set_render_target),
    B7(D3DDevice_CreateTexture2, d3d_hle_device_create_texture2),
    B1(D3DBaseTexture_GetLevelCount,
       d3d_hle_base_texture_get_level_count),
    B3(D3DTexture_GetLevelDesc, d3d_hle_texture_get_level_desc),
    B2(D3DTexture_GetSurfaceLevel2,
       d3d_hle_texture_get_surface_level2),
    B3(D3DCubeTexture_GetCubeMapSurface2,
       d3d_hle_cube_texture_get_cube_map_surface2),
    B3(D3DVolumeTexture_GetLevelDesc,
       d3d_hle_volume_texture_get_level_desc),
    B1(D3DResource_AddRef, d3d_hle_resource_add_ref),
    B1(D3DResource_Release, d3d_hle_resource_release),
    B1(D3DResource_GetType, d3d_hle_resource_get_type_std),
    B1(D3DResource_IsBusy, d3d_hle_resource_is_busy_std),
    B2(D3DResource_Register, d3d_hle_resource_register_std),
    B2(D3DSurface_GetDesc, d3d_hle_surface_get_desc),
    B4(D3DSurface_LockRect, d3d_hle_surface_lock_rect),
    B6(D3DDevice_Clear, d3d_hle_device_clear),
    A5(D3DDevice_UpdateOverlay, d3d_hle_device_update_overlay,
       ST, AX, ST, ST, ST),
    B1(D3DDevice_EnableOverlay, d3d_hle_device_enable_overlay),
    B1(D3DDevice_Swap, d3d_hle_device_swap_std),
    B2(D3DDevice_SetBackBufferScale,
       d3d_hle_device_set_back_buffer_scale),
    A6(D3D_CheckDeviceFormat, d3d_hle_direct3d_check_device_format,
       DX, ST, SI, ST, ST, CX),
    B1(D3DDevice_CreateVertexBuffer2,
       d3d_hle_device_create_vertex_buffer2),
    A1(D3DDevice_CreateIndexBuffer2,
       d3d_hle_device_create_index_buffer2, AX),
    B1(D3DVertexBuffer_Lock2, d3d_hle_vertex_buffer_lock2_std),
    B1(D3D_SetFence, d3d_hle_set_fence),
    B2(D3D_BlockOnTime, d3d_hle_block_on_time),
    A5(Lock3DSurface, d3d_hle_lock_3d_surface, ST, ST, ST, AX, ST),
    B3(D3DDevice_SetStreamSource,
       d3d_hle_device_set_stream_source_std),
    A2(D3DDevice_GetStreamSource2,
       d3d_hle_device_get_stream_source2, AX, DI),
    A2(D3DDevice_LoadVertexShader,
       d3d_hle_device_load_vertex_shader, AX, ST),
    B2(D3DDevice_LoadVertexShaderProgram,
       d3d_hle_device_load_vertex_shader_program),
    A2(D3DDevice_SelectVertexShader,
       d3d_hle_device_select_vertex_shader, AX, BX),
    B4(D3DDevice_CreateVertexShader,
       d3d_hle_device_create_vertex_shader),
    A1(D3DDevice_DeleteVertexShader,
       d3d_hle_device_delete_vertex_shader, AX),
    B1(D3DDevice_SetVertexShader, d3d_hle_device_set_vertex_shader),
    A2(D3DDevice_SetVertexShaderConstant1,
       d3d_hle_device_set_vertex_shader_constant1, CX, DX),
    A2(D3DDevice_SetVertexShaderConstant1Fast,
       d3d_hle_device_set_vertex_shader_constant1_fast, CX, DX),
    A2(D3DDevice_SetVertexShaderConstant4,
       d3d_hle_device_set_vertex_shader_constant4, CX, DX),
    A3(D3DDevice_SetVertexShaderConstantNotInline,
       d3d_hle_device_set_vertex_shader_constant_not_inline, BX, DX, AX),
    A3(D3DDevice_SetVertexShaderConstantNotInlineFast,
       d3d_hle_device_set_vertex_shader_constant_not_inline_fast,
       CX, DX, ST),
    B3(D3DDevice_SetVertexShaderInput,
       d3d_hle_device_set_vertex_shader_input),
    B3(D3DDevice_SetVertexShaderInputDirect,
       d3d_hle_device_set_vertex_shader_input_direct),
    B4(D3DDevice_DrawVerticesUP, d3d_hle_device_draw_vertices_up),
    A5(D3DDevice_DrawIndexedVerticesUP,
       d3d_hle_device_draw_indexed_vertices_up, ST, AX, ST, ST, ST),
    B3(D3DDevice_DrawVertices, d3d_hle_device_draw_vertices_std),
    B3(D3DDevice_DrawIndexedVertices,
       d3d_hle_device_draw_indexed_vertices),
    B3(D3DDevice_SetVertexData2f, d3d_hle_device_set_vertex_data2f),
    B5(D3DDevice_SetVertexData4f,
       d3d_hle_device_set_vertex_data4f_std),
    B3(D3DDevice_SetVertexData2s, d3d_hle_device_set_vertex_data2s),
    B2(D3DDevice_SetVertexDataColor,
       d3d_hle_device_set_vertex_data_color),
    B1(D3DDevice_Begin, d3d_hle_device_begin),
    B0(D3DDevice_End, d3d_hle_device_end),
    B2(D3DDevice_CreatePixelShader, d3d_hle_device_create_pixel_shader),
    A1(D3DDevice_DeletePixelShader,
       d3d_hle_device_delete_pixel_shader, AX),
    B1(D3DDevice_SetPixelShader, d3d_hle_device_set_pixel_shader_std),
    B3(D3DDevice_SetPixelShaderConstant,
       d3d_hle_device_set_pixel_shader_constant_std),
    B1(D3DDevice_SetRenderState_PSTextureModes,
       d3d_hle_device_set_render_state_ps_texture_modes),
    B1(D3DDevice_SetRenderState_EdgeAntiAlias,
       d3d_hle_device_set_render_state_edge_anti_alias),
    B1(D3DDevice_SetRenderState_ShadowFunc,
       d3d_hle_device_set_render_state_shadow_func),
    B1(D3DDevice_SetRenderState_FrontFace,
       d3d_hle_device_set_render_state_front_face),
    B1(D3DDevice_SetRenderState_NormalizeNormals,
       d3d_hle_device_set_render_state_normalize_normals),
    B1(D3DDevice_SetRenderState_LineWidth,
       d3d_hle_device_set_render_state_line_width),
    B1(D3DDevice_SetRenderState_LogicOp,
       d3d_hle_device_set_render_state_logic_op),
    B1(D3DDevice_SetRenderState_FillMode,
       d3d_hle_device_set_render_state_fill_mode),
    B1(D3DDevice_SetRenderState_BackFillMode,
       d3d_hle_device_set_render_state_back_fill_mode),
    B1(D3DDevice_SetRenderState_VertexBlend,
       d3d_hle_device_set_render_state_vertex_blend),
    B3(D3DDevice_SetDepthClipPlanes,
       d3d_hle_device_set_depth_clip_planes),
    B1(D3DDevice_SetRenderState_YuvEnable,
       d3d_hle_device_set_render_state_yuv_enable),
    B1(D3DDevice_SetRenderState_MultiSampleMode,
       d3d_hle_device_set_render_state_multisample_mode),
    B1(D3DDevice_SetRenderState_MultiSampleMask,
       d3d_hle_device_set_render_state_multisample_mask),
    B1(D3DDevice_SetRenderState_SampleAlpha,
       d3d_hle_device_set_render_state_sample_alpha),
    B5(D3DTexture_LockRect, d3d_hle_texture_lock_rect),
    B3(D3DCubeTexture_GetCubeMapSurface2,
       d3d_hle_cube_texture_get_cube_map_surface2),
    B6(D3DCubeTexture_LockRect, d3d_hle_cube_texture_lock_rect),
    B5(D3DVolumeTexture_LockBox, d3d_hle_volume_texture_lock_box),
    B3(D3DDevice_GetVisibilityTestResult,
       d3d_hle_device_get_visibility_test_result),
    B1(D3DDevice_SetFlickerFilter, d3d_hle_device_set_flicker_filter),
    B1(D3DDevice_SetSoftDisplayFilter,
       d3d_hle_device_set_soft_display_filter),
    B0(D3DDevice_BeginVisibilityTest,
       d3d_hle_device_begin_visibility_test),
    B1(D3DDevice_EndVisibilityTest,
       d3d_hle_device_end_visibility_test),
    B1(D3DDevice_GetDisplayFieldStatus,
       d3d_hle_device_get_display_field_status),
    B3(D3DDevice_SetScissors, d3d_hle_device_set_scissors),
    B2(D3DDevice_SetScreenSpaceOffset,
       d3d_hle_device_set_screen_space_offset),
    B4(D3DDevice_CreateSurface2, d3d_hle_device_create_surface2),
    B2(D3DPalette_Lock2, d3d_hle_palette_lock2),
    B1(D3DPalette_GetSize, d3d_hle_palette_get_size),
    B2(D3D_SetPushBufferSize, d3d_hle_direct3d_set_push_buffer_size),
    B6(Direct3D_CreateDevice, d3d_hle_direct3d_create_device_std),
    B3(Direct3D_CreateDevice, automatic_create_device_compact),
    /* MakeSpace returns title-owned push-buffer storage. The xemu bridge has
     * no safe synthetic guest allocator, so an automatic profile must leave
     * the allocation and command-stream ownership with the native XDK. */
    B0(D3DDevice_MakeSpace, NULL),
};

#undef ST
#undef AX
#undef BX
#undef CX
#undef DX
#undef BP
#undef SI
#undef DI

enum { XEMU_D3D_HLE_MAX_DISCOVERED_HOOKS = 384 };

typedef struct XemuD3DHleScan {
    XemuD3DHleHook hooks[XEMU_D3D_HLE_MAX_DISCOVERED_HOOKS];
    size_t hook_count;
    uint32_t recognized_functions;
    uint32_t unsupported_functions;
    uint32_t duplicate_functions;
    uint32_t unsupported_mutating_functions;
    uint32_t unsupported_native_safe_functions;
    uint32_t uncovered_abi_functions;
    uint32_t build_version;
    uint32_t device_global_va;
    uint32_t deferred_texture_state_va;
    uint32_t deferred_render_state_va;
    bool has_swap;
    bool has_draw;
} XemuD3DHleScan;

static XemuD3DHleScan *active_scan;
static XemuD3DHleGuestRead guest_read;
static XemuD3DHleProfile automatic_profile;
static XemuD3DHleHook automatic_hooks[XEMU_D3D_HLE_MAX_DISCOVERED_HOOKS];
static char automatic_name[160];

static const XemuD3DHleBinding *find_binding(const char *name, size_t length,
                                             unsigned param_count)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(bindings); ++i) {
        if (strlen(bindings[i].name) == length &&
            memcmp(bindings[i].name, name, length) == 0 &&
            bindings[i].param_count == param_count) {
            return &bindings[i];
        }
    }
    return NULL;
}

static size_t canonical_name_length(const char *name)
{
    const char *marker = strstr(name, "__LTCG");
    const char *cursor;

    if (!marker)
        return strlen(name);
    cursor = marker;
    while (cursor > name && isdigit((unsigned char)cursor[-1]))
        --cursor;
    if (cursor > name && cursor[-1] == '_')
        --cursor;
    return (size_t)(cursor - name);
}

static bool canonical_name_is(const char *name, size_t length,
                              const char *expected)
{
    return strlen(expected) == length &&
           memcmp(name, expected, length) == 0;
}

static bool discovery_name_is_exact(
    const char *name, size_t length, const char *expected)
{
    return strlen(expected) == length && memcmp(name, expected, length) == 0;
}

static bool discovery_name_has_prefix(
    const char *name, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    return length >= prefix_length &&
           memcmp(name, prefix, prefix_length) == 0;
}

static bool discovery_name_is_object_returning(
    const char *name, size_t length)
{
    static const char *const exact[] = {
        "D3DDevice_GetBackBuffer",
        "D3DDevice_GetBackBuffer2",
        "D3DDevice_GetRenderTarget",
        "D3DDevice_GetRenderTarget2",
        "D3DDevice_GetDepthStencilSurface",
        "D3DDevice_GetDepthStencilSurface2",
        "D3DDevice_GetPersistedSurface2",
        "D3DTexture_GetSurfaceLevel",
        "D3DTexture_GetSurfaceLevel2",
        "D3DCubeTexture_GetCubeMapSurface",
        "D3DCubeTexture_GetCubeMapSurface2",
        "D3D_CreateTexture",
        "D3D_CreateStandAloneSurface",
    };
    static const char *const prefixes[] = {
        "Direct3D_Create",
        "D3DDevice_Create",
        "D3D8_Lock",
        "D3DTexture_Lock",
        "D3DCubeTexture_Lock",
        "D3DVolumeTexture_Lock",
        "D3DSurface_Lock",
        "D3DVertexBuffer_Lock",
        "D3DPalette_Lock",
        "IDirect3DVertexBuffer8_Lock",
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(exact); ++i) {
        if (discovery_name_is_exact(name, length, exact[i]))
            return true;
    }
    for (i = 0; i < G_N_ELEMENTS(prefixes); ++i) {
        if (discovery_name_has_prefix(name, length, prefixes[i]))
            return true;
    }
    return false;
}

static bool discovery_name_is_native_safe(
    const char *name, size_t length)
{
    static const char *const exact[] = {
        "D3DDevice_AddRef",
        "D3DDevice_Release",
        "D3DResource_AddRef",
        "D3DResource_GetType",
        "D3DDevice_MakeSpace",
        "D3DDevice_IsFencePending",
        "D3D8_Get2DSurfaceDesc",
        "D3DBaseTexture_GetLevelCount",
    };
    static const char *const prefixes[] = {
        "D3D_Get",
        "D3D_Enum",
        "D3D_Check",
        "D3DResource_Get",
        "D3DBaseTexture_Get",
        "D3DSurface_Get",
        "D3DVertexBuffer_Get",
        "D3D_CMiniport_Get",
        "D3D_CMiniport_Is",
        "Direct3D_Check",
    };
    size_t i;

    if (discovery_name_is_object_returning(name, length))
        return false;
    for (i = 0; i < G_N_ELEMENTS(exact); ++i) {
        if (discovery_name_is_exact(name, length, exact[i]))
            return true;
    }
    for (i = 0; i < G_N_ELEMENTS(prefixes); ++i) {
        if (discovery_name_has_prefix(name, length, prefixes[i]))
            return true;
    }
    if (discovery_name_has_prefix(name, length, "D3DDevice_Get") ||
        discovery_name_has_prefix(name, length, "D3DTexture_Get"))
        return !discovery_name_is_object_returning(name, length);
    return false;
}

static void discovery_note_unsupported(
    XemuD3DHleScan *scan, const char *name, size_t length, bool abi)
{
    ++scan->unsupported_functions;
    if (abi)
        ++scan->uncovered_abi_functions;
    if (discovery_name_is_native_safe(name, length))
        ++scan->unsupported_native_safe_functions;
    else
        ++scan->unsupported_mutating_functions;
}

static uint8_t convert_param(XbSDBParamType type)
{
    switch (type) {
    case param_psh: return XEMU_D3D_ABI_STACK;
    case param_eax: return XEMU_D3D_ABI_EAX;
    case param_ebx: return XEMU_D3D_ABI_EBX;
    case param_ecx: return XEMU_D3D_ABI_ECX;
    case param_edx: return XEMU_D3D_ABI_EDX;
    case param_ebp: return XEMU_D3D_ABI_EBP;
    case param_esi: return XEMU_D3D_ABI_ESI;
    case param_edi: return XEMU_D3D_ABI_EDI;
    default: return XEMU_D3D_ABI_NONE;
    }
}

static void set_special(XemuD3DHleSpecialHooks *special,
                        const char *name, size_t length, uint32_t address)
{
#define SET(field_, symbol_)                                                \
    do {                                                                    \
        static const char text_[] = #symbol_;                               \
        if (length == sizeof(text_) - 1u &&                                 \
            memcmp(name, text_, sizeof(text_) - 1u) == 0)                   \
            special->field_ = address;                                      \
    } while (0)
    /* The automatic profile preserves native CreateDevice so helpers which
     * are not yet bound still see a valid XDK CDevice. Calls which allocate
     * or expose guest-owned objects also execute natively and are mirrored
    * after return; xemu must never manufacture pointers in the title heap. */
    SET(get_back_buffer, D3DDevice_GetBackBuffer2);
    SET(get_render_target, D3DDevice_GetRenderTarget2);
    SET(get_depth_stencil, D3DDevice_GetDepthStencilSurface2);
    SET(create_texture, D3DDevice_CreateTexture2);
    SET(create_surface, D3DDevice_CreateSurface2);
    SET(texture_get_surface_level, D3DTexture_GetSurfaceLevel2);
    SET(cube_get_surface_level, D3DCubeTexture_GetCubeMapSurface2);
    SET(texture_lock_rect, D3DTexture_LockRect);
    SET(cube_texture_lock_rect, D3DCubeTexture_LockRect);
    SET(volume_texture_lock_box, D3DVolumeTexture_LockBox);
    SET(set_texture, D3DDevice_SetTexture);
    SET(switch_texture, D3DDevice_SwitchTexture);
    SET(resource_release, D3DResource_Release);
    SET(surface_lock_rect, D3DSurface_LockRect);
    SET(create_device, Direct3D_CreateDevice);
    SET(create_vertex_buffer, D3DDevice_CreateVertexBuffer2);
    SET(create_index_buffer, D3DDevice_CreateIndexBuffer2);
    SET(lock_3d_surface, Lock3DSurface);
    SET(create_vertex_shader, D3DDevice_CreateVertexShader);
    SET(delete_vertex_shader, D3DDevice_DeleteVertexShader);
    SET(create_pixel_shader, D3DDevice_CreatePixelShader);
    SET(delete_pixel_shader, D3DDevice_DeletePixelShader);
#undef SET
}

static void register_symbol(const char *library_str, uint32_t library_flag,
                            uint32_t xref_index, const char *symbol_str,
                            xbaddr address, uint32_t build_version,
                            uint32_t symbol_type, uint32_t call_type,
                            uint32_t param_count,
                            const XbSDBSymbolParam *param_list)
{
    XemuD3DHleScan *scan = active_scan;
    const XemuD3DHleBinding *binding;
    XemuD3DHleHook *hook;
    size_t name_length;
    uint32_t stack_bytes = 0;
    size_t i;

    (void)library_str;
    (void)library_flag;
    (void)xref_index;
    if (!scan || !symbol_str || !address)
        return;
    scan->build_version = MAX(scan->build_version, build_version);
    if (symbol_type == symbol_variable) {
        if (strcmp(symbol_str, "D3D_g_pDevice") == 0)
            scan->device_global_va = address;
        else if (strcmp(symbol_str, "D3D_g_DeferredTextureState") == 0)
            scan->deferred_texture_state_va = address;
        else if (strcmp(symbol_str, "D3D_g_DeferredRenderState") == 0)
            scan->deferred_render_state_va = address;
        return;
    }
    if (symbol_type != symbol_function)
        return;

    /* XbSymbolDatabase spells a C `(void)` parameter list as one
     * zero-width param_void sentinel. It is ABI-equivalent to no arguments;
     * counting it as a real argument rejects every B0 binding, including
     * D3DDevice_End while D3DDevice_Begin remains active. */
    if (param_count == 1u && param_list &&
        param_list[0].type == param_void) {
        param_count = 0;
    }
    name_length = canonical_name_length(symbol_str);
    ++scan->recognized_functions;
    if (param_count > XEMU_D3D_HLE_MAX_ABI_ARGS) {
        discovery_note_unsupported(scan, symbol_str, name_length, true);
        return;
    }
    binding = find_binding(symbol_str, name_length, param_count);
    if (!binding) {
        discovery_note_unsupported(scan, symbol_str, name_length, false);
        return;
    }
    for (i = 0; i < param_count; ++i) {
        if (param_list[i].type == param_psh)
            stack_bytes += 4u;
        else if (param_list[i].type == param_psh2 ||
                 convert_param(param_list[i].type) == XEMU_D3D_ABI_NONE) {
            discovery_note_unsupported(scan, symbol_str, name_length, true);
            return;
        }
    }
    for (i = 0; i < scan->hook_count; ++i) {
        if (scan->hooks[i].address == address) {
            ++scan->duplicate_functions;
            return;
        }
    }
    if (scan->hook_count == G_N_ELEMENTS(scan->hooks)) {
        discovery_note_unsupported(scan, symbol_str, name_length, false);
        return;
    }
    hook = &scan->hooks[scan->hook_count++];
    memset(hook, 0, sizeof(*hook));
    hook->address = address;
    hook->entry = binding->entry;
    hook->name = binding->name;
    hook->automatic = 1;
    hook->source_param_count = param_count;
    hook->source_stack_bytes = stack_bytes;
    hook->source_caller_cleanup = call_type == call_cdecl;
    hook->target_param_count = binding->param_count;
    memcpy(hook->target_params, binding->params,
           sizeof(hook->target_params));
    for (i = 0; i < param_count; ++i)
        hook->source_params[i] = convert_param(param_list[i].type);
    set_special(&automatic_profile.special, symbol_str, name_length, address);
    if (canonical_name_is(symbol_str, name_length,
                          "Direct3D_CreateDevice")) {
        automatic_profile.create_device_parameters_arg =
            param_count == 3u ? 1u : 4u;
    }
    if (canonical_name_is(symbol_str, name_length, "D3DDevice_Swap"))
        scan->has_swap = true;
    if (canonical_name_is(symbol_str, name_length,
                          "D3DDevice_DrawVertices") ||
        canonical_name_is(symbol_str, name_length,
                          "D3DDevice_DrawIndexedVertices") ||
        canonical_name_is(symbol_str, name_length,
                          "D3DDevice_DrawVerticesUP") ||
        canonical_name_is(symbol_str, name_length,
                          "D3DDevice_DrawIndexedVerticesUP")) {
        scan->has_draw = true;
    }
}

static int compare_hooks(const void *left, const void *right)
{
    const XemuD3DHleHook *a = left;
    const XemuD3DHleHook *b = right;
    return a->address < b->address ? -1 : a->address > b->address ? 1 : 0;
}

static bool add_lazy_set_state(XemuD3DHleScan *scan, uint8_t *image,
                               const xbe_header *header,
                               const xbe_section_header *sections,
                               uint32_t image_end,
                               uint32_t *dirty_flags_va)
{
    size_t section_index;

    for (section_index = 0; section_index < header->dwSections;
         ++section_index) {
        const xbe_section_header *section = &sections[section_index];
        uint32_t start = section->dwVirtualAddr;
        uint32_t size = section->dwSizeofRaw;
        uint32_t offset;

        if (!(section->dwFlags_value & XBE_SECTION_HEADER_FLAGS_PRELOAD) ||
            !(section->dwFlags_value & XBE_SECTION_HEADER_FLAGS_EXECUTABLE) ||
            start >= image_end || size > image_end - start)
            continue;
        for (offset = 0; offset + 24u <= size; ++offset) {
            const uint8_t *p = image + start + offset;
            uint32_t dirty;
            uint32_t device;
            size_t look;
            bool has_combiner_dispatch = false;

            if (p[0] != 0x53 || p[1] != 0x8B || p[2] != 0x1D ||
                p[7] != 0xF6 || p[8] != 0xC7 || p[9] != 0x01 ||
                p[10] != 0x56 || p[11] != 0x8B || p[12] != 0x35)
                continue;
            memcpy(&dirty, p + 3, sizeof(dirty));
            memcpy(&device, p + 13, sizeof(device));
            if (!dirty || !device || dirty == device ||
                (scan->device_global_va && device != scan->device_global_va))
                continue;
            for (look = 17; look + 2 < MIN((uint32_t)96, size - offset);
                 ++look) {
                if (p[look] == 0xF6 &&
                    (p[look + 1] == 0xC7 || p[look + 1] == 0xC3) &&
                    p[look + 2] == 0x08) {
                    has_combiner_dispatch = true;
                    break;
                }
            }
            if (!has_combiner_dispatch ||
                scan->hook_count == G_N_ELEMENTS(scan->hooks))
                continue;
            scan->hooks[scan->hook_count++] = (XemuD3DHleHook) {
                .address = start + offset,
                .entry = d3d_hle_lazy_set_state,
                .name = "D3D::LazySetState",
                .automatic = 1,
            };
            *dirty_flags_va = dirty;
            return true;
        }
    }
    return false;
}

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity)
        g_strlcpy(error, message, capacity);
}

static size_t snapshot_mapped_pages(
    XemuD3DHleGuestRead read_guest, uint32_t address,
    uint8_t *destination, uint32_t size)
{
    uint32_t offset = 0;
    size_t copied = 0;

    while (offset < size) {
        uint32_t page_remaining =
            0x1000u - ((address + offset) & 0x0FFFu);
        uint32_t chunk = MIN(page_remaining, size - offset);

        if (read_guest(address + offset, destination + offset, chunk)) {
            copied += chunk;
        }
        offset += chunk;
    }
    return copied;
}

static bool discovery_section_name_is(
    XemuD3DHleGuestRead read_guest, const xbe_section_header *section,
    const char *expected)
{
    char name[9] = { 0 };
    size_t length = strlen(expected);

    if (!section || !section->SectionNameAddr || length > 8u ||
        !read_guest(section->SectionNameAddr, name, sizeof(name) - 1u))
        return false;
    return g_ascii_strncasecmp(name, expected, length) == 0 &&
           (length == 8u || name[length] == '\0');
}

static bool discovery_section_is_scan_candidate(
    XemuD3DHleGuestRead read_guest, const xbe_section_header *section)
{
    return (section->dwFlags_value &
            XBE_SECTION_HEADER_FLAGS_EXECUTABLE) ||
           discovery_section_name_is(read_guest, section, "D3D") ||
           discovery_section_name_is(read_guest, section, ".text") ||
           discovery_section_name_is(read_guest, section, "FLASHROM");
}

static bool discovery_section_is_named_or_kernel(
    XemuD3DHleGuestRead read_guest, const xbe_section_header *section,
    xbaddr kernel_thunk)
{
    uint32_t end = section->dwVirtualAddr + section->dwSizeofRaw;

    return discovery_section_name_is(read_guest, section, "D3D") ||
           discovery_section_name_is(read_guest, section, ".text") ||
           discovery_section_name_is(read_guest, section, "FLASHROM") ||
           (kernel_thunk >= section->dwVirtualAddr && kernel_thunk < end);
}

static uint8_t *discovery_sparse_alloc(size_t size)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *memory = mmap(NULL, size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
#endif
}

static bool discovery_sparse_commit(uint8_t *base,
                                    uint32_t offset, size_t size)
{
    const size_t page_size = 4096u;
    uintptr_t start;
    uintptr_t end;

    if (!base || !size)
        return false;
    start = ((uintptr_t)base + offset) & ~(page_size - 1u);
    end = ((uintptr_t)base + offset + size + page_size - 1u) &
          ~(page_size - 1u);
#ifdef _WIN32
    return VirtualAlloc((void *)start, end - start,
                        MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    return mprotect((void *)start, end - start,
                    PROT_READ | PROT_WRITE) == 0;
#endif
}

static void discovery_sparse_free(uint8_t *base, size_t size)
{
    if (!base)
        return;
#ifdef _WIN32
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, size);
#endif
}

const XemuD3DHleProfile *xemu_d3d_hle_discover(
    XemuD3DHleGuestRead read_guest, bool *retryable,
    char *error, size_t error_capacity)
{
    xbe_header header;
    xbe_header *snapshot_header;
    xbe_section_header *sections;
    xbe_certificate *certificate = NULL;
    XbSDBLibraryHeader libraries = { 0 };
    XbSDBSectionHeader scan_sections = { 0 };
    XbSDBContextHandle context = NULL;
    XemuD3DHleScan scan = { 0 };
    uint8_t *image = NULL;
    uint32_t dirty_flags_va = 0;
    uint32_t image_end;
    unsigned d3d_libraries = 0;
    unsigned incomplete_sections = 0;
    size_t copied_section_bytes = 0;
    size_t missing_section_bytes = 0;
    size_t *section_copied = NULL;
    size_t i;
    char *title = NULL;
    bool ok = false;

    memset(&automatic_profile, 0, sizeof(automatic_profile));
    if (retryable)
        *retryable = false;
    if (!read_guest || !read_guest(0x00010000u, &header, sizeof(header)) ||
        header.dwMagic != 0x48454258u || header.dwBaseAddr != 0x00010000u) {
        set_error(error, error_capacity, "loaded XBE header is not ready");
        return NULL;
    }
    if (!header.dwSizeofImage || header.dwSizeofImage > 0x08000000u ||
        header.dwSizeofImage > UINT32_MAX - header.dwBaseAddr ||
        header.dwSizeofHeaders < sizeof(header) ||
        header.dwSizeofHeaders > 0x00100000u ||
        header.dwSizeofHeaders > header.dwSizeofImage ||
        !header.dwSections || header.dwSections > 256u) {
        set_error(error, error_capacity, "loaded XBE dimensions are invalid");
        return NULL;
    }

    /* dwSizeofImage is a length beginning at dwBaseAddr, not an absolute
     * virtual end address. Keep the snapshot VA-indexed for XbSDB. */
    image_end = header.dwBaseAddr + header.dwSizeofImage;
    image = discovery_sparse_alloc(image_end);
    if (!image ||
        !discovery_sparse_commit(image, header.dwBaseAddr,
                                 header.dwSizeofHeaders) ||
        !read_guest(header.dwBaseAddr, image + header.dwBaseAddr,
                    header.dwSizeofHeaders)) {
        set_error(error, error_capacity, "cannot snapshot XBE headers");
        goto out;
    }
    snapshot_header = (xbe_header *)(image + header.dwBaseAddr);
    if (header.dwSections >
            header.dwSizeofHeaders / sizeof(xbe_section_header) ||
        header.pSectionHeadersAddr < header.dwBaseAddr ||
        header.pSectionHeadersAddr - header.dwBaseAddr >
            header.dwSizeofHeaders -
                header.dwSections * sizeof(xbe_section_header)) {
        set_error(error, error_capacity, "XBE section table is outside headers");
        goto out;
    }
    sections = (xbe_section_header *)(image + header.pSectionHeadersAddr);
    section_copied = g_new0(size_t, header.dwSections);
    if (!section_copied) {
        set_error(error, error_capacity,
                  "cannot allocate XBE section coverage table");
        goto out;
    }
    for (i = 0; i < header.dwSections; ++i) {
        uint32_t address = sections[i].dwVirtualAddr;
        uint32_t size = sections[i].dwSizeofRaw;
        size_t copied;

        if (!size || !discovery_section_is_scan_candidate(
                read_guest, &sections[i])) {
            continue;
        }
        if (address < header.dwBaseAddr || address >= image_end ||
            size > image_end - address) {
            if (error && error_capacity) {
                g_snprintf(error, error_capacity,
                           "preload section %zu (%08X+%X) exceeds XBE image",
                           i, address, size);
            }
            goto out;
        }
        if (!discovery_sparse_commit(image, address, size)) {
            set_error(error, error_capacity,
                      "cannot commit XBE section snapshot");
            goto out;
        }
        copied = snapshot_mapped_pages(
            read_guest, address, image + address, size);
        section_copied[i] = copied;
        copied_section_bytes += copied;
        if (copied != size) {
            ++incomplete_sections;
            missing_section_bytes += size - copied;
        }
    }
    if (!copied_section_bytes) {
        if (retryable)
            *retryable = true;
        set_error(error, error_capacity,
                  "no mapped XBE section pages were available to scan");
        goto out;
    }
    if (incomplete_sections) {
        fprintf(stderr,
                "[D3D-HLE] XBE snapshot: retained %zu mapped bytes; "
                "%zu unavailable bytes across %u preload section%s "
                "left zero-filled\n",
                copied_section_bytes, missing_section_bytes,
                incomplete_sections,
                incomplete_sections == 1u ? "" : "s");
    }
    libraries.count = XbSDB_GenerateLibraryFilter(snapshot_header, NULL);
    if (!libraries.count) {
        set_error(error, error_capacity, "XBE has no discoverable XDK libraries");
        goto out;
    }
    libraries.filters = g_new0(XbSDBLibrary, libraries.count);
    XbSDB_GenerateLibraryFilter(snapshot_header, &libraries);
    for (i = 0; i < libraries.count; ++i) {
        if (libraries.filters[i].flag &
            (XBSDBLIB_D3D8 | XBSDBLIB_D3D8LTCG)) {
            libraries.filters[d3d_libraries++] = libraries.filters[i];
        }
    }
    libraries.count = d3d_libraries;
    if (!libraries.count) {
        set_error(error, error_capacity, "XBE does not link a known D3D8 XDK");
        goto out;
    }

    fprintf(stderr,
            "[D3D-HLE] scanning loaded XBE against %u linked D3D8 "
            "library record%s\n",
            libraries.count, libraries.count == 1u ? "" : "s");

    scan_sections.count = 0;
    {
        xbaddr kernel_thunk = XbSDB_GetKernelThunkAddress(snapshot_header);
        for (i = 0; i < header.dwSections; ++i) {
            if (section_copied[i] >= sections[i].dwSizeofRaw &&
                discovery_section_is_named_or_kernel(
                    read_guest, &sections[i], kernel_thunk))
                ++scan_sections.count;
        }
    }
    if (!scan_sections.count) {
        set_error(error, error_capacity, "XBE has no D3D-scannable sections");
        goto out;
    }
    scan_sections.filters = g_new0(XbSDBSection, scan_sections.count);
    {
        xbaddr kernel_thunk = XbSDB_GetKernelThunkAddress(snapshot_header);
        size_t filter_index = 0;
        for (i = 0; i < header.dwSections; ++i) {
            XbSDBSection *filter;

            if (section_copied[i] < sections[i].dwSizeofRaw ||
                !discovery_section_is_named_or_kernel(
                    read_guest, &sections[i], kernel_thunk))
                continue;
            filter = &scan_sections.filters[filter_index++];
            memset(filter, 0, sizeof(*filter));
            memcpy(filter->name, "        ", sizeof(filter->name));
            if (sections[i].SectionNameAddr)
                (void)read_guest(sections[i].SectionNameAddr,
                                  filter->name, sizeof(filter->name));
            filter->xb_virt_addr = sections[i].dwVirtualAddr;
            filter->buffer_size = sections[i].dwSizeofRaw;
            filter->buffer_lower = image + sections[i].dwVirtualAddr;
        }
    }
    if (!XbSDB_CreateContext(&context, register_symbol, libraries,
                             scan_sections,
                             XbSDB_GetKernelThunkAddress(snapshot_header)) ||
        !XbSDBContext_RegisterLibrary(
            context, XBSDBLIB_D3D8 | XBSDBLIB_D3D8LTCG)) {
        set_error(error, error_capacity, "cannot initialize D3D signature scan");
        goto out;
    }

    active_scan = &scan;
    XbSDBContext_ScanManual(context);
    XbSDBContext_ScanAllLibraryFilter(context);
    XbSDBContext_RegisterXRefs(context);
    active_scan = NULL;
    XbSDBContext_Release(context);
    context = NULL;

    (void)add_lazy_set_state(&scan, image, snapshot_header, sections,
                             image_end,
                             &dirty_flags_va);
    if (!automatic_profile.special.create_device || !scan.has_swap ||
        !scan.has_draw) {
        bool loader_snapshot = missing_section_bytes != 0;

        if (retryable && loader_snapshot)
            *retryable = true;
        set_error(error, error_capacity,
                  loader_snapshot
                      ? "D3D core is not available in the loader snapshot yet"
                      : "D3D scan did not resolve CreateDevice, Swap, and a draw path");
        goto out;
    }

    qsort(scan.hooks, scan.hook_count, sizeof(scan.hooks[0]), compare_hooks);
    memcpy(automatic_hooks, scan.hooks,
           scan.hook_count * sizeof(scan.hooks[0]));
    if (header.pCertificateAddr >= header.dwBaseAddr &&
        header.pCertificateAddr <=
            image_end - sizeof(xbe_certificate)) {
        certificate = (xbe_certificate *)(image + header.pCertificateAddr);
        title = g_utf16_to_utf8((const gunichar2 *)certificate->wszTitleName,
                                40, NULL, NULL, NULL);
    }
    g_snprintf(automatic_name, sizeof(automatic_name),
               "Automatic XDK D3D8%s%s (build %u)",
               title && title[0] ? ": " : "",
               title && title[0] ? title : "",
               scan.build_version);

    automatic_profile.name = automatic_name;
    automatic_profile.source_xbe_sha256 = "runtime-signature-discovery";
    automatic_profile.reviewed_required_hook_count = scan.hook_count;
    automatic_profile.reviewed_implemented_hook_count = scan.hook_count;
    automatic_profile.reviewed_blocker_count = scan.unsupported_functions;
    automatic_profile.discovery_recognized_count =
        scan.recognized_functions;
    automatic_profile.discovery_unsupported_count =
        scan.unsupported_functions;
    automatic_profile.discovery_duplicate_count = scan.duplicate_functions;
    automatic_profile.discovery_mutating_uncovered_count =
        scan.unsupported_mutating_functions;
    automatic_profile.discovery_uncovered_abi_count =
        scan.uncovered_abi_functions;
    automatic_profile.xbe_base = header.dwBaseAddr;
    automatic_profile.xbe_headers_size = header.dwSizeofHeaders;
    automatic_profile.xbe_image_size = header.dwSizeofImage;
    automatic_profile.xbe_timestamp = header.dwTimeDate;
    automatic_profile.xbe_title_id = certificate ? certificate->dwTitleId : 0;
    automatic_profile.xbe_section_count = header.dwSections;
    automatic_profile.dirty_flags_va = dirty_flags_va;
    automatic_profile.deferred_texture_state_va =
        scan.deferred_texture_state_va;
    automatic_profile.fog_state_va = scan.deferred_render_state_va;
    automatic_profile.bootstrap = XEMU_D3D_HLE_BOOTSTRAP_MIRROR_NATIVE;
    automatic_profile.hooks = automatic_hooks;
    automatic_profile.hook_count = scan.hook_count;
    guest_read = read_guest;
    fprintf(stderr,
            "[D3D-HLE] automatic scan: XDK=%u recognized=%u bound=%zu "
            "unsupported=%u duplicates=%u core=create/swap/draw lazy=%s\n",
            scan.build_version, scan.recognized_functions, scan.hook_count,
            scan.unsupported_functions, scan.duplicate_functions,
            dirty_flags_va ? "yes" : "no");
    ok = true;
out:
    active_scan = NULL;
    if (context)
        XbSDBContext_Release(context);
    g_free(scan_sections.filters);
    g_free(libraries.filters);
    g_free(section_copied);
    g_free(title);
    discovery_sparse_free(image, image_end);
    return ok ? &automatic_profile : NULL;
}

static uint32_t register_value(uint8_t location)
{
    switch (location) {
    case XEMU_D3D_ABI_EAX: return g_eax;
    case XEMU_D3D_ABI_EBX: return g_ebx;
    case XEMU_D3D_ABI_ECX: return g_ecx;
    case XEMU_D3D_ABI_EDX: return g_edx;
    case XEMU_D3D_ABI_EBP: return g_ebp;
    case XEMU_D3D_ABI_ESI: return g_esi;
    case XEMU_D3D_ABI_EDI: return g_edi;
    default: return 0;
    }
}

static void set_register_value(uint8_t location, uint32_t value)
{
    switch (location) {
    case XEMU_D3D_ABI_EAX: g_eax = value; break;
    case XEMU_D3D_ABI_EBX: g_ebx = value; break;
    case XEMU_D3D_ABI_ECX: g_ecx = value; break;
    case XEMU_D3D_ABI_EDX: g_edx = value; break;
    case XEMU_D3D_ABI_EBP: g_ebp = value; break;
    case XEMU_D3D_ABI_ESI: g_esi = value; break;
    case XEMU_D3D_ABI_EDI: g_edi = value; break;
    default: break;
    }
}

bool xemu_d3d_hle_discovered_argument(
    const XemuD3DHleHook *hook, unsigned index, uint32_t *value)
{
    unsigned stack_index = 0;
    unsigned i;

    if (!hook || !hook->automatic || index >= hook->source_param_count ||
        !value)
        return false;
    for (i = 0; i <= index; ++i) {
        uint8_t location = hook->source_params[i];
        if (i == index) {
            if (location == XEMU_D3D_ABI_STACK) {
                *value = d3d_hle_guest_stack_u32(stack_index);
                return true;
            }
            if (location != XEMU_D3D_ABI_NONE) {
                *value = register_value(location);
                return true;
            }
            return false;
        }
        if (location == XEMU_D3D_ABI_STACK)
            ++stack_index;
    }
    return false;
}

bool xemu_d3d_hle_invoke_discovered(const XemuD3DHleHook *hook)
{
    uint32_t args[XEMU_D3D_HLE_MAX_ABI_ARGS] = { 0 };
    uint32_t saved_stack[1 + XEMU_D3D_HLE_MAX_ABI_ARGS] = { 0 };
    uint32_t original_regs[7] = {
        g_ebx, g_ecx, g_edx, g_ebp, g_esi, g_edi, g_esp,
    };
    uint32_t original_esp = g_esp;
    uint32_t return_pc;
    uint32_t fake_esp;
    uint32_t result;
    unsigned target_stack_count = 0;
    unsigned stack_index = 0;
    unsigned i;

    if (!hook || !hook->automatic || !hook->entry || !guest_read ||
        hook->source_param_count != hook->target_param_count ||
        !guest_read(original_esp, &return_pc, sizeof(return_pc)))
        return false;
    for (i = 0; i < hook->source_param_count; ++i) {
        if (!xemu_d3d_hle_discovered_argument(hook, i, &args[i]))
            return false;
        if (hook->target_params[i] == XEMU_D3D_ABI_STACK)
            ++target_stack_count;
    }
    fake_esp = (original_esp - 32u -
                4u * (1u + target_stack_count)) & ~15u;
    if (fake_esp >= original_esp ||
        !guest_read(fake_esp, saved_stack,
                    4u * (1u + target_stack_count)))
        return false;
    *(uint32_t *)xbox_guest_ptr(fake_esp) = return_pc;
    for (i = 0; i < hook->target_param_count; ++i) {
        if (hook->target_params[i] == XEMU_D3D_ABI_STACK) {
            *(uint32_t *)xbox_guest_ptr(
                fake_esp + 4u + 4u * stack_index++) = args[i];
        } else {
            set_register_value(hook->target_params[i], args[i]);
        }
    }
    g_esp = fake_esp;
    hook->entry();
    result = g_eax;
    for (i = 0; i < 1u + target_stack_count; ++i)
        *(uint32_t *)xbox_guest_ptr(fake_esp + i * 4u) = saved_stack[i];

    g_ebx = original_regs[0];
    g_ecx = original_regs[1];
    g_edx = original_regs[2];
    g_ebp = original_regs[3];
    g_esi = original_regs[4];
    g_edi = original_regs[5];
    g_eax = result;
    g_esp = original_esp + 4u +
        (hook->source_caller_cleanup ? 0u : hook->source_stack_bytes);
    return true;
}
