#include "d3d_hle_guest.h"

extern uint32_t g_eax;
extern uint32_t g_ecx;
extern uint32_t g_edx;

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_device_set_texture_stage_state_not_inline_gen_unused(void);

void d3d_hle_device_set_texture_stage_state_not_inline(void)
{
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_texture_stage_state_not_inline_gen_unused();
        return;
    }

    /*
     * Reviewed XDK register helper: EAX is the stage, EDX is the
     * D3DTEXTURESTAGESTATETYPE, and ECX is the value. It returns with a
     * plain ret and has no stack arguments.
     */
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_set_texture_stage_state(
        g_eax, (D3DTEXTURESTAGESTATETYPE)g_edx, g_ecx);
}
