#ifndef XEMU_D3D_HLE_XBOX_MEMORY_LAYOUT_H
#define XEMU_D3D_HLE_XBOX_MEMORY_LAYOUT_H

#include <stdint.h>

#include "../xemu_d3d_hle_guest_heap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_PHYS_RAM (64u * 1024u * 1024u)
#define XBOX_HEAP_EXT_BASE XEMU_D3D_HLE_SYNTHETIC_VA_BASE
#define XBOX_HEAP_EXT_SIZE XEMU_D3D_HLE_SYNTHETIC_SIZE

uint8_t *xbox_guest_ptr(uint32_t va);
uint8_t *xbox_guest_phys_ptr(uint32_t physical, size_t size);
bool xbox_guest_host_to_phys(const void *host, uint32_t *physical);
uint32_t xbox_HeapAllocRange(uint32_t size, uint32_t alignment,
                             uint32_t low, uint32_t high);
void xbox_HeapFree(uint32_t xbox_va);
bool xbox_HeapSyntheticAvailable(void);
void xbox_HeapSyntheticReset(void);

#ifdef __cplusplus
}
#endif

#endif
