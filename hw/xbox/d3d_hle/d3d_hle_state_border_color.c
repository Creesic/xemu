#include "d3d_hle_guest.h"

extern uint32_t g_eax;
extern uint32_t g_ebx;

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_device_set_texture_state_border_color_gen_unused(void);

void d3d_hle_device_set_texture_state_border_color(void)
{
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_texture_state_border_color_gen_unused();
        return;
    }

    /*
     * MM3's reviewed 66-byte LTCG body consumes the stage in EAX and the
     * border colour in EBX, then returns with a plain ret.
     */
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_set_texture_stage_state(
        g_eax, D3DTSS_BORDERCOLOR, g_ebx);
}
