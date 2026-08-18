#include "d3d_hle_guest.h"

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_device_set_render_state_cull_mode_gen_unused(void);

void d3d_hle_device_set_render_state_cull_mode(void)
{
    uint32_t value;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_set_render_state_cull_mode_gen_unused();
        return;
    }

    value = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    /*
     * Xbox exposes the requested winding with NV097 FRONT_FACE encodings.
     * The XDK body combines that with its cached front-face state to program
     * CULL_FACE; Plume's D3D8 contract instead stores the PC-style D3DCULL.
     */
    d3d_hle_guest_set_cull_mode(value);
}
