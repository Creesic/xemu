#include "d3d_hle_guest.h"

extern uint32_t g_eax;
extern uint32_t g_ebx;

extern void d3d_hle_device_set_stream_source_gen_unused(void);

void d3d_hle_device_set_stream_source(void)
{
    uint32_t stream;
    uint32_t resource_va;
    uint32_t stride;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_stream_source_gen_unused();
        return;
    }

    /* Reviewed LTCG ABI: stream=eax, vertex buffer=ebx, stride=[esp+4]. */
    stream = g_eax;
    resource_va = g_ebx;
    stride = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_set_stream_source(stream, resource_va, stride);
}
