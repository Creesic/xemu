#ifndef HW_XBOX_D3D_HLE_XEMU_D3D_HLE_DISCOVERY_H
#define HW_XBOX_D3D_HLE_XEMU_D3D_HLE_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xemu_d3d_hle_profile.h"

typedef bool (*XemuD3DHleGuestRead)(
    uint32_t address, void *output, size_t size);

/* Scan the currently loaded XBE and construct an ABI-aware hook profile. */
const XemuD3DHleProfile *xemu_d3d_hle_discover(
    XemuD3DHleGuestRead read_guest, bool *retryable,
    char *error, size_t error_capacity);

/* Read a logical public argument from an automatically discovered ABI. */
bool xemu_d3d_hle_discovered_argument(
    const XemuD3DHleHook *hook, unsigned index, uint32_t *value);

/* Marshal a discovered XDK ABI into the reviewed wrapper's ABI and invoke it. */
bool xemu_d3d_hle_invoke_discovered(const XemuD3DHleHook *hook);

/* True when an XBE section header describes memory the automatic detector
 * scans (executable or a named D3D runtime section). Non-scan sections
 * (streamed audio/video/track data) can never change a discovery verdict.
 * `section_header` is a const xbe_section_header *. */
bool xemu_d3d_hle_discovery_is_scan_target(
    XemuD3DHleGuestRead read_guest, const void *section_header);

#endif
