#include "d3d_hle_guest.h"

extern uint32_t g_eax;
extern uint32_t g_ecx;

extern void d3d_hle_device_draw_vertices_gen_unused(void);

void d3d_hle_device_draw_vertices(void)
{
    uint32_t vertex_count;
    uint32_t start_vertex;
    D3DPRIMITIVETYPE primitive_type;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_draw_vertices_gen_unused();
        return;
    }

    /* Reviewed LTCG ABI: vertex count=eax, start vertex=ecx, type=[esp+4]. */
    vertex_count = g_eax;
    start_vertex = g_ecx;
    primitive_type =
        (D3DPRIMITIVETYPE)d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_draw_vertices(
        primitive_type, start_vertex, vertex_count);
}
