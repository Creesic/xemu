#include "d3d_hle_guest.h"

extern void d3d_hle_texture_get_level_desc_gen_unused(void);

void d3d_hle_texture_get_level_desc(void)
{
    uint32_t texture_va;
    uint32_t level;
    uint32_t desc_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_texture_get_level_desc_gen_unused();
        return;
    }

    texture_va = d3d_hle_guest_stack_u32(0);
    level = d3d_hle_guest_stack_u32(1);
    desc_va = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    d3d_hle_guest_surface_desc(texture_va, level, desc_va);
}
