#ifndef HW_XBOX_D3D_HLE_XEMU_D3D_HLE_SPY_H
#define HW_XBOX_D3D_HLE_XEMU_D3D_HLE_SPY_H

#include <stdbool.h>
#include <stddef.h>

#include "xemu_d3d_hle_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

void xemu_d3d_hle_spy_init(bool hle_requested);
bool xemu_d3d_hle_spy_enabled(void);
const char *xemu_d3d_hle_spy_intern_name(const char *name, size_t length);
void xemu_d3d_hle_spy_bind(const XemuD3DHleProfile *profile);
void xemu_d3d_hle_spy_note(const XemuD3DHleHook *hook);
void xemu_d3d_hle_spy_dump(const char *reason);
void xemu_d3d_hle_spy_reset(void);
unsigned xemu_d3d_hle_spy_symbol_count(void);
unsigned xemu_d3d_hle_spy_called_holes(void);
const char *xemu_d3d_hle_spy_class_name(const XemuD3DHleHook *hook);
void xemu_d3d_hle_spy_on_f2_poll(int active);
bool xemu_d3d_hle_spy_capture_seen_swap(void);

#ifdef __cplusplus
}
#endif

#endif
