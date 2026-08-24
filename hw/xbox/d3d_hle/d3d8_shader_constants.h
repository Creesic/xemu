#ifndef HW_XBOX_D3D_HLE_D3D8_SHADER_CONSTANTS_H
#define HW_XBOX_D3D_HLE_D3D8_SHADER_CONSTANTS_H

#include <stdint.h>

enum {
    XBOX_D3DSCM_96CONSTANTS = 0,
    XBOX_D3DSCM_192CONSTANTS = 1,
    XBOX_D3DSCM_192CONSTANTSANDFIXEDPIPELINE = 2,
    XBOX_D3DSCM_NORESERVEDCONSTANTS = 0x10,
};

/* Translate the Xbox public -96..95 register namespace into NV2A's 0..191
 * hardware slots while enforcing the active XDK constant-mode contract. */
static inline int xbox_d3d8_shader_constant_index(
    uint32_t mode, int start_register, int count)
{
    uint32_t base_mode = mode & ~XBOX_D3DSCM_NORESERVEDCONSTANTS;
    int end_register;

    if (mode & ~(XBOX_D3DSCM_NORESERVEDCONSTANTS | 3u))
        return -1;
    if (base_mode > XBOX_D3DSCM_192CONSTANTSANDFIXEDPIPELINE || count <= 0)
        return -1;
    if (start_register < -96 || start_register > 95 || count > 192)
        return -1;
    end_register = start_register + count;
    if (end_register <= start_register || end_register > 96)
        return -1;
    if (base_mode == XBOX_D3DSCM_96CONSTANTS && start_register < 0)
        return -1;
    if (base_mode != XBOX_D3DSCM_96CONSTANTS &&
        !(mode & XBOX_D3DSCM_NORESERVEDCONSTANTS) &&
        start_register <= -37 && end_register > -38) {
        return -1;
    }
    return start_register + 96;
}

#endif
