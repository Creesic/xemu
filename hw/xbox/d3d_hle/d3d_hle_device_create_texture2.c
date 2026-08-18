#include "d3d_hle_guest.h"

extern void d3d_hle_device_create_texture2_gen_unused(void);

void d3d_hle_device_create_texture2(void)
{
    uint32_t args[7];
    unsigned i;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_create_texture2_gen_unused();
        return;
    }
    for (i = 0; i < 7; ++i)
        args[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(28);
    d3d_hle_guest_return_u32(d3d_hle_guest_create_texture2(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6]));
}
