#ifndef HW_XBOX_D3D_HLE_D3D8_TILE_STATE_H
#define HW_XBOX_D3D_HLE_D3D8_TILE_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XboxD3DTile {
    uint32_t flags;
    uint32_t memory;
    uint32_t size;
    uint32_t pitch;
    uint32_t z_start_tag;
    uint32_t z_offset;
} XboxD3DTile;

static inline uint32_t xbox_d3d8_normalize_physical(uint32_t address)
{
    return address & UINT32_C(0x0FFFFFFF);
}

static inline bool xbox_d3d8_tile_contains(
    const XboxD3DTile *tile, uint32_t address)
{
    uint32_t start;
    uint32_t target;

    if (!tile || !tile->memory || !tile->size)
        return false;
    start = xbox_d3d8_normalize_physical(tile->memory);
    target = xbox_d3d8_normalize_physical(address);
    return target >= start && target - start < tile->size;
}

#endif
