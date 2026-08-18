#include "d3d_hle_guest.h"

extern uint32_t g_ecx;

extern void d3d_hle_resource_is_busy_gen_unused(void);

void d3d_hle_resource_is_busy(void)
{
    uint32_t resource_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_resource_is_busy_gen_unused();
        return;
    }

    /* MM3's reviewed XDK body is fastcall(resource=ecx), plain ret. */
    resource_va = g_ecx;
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_return_u32(
        d3d_hle_guest_resource_is_busy(resource_va));
}
