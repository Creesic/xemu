#include "d3d_hle_guest.h"

extern void d3d_hle_device_create_pixel_shader_gen_unused(void);

void d3d_hle_device_create_pixel_shader(void)
{
    uint32_t definition_va;
    uint32_t output_va;
    HRESULT result;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_create_pixel_shader_gen_unused();
        return;
    }
    definition_va = d3d_hle_guest_stack_u32(0);
    output_va = d3d_hle_guest_stack_u32(1);
    d3d_hle_guest_stdcall_return(8);
    result = d3d_hle_guest_create_pixel_shader(definition_va, output_va);
    d3d_hle_guest_return_u32((uint32_t)result);
}
