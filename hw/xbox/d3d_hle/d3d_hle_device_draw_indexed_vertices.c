#include "d3d_hle_guest.h"

extern void d3d_hle_device_draw_indexed_vertices_gen_unused(void);

void d3d_hle_device_draw_indexed_vertices(void)
{
    D3DPRIMITIVETYPE primitive_type;
    uint32_t index_count;
    uint32_t indices_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_draw_indexed_vertices_gen_unused();
        return;
    }

    primitive_type =
        (D3DPRIMITIVETYPE)d3d_hle_guest_stack_u32(0);
    index_count = d3d_hle_guest_stack_u32(1);
    indices_va = d3d_hle_guest_stack_u32(2);
    d3d_hle_guest_stdcall_return(12);
    d3d_hle_guest_draw_indexed_vertices(
        primitive_type, index_count, indices_va);
}
