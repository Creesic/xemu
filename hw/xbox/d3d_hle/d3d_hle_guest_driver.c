#include "d3d_hle_guest_driver.h"

static d3d_hle_guest_driver_prepare_callback s_prepare_callback;
static unsigned s_requirements;

void d3d_hle_guest_driver_set_prepare_callback(
    d3d_hle_guest_driver_prepare_callback callback,
    unsigned requirements)
{
    s_prepare_callback = callback;
    s_requirements = callback ? requirements : 0;
}

unsigned d3d_hle_guest_driver_requirements(void)
{
    return s_requirements;
}

int d3d_hle_guest_driver_prepare(void)
{
    return !s_prepare_callback || s_prepare_callback();
}
