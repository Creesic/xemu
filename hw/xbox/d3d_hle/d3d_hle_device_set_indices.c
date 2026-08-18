#include "d3d_hle_guest.h"

extern uint32_t g_ebx;

extern void d3d_hle_device_set_indices_gen_unused(void);

void d3d_hle_device_set_indices(void)
{
    uint32_t resource_va;
    uint32_t base_vertex;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_indices_gen_unused();
        return;
    }

    /* Reviewed LTCG ABI: index buffer=ebx, base vertex=[esp+4]. */
    resource_va = g_ebx;
    base_vertex = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_set_indices(resource_va, base_vertex);
}
