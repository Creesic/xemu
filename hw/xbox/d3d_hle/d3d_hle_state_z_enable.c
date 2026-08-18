#include "d3d_hle_guest.h"

extern void d3d_hle_device_set_render_state_z_enable_gen_unused(void);

void d3d_hle_device_set_render_state_z_enable(void)
{
    uint32_t value;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_render_state_z_enable_gen_unused();
        return;
    }
    value = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_set_z_enable(value);
}
