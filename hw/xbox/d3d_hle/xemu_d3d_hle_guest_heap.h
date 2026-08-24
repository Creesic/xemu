#ifndef HW_XBOX_D3D_HLE_XEMU_D3D_HLE_GUEST_HEAP_H
#define HW_XBOX_D3D_HLE_XEMU_D3D_HLE_GUEST_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XEMU_D3D_HLE_SYNTHETIC_PHYS_BASE      0x04000000u
#define XEMU_D3D_HLE_SYNTHETIC_REGION_SIZE    0x04000000u
#define XEMU_D3D_HLE_SYNTHETIC_PAGE_TABLES    15u
#define XEMU_D3D_HLE_SYNTHETIC_DATA_PHYS_BASE 0x04400000u
#define XEMU_D3D_HLE_SYNTHETIC_VA_BASE        0x84400000u
#define XEMU_D3D_HLE_SYNTHETIC_SIZE           0x03C00000u

typedef struct XemuD3DHleGuestHeapBlock {
    uint32_t address;
    uint32_t size;
    bool free;
} XemuD3DHleGuestHeapBlock;

typedef struct XemuD3DHleGuestHeap {
    uint32_t base;
    uint32_t size;
    XemuD3DHleGuestHeapBlock *blocks;
    size_t block_count;
    size_t block_capacity;
} XemuD3DHleGuestHeap;

bool xemu_d3d_hle_guest_heap_init(
    XemuD3DHleGuestHeap *heap, uint32_t base, uint32_t size);
void xemu_d3d_hle_guest_heap_destroy(XemuD3DHleGuestHeap *heap);
void xemu_d3d_hle_guest_heap_reset(XemuD3DHleGuestHeap *heap);
uint32_t xemu_d3d_hle_guest_heap_alloc(
    XemuD3DHleGuestHeap *heap, uint32_t size, uint32_t alignment);
bool xemu_d3d_hle_guest_heap_free(
    XemuD3DHleGuestHeap *heap, uint32_t address);

#endif
