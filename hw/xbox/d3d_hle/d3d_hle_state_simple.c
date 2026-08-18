#include "d3d_hle_guest.h"

extern uint32_t g_ecx;
extern uint32_t g_edx;
extern void d3d_hle_device_set_render_state_simple_gen_unused(void);

void d3d_hle_device_set_render_state_simple(void)
{
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_render_state_simple_gen_unused();
        return;
    }
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_set_simple(g_ecx, g_edx);
}
