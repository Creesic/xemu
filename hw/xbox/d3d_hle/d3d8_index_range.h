#ifndef XRECOMP_D3D8_INDEX_RANGE_H
#define XRECOMP_D3D8_INDEX_RANGE_H

#include <stdint.h>

typedef struct D3D8Index16Range {
    uint32_t minimum;
    uint32_t maximum;
} D3D8Index16Range;

/* Validate a 16-bit index list against the caller-declared D3D range and
 * return its observed bounds. Pure helper shared by the compatibility device
 * and its semantic test. */
static inline int d3d8_index16_range(
    const uint16_t *indices, uint32_t count,
    uint32_t declared_minimum, uint32_t declared_count,
    D3D8Index16Range *range)
{
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0;
    uint32_t i;

    if (!indices || !count || !declared_count || !range)
        return 0;
    for (i = 0; i < count; ++i) {
        uint32_t index = indices[i];
        if (index < declared_minimum ||
            index - declared_minimum >= declared_count)
            return 0;
        if (index < minimum)
            minimum = index;
        if (index > maximum)
            maximum = index;
    }
    range->minimum = minimum;
    range->maximum = maximum;
    return 1;
}

#endif
