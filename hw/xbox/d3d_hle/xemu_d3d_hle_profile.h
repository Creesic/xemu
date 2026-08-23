#ifndef HW_XBOX_D3D_HLE_XEMU_D3D_HLE_PROFILE_H
#define HW_XBOX_D3D_HLE_XEMU_D3D_HLE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*XemuD3DHleEntry)(void);

typedef enum XemuD3DHleAbiLocation {
    XEMU_D3D_ABI_NONE = 0,
    XEMU_D3D_ABI_STACK,
    XEMU_D3D_ABI_EAX,
    XEMU_D3D_ABI_EBX,
    XEMU_D3D_ABI_ECX,
    XEMU_D3D_ABI_EDX,
    XEMU_D3D_ABI_EBP,
    XEMU_D3D_ABI_ESI,
    XEMU_D3D_ABI_EDI,
} XemuD3DHleAbiLocation;

enum { XEMU_D3D_HLE_MAX_ABI_ARGS = 8 };

enum {
    XEMU_D3D_HLE_OBSERVE_NONE = 0,
    XEMU_D3D_HLE_OBSERVE_SAFE = 1,
    XEMU_D3D_HLE_OBSERVE_MUTATING = 2,
    XEMU_D3D_HLE_OBSERVE_ABI_HOLE = 3,
};

typedef enum XemuD3DHleHookPolicy {
    /* The compatibility wrapper wholly replaces the native XDK body. */
    XEMU_D3D_HLE_HOOK_REPLACE = 0,
    /* Execute the native body, then adopt/mirror its guest-visible result. */
    XEMU_D3D_HLE_HOOK_NATIVE_THEN_MIRROR,
    /* Reviewed as not mutating renderer-owned GPU or guest D3D state. */
    XEMU_D3D_HLE_HOOK_NATIVE_SAFE,
    /* May run only before Plume activation; post-activation entry is fatal. */
    XEMU_D3D_HLE_HOOK_BOOTSTRAP_ONLY,
    /* Spy-only: log/count, then always fall through to the native XDK body. */
    XEMU_D3D_HLE_HOOK_OBSERVE,
} XemuD3DHleHookPolicy;

typedef struct XemuD3DHleHook {
    uint32_t address;
    XemuD3DHleEntry entry;
    const char *name;
    /* Populated only for automatically discovered XDK variants. */
    uint8_t automatic;
    uint8_t source_param_count;
    uint8_t source_stack_bytes;
    uint8_t source_caller_cleanup;
    XemuD3DHleHookPolicy policy;
    uint8_t source_params[XEMU_D3D_HLE_MAX_ABI_ARGS];
    uint8_t target_param_count;
    uint8_t target_params[XEMU_D3D_HLE_MAX_ABI_ARGS];
    uint8_t observe_class;
} XemuD3DHleHook;

typedef struct XemuD3DHleSpecialHooks {
    uint32_t get_back_buffer;
    uint32_t get_render_target;
    uint32_t get_depth_stencil;
    uint32_t create_texture;
    uint32_t create_surface;
    uint32_t texture_get_surface_level;
    uint32_t cube_get_surface_level;
    uint32_t texture_lock_rect;
    uint32_t cube_texture_lock_rect;
    uint32_t volume_texture_lock_box;
    uint32_t set_texture;
    uint32_t switch_texture;
    uint32_t resource_release;
    uint32_t surface_lock_rect;
    uint32_t create_device;
    uint32_t create_vertex_buffer;
    uint32_t create_index_buffer;
    uint32_t lock_3d_surface;
    uint32_t create_vertex_shader;
    uint32_t delete_vertex_shader;
    uint32_t create_pixel_shader;
    uint32_t delete_pixel_shader;
} XemuD3DHleSpecialHooks;

typedef enum XemuD3DHleBootstrap {
    /* Preserve the title's XDK CreateDevice and mirror it after return. */
    XEMU_D3D_HLE_BOOTSTRAP_MIRROR_NATIVE = 0,
    /* Create the host device before executing a reviewed HLE wrapper. */
    XEMU_D3D_HLE_BOOTSTRAP_DIRECT = 1,
} XemuD3DHleBootstrap;

typedef struct XemuD3DHleProfile {
    const char *name;
    const char *source_xbe_sha256;
    uint32_t reviewed_required_hook_count;
    uint32_t reviewed_implemented_hook_count;
    uint32_t reviewed_blocker_count;
    /* Non-zero only for runtime-discovered profiles. Any unresolved linked
     * D3D function keeps automatic Plume attach fail-closed. */
    uint32_t discovery_recognized_count;
    uint32_t discovery_unsupported_count;
    uint32_t discovery_duplicate_count;
    uint32_t discovery_ambiguous_count;
    uint32_t discovery_mutating_uncovered_count;
    uint32_t discovery_uncovered_abi_count;
    uint32_t xbe_base;
    uint32_t xbe_headers_size;
    uint32_t xbe_image_size;
    uint32_t xbe_timestamp;
    uint32_t xbe_title_id;
    uint32_t xbe_section_count;
    uint32_t d3d_section_va;
    uint32_t d3d_section_size;
    const char *d3d_section_sha1;
    uint32_t dirty_flags_va;
    uint32_t deferred_texture_state_va;
    uint32_t fog_state_va;
    XemuD3DHleBootstrap bootstrap;
    /* Zero-based public argument index of D3DPRESENT_PARAMETERS for a
     * direct CreateDevice wrapper. Ignored for native-mirror profiles. */
    uint32_t create_device_parameters_arg;
    const XemuD3DHleHook *hooks;
    size_t hook_count;
    XemuD3DHleSpecialHooks special;
} XemuD3DHleProfile;

extern const XemuD3DHleProfile xemu_d3d_hle_mm3_profile;
extern const XemuD3DHleProfile xemu_d3d_hle_pgr2_profile;

const XemuD3DHleHook *xemu_d3d_hle_profile_find_hook(
    const XemuD3DHleProfile *profile, uint32_t pc);
const XemuD3DHleProfile *const *xemu_d3d_hle_profiles(size_t *count);
void xemu_d3d_hle_profile_range(uint32_t *first, uint32_t *last);
bool xemu_d3d_hle_profile_validate(
    const XemuD3DHleProfile *profile, char *error, size_t error_capacity);
bool xemu_d3d_hle_profiles_validate(char *error, size_t error_capacity);

#endif
