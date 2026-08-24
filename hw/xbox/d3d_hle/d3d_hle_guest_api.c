#include "d3d_hle_guest.h"

#include <stdlib.h>

extern uint32_t g_eax;
extern uint32_t g_ebx;
extern uint32_t g_ecx;
extern uint32_t g_edx;
extern uint32_t g_esi;
extern uint32_t g_edi;

#if defined(XRECOMP_D3D_HLE_TEST_NO_FALLBACKS)
#define HLE_FALLBACK(symbol)                                                \
    do {                                                                    \
        (void)sizeof(#symbol);                                              \
        if (!d3d_hle_guest_native_active())                                 \
            abort();                                                        \
    } while (0)
#else
#define HLE_FALLBACK(symbol)                                                \
    do {                                                                    \
        if (!d3d_hle_guest_native_active()) {                               \
            symbol##_gen_unused();                                          \
            return;                                                         \
        }                                                                   \
    } while (0)
#endif

#define RET(value) d3d_hle_guest_return_u32((uint32_t)(value))

extern void d3d_hle_device_get_display_mode_gen_unused(void);
void d3d_hle_device_get_display_mode(void)
{
    uint32_t output;
    HLE_FALLBACK(d3d_hle_device_get_display_mode);
    output = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_get_display_mode(output);
    RET(S_OK);
}

extern void d3d_hle_device_reset_gen_unused(void);
void d3d_hle_device_reset(void)
{
    uint32_t parameters = g_edi;
    HLE_FALLBACK(d3d_hle_device_reset);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_reset(parameters));
}

extern void d3d_hle_device_set_render_target_gen_unused(void);
void d3d_hle_device_set_render_target(void)
{
    uint32_t color = g_ecx;
    uint32_t depth = g_eax;
    HLE_FALLBACK(d3d_hle_device_set_render_target);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_set_render_target(color, depth, 0));
}

extern void d3d_hle_device_get_back_buffer2_gen_unused(void);
void d3d_hle_device_get_back_buffer2(void)
{
    uint32_t index = g_eax;
    HLE_FALLBACK(d3d_hle_device_get_back_buffer2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_back_buffer(index));
}

extern void d3d_hle_device_set_gamma_ramp_gen_unused(void);
void d3d_hle_device_set_gamma_ramp(void)
{
    uint32_t flags;
    uint32_t ramp;
    HLE_FALLBACK(d3d_hle_device_set_gamma_ramp);
    flags = d3d_hle_guest_stack_u32(0);
    ramp = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    d3d_hle_guest_set_gamma_ramp(flags, ramp);
}

extern void d3d_hle_device_get_gamma_ramp_gen_unused(void);
void d3d_hle_device_get_gamma_ramp(void)
{
    uint32_t ramp = g_edx;
    HLE_FALLBACK(d3d_hle_device_get_gamma_ramp);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_get_gamma_ramp(ramp);
}

extern void d3d_hle_device_copy_rects_gen_unused(void);
void d3d_hle_device_copy_rects(void)
{
    uint32_t a[5];
    unsigned i;
    HLE_FALLBACK(d3d_hle_device_copy_rects);
    for (i = 0; i < 5; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(20);
    RET(d3d_hle_guest_copy_rects(a[0], a[1], a[2], a[3], a[4]));
}

extern void d3d_hle_device_get_render_target2_gen_unused(void);
void d3d_hle_device_get_render_target2(void)
{
    HLE_FALLBACK(d3d_hle_device_get_render_target2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_render_target());
}

extern void d3d_hle_device_get_depth_stencil_surface2_gen_unused(void);
void d3d_hle_device_get_depth_stencil_surface2(void)
{
    HLE_FALLBACK(d3d_hle_device_get_depth_stencil_surface2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_depth_stencil());
}

extern void d3d_hle_device_set_transform_gen_unused(void);
void d3d_hle_device_set_transform(void)
{
    uint32_t state = g_eax;
    uint32_t matrix = g_edx;
    HLE_FALLBACK(d3d_hle_device_set_transform);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_set_transform(state, matrix));
}

extern void d3d_hle_device_get_transform_gen_unused(void);
void d3d_hle_device_get_transform(void)
{
    uint32_t state = g_eax;
    uint32_t matrix = g_edx;
    HLE_FALLBACK(d3d_hle_device_get_transform);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_transform(state, matrix));
}

extern void d3d_hle_device_set_viewport_gen_unused(void);
void d3d_hle_device_set_viewport(void)
{
    uint32_t viewport;
    HLE_FALLBACK(d3d_hle_device_set_viewport);
    viewport = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_set_viewport(viewport));
}

extern void d3d_hle_device_get_viewport_gen_unused(void);
void d3d_hle_device_get_viewport(void)
{
    uint32_t viewport = g_edx;
    HLE_FALLBACK(d3d_hle_device_get_viewport);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_viewport(viewport));
}

