#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "xemu_d3d_hle.h"

void xemu_d3d_hle_install(CPUState *cpu, MemoryRegion *ram,
                          MemoryRegion *system_memory)
{
    (void)cpu;
    (void)ram;
    (void)system_memory;
}

bool xemu_d3d_hle_owns_window(void)
{
    return false;
}

bool xemu_d3d_hle_set_surface_scale_factor(unsigned int scale)
{
    (void)scale;
    return false;
}

unsigned int xemu_d3d_hle_get_surface_scale_factor(void)
{
    return 1;
}

XemuD3DHleStatus xemu_d3d_hle_status(void)
{
    return XEMU_D3D_HLE_STATUS_UNAVAILABLE;
}

bool xemu_d3d_hle_requested(void)
{
    return false;
}

bool xemu_d3d_hle_environment_override(void)
{
    return false;
}

const char *xemu_d3d_hle_active_backend_name(void)
{
    return NULL;
}

const char *xemu_d3d_hle_active_profile_name(void)
{
    return NULL;
}

const char *xemu_d3d_hle_status_detail(void)
{
    return NULL;
}

void xemu_d3d_hle_vblank(uint32_t pcrtc_start)
{
    (void)pcrtc_start;
}

void xemu_d3d_hle_overlay_state_changed(bool enabled)
{
    (void)enabled;
}

void xemu_d3d_hle_publish_overlay(const void *pixels, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height,
                                  uint32_t pitch, bool visible)
{
    (void)pixels;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)pitch;
    (void)visible;
}
