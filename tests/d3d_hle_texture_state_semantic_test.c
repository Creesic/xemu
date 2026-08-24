#include "d3d8_texture_state.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_BUMPENVMAT00) == 0);
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_BUMPENVMAT01) == 1);
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_BUMPENVMAT11) == 3);
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_BUMPENVMAT10) == 2);
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_BUMPENVLSCALE) == 4);
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_BUMPENVLOFFSET) == 5);
    assert(xbox_d3d8_bump_env_component(XBOX_D3DTSS_COLORKEYCOLOR) == -1);
    assert(xbox_d3d8_bump_env_component(0xFFFFFFFFu) == -1);
    assert(XBOX_D3DTCOLORKEYOP_DISABLE == 0);
    assert(XBOX_D3DTCOLORKEYOP_ALPHA == 1);
    assert(XBOX_D3DTCOLORKEYOP_RGBA == 2);
    assert(XBOX_D3DTCOLORKEYOP_KILL == 3);
    assert(xbox_d3d8_color_key_mask(0x07u) == 0x00FFFFFFu);
    assert(xbox_d3d8_color_key_mask(0x1Eu) == 0x00FFFFFFu);
    assert(xbox_d3d8_color_key_mask(0x06u) == 0xFFFFFFFFu);
    puts("d3d_hle_texture_state_semantic_test: OK");
    return 0;
}