extern void d3d_hle_device_get_texture2_gen_unused(void);
void d3d_hle_device_get_texture2(void)
{
    uint32_t stage = g_esi;
    HLE_FALLBACK(d3d_hle_device_get_texture2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_texture(stage));
}

extern void d3d_hle_device_set_texture_gen_unused(void);
void d3d_hle_device_set_texture(void)
{
    uint32_t stage = g_eax;
    uint32_t texture;
    HLE_FALLBACK(d3d_hle_device_set_texture);
    texture = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_set_texture(stage, texture));
}

extern void d3d_hle_device_get_indices2_gen_unused(void);
void d3d_hle_device_get_indices2(void)
{
    uint32_t base_output = g_edi;
    HLE_FALLBACK(d3d_hle_device_get_indices2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_indices(base_output));
}

extern void d3d_hle_device_add_ref_gen_unused(void);
void d3d_hle_device_add_ref(void)
{
    HLE_FALLBACK(d3d_hle_device_add_ref);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_device_add_ref());
}

extern void d3d_hle_device_release_gen_unused(void);
void d3d_hle_device_release(void)
{
    HLE_FALLBACK(d3d_hle_device_release);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_device_release());
}

extern void d3d_hle_device_block_until_vertical_blank_gen_unused(void);
void d3d_hle_device_block_until_vertical_blank(void)
{
    HLE_FALLBACK(d3d_hle_device_block_until_vertical_blank);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_block_until_vertical_blank();
}

#define DEFINE_STACK_STATE_WRAPPER(symbol, state_value)                    \
    extern void symbol##_gen_unused(void);                                  \
    void symbol(void)                                                       \
    {                                                                       \
        uint32_t value;                                                     \
        HLE_FALLBACK(symbol);                                               \
        value = d3d_hle_guest_stack_u32(0);                                 \
        d3d_hle_guest_stdcall_return(4);                                    \
        d3d_hle_guest_set_render_state((state_value), value);               \
    }

DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_stencil_enable,
    D3DRS_STENCILENABLE)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_texture_factor,
    D3DRS_TEXTUREFACTOR)

/* Xbox-only policy states: retain a title-neutral shadow slot. */
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_dxt1_noise_enable,
    (D3DRENDERSTATETYPE)300)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_occlusion_cull_enable,
    (D3DRENDERSTATETYPE)301)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_stencil_cull_enable,
    (D3DRENDERSTATETYPE)302)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_rop_z_cmp_always_read,
    (D3DRENDERSTATETYPE)303)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_rop_z_read,
    (D3DRENDERSTATETYPE)304)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_do_not_cull_uncompressed,
    (D3DRENDERSTATETYPE)305)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_two_sided_lighting,
    (D3DRENDERSTATETYPE)306)
DEFINE_STACK_STATE_WRAPPER(
    d3d_hle_device_set_render_state_multisample_render_target_mode,
    (D3DRENDERSTATETYPE)307)

extern void d3d_hle_common_set_render_target_gen_unused(void);
void d3d_hle_common_set_render_target(void)
{
    uint32_t color;
    uint32_t depth;
    uint32_t viewport;
    HLE_FALLBACK(d3d_hle_common_set_render_target);
    color = d3d_hle_guest_stack_u32(0);
    depth = d3d_hle_guest_stack_u32(1);
    viewport = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    RET(d3d_hle_guest_set_render_target(color, depth, viewport));
}

extern void d3d_hle_base_texture_get_level_count_gen_unused(void);
void d3d_hle_base_texture_get_level_count(void)
{
    uint32_t texture;
    HLE_FALLBACK(d3d_hle_base_texture_get_level_count);
    texture = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_base_texture_get_level_count(texture));
}

extern void d3d_hle_cube_texture_get_cube_map_surface2_gen_unused(void);
void d3d_hle_cube_texture_get_cube_map_surface2(void)
{
    uint32_t texture;
    uint32_t face;
    uint32_t level;
    HLE_FALLBACK(d3d_hle_cube_texture_get_cube_map_surface2);
    texture = d3d_hle_guest_stack_u32(0);
    face = d3d_hle_guest_stack_u32(1);
    level = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    RET(d3d_hle_guest_cube_get_surface(texture, face, level));
}

extern void d3d_hle_get_2d_surface_desc_gen_unused(void);
void d3d_hle_get_2d_surface_desc(void)
{
    uint32_t resource;
    uint32_t level;
    uint32_t desc;
    HLE_FALLBACK(d3d_hle_get_2d_surface_desc);
    resource = d3d_hle_guest_stack_u32(0);
    level = d3d_hle_guest_stack_u32(1);
    desc = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    d3d_hle_guest_surface_desc(resource, level, desc);
}

