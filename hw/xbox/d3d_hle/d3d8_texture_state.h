#ifndef HW_XBOX_D3D_HLE_D3D8_TEXTURE_STATE_H
#define HW_XBOX_D3D_HLE_D3D8_TEXTURE_STATE_H

#include <stdint.h>

/* Xbox XDK texture-stage indices. The compatibility IDirect3DDevice8 uses a
 * host-normalized enum, so guest leaf wrappers must not pass these through as
 * host D3DTEXTURESTAGESTATETYPE values. */
typedef enum XboxD3DTextureStageState {
    XBOX_D3DTSS_ADDRESSU = 0,
    XBOX_D3DTSS_ADDRESSV = 1,
    XBOX_D3DTSS_ADDRESSW = 2,
    XBOX_D3DTSS_MAGFILTER = 3,
    XBOX_D3DTSS_MINFILTER = 4,
    XBOX_D3DTSS_MIPFILTER = 5,
    XBOX_D3DTSS_MIPMAPLODBIAS = 6,
    XBOX_D3DTSS_MAXMIPLEVEL = 7,
    XBOX_D3DTSS_MAXANISOTROPY = 8,
    XBOX_D3DTSS_COLORKEYOP = 9,
    XBOX_D3DTSS_COLORSIGN = 10,
    XBOX_D3DTSS_ALPHAKILL = 11,
    XBOX_D3DTSS_COLOROP = 12,
    XBOX_D3DTSS_COLORARG0 = 13,
    XBOX_D3DTSS_COLORARG1 = 14,
    XBOX_D3DTSS_COLORARG2 = 15,
    XBOX_D3DTSS_ALPHAOP = 16,
    XBOX_D3DTSS_ALPHAARG0 = 17,
    XBOX_D3DTSS_ALPHAARG1 = 18,
    XBOX_D3DTSS_ALPHAARG2 = 19,
    XBOX_D3DTSS_RESULTARG = 20,
    XBOX_D3DTSS_TEXTURETRANSFORMFLAGS = 21,
    XBOX_D3DTSS_BUMPENVMAT00 = 22,
    XBOX_D3DTSS_BUMPENVMAT01 = 23,
    XBOX_D3DTSS_BUMPENVMAT11 = 24,
    XBOX_D3DTSS_BUMPENVMAT10 = 25,
    XBOX_D3DTSS_BUMPENVLSCALE = 26,
    XBOX_D3DTSS_BUMPENVLOFFSET = 27,
    XBOX_D3DTSS_TEXCOORDINDEX = 28,
    XBOX_D3DTSS_BORDERCOLOR = 29,
    XBOX_D3DTSS_COLORKEYCOLOR = 30,
} XboxD3DTextureStageState;

typedef enum XboxD3DTextureColorKeyOp {
    XBOX_D3DTCOLORKEYOP_DISABLE = 0,
    XBOX_D3DTCOLORKEYOP_ALPHA = 1,
    XBOX_D3DTCOLORKEYOP_RGBA = 2,
    XBOX_D3DTCOLORKEYOP_KILL = 3,
} XboxD3DTextureColorKeyOp;

/* Return the host combiner's authored bump component order:
 * m00, m01, m10, m11, luminance scale, luminance offset. */
static inline int xbox_d3d8_bump_env_component(uint32_t type)
{
    switch (type) {
    case XBOX_D3DTSS_BUMPENVMAT00: return 0;
    case XBOX_D3DTSS_BUMPENVMAT01: return 1;
    case XBOX_D3DTSS_BUMPENVMAT10: return 2;
    case XBOX_D3DTSS_BUMPENVMAT11: return 3;
    case XBOX_D3DTSS_BUMPENVLSCALE: return 4;
    case XBOX_D3DTSS_BUMPENVLOFFSET: return 5;
    default: return -1;
    }
}

static inline uint32_t xbox_d3d8_color_key_mask(uint32_t format)
{
    switch (format & 0xFFu) {
    case 0x07u: /* D3DFMT_X8R8G8B8 */
    case 0x1Eu: /* D3DFMT_LIN_X8R8G8B8 */
        return UINT32_C(0x00FFFFFF);
    default:
        return UINT32_MAX;
    }
}

#endif
