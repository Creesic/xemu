#include "d3d8_shader_constants.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(xbox_d3d8_shader_constant_index(XBOX_D3DSCM_96CONSTANTS,
                                           0, 1) == 96);
    assert(xbox_d3d8_shader_constant_index(XBOX_D3DSCM_96CONSTANTS,
                                           95, 1) == 191);
    assert(xbox_d3d8_shader_constant_index(XBOX_D3DSCM_96CONSTANTS,
                                           -1, 1) == -1);
    assert(xbox_d3d8_shader_constant_index(XBOX_D3DSCM_192CONSTANTS,
                                           -96, 1) == 0);
    assert(xbox_d3d8_shader_constant_index(XBOX_D3DSCM_192CONSTANTS,
                                           -38, 2) == -1);
    assert(xbox_d3d8_shader_constant_index(
               XBOX_D3DSCM_192CONSTANTS | XBOX_D3DSCM_NORESERVEDCONSTANTS,
               -38, 2) == 58);
    assert(xbox_d3d8_shader_constant_index(
               XBOX_D3DSCM_192CONSTANTSANDFIXEDPIPELINE,
               95, 2) == -1);
    assert(xbox_d3d8_shader_constant_index(3, 0, 1) == -1);
    puts("d3d_hle_shader_constant_semantic_test: OK");
    return 0;
}
