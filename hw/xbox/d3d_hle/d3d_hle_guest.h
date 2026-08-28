/*
 * Shared guest-ABI support for signature-resolved Xbox D3D HLE hooks.
 *
 * Native hook wrappers keep the lifted body as <symbol>_gen_unused and
 * delegate to it whenever the compatibility NV2A frontend is active. This
 * lets one generated binary retain the known-good path while the HLE
 * inventory is completed.
 */
#ifndef XRECOMP_D3D_HLE_GUEST_H
#define XRECOMP_D3D_HLE_GUEST_H

#include "d3d8_xbox.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*D3DHleGuestFatalDiagnostic)(void);
typedef int (*D3DHleGuestReadRange)(
    uint32_t va, void *output, size_t size);

void d3d_hle_guest_set_fatal_diagnostic(
    D3DHleGuestFatalDiagnostic diagnostic);
void d3d_hle_guest_set_read_range(D3DHleGuestReadRange reader);
int d3d_hle_guest_native_active(void);
HRESULT d3d_hle_guest_start_host_device(
    uint32_t parameters_va, uintptr_t native_window);
HRESULT d3d_hle_guest_restart_host_device(uintptr_t native_window);
HRESULT d3d_hle_guest_register_vertex_buffer(
    uint32_t object_va, uint32_t length);
HRESULT d3d_hle_guest_register_index_buffer(
    uint32_t object_va, uint32_t length);
HRESULT d3d_hle_guest_register_native_resource(uint32_t object_va);
HRESULT d3d_hle_guest_register_vertex_shader(
    uint32_t declaration_va, uint32_t program_va, uint32_t shader_handle,
    uint32_t usage);
HRESULT d3d_hle_guest_register_vertex_shader_snapshot(
    const uint32_t *declaration, size_t declaration_dwords,
    const uint32_t *program, size_t program_dwords,
    uint32_t source_program_va, uint32_t shader_handle, uint32_t usage);
HRESULT d3d_hle_guest_register_pixel_shader(
    const uint32_t definition[60], uint32_t shader_handle);
void d3d_hle_guest_unregister_vertex_shader(uint32_t shader_handle);
void d3d_hle_guest_unregister_pixel_shader(uint32_t shader_handle);
uint32_t d3d_hle_guest_stack_u32(unsigned argument_index);
void d3d_hle_guest_stdcall_return(unsigned argument_bytes);
void d3d_hle_guest_return_u32(uint32_t value);
void d3d_hle_guest_write_u32(uint32_t va, uint32_t value);
void d3d_hle_guest_get_device_caps(uint32_t caps_va);

enum {
    XRECOMP_XBOX_D3DCULL_NONE = 0x000,
    XRECOMP_XBOX_D3DCULL_CW = 0x900,
    XRECOMP_XBOX_D3DCULL_CCW = 0x901,
};

enum {
    XRECOMP_XBOX_D3DRTYPE_NONE = 0,
    XRECOMP_XBOX_D3DRTYPE_SURFACE = 1,
    XRECOMP_XBOX_D3DRTYPE_VOLUME = 2,
    XRECOMP_XBOX_D3DRTYPE_TEXTURE = 3,
    XRECOMP_XBOX_D3DRTYPE_VOLUMETEXTURE = 4,
    XRECOMP_XBOX_D3DRTYPE_CUBETEXTURE = 5,
    XRECOMP_XBOX_D3DRTYPE_VERTEXBUFFER = 6,
    XRECOMP_XBOX_D3DRTYPE_INDEXBUFFER = 7,
    XRECOMP_XBOX_D3DRTYPE_PALETTE = 8,
    XRECOMP_XBOX_D3DRTYPE_PUSHBUFFER = 9,
    XRECOMP_XBOX_D3DRTYPE_FIXUP = 10,
};

enum {
    /* Internal normalized slot for NV097_SET_BLEND_COLOR. */
    XRECOMP_D3DRS_BLEND_COLOR = 199,
    /*
     * Internal state slots used to carry Xbox polygon-offset floats through
     * the D3D8 compatibility device into Plume. They occupy the two unused
     * tail entries of the device's 256-entry state array.
     */
    XRECOMP_D3DRS_DEPTH_BIAS_CONSTANT = 254,
    XRECOMP_D3DRS_DEPTH_BIAS_SLOPE = 255,
};

