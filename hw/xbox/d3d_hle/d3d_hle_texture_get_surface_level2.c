#include "d3d_hle_guest.h"

extern void d3d_hle_texture_get_surface_level2_gen_unused(void);

void d3d_hle_texture_get_surface_level2(void)
{
    uint32_t texture_va;
    uint32_t level;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_texture_get_surface_level2_gen_unused();
        return;
    }
    texture_va = d3d_hle_guest_stack_u32(0);
    level = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    d3d_hle_guest_return_u32(
        d3d_hle_guest_texture_get_surface_level2(texture_va, level));
}
