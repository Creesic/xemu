#include "d3d_hle_guest.h"

extern uint32_t g_eax;
extern void d3d_hle_device_load_vertex_shader_gen_unused(void);

void d3d_hle_device_load_vertex_shader(void)
{
    uint32_t address;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_load_vertex_shader_gen_unused();
        return;
    }
    address = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_load_vertex_shader(g_eax, address);
}