uint32_t d3d_hle_guest_resource_type(uint32_t resource_va);
void d3d_hle_guest_surface_desc(
    uint32_t resource_va, uint32_t level, uint32_t desc_va);
void d3d_hle_guest_set_cull_mode(uint32_t xbox_cull_mode);
void d3d_hle_guest_set_front_face(uint32_t xbox_front_face);
void d3d_hle_guest_set_fill_mode(uint32_t xbox_fill_mode);
void d3d_hle_guest_set_render_state(D3DRENDERSTATETYPE state,
                                    uint32_t value);
void d3d_hle_guest_set_stipple(uint32_t pattern_va);
void d3d_hle_guest_set_texture_stage_state(
    uint32_t stage, D3DTEXTURESTAGESTATETYPE state, uint32_t value);
void d3d_hle_guest_set_vertex_shader_constant(
    int32_t start_register, uint32_t constant_data_va, uint32_t count);
void d3d_hle_guest_set_vertex_shader_constant_hardware(
    uint32_t start_register, uint32_t constant_data_va, uint32_t count);
void d3d_hle_guest_set_shader_constant_mode(uint32_t mode);
void d3d_hle_guest_set_stream_source(
    uint32_t stream, uint32_t resource_va, uint32_t stride);
void d3d_hle_guest_set_indices(
    uint32_t resource_va, uint32_t base_vertex);
void d3d_hle_guest_draw_vertices(
    D3DPRIMITIVETYPE primitive_type, uint32_t start_vertex,
    uint32_t vertex_count);
void d3d_hle_guest_draw_indexed_vertices(
    D3DPRIMITIVETYPE primitive_type, uint32_t index_count,
    uint32_t indices_va);
uint32_t d3d_hle_guest_resource_is_busy(uint32_t resource_va);
void d3d_hle_guest_switch_texture(
    uint32_t method_header, uint32_t data, uint32_t format);
int d3d_hle_guest_adopt_switch_texture(
    uint32_t texture_va, uint32_t data, uint32_t format);
void d3d_hle_guest_set_z_enable(uint32_t value);
void d3d_hle_guest_set_stencil_fail(uint32_t value);
void d3d_hle_guest_set_z_bias(uint32_t value);
void d3d_hle_guest_set_multisample_antialias(uint32_t value);
void d3d_hle_guest_set_simple(uint32_t method_header, uint32_t value);
void d3d_hle_guest_set_texcoord_index(uint32_t stage, uint32_t value);
uint32_t d3d_hle_guest_create_texture2(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t levels,
    uint32_t usage, uint32_t format, uint32_t resource_type);
uint32_t d3d_hle_guest_texture_get_surface_level2(
    uint32_t texture_va, uint32_t level);
uint32_t d3d_hle_guest_resource_release(uint32_t resource_va);
void d3d_hle_guest_destroy_resource_by_va(uint32_t resource_va);
void d3d_hle_guest_note_native_resource_release(uint32_t resource_va);
void d3d_hle_guest_surface_lock_rect(
    uint32_t surface_va, uint32_t locked_rect_va, uint32_t rect_va,
    uint32_t flags);
void d3d_hle_guest_prepare_surface_cpu_lock(uint32_t surface_va);
void d3d_hle_guest_note_surface_cpu_write(
    uint32_t surface_va, uint32_t flags);
uint32_t d3d_hle_guest_vertex_buffer_lock2(
    uint32_t vertex_buffer_va, uint32_t flags);
void d3d_hle_guest_load_vertex_shader(
    uint32_t shader_handle, uint32_t address);
HRESULT d3d_hle_guest_load_vertex_shader_program(
    uint32_t function_va, uint32_t address);
HRESULT d3d_hle_guest_create_vertex_shader(
    uint32_t declaration_va, uint32_t program_va, uint32_t output_va,
    uint32_t usage);
HRESULT d3d_hle_guest_create_pixel_shader(
    uint32_t definition_va, uint32_t output_va);
void d3d_hle_guest_get_display_mode(uint32_t mode_va);
HRESULT d3d_hle_guest_reset(uint32_t parameters_va);
HRESULT d3d_hle_guest_set_render_target(
    uint32_t color_va, uint32_t depth_va, uint32_t viewport_va);
