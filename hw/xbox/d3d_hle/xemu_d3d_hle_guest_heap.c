#include "xemu_d3d_hle_guest_heap.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool guest_heap_ensure_capacity(
    XemuD3DHleGuestHeap *heap, size_t needed)
{
    XemuD3DHleGuestHeapBlock *blocks;
    size_t capacity;

    if (heap->block_capacity >= needed)
        return true;
    capacity = heap->block_capacity ? heap->block_capacity : 8u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u)
            return false;
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*blocks))
        return false;
    blocks = (XemuD3DHleGuestHeapBlock *)realloc(
        heap->blocks, capacity * sizeof(*blocks));
    if (!blocks)
        return false;
    heap->blocks = blocks;
    heap->block_capacity = capacity;
    return true;
}

bool xemu_d3d_hle_guest_heap_init(
    XemuD3DHleGuestHeap *heap, uint32_t base, uint32_t size)
{
    if (!heap || !base || !size ||
        (uint64_t)base + size > (uint64_t)UINT32_MAX + 1u) {
        return false;
    }
    memset(heap, 0, sizeof(*heap));
    heap->base = base;
    heap->size = size;
    if (!guest_heap_ensure_capacity(heap, 1u)) {
        memset(heap, 0, sizeof(*heap));
        return false;
    }
    xemu_d3d_hle_guest_heap_reset(heap);
    return true;
}

void xemu_d3d_hle_guest_heap_destroy(XemuD3DHleGuestHeap *heap)
{
    if (!heap)
        return;
    free(heap->blocks);
    memset(heap, 0, sizeof(*heap));
}

void xemu_d3d_hle_guest_heap_reset(XemuD3DHleGuestHeap *heap)
{
    if (!heap || !heap->blocks || !heap->block_capacity || !heap->size)
        return;
    heap->blocks[0].address = heap->base;
    heap->blocks[0].size = heap->size;
    heap->blocks[0].free = true;
    heap->block_count = 1u;
}

uint32_t xemu_d3d_hle_guest_heap_alloc(
    XemuD3DHleGuestHeap *heap, uint32_t size, uint32_t alignment)
{
    size_t i;

    if (!heap || !heap->blocks || !size || !alignment ||
        (alignment & (alignment - 1u)) != 0u) {
        return 0;
    }
    for (i = 0; i < heap->block_count; ++i) {
        XemuD3DHleGuestHeapBlock original = heap->blocks[i];
        uint64_t block_end;
        uint64_t aligned;
        uint64_t allocation_end;
        uint32_t head;
        uint32_t tail;
        size_t pieces;
        size_t at;

        if (!original.free)
            continue;
        block_end = (uint64_t)original.address + original.size;
        aligned = ((uint64_t)original.address + alignment - 1u) &
                  ~((uint64_t)alignment - 1u);
        allocation_end = aligned + size;
        if (aligned > UINT32_MAX || allocation_end > block_end)
            continue;
        head = (uint32_t)(aligned - original.address);
        tail = (uint32_t)(block_end - allocation_end);
        pieces = 1u + (head != 0u) + (tail != 0u);
        if (!guest_heap_ensure_capacity(
                heap, heap->block_count + pieces - 1u)) {
            return 0;
        }
        if (pieces > 1u) {
            memmove(&heap->blocks[i + pieces], &heap->blocks[i + 1u],
                    (heap->block_count - i - 1u) * sizeof(heap->blocks[0]));
        }
        at = i;
        if (head) {
            heap->blocks[at++] = (XemuD3DHleGuestHeapBlock) {
                original.address, head, true
            };
        }
        heap->blocks[at++] = (XemuD3DHleGuestHeapBlock) {
            (uint32_t)aligned, size, false
        };
        if (tail) {
            heap->blocks[at] = (XemuD3DHleGuestHeapBlock) {
                (uint32_t)allocation_end, tail, true
            };
        }
        heap->block_count += pieces - 1u;
        return (uint32_t)aligned;
    }
    return 0;
}

bool xemu_d3d_hle_guest_heap_free(
    XemuD3DHleGuestHeap *heap, uint32_t address)
{
    size_t i;

    if (!heap || !heap->blocks || !address)
        return false;
    for (i = 0; i < heap->block_count; ++i) {
        XemuD3DHleGuestHeapBlock *block = &heap->blocks[i];

        if (block->address != address || block->free)
            continue;
        block->free = true;
        if (i > 0u && heap->blocks[i - 1u].free &&
            (uint64_t)heap->blocks[i - 1u].address +
                heap->blocks[i - 1u].size == block->address) {
            heap->blocks[i - 1u].size += block->size;
            memmove(&heap->blocks[i], &heap->blocks[i + 1u],
                    (heap->block_count - i - 1u) * sizeof(heap->blocks[0]));
            --heap->block_count;
            --i;
            block = &heap->blocks[i];
        }
        if (i + 1u < heap->block_count && heap->blocks[i + 1u].free &&
            (uint64_t)block->address + block->size ==
                heap->blocks[i + 1u].address) {
            block->size += heap->blocks[i + 1u].size;
            memmove(&heap->blocks[i + 1u], &heap->blocks[i + 2u],
                    (heap->block_count - i - 2u) * sizeof(heap->blocks[0]));
            --heap->block_count;
        }
        return true;
    }
    return false;
}
