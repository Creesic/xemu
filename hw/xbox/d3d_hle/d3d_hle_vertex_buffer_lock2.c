#include "d3d_hle_guest.h"

extern uint32_t g_edi;
extern void d3d_hle_vertex_buffer_lock2_gen_unused(void);

void d3d_hle_vertex_buffer_lock2(void)
{
    uint32_t flags;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_vertex_buffer_lock2_gen_unused();
        return;
    }
    flags = d3d_hle_guest_stack_u32(0) & 0xFFu;
    d3d_hle_guest_stdcall_return(4);
    d3d_hle_guest_return_u32(
        d3d_hle_guest_vertex_buffer_lock2(g_edi, flags));
}