uint32_t d3d_hle_guest_get_back_buffer(uint32_t index);
HRESULT d3d_hle_guest_mirror_back_buffer(uint32_t object_va);
uint32_t d3d_hle_guest_get_render_target(void);
uint32_t d3d_hle_guest_get_depth_stencil(void);
void d3d_hle_guest_set_gamma_ramp(uint32_t flags, uint32_t ramp_va);
void d3d_hle_guest_get_gamma_ramp(uint32_t ramp_va);
HRESULT d3d_hle_guest_copy_rects(
    uint32_t source_va, uint32_t rects_va, uint32_t count,
    uint32_t destination_va, uint32_t points_va);
HRESULT d3d_hle_guest_set_transform(uint32_t state, uint32_t matrix_va);
HRESULT d3d_hle_guest_get_transform(uint32_t state, uint32_t matrix_va);
HRESULT d3d_hle_guest_set_viewport(uint32_t viewport_va);
HRESULT d3d_hle_guest_get_viewport(uint32_t viewport_va);
uint32_t d3d_hle_guest_get_texture(uint32_t stage);
HRESULT d3d_hle_guest_set_texture(uint32_t stage, uint32_t texture_va);
HRESULT d3d_hle_guest_set_palette(uint32_t stage, uint32_t palette_va);
uint32_t d3d_hle_guest_get_indices(uint32_t base_vertex_va);
uint32_t d3d_hle_guest_get_stream_source(
    uint32_t stream, uint32_t stride_va);
uint32_t d3d_hle_guest_device_add_ref(void);
uint32_t d3d_hle_guest_device_release(void);
bool d3d_hle_guest_synthetic_allocator_available(void);
void d3d_hle_guest_reset_session(void);
void d3d_hle_guest_reset_registry(void);
void d3d_hle_guest_teardown_host_device(void);
int d3d_hle_guest_vblank_scanout(void);
uint32_t d3d_hle_guest_base_texture_get_level_count(uint32_t texture_va);
uint32_t d3d_hle_guest_cube_get_surface(
    uint32_t texture_va, uint32_t face, uint32_t level);
void d3d_hle_guest_volume_get_level_desc(
    uint32_t texture_va, uint32_t level, uint32_t desc_va);
HRESULT d3d_hle_guest_resource_register(
    uint32_t resource_va, uint32_t base_va);
uint32_t d3d_hle_guest_create_surface(
    uint32_t width, uint32_t height, uint32_t format);
HRESULT d3d_hle_guest_clear(
    uint32_t count, uint32_t rects_va, uint32_t flags, uint32_t color,
    uint32_t depth_bits, uint32_t stencil);
HRESULT d3d_hle_guest_update_overlay(
    uint32_t surface_va, uint32_t source_rect_va,
    uint32_t destination_surface_va, uint32_t destination_rect_va,
    uint32_t color_key, uint32_t flags);
void d3d_hle_guest_enable_overlay(uint32_t enable);
HRESULT d3d_hle_guest_swap(uint32_t flags);
HRESULT d3d_hle_guest_set_back_buffer_scale(
    uint32_t horizontal_bits, uint32_t vertical_bits);
HRESULT d3d_hle_guest_check_device_format(
    uint32_t adapter, uint32_t device_type, uint32_t adapter_format,
    uint32_t usage, uint32_t resource_type, uint32_t check_format);
HRESULT d3d_hle_guest_create_device(
    uint32_t adapter, uint32_t device_type, uint32_t focus_window,
    uint32_t behavior_flags, uint32_t parameters_va, uint32_t output_va);