extern void d3d_hle_volume_texture_get_level_desc_gen_unused(void);
void d3d_hle_volume_texture_get_level_desc(void)
{
    uint32_t texture;
    uint32_t level;
    uint32_t desc;
    HLE_FALLBACK(d3d_hle_volume_texture_get_level_desc);
    texture = d3d_hle_guest_stack_u32(0);
    level = d3d_hle_guest_stack_u32(1);
    desc = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    d3d_hle_guest_volume_get_level_desc(texture, level, desc);
}

extern void d3d_hle_resource_register_gen_unused(void);
void d3d_hle_resource_register(void)
{
    uint32_t resource = g_ecx;
    uint32_t base = g_edx;
    HLE_FALLBACK(d3d_hle_resource_register);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_resource_register(resource, base));
}

extern void d3d_hle_create_surface_with_contiguous_header_gen_unused(void);
void d3d_hle_create_surface_with_contiguous_header(void)
{
    uint32_t width;
    uint32_t height;
    uint32_t format;
    HLE_FALLBACK(d3d_hle_create_surface_with_contiguous_header);
    width = d3d_hle_guest_stack_u32(0);
    height = d3d_hle_guest_stack_u32(1);
    format = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    RET(d3d_hle_guest_create_surface(width, height, format));
}

extern void d3d_hle_device_clear_gen_unused(void);
void d3d_hle_device_clear(void)
{
    uint32_t a[6];
    unsigned i;
    HLE_FALLBACK(d3d_hle_device_clear);
    for (i = 0; i < 6; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(24);
    RET(d3d_hle_guest_clear(
        a[0], a[1], a[2], a[3], a[4], a[5]));
}

extern void d3d_hle_device_update_overlay_gen_unused(void);
void d3d_hle_device_update_overlay(void)
{
    uint32_t source_rect = g_eax;
    uint32_t surface;
    uint32_t destination_rect;
    uint32_t enable_color_key;
    uint32_t color_key;
    HLE_FALLBACK(d3d_hle_device_update_overlay);
    surface = d3d_hle_guest_stack_u32(0);
    destination_rect = d3d_hle_guest_stack_u32(1);
    enable_color_key = d3d_hle_guest_stack_u32(2);
    color_key = d3d_hle_guest_stack_u32(3);
    d3d_hle_guest_stdcall_return(16);
    RET(d3d_hle_guest_update_overlay(
        surface, source_rect, 0, destination_rect, color_key,
        enable_color_key));
}

extern void d3d_hle_device_enable_overlay_gen_unused(void);
void d3d_hle_device_enable_overlay(void)
{
    uint32_t enable;
    HLE_FALLBACK(d3d_hle_device_enable_overlay);
    enable = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_enable_overlay(enable);
}

extern void d3d_hle_device_swap_gen_unused(void);
void d3d_hle_device_swap(void)
{
    uint32_t flags = g_eax;
    HLE_FALLBACK(d3d_hle_device_swap);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_swap(flags));
}

extern void d3d_hle_device_set_back_buffer_scale_gen_unused(void);
void d3d_hle_device_set_back_buffer_scale(void)
{
    uint32_t horizontal;
    uint32_t vertical;
    HLE_FALLBACK(d3d_hle_device_set_back_buffer_scale);
    horizontal = d3d_hle_guest_stack_u32(0);
    vertical = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    RET(d3d_hle_guest_set_back_buffer_scale(horizontal, vertical));
}

extern void d3d_hle_direct3d_check_device_format_gen_unused(void);
void d3d_hle_direct3d_check_device_format(void)
{
    uint32_t adapter = g_edx;
    uint32_t check_format = g_ecx;
    uint32_t adapter_format = g_esi;
    uint32_t device_type;
    uint32_t usage;
    uint32_t resource_type;
    HLE_FALLBACK(d3d_hle_direct3d_check_device_format);
    device_type = d3d_hle_guest_stack_u32(0);
    usage = d3d_hle_guest_stack_u32(1);
    resource_type = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    RET(d3d_hle_guest_check_device_format(
        adapter, device_type, adapter_format, usage, resource_type,
        check_format));
}

extern void d3d_hle_direct3d_create_device_gen_unused(void);
void d3d_hle_direct3d_create_device(void)
{
    uint32_t behavior_flags = g_eax;
    uint32_t output = g_ecx;
    uint32_t adapter;
    uint32_t device_type;
    uint32_t focus_window;
    uint32_t parameters;
    HLE_FALLBACK(d3d_hle_direct3d_create_device);
    adapter = d3d_hle_guest_stack_u32(0);
    device_type = d3d_hle_guest_stack_u32(1);
    focus_window = d3d_hle_guest_stack_u32(2);
    parameters = d3d_hle_guest_stack_u32(3);
    d3d_hle_guest_stdcall_return(16);
    RET(d3d_hle_guest_create_device(
        adapter, device_type, focus_window, behavior_flags,
        parameters, output));
}

