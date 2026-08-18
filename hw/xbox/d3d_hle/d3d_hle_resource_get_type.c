#include "d3d_hle_guest.h"

extern uint32_t g_ecx;

/* Retained lifted implementation, emitted by tools.recomp for this binding. */
extern void d3d_hle_resource_get_type_gen_unused(void);

void d3d_hle_resource_get_type(void)
{
    uint32_t resource_va;
    if (!d3d_hle_guest_native_active()) {
        d3d_hle_resource_get_type_gen_unused();
        return;
    }

    /*
     * This binding deliberately targets the undecorated XDK fastcall variant.
     * Decorated stdcall variants remain lifted until they have their own
     * ABI-specific wrapper and inventory entry.
     */
    resource_va = g_ecx;
    d3d_hle_guest_stdcall_return(0);
    d3d_hle_guest_return_u32(
        d3d_hle_guest_resource_type(resource_va));
}
