#include "d3d_hle_guest.h"

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_device_get_direct3d_gen_unused(void);

void d3d_hle_device_get_direct3d(void)
{
    uint32_t output_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_device_get_direct3d_gen_unused();
        return;
    }

    output_va = d3d_hle_guest_stack_u32(0);
    d3d_hle_guest_stdcall_return(4);
    /*
     * Xbox D3D is a process-global singleton. The XDK implementation exposes
     * the sentinel value 1 rather than a host COM pointer.
     */
    d3d_hle_guest_write_u32(output_va, 1u);
}
