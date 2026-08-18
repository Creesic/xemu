#include "d3d_hle_guest.h"

extern void d3d_hle_device_create_vertex_shader_gen_unused(void);

void d3d_hle_device_create_vertex_shader(void)
{
    uint32_t args[4];
    unsigned i;
    HRESULT result;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_create_vertex_shader_gen_unused();
        return;
    }
    for (i = 0; i < 4; ++i)
        args[i] = d3d_hle_guest_stack_u32(i);
    d3d_hle_guest_stdcall_return(16);
    result = d3d_hle_guest_create_vertex_shader(
        args[0], args[1], args[2], args[3]);
    d3d_hle_guest_return_u32((uint32_t)result);
}