extern void d3d_hle_device_create_vertex_buffer2_gen_unused(void);
void d3d_hle_device_create_vertex_buffer2(void)
{
    uint32_t length;
    HLE_FALLBACK(d3d_hle_device_create_vertex_buffer2);
    length = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_create_vertex_buffer(length));
}

extern void d3d_hle_device_create_index_buffer2_gen_unused(void);
void d3d_hle_device_create_index_buffer2(void)
{
    uint32_t length = g_eax;
    HLE_FALLBACK(d3d_hle_device_create_index_buffer2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_create_index_buffer(length));
}

extern void d3d_hle_set_fence_gen_unused(void);
void d3d_hle_set_fence(void)
{
    uint32_t flags;
    HLE_FALLBACK(d3d_hle_set_fence);
    flags = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_set_fence(flags));
}

extern void d3d_hle_block_on_time_gen_unused(void);
void d3d_hle_block_on_time(void)
{
    uint32_t fence;
    uint32_t flags;
    HLE_FALLBACK(d3d_hle_block_on_time);
    fence = d3d_hle_guest_stack_u32(0);
    flags = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    d3d_hle_guest_block_on_time(fence, flags);
}

extern void d3d_hle_lock_2d_surface_gen_unused(void);
void d3d_hle_lock_2d_surface(void)
{
    uint32_t rect = g_eax;
    uint32_t locked = g_esi;
    uint32_t texture;
    uint32_t face;
    uint32_t level;
    uint32_t flags;
    HLE_FALLBACK(d3d_hle_lock_2d_surface);
    texture = d3d_hle_guest_stack_u32(0);
    face = d3d_hle_guest_stack_u32(1);
    level = d3d_hle_guest_stack_u32(2);
    flags = d3d_hle_guest_stack_u32(3);
    d3d_hle_guest_stdcall_return(16);
    d3d_hle_guest_lock_2d_surface(
        texture, face, level, locked, rect, flags);
}

extern void d3d_hle_lock_3d_surface_gen_unused(void);
void d3d_hle_lock_3d_surface(void)
{
    uint32_t box = g_eax;
    uint32_t texture;
    uint32_t level;
    uint32_t locked;
    uint32_t flags;
    HLE_FALLBACK(d3d_hle_lock_3d_surface);
    texture = d3d_hle_guest_stack_u32(0);
    level = d3d_hle_guest_stack_u32(1);
    locked = d3d_hle_guest_stack_u32(2);
    flags = d3d_hle_guest_stack_u32(3);
    d3d_hle_guest_stdcall_return(16);
    d3d_hle_guest_lock_3d_surface(
        texture, level, locked, box, flags);
}

extern void d3d_hle_device_get_stream_source2_gen_unused(void);
void d3d_hle_device_get_stream_source2(void)
{
    uint32_t stream = g_eax;
    uint32_t stride_output = g_edi;
    HLE_FALLBACK(d3d_hle_device_get_stream_source2);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_get_stream_source(stream, stride_output));
}

extern void d3d_hle_device_select_vertex_shader_gen_unused(void);
void d3d_hle_device_select_vertex_shader(void)
{
    uint32_t handle = g_eax;
    uint32_t address = g_ebx;
    HLE_FALLBACK(d3d_hle_device_select_vertex_shader);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_select_vertex_shader(handle, address);
}

extern void d3d_hle_device_load_vertex_shader_program_gen_unused(void);
void d3d_hle_device_load_vertex_shader_program(void)
{
    uint32_t function;
    uint32_t address;
    HLE_FALLBACK(d3d_hle_device_load_vertex_shader_program);
    function = d3d_hle_guest_stack_u32(0);
    address = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    RET(d3d_hle_guest_load_vertex_shader_program(function, address));
}

extern void d3d_hle_device_delete_vertex_shader_gen_unused(void);
void d3d_hle_device_delete_vertex_shader(void)
{
    uint32_t handle = g_eax;
    HLE_FALLBACK(d3d_hle_device_delete_vertex_shader);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_delete_vertex_shader(handle);
}

extern void d3d_hle_device_set_vertex_shader_gen_unused(void);
void d3d_hle_device_set_vertex_shader(void)
{
    uint32_t handle;
    HLE_FALLBACK(d3d_hle_device_set_vertex_shader);
    /* MM3's reviewed 354-byte body is stdcall with one stack argument. */
    handle = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_set_vertex_shader(handle));
}

extern void d3d_hle_device_set_vertex_shader_constant_not_inline_gen_unused(
    void);