uint32_t d3d_hle_guest_create_vertex_buffer(uint32_t length);
uint32_t d3d_hle_guest_create_index_buffer(uint32_t length);
uint32_t d3d_hle_guest_set_fence(uint32_t flags);
uint32_t d3d_hle_guest_insert_fence(void);
void d3d_hle_guest_block_on_fence(uint32_t fence);
uint32_t d3d_hle_guest_is_busy(void);
void d3d_hle_guest_block_until_not_busy(uint32_t resource_va);
HRESULT d3d_hle_guest_set_material(uint32_t material_va);
HRESULT d3d_hle_guest_set_light(uint32_t index, uint32_t light_va);
HRESULT d3d_hle_guest_light_enable(uint32_t index, uint32_t enable);
HRESULT d3d_hle_guest_set_pixel_shader_program(uint32_t definition_va);
void d3d_hle_guest_set_swap_callback(uint32_t callback_va);
void d3d_hle_guest_set_vertical_blank_callback(uint32_t callback_va);
void d3d_hle_guest_insert_callback(
    uint32_t type, uint32_t callback_va, uint32_t context);
void d3d_hle_guest_lazy_set_point_params(uint32_t device_va);
void d3d_hle_guest_block_on_time(uint32_t fence, uint32_t flags);
void d3d_hle_guest_block_on_resource(uint32_t resource_va);
void d3d_hle_guest_set_bump_env(
    uint32_t stage, uint32_t type, uint32_t value);
void d3d_hle_guest_set_color_key(uint32_t stage, uint32_t value);
void d3d_hle_guest_set_xbox_texture_stage_state(
    uint32_t stage, uint32_t type, uint32_t value);
void d3d_hle_guest_set_tile(uint32_t index, uint32_t tile_va);
void d3d_hle_guest_get_tile(uint32_t index, uint32_t tile_va);
void d3d_hle_guest_lock_2d_surface(
    uint32_t pixel_container_va, uint32_t face, uint32_t level,
    uint32_t locked_rect_va, uint32_t rect_va, uint32_t flags);
void d3d_hle_guest_lock_3d_surface(
    uint32_t texture_va, uint32_t level, uint32_t locked_box_va,
    uint32_t box_va, uint32_t flags);
void d3d_hle_guest_note_resource_cpu_write(uint32_t resource_va);
void d3d_hle_guest_select_vertex_shader(
    uint32_t shader_handle, uint32_t address);
void d3d_hle_guest_select_vertex_shader_direct(
    uint32_t declaration_va, uint32_t address);
void d3d_hle_guest_delete_vertex_shader(uint32_t shader_handle);
HRESULT d3d_hle_guest_set_vertex_shader(uint32_t shader_handle);
void d3d_hle_guest_draw_vertices_up(
    uint32_t primitive_type, uint32_t vertex_count,
    uint32_t vertices_va, uint32_t stride);
void d3d_hle_guest_draw_indexed_vertices_up(
    uint32_t primitive_type, uint32_t vertex_count,
    uint32_t indices_va, uint32_t vertices_va, uint32_t stride);
void d3d_hle_guest_set_vertex_data4f(
    uint32_t reg, uint32_t x, uint32_t y, uint32_t z, uint32_t w);
