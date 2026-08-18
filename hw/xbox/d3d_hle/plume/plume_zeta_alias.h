#ifndef XGPU_PLUME_ZETA_ALIAS_H
#define XGPU_PLUME_ZETA_ALIAS_H

/* Shared lane arithmetic for the GPU zeta->Y16 alias conversion
 * (docs/technical/plume-gpu-zeta-alias-conversion.md). One guest Z24S8
 * dword yields two little-endian Y16 texels; the stencil plane is
 * unreachable from a host depth resolve and packs as zero, exactly like
 * PlumeDraw::downloadZetaSurface. The HLSL conversion pass must
 * implement THESE formulas; tests/plume_zeta_alias_pack_test.cpp holds
 * the shared vectors both sides are checked against. */

#include <stdint.h>

/* Depth in [0,1] -> packed guest dword (z24 << 8) | stencil(0). Clamp the
 * integer, not just the float: 1.0f * 16777215.0f + 0.5f is not
 * representable in a 24-bit mantissa and rounds UP to 0x1000000, whose
 * << 8 would overflow to zero. */
static inline uint32_t plume_zeta_alias_pack_dword(float depth)
{
    uint32_t z24;
    if (depth < 0.0f)
        depth = 0.0f;
    else if (depth > 1.0f)
        depth = 1.0f;
    z24 = (uint32_t)(depth * 16777215.0f + 0.5f);
    if (z24 > 0xFFFFFFu)
        z24 = 0xFFFFFFu;
    return z24 << 8;
}

/* Y16 texel for alias column x: even columns read the low word of the
 * dword at zeta column x/2, odd columns the high word. */
static inline uint16_t plume_zeta_alias_lane(uint32_t packed_dword,
                                             uint32_t alias_x_odd)
{
    return alias_x_odd ? (uint16_t)(packed_dword >> 16)
                       : (uint16_t)(packed_dword & 0xFFFFu);
}

#endif /* XGPU_PLUME_ZETA_ALIAS_H */
