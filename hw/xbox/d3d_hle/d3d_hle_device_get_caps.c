#include "d3d_hle_guest.h"

extern void d3d_hle_device_get_device_caps_gen_unused(void);

void d3d_hle_device_get_device_caps(void)
{
    uint32_t caps_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_get_device_caps_gen_unused();
        return;
    }

    caps_va = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_get_device_caps(caps_va);
}