void d3d_hle_guest_set_vertex_data2f(uint32_t reg, uint32_t x, uint32_t y);
void d3d_hle_guest_set_vertex_data2s(uint32_t reg, uint32_t a, uint32_t b);
void d3d_hle_guest_set_vertex_data_color(uint32_t reg, uint32_t color);
void d3d_hle_guest_set_vertex_data4ub(
    uint32_t reg, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void d3d_hle_guest_begin(uint32_t primitive_type);
void d3d_hle_guest_end(void);
void d3d_hle_guest_begin_visibility_test(void);
HRESULT d3d_hle_guest_end_visibility_test(uint32_t index);
HRESULT d3d_hle_guest_get_visibility_test_result(
    uint32_t index, uint32_t result_va, uint32_t timestamp_va);
void d3d_hle_guest_get_display_field_status(uint32_t status_va);
void d3d_hle_guest_set_scissors(
    uint32_t count, uint32_t exclusive, uint32_t rects_va);
void d3d_hle_guest_set_screen_space_offset(uint32_t x_bits, uint32_t y_bits);
HRESULT d3d_hle_guest_set_vertex_shader_input(
    uint32_t handle, uint32_t stream_count, uint32_t inputs_va);
HRESULT d3d_hle_guest_set_vertex_shader_input_direct(
    uint32_t stream_count, uint32_t inputs_va);
void d3d_hle_guest_set_logic_op(uint32_t value);
void d3d_hle_guest_set_vertex_blend(uint32_t value);
void d3d_hle_guest_drain_inline_push(void);
uint32_t d3d_hle_guest_make_space(void);
uint32_t d3d_hle_guest_begin_push(uint32_t count);
void d3d_hle_guest_end_push(uint32_t push_va);
void d3d_hle_guest_kick_push_buffer(void);
void d3d_hle_guest_begin_state_big(uint32_t count);
void d3d_hle_guest_make_requested_space(
    uint32_t requested, uint32_t maximum);
uint32_t d3d_hle_guest_palette_size(uint32_t palette_va);
uint32_t d3d_hle_guest_palette_lock2(uint32_t palette_va, uint32_t flags);
uint32_t d3d_hle_guest_resource_add_ref(uint32_t resource_va);
uint32_t d3d_hle_guest_create_surface2(
    uint32_t width, uint32_t height, uint32_t usage, uint32_t format);
HRESULT d3d_hle_guest_create_image_surface(
    uint32_t width, uint32_t height, uint32_t format, uint32_t out_va);
uint32_t d3d_hle_guest_create_palette2(uint32_t size);
void d3d_hle_guest_delete_pixel_shader(uint32_t shader_handle);
HRESULT d3d_hle_guest_set_pixel_shader(uint32_t shader_handle);
void d3d_hle_guest_set_pixel_shader_constant(
    uint32_t start_register, uint32_t data_va, uint32_t count);
void d3d_hle_guest_kickoff_and_wait_for_idle(void);
/* Bound stream-0 / index-buffer guest identity for persistent mesh keys. */
int d3d_hle_guest_bound_mesh_identity(
    uint32_t *vb_data_va, uint64_t *vb_generation,
    uint32_t *ib_data_va, uint64_t *ib_generation);

/* Signature-resolved native binding targets. */
void d3d_hle_device_get_direct3d(void);
void d3d_hle_device_get_device_caps(void);
void d3d_hle_resource_get_type(void);
void d3d_hle_texture_get_level_desc(void);
void d3d_hle_surface_get_desc(void);
void d3d_hle_device_set_stream_source(void);
void d3d_hle_device_set_indices(void);
void d3d_hle_device_draw_vertices(void);
void d3d_hle_device_draw_indexed_vertices(void);
void d3d_hle_resource_is_busy(void);
void d3d_hle_device_set_render_state_cull_mode(void);
void d3d_hle_device_set_render_state_fog_color(void);
void d3d_hle_device_set_texture_state_border_color(void);
void d3d_hle_device_set_vertex_shader_constant1(void);
void d3d_hle_device_set_vertex_shader_constant4(void);
void d3d_hle_device_switch_texture(void);
void d3d_hle_device_set_texture_stage_state_not_inline(void);
void d3d_hle_lazy_set_state(void);
void d3d_hle_device_set_render_state_z_enable(void);
void d3d_hle_device_set_render_state_stencil_fail(void);
void d3d_hle_device_set_render_state_z_bias(void);
void d3d_hle_device_set_render_state_multisample_antialias(void);
void d3d_hle_device_set_render_state_simple(void);
void d3d_hle_device_set_texture_state_texcoord_index(void);
void d3d_hle_device_create_texture2(void);
void d3d_hle_texture_get_surface_level2(void);
void d3d_hle_resource_release(void);
void d3d_hle_surface_lock_rect(void);
void d3d_hle_vertex_buffer_lock2(void);
void d3d_hle_device_load_vertex_shader(void);
void d3d_hle_device_load_vertex_shader_program(void);
void d3d_hle_device_create_vertex_shader(void);
void d3d_hle_device_create_pixel_shader(void);
void d3d_hle_device_get_display_mode(void);
void d3d_hle_device_reset(void);
void d3d_hle_device_set_render_target(void);
void d3d_hle_device_get_back_buffer2(void);
void d3d_hle_device_set_gamma_ramp(void);
void d3d_hle_device_get_gamma_ramp(void);
void d3d_hle_device_copy_rects(void);
void d3d_hle_device_get_render_target2(void);
void d3d_hle_device_get_depth_stencil_surface2(void);
void d3d_hle_device_set_transform(void);
void d3d_hle_device_get_transform(void);
void d3d_hle_device_set_viewport(void);
void d3d_hle_device_get_viewport(void);
void d3d_hle_device_get_texture2(void);
void d3d_hle_device_set_texture(void);
void d3d_hle_device_get_indices2(void);
void d3d_hle_device_add_ref(void);
void d3d_hle_device_release(void);
void d3d_hle_device_set_render_state_stencil_enable(void);
void d3d_hle_device_set_render_state_texture_factor(void);
void d3d_hle_device_set_render_state_dxt1_noise_enable(void);
void d3d_hle_device_set_render_state_occlusion_cull_enable(void);
void d3d_hle_device_set_render_state_stencil_cull_enable(void);
void d3d_hle_device_set_render_state_rop_z_cmp_always_read(void);
void d3d_hle_device_set_render_state_rop_z_read(void);
void d3d_hle_device_set_render_state_do_not_cull_uncompressed(void);
void d3d_hle_device_set_render_state_multisample_render_target_mode(void);
void d3d_hle_device_set_render_state_two_sided_lighting(void);
void d3d_hle_common_set_render_target(void);
void d3d_hle_base_texture_get_level_count(void);
void d3d_hle_get_2d_surface_desc(void);
void d3d_hle_cube_texture_get_cube_map_surface2(void);
void d3d_hle_volume_texture_get_level_desc(void);
void d3d_hle_resource_register(void);
void d3d_hle_create_surface_with_contiguous_header(void);
void d3d_hle_device_clear(void);
void d3d_hle_device_update_overlay(void);
void d3d_hle_device_enable_overlay(void);
void d3d_hle_device_swap(void);
void d3d_hle_device_set_back_buffer_scale(void);
void d3d_hle_direct3d_check_device_format(void);
void d3d_hle_direct3d_create_device(void);
void d3d_hle_device_create_vertex_buffer2(void);
void d3d_hle_device_create_index_buffer2(void);
void d3d_hle_set_fence(void);
void d3d_hle_block_on_time(void);
void d3d_hle_lock_2d_surface(void);
void d3d_hle_lock_3d_surface(void);
void d3d_hle_device_get_stream_source2(void);
void d3d_hle_device_select_vertex_shader(void);
void d3d_hle_device_select_vertex_shader_direct(void);
void d3d_hle_device_delete_vertex_shader(void);
void d3d_hle_device_set_vertex_shader(void);
void d3d_hle_device_set_vertex_shader_constant_not_inline(void);
void d3d_hle_device_draw_vertices_up(void);
void d3d_hle_device_draw_indexed_vertices_up(void);
void d3d_hle_device_set_vertex_data4f(void);
void d3d_hle_device_delete_pixel_shader(void);
void d3d_hle_device_set_pixel_shader(void);
void d3d_hle_device_set_pixel_shader_constant(void);
void d3d_hle_kickoff_and_wait_for_idle(void);
void d3d_hle_device_begin(void);
void d3d_hle_device_end(void);
void d3d_hle_device_set_vertex_data2f(void);
void d3d_hle_device_set_vertex_data2s(void);
void d3d_hle_device_set_vertex_data_color(void);
void d3d_hle_device_begin_visibility_test(void);
void d3d_hle_device_end_visibility_test(void);
void d3d_hle_device_get_visibility_test_result(void);
void d3d_hle_device_get_display_field_status(void);
void d3d_hle_device_set_scissors(void);
void d3d_hle_device_set_screen_space_offset(void);
void d3d_hle_device_set_depth_clip_planes(void);
void d3d_hle_device_set_flicker_filter(void);
void d3d_hle_device_set_soft_display_filter(void);
void d3d_hle_direct3d_set_push_buffer_size(void);
void d3d_hle_device_set_render_state_fill_mode(void);
void d3d_hle_device_set_render_state_front_face(void);
void d3d_hle_device_set_render_state_logic_op(void);
void d3d_hle_device_set_render_state_vertex_blend(void);
void d3d_hle_device_set_render_state_edge_anti_alias(void);
void d3d_hle_device_set_render_state_multisample_mask(void);
void d3d_hle_device_set_render_state_normalize_normals(void);
void d3d_hle_device_set_render_state_ps_texture_modes(void);
void d3d_hle_device_set_render_state_back_fill_mode(void);
void d3d_hle_device_set_render_state_line_width(void);
void d3d_hle_device_set_render_state_sample_alpha(void);
void d3d_hle_device_set_render_state_shadow_func(void);
void d3d_hle_device_set_render_state_yuv_enable(void);
void d3d_hle_device_set_render_state_multisample_mode(void);
void d3d_hle_device_set_vertex_shader_constant1_fast(void);
void d3d_hle_device_set_vertex_shader_constant_not_inline_fast(void);
void d3d_hle_device_set_vertex_shader_input(void);
void d3d_hle_device_set_vertex_shader_input_direct(void);
void d3d_hle_palette_get_size(void);
void d3d_hle_palette_lock2(void);
void d3d_hle_resource_add_ref(void);
void d3d_hle_texture_lock_rect(void);
void d3d_hle_device_create_surface2(void);
void d3d_hle_device_make_space(void);
void d3d_hle_device_begin_push_std(void);
void d3d_hle_device_begin_push2_std(void);
void d3d_hle_device_end_push_std(void);
void d3d_hle_device_kick_push_buffer_std(void);
void d3d_hle_device_begin_state_big_std(void);
void d3d_hle_make_requested_space_std(void);
void d3d_hle_cube_texture_lock_rect(void);
void d3d_hle_volume_texture_lock_box(void);
void d3d_hle_device_set_texture_state_border_color_std(void);
void d3d_hle_device_set_texture_state_bump_env_std(void);
void d3d_hle_device_set_texture_state_color_key_std(void);
void d3d_hle_block_on_resource_std(void);
void d3d_hle_device_set_shader_constant_mode_std(void);
void d3d_hle_lock_2d_surface_std(void);
void d3d_hle_device_set_tile_std(void);
void d3d_hle_device_get_tile_std(void);
void d3d_hle_device_set_vertex_data4ub_std(void);
void d3d_hle_device_set_render_state_not_inline_std(void);
void d3d_hle_device_insert_fence_std(void);
void d3d_hle_device_block_on_fence_std(void);
void d3d_hle_device_is_busy_std(void);
void d3d_hle_resource_block_until_not_busy_std(void);
void d3d_hle_device_set_material_std(void);
void d3d_hle_device_set_light_std(void);
void d3d_hle_device_light_enable_std(void);
void d3d_hle_device_set_pixel_shader_program_std(void);
void d3d_hle_device_set_swap_callback_std(void);
void d3d_hle_device_set_vertical_blank_callback_std(void);
void d3d_hle_device_insert_callback_std(void);
void d3d_hle_lazy_set_point_params_std(void);
void d3d_hle_device_set_palette_std(void);
void d3d_hle_resource_get_type_std(void);
void d3d_hle_device_set_stream_source_std(void);
void d3d_hle_device_set_indices_std(void);
void d3d_hle_device_draw_vertices_std(void);
void d3d_hle_resource_is_busy_std(void);
void d3d_hle_vertex_buffer_lock2_std(void);
void d3d_hle_device_set_render_target_std(void);
void d3d_hle_device_get_back_buffer2_std(void);
void d3d_hle_device_set_transform_std(void);
void d3d_hle_device_set_texture_std(void);
void d3d_hle_resource_register_std(void);
void d3d_hle_device_swap_std(void);
void d3d_hle_direct3d_create_device_std(void);
void d3d_hle_device_set_vertex_data4f_std(void);
void d3d_hle_device_set_pixel_shader_std(void);
void d3d_hle_device_set_pixel_shader_constant_std(void);
void d3d_hle_device_reset_std(void);
void d3d_hle_device_set_render_target_fast_std(void);
void d3d_hle_device_create_palette2_std(void);
void d3d_hle_device_create_image_surface_std(void);
void d3d_hle_device_end_push_buffer_std(void);
void d3d_hle_device_set_stipple_std(void);
void d3d_hle_cdevice_kickoff_std(void);
void d3d_hle_destroy_resource_std(void);
void d3d_hle_noop_std_0(void);
void d3d_hle_noop_std_1(void);
void d3d_hle_noop_std_2(void);
void d3d_hle_noop_std_6(void);

#ifdef __cplusplus
}
#endif

#endif /* XRECOMP_D3D_HLE_GUEST_H */