void d3d_hle_device_set_vertex_shader_constant_not_inline(void)
{
    uint32_t dword_count = g_eax;
    uint32_t data = g_edx;
    uint32_t start = g_ebx;
    HLE_FALLBACK(d3d_hle_device_set_vertex_shader_constant_not_inline);
    d3d_hle_guest_stdcall_return(0);
    /*
     * The XDK's internal not-inline helper receives the payload size in
     * DWORDs.  The host D3D8 API instead expects a count of float4
     * registers.  Its callers multiply the register count by four before
     * entering this helper.
     */
    d3d_hle_guest_set_vertex_shader_constant(
        (int32_t)start, data, dword_count / 4u);
}

extern void d3d_hle_device_draw_vertices_up_gen_unused(void);
void d3d_hle_device_draw_vertices_up(void)
{
    uint32_t a[4];
    unsigned i;
    HLE_FALLBACK(d3d_hle_device_draw_vertices_up);
    for (i = 0; i < 4; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(16);
    d3d_hle_guest_draw_vertices_up(a[0], a[1], a[2], a[3]);
}

extern void d3d_hle_device_draw_indexed_vertices_up_gen_unused(void);
void d3d_hle_device_draw_indexed_vertices_up(void)
{
    uint32_t a[5];
    unsigned i;
    HLE_FALLBACK(d3d_hle_device_draw_indexed_vertices_up);
    for (i = 0; i < 5; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(20);
    d3d_hle_guest_draw_indexed_vertices_up(
        a[0], a[1], a[2], a[3], a[4]);
}

extern void d3d_hle_device_set_vertex_data4f_gen_unused(void);
void d3d_hle_device_set_vertex_data4f(void)
{
    uint32_t reg = g_edi;
    uint32_t a[4];
    unsigned i;
    HLE_FALLBACK(d3d_hle_device_set_vertex_data4f);
    for (i = 0; i < 4; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(16);
    d3d_hle_guest_set_vertex_data4f(reg, a[0], a[1], a[2], a[3]);
}

extern void d3d_hle_device_delete_pixel_shader_gen_unused(void);
void d3d_hle_device_delete_pixel_shader(void)
{
    uint32_t handle = g_eax;
    HLE_FALLBACK(d3d_hle_device_delete_pixel_shader);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_delete_pixel_shader(handle);
}

extern void d3d_hle_device_set_pixel_shader_gen_unused(void);
void d3d_hle_device_set_pixel_shader(void)
{
    uint32_t handle = g_eax;
    HLE_FALLBACK(d3d_hle_device_set_pixel_shader);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_set_pixel_shader(handle));
}

extern void d3d_hle_device_set_pixel_shader_constant_gen_unused(void);
void d3d_hle_device_set_pixel_shader_constant(void)
{
    uint32_t count = g_eax;
    uint32_t start = g_ecx;
    uint32_t data;
    HLE_FALLBACK(d3d_hle_device_set_pixel_shader_constant);
    data = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_set_pixel_shader_constant(start, data, count);
}

extern void d3d_hle_kickoff_and_wait_for_idle_gen_unused(void);
void d3d_hle_kickoff_and_wait_for_idle(void)
{
    HLE_FALLBACK(d3d_hle_kickoff_and_wait_for_idle);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_kickoff_and_wait_for_idle();
}

/* Public stdcall XDK entry points used by the reviewed 5788-era profile. */
extern void d3d_hle_device_begin_gen_unused(void);
void d3d_hle_device_begin(void)
{
    uint32_t primitive;
    HLE_FALLBACK(d3d_hle_device_begin);
    primitive = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_begin(primitive);
}

extern void d3d_hle_device_end_gen_unused(void);
void d3d_hle_device_end(void)
{
    HLE_FALLBACK(d3d_hle_device_end);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_end();
}

#define DEFINE_STACK_WRAPPER3(symbol, call_expr)                            \
    extern void symbol##_gen_unused(void);                                  \
    void symbol(void)                                                       \
    {                                                                       \
        uint32_t a[3];                                                      \
        unsigned i;                                                         \
        HLE_FALLBACK(symbol);                                               \
        for (i = 0; i < 3; ++i)                                            \
            a[i] = d3d_hle_guest_stack_u32(i);                              \
        d3d_hle_guest_stdcall_return(12);                                   \
        call_expr;                                                          \
    }

DEFINE_STACK_WRAPPER3(d3d_hle_device_set_vertex_data2f,
    d3d_hle_guest_set_vertex_data2f(a[0], a[1], a[2]))
DEFINE_STACK_WRAPPER3(d3d_hle_device_set_vertex_data2s,
    d3d_hle_guest_set_vertex_data2s(a[0], a[1], a[2]))

extern void d3d_hle_device_set_vertex_data_color_gen_unused(void);
void d3d_hle_device_set_vertex_data_color(void)
{
    uint32_t reg;
    uint32_t color;
    HLE_FALLBACK(d3d_hle_device_set_vertex_data_color);
    reg = d3d_hle_guest_stack_u32(0);
    color = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    d3d_hle_guest_set_vertex_data_color(reg, color);
}

extern void d3d_hle_device_begin_visibility_test_gen_unused(void);
void d3d_hle_device_begin_visibility_test(void)
{
    HLE_FALLBACK(d3d_hle_device_begin_visibility_test);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_begin_visibility_test();
}

extern void d3d_hle_device_end_visibility_test_gen_unused(void);
void d3d_hle_device_end_visibility_test(void)
{
    uint32_t index;
    HLE_FALLBACK(d3d_hle_device_end_visibility_test);
    index = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_end_visibility_test(index));
}

DEFINE_STACK_WRAPPER3(d3d_hle_device_get_visibility_test_result,
    RET(d3d_hle_guest_get_visibility_test_result(a[0], a[1], a[2])))

extern void d3d_hle_device_get_display_field_status_gen_unused(void);
void d3d_hle_device_get_display_field_status(void)
{
    uint32_t status;
    HLE_FALLBACK(d3d_hle_device_get_display_field_status);
    status = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_get_display_field_status(status);
}

DEFINE_STACK_WRAPPER3(d3d_hle_device_set_scissors,
    d3d_hle_guest_set_scissors(a[0], a[1], a[2]))

extern void d3d_hle_device_set_screen_space_offset_gen_unused(void);
void d3d_hle_device_set_screen_space_offset(void)
{
    uint32_t x;
    uint32_t y;
    HLE_FALLBACK(d3d_hle_device_set_screen_space_offset);
    x = d3d_hle_guest_stack_u32(0);
    y = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    d3d_hle_guest_set_screen_space_offset(x, y);
}

#define DEFINE_IGNORED_STACK_WRAPPER(symbol, bytes)                         \
    extern void symbol##_gen_unused(void);                                  \
    void symbol(void)                                                       \
    {                                                                       \
        HLE_FALLBACK(symbol);                                               \
        d3d_hle_guest_stdcall_return(bytes);                                \
    }

DEFINE_IGNORED_STACK_WRAPPER(d3d_hle_device_set_depth_clip_planes, 12)
DEFINE_IGNORED_STACK_WRAPPER(d3d_hle_device_set_flicker_filter, 4)
DEFINE_IGNORED_STACK_WRAPPER(d3d_hle_device_set_soft_display_filter, 4)
DEFINE_IGNORED_STACK_WRAPPER(d3d_hle_direct3d_set_push_buffer_size, 8)

#define DEFINE_SINGLE_STATE_WRAPPER(symbol, call_expr)                      \
    extern void symbol##_gen_unused(void);                                  \
    void symbol(void)                                                       \
    {                                                                       \
        uint32_t value;                                                     \
        HLE_FALLBACK(symbol);                                               \
        value = d3d_hle_guest_stack_u32(0);                                 \
        d3d_hle_guest_stdcall_return(4);                                    \
        call_expr;                                                          \
    }

DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_fill_mode,
    d3d_hle_guest_set_fill_mode(value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_front_face,
    d3d_hle_guest_set_front_face(value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_logic_op,
    d3d_hle_guest_set_logic_op(value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_vertex_blend,
    d3d_hle_guest_set_vertex_blend(value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_edge_anti_alias,
    d3d_hle_guest_set_render_state(D3DRS_EDGEANTIALIAS, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_multisample_mask,
    d3d_hle_guest_set_render_state(D3DRS_MULTISAMPLEMASK, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_normalize_normals,
    d3d_hle_guest_set_render_state(D3DRS_NORMALIZENORMALS, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_ps_texture_modes,
    d3d_hle_guest_set_render_state(D3DRS_PSTEXTUREMODES, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_back_fill_mode,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)308, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_line_width,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)309, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_sample_alpha,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)310, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_shadow_func,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)311, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_yuv_enable,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)312, value))
DEFINE_SINGLE_STATE_WRAPPER(d3d_hle_device_set_render_state_multisample_mode,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)313, value))

