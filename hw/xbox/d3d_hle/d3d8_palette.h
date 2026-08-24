#ifndef HW_XBOX_D3D_HLE_D3D8_PALETTE_H
#define HW_XBOX_D3D_HLE_D3D8_PALETTE_H

#include <stdint.h>

static inline uint32_t xbox_d3d8_palette_entry_count(uint32_t common)
{
    return 256u >> ((common >> 30) & 3u);
}

static inline uint32_t xbox_d3d8_palette_lookup(
    const uint32_t *palette, uint32_t entries, uint32_t index)
{
    if (!palette || !entries || (entries & (entries - 1u)) != 0u)
        return 0;
    return palette[index & (entries - 1u)];
}

#endif
