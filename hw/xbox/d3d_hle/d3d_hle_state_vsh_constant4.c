#include "d3d_hle_guest.h"

extern uint32_t g_ecx;
extern uint32_t g_edx;

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_device_set_vertex_shader_constant4_gen_unused(void);

void d3d_hle_device_set_vertex_shader_constant4(void)
{
    int32_t start_register;
    uint32_t constant_data_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_vertex_shader_constant4_gen_unused();
        return;
    }

    /* XDK flat helper is __fastcall(Register=ecx, pConstantData=edx). */
    start_register = (int32_t)g_ecx;
    constant_data_va = g_edx;
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_set_vertex_shader_constant(
        start_register, constant_data_va, 4);
}