extern void d3d_hle_device_set_vertex_shader_constant1_fast_gen_unused(void);
void d3d_hle_device_set_vertex_shader_constant1_fast(void)
{
    HLE_FALLBACK(d3d_hle_device_set_vertex_shader_constant1_fast);
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_set_vertex_shader_constant_hardware(g_ecx, g_edx, 1);
}

extern void
d3d_hle_device_set_vertex_shader_constant_not_inline_fast_gen_unused(void);
void d3d_hle_device_set_vertex_shader_constant_not_inline_fast(void)
{
    uint32_t dword_count;
    HLE_FALLBACK(d3d_hle_device_set_vertex_shader_constant_not_inline_fast);
    dword_count = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_set_vertex_shader_constant_hardware(
        g_ecx, g_edx, dword_count / 4u);
}

DEFINE_STACK_WRAPPER3(d3d_hle_device_set_vertex_shader_input,
    RET(d3d_hle_guest_set_vertex_shader_input(a[0], a[1], a[2])))
DEFINE_STACK_WRAPPER3(d3d_hle_device_set_vertex_shader_input_direct,
    RET(d3d_hle_guest_set_vertex_shader_input_direct(a[1], a[2])))

extern void d3d_hle_palette_get_size_gen_unused(void);
void d3d_hle_palette_get_size(void)
{
    uint32_t palette;
    HLE_FALLBACK(d3d_hle_palette_get_size);
    palette = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_palette_size(palette));
}

