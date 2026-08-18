#include "d3d_hle_guest.h"

extern void d3d_hle_resource_release_gen_unused(void);

void d3d_hle_resource_release(void)
{
    uint32_t resource_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_resource_release_gen_unused();
        return;
    }
    resource_va = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_return_u32(
        d3d_hle_guest_resource_release(resource_va));
}
