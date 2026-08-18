#ifndef XEMU_D3D_HLE_XBOX_MEMORY_LAYOUT_H
#define XEMU_D3D_HLE_XBOX_MEMORY_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_PHYS_RAM (64u * 1024u * 1024u)
#define XBOX_HEAP_EXT_BASE XBOX_PHYS_RAM
#define XBOX_HEAP_EXT_SIZE 0u

uint8_t *xbox_guest_ptr(uint32_t va);
uint32_t xbox_HeapAllocRange(uint32_t size, uint32_t alignment,
                             uint32_t low, uint32_t high);
void xbox_HeapFree(uint32_t xbox_va);

#ifdef __cplusplus
}
#endif

#endif
