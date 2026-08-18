#include "d3d_hle_guest.h"

extern void d3d_hle_surface_get_desc_gen_unused(void);

void d3d_hle_surface_get_desc(void)
{
    uint32_t surface_va;
    uint32_t desc_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_surface_get_desc_gen_unused();
        return;
    }

    surface_va = d3d_hle_guest_stack_u32(0);
    desc_va = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    d3d_hle_guest_surface_desc(surface_va, 0, desc_va);
}
