#include "d3d_hle_guest.h"

extern uint32_t g_ecx;
extern uint32_t g_edx;
extern void d3d_hle_device_switch_texture_gen_unused(void);

void d3d_hle_device_switch_texture(void)
{
    uint32_t format;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_switch_texture_gen_unused();
        return;
    }
    format = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_switch_texture(g_ecx, g_edx, format);
}
