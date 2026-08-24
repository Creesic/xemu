#include "xemu_d3d_hle_guest_heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    XemuD3DHleGuestHeap heap = {0};
    uint32_t first;
    uint32_t second;
    uint32_t reused;
    uint32_t coalesced;

    assert(!xemu_d3d_hle_guest_heap_init(&heap, 0xFFF00000u, 0x00200000u));
    assert(xemu_d3d_hle_guest_heap_init(
        &heap, XEMU_D3D_HLE_SYNTHETIC_VA_BASE,
        XEMU_D3D_HLE_SYNTHETIC_SIZE));

    assert(xemu_d3d_hle_guest_heap_alloc(&heap, 0, 16) == 0);
    assert(xemu_d3d_hle_guest_heap_alloc(&heap, 16, 3) == 0);

    first = xemu_d3d_hle_guest_heap_alloc(&heap, 20, 16);
    second = xemu_d3d_hle_guest_heap_alloc(&heap, 96, 128);
    assert(first == XEMU_D3D_HLE_SYNTHETIC_VA_BASE);
    assert((second & 127u) == 0);
    assert(second >= first + 20u);

    assert(!xemu_d3d_hle_guest_heap_free(&heap, second + 4u));
    assert(xemu_d3d_hle_guest_heap_free(&heap, first));
    assert(!xemu_d3d_hle_guest_heap_free(&heap, first));
    reused = xemu_d3d_hle_guest_heap_alloc(&heap, 16, 16);
    assert(reused == first);

    assert(xemu_d3d_hle_guest_heap_free(&heap, reused));
    assert(xemu_d3d_hle_guest_heap_free(&heap, second));
    coalesced = xemu_d3d_hle_guest_heap_alloc(&heap, 192, 64);
    assert(coalesced == XEMU_D3D_HLE_SYNTHETIC_VA_BASE);

    xemu_d3d_hle_guest_heap_reset(&heap);
    assert(xemu_d3d_hle_guest_heap_alloc(&heap, 64, 64) ==
           XEMU_D3D_HLE_SYNTHETIC_VA_BASE);

    xemu_d3d_hle_guest_heap_destroy(&heap);
    puts("d3d_hle_synthetic_heap_test: OK");
    return 0;
}