extern void d3d_hle_palette_lock2_gen_unused(void);
void d3d_hle_palette_lock2(void)
{
    uint32_t palette;
    uint32_t flags;
    HLE_FALLBACK(d3d_hle_palette_lock2);
    palette = d3d_hle_guest_stack_u32(0);
    flags = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    RET(d3d_hle_guest_palette_lock2(palette, flags));
}

extern void d3d_hle_resource_add_ref_gen_unused(void);
void d3d_hle_resource_add_ref(void)
{
    uint32_t resource;
    HLE_FALLBACK(d3d_hle_resource_add_ref);
    resource = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    RET(d3d_hle_guest_resource_add_ref(resource));
}

extern void d3d_hle_texture_lock_rect_gen_unused(void);
void d3d_hle_texture_lock_rect(void)
{
    uint32_t a[5];
    unsigned i;
    HLE_FALLBACK(d3d_hle_texture_lock_rect);
    for (i = 0; i < 5; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(20);
    d3d_hle_guest_lock_2d_surface(a[0], 0, a[1], a[2], a[3], a[4]);
}

extern void d3d_hle_device_create_surface2_gen_unused(void);
void d3d_hle_device_create_surface2(void)
{
    uint32_t a[4];
    unsigned i;
    HLE_FALLBACK(d3d_hle_device_create_surface2);
    for (i = 0; i < 4; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(16);
    RET(d3d_hle_guest_create_surface2(a[0], a[1], a[2], a[3]));
}

extern void d3d_hle_device_make_space_gen_unused(void);
void d3d_hle_device_make_space(void)
{
    HLE_FALLBACK(d3d_hle_device_make_space);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_make_space());
}

extern void d3d_hle_cube_texture_lock_rect_gen_unused(void);
void d3d_hle_cube_texture_lock_rect(void)
{
    uint32_t a[6];
    unsigned i;
    HLE_FALLBACK(d3d_hle_cube_texture_lock_rect);
    for (i = 0; i < 6; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(24);
    d3d_hle_guest_lock_2d_surface(a[0], a[1], a[2], a[3], a[4], a[5]);
}

extern void d3d_hle_volume_texture_lock_box_gen_unused(void);
void d3d_hle_volume_texture_lock_box(void)
{
    uint32_t a[5];
    unsigned i;
    HLE_FALLBACK(d3d_hle_volume_texture_lock_box);
    for (i = 0; i < 5; ++i)
        a[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(20);
    d3d_hle_guest_lock_3d_surface(a[0], a[1], a[2], a[3], a[4]);
}

#define DEFINE_STD_WRAPPER(symbol, nargs, call_expr)                        \
    extern void symbol##_gen_unused(void);                                  \
    void symbol(void)                                                       \
    {                                                                       \
        uint32_t a[(nargs) > 0 ? (nargs) : 1];                              \
        unsigned i;                                                         \
        HLE_FALLBACK(symbol);                                               \
        for (i = 0; i < (nargs); ++i)                                      \
            a[i] = d3d_hle_guest_stack_u32(i);                              \
        (void)a;                                                            \
        d3d_hle_guest_stdcall_return((nargs) * 4u);                         \
        call_expr;                                                          \
    }

DEFINE_STD_WRAPPER(d3d_hle_device_set_texture_state_border_color_std, 2,
    d3d_hle_guest_set_texture_stage_state(a[0], D3DTSS_BORDERCOLOR, a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_texture_state_bump_env_std, 3,
    d3d_hle_guest_set_bump_env(a[0], a[1], a[2]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_texture_state_color_key_std, 2,
    d3d_hle_guest_set_color_key(a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_block_on_resource_std, 1,
    d3d_hle_guest_block_on_resource(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_shader_constant_mode_std, 1,
    d3d_hle_guest_set_shader_constant_mode(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_lock_2d_surface_std, 6,
    d3d_hle_guest_lock_2d_surface(
        a[0], a[1], a[2], a[3], a[4], a[5]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_tile_std, 2,
    d3d_hle_guest_set_tile(a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_get_tile_std, 2,
    d3d_hle_guest_get_tile(a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_vertex_data4ub_std, 5,
    d3d_hle_guest_set_vertex_data4ub(
        a[0], a[1], a[2], a[3], a[4]))

/* Spy-census called holes: shared automatic-attach wrappers. */
DEFINE_STD_WRAPPER(d3d_hle_device_set_render_state_not_inline_std, 2,
    d3d_hle_guest_set_render_state((D3DRENDERSTATETYPE)a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_block_on_fence_std, 1,
    d3d_hle_guest_block_on_fence(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_resource_block_until_not_busy_std, 1,
    d3d_hle_guest_block_until_not_busy(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_material_std, 1,
    (void)d3d_hle_guest_set_material(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_light_std, 2,
    (void)d3d_hle_guest_set_light(a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_light_enable_std, 2,
    (void)d3d_hle_guest_light_enable(a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_pixel_shader_program_std, 1,
    (void)d3d_hle_guest_set_pixel_shader_program(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_swap_callback_std, 1,
    d3d_hle_guest_set_swap_callback(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_vertical_blank_callback_std, 1,
    d3d_hle_guest_set_vertical_blank_callback(a[0]))
DEFINE_STD_WRAPPER(d3d_hle_device_insert_callback_std, 3,
    d3d_hle_guest_insert_callback(a[0], a[1], a[2]))
DEFINE_STD_WRAPPER(d3d_hle_lazy_set_point_params_std, 1,
    d3d_hle_guest_lazy_set_point_params(a[0]))

/* Both return a value to the guest, so they cannot use the void wrapper. */
extern void d3d_hle_device_insert_fence_std_gen_unused(void);
void d3d_hle_device_insert_fence_std(void)
{
    HLE_FALLBACK(d3d_hle_device_insert_fence_std);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_insert_fence());
}

extern void d3d_hle_device_is_busy_std_gen_unused(void);
void d3d_hle_device_is_busy_std(void)
{
    HLE_FALLBACK(d3d_hle_device_is_busy_std);
    d3d_hle_guest_stdcall_return(0);
    RET(d3d_hle_guest_is_busy());
}
DEFINE_STD_WRAPPER(d3d_hle_device_set_palette_std, 2,
    RET(d3d_hle_guest_set_palette(a[0], a[1])))
DEFINE_STD_WRAPPER(d3d_hle_resource_get_type_std, 1,
    RET(d3d_hle_guest_resource_type(a[0])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_stream_source_std, 3,
    d3d_hle_guest_set_stream_source(a[0], a[1], a[2]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_indices_std, 2,
    d3d_hle_guest_set_indices(a[0], a[1]))
DEFINE_STD_WRAPPER(d3d_hle_device_draw_vertices_std, 3,
    d3d_hle_guest_draw_vertices(a[0], a[1], a[2]))
DEFINE_STD_WRAPPER(d3d_hle_resource_is_busy_std, 1,
    RET(d3d_hle_guest_resource_is_busy(a[0])))
DEFINE_STD_WRAPPER(d3d_hle_vertex_buffer_lock2_std, 2,
    RET(d3d_hle_guest_vertex_buffer_lock2(a[0], a[1])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_render_target_std, 2,
    RET(d3d_hle_guest_set_render_target(a[0], a[1], 0)))
DEFINE_STD_WRAPPER(d3d_hle_device_get_back_buffer2_std, 1,
    RET(d3d_hle_guest_get_back_buffer(a[0])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_transform_std, 2,
    RET(d3d_hle_guest_set_transform(a[0], a[1])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_texture_std, 2,
    RET(d3d_hle_guest_set_texture(a[0], a[1])))
DEFINE_STD_WRAPPER(d3d_hle_resource_register_std, 2,
    RET(d3d_hle_guest_resource_register(a[0], a[1])))
DEFINE_STD_WRAPPER(d3d_hle_device_swap_std, 1,
    RET(d3d_hle_guest_swap(a[0])))
DEFINE_STD_WRAPPER(d3d_hle_direct3d_create_device_std, 6,
    RET(d3d_hle_guest_create_device(a[0], a[1], a[2], a[3], a[4], a[5])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_vertex_data4f_std, 5,
    d3d_hle_guest_set_vertex_data4f(a[0], a[1], a[2], a[3], a[4]))
DEFINE_STD_WRAPPER(d3d_hle_device_set_pixel_shader_std, 1,
    RET(d3d_hle_guest_set_pixel_shader(a[0])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_pixel_shader_constant_std, 3,
    d3d_hle_guest_set_pixel_shader_constant(a[0], a[1], a[2]))
DEFINE_STD_WRAPPER(d3d_hle_device_reset_std, 1,
    RET(d3d_hle_guest_reset(a[0])))
DEFINE_STD_WRAPPER(d3d_hle_device_set_render_target_fast_std, 3,
    RET(d3d_hle_guest_set_render_target(a[0], a[1], 0)))
