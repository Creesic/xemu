#include "d3d_hle_guest.h"

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_device_set_render_state_fog_color_gen_unused(void);

void d3d_hle_device_set_render_state_fog_color(void)
{
    uint32_t value;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_render_state_fog_color_gen_unused();
        return;
    }

    value = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    /*
     * Keep the API D3DCOLOR. The lifted XDK body byte-swaps it only while
     * encoding NV097_SET_FOG_COLOR; Plume consumes the original D3D value.
     */
    d3d_hle_guest_set_render_state(D3DRS_FOGCOLOR, value);
}
