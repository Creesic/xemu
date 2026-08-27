#ifndef XEMU_D3D_HLE_H
#define XEMU_D3D_HLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState CPUState;
typedef struct MemoryRegion MemoryRegion;

typedef enum XemuD3DHleStatus {
    XEMU_D3D_HLE_STATUS_UNAVAILABLE = 0,
    XEMU_D3D_HLE_STATUS_DISABLED,
    XEMU_D3D_HLE_STATUS_ARMED,
    XEMU_D3D_HLE_STATUS_PROFILE_REJECTED,
    XEMU_D3D_HLE_STATUS_PROFILE_VERIFIED,
    XEMU_D3D_HLE_STATUS_ACTIVE,
    XEMU_D3D_HLE_STATUS_FAILED,
} XemuD3DHleStatus;

/* Install the dormant, fail-closed TCG entry hook on the Xbox CPU. */
void xemu_d3d_hle_install(CPUState *cpu, MemoryRegion *ram,
                          MemoryRegion *system_memory);

/* Reset the active XBE/D3D session while keeping the process-lifetime hook
 * installed. The next valid XBE identity is discovered from scratch. */
void xemu_d3d_hle_session_reset(const char *why);

/* True only after profile discovery and successful Plume startup. */
bool xemu_d3d_hle_owns_window(void);
/* Route Xemu's shared internal-resolution option to the active Plume owner. */
bool xemu_d3d_hle_set_surface_scale_factor(unsigned int scale);
unsigned int xemu_d3d_hle_get_surface_scale_factor(void);

/* Live frontend state for UI/reporting. ACTIVE is reached only after the
 * loaded XBE has a usable D3D dispatch and Plume initialization succeeds. */
XemuD3DHleStatus xemu_d3d_hle_status(void);
bool xemu_d3d_hle_requested(void);
bool xemu_d3d_hle_environment_override(void);
const char *xemu_d3d_hle_active_backend_name(void);
const char *xemu_d3d_hle_active_profile_name(void);
const char *xemu_d3d_hle_status_detail(void);

/* Service Plume presentation from the emulated PCRTC VBlank. */
void xemu_d3d_hle_vblank(uint32_t pcrtc_start);

/* Mark movie-overlay transitions so opt-in diagnostics can focus on the
 * title/menu phase instead of exhausting their bounded trace during FMV. */
void xemu_d3d_hle_overlay_state_changed(bool enabled);

/* Publish xemu's straight-alpha RGBA8 HUD for Plume's host compositor. */
void xemu_d3d_hle_publish_overlay(const void *pixels, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height,
                                  uint32_t pitch, bool visible);

#ifdef __cplusplus
}
#endif

#endif
