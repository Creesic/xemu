#include "d3d8_tile_state.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    XboxD3DTile tile = {
        .memory = 0x83E00000u,
        .size = 0x00200000u,
        .pitch = 0x0A00u,
    };
    assert(xbox_d3d8_tile_contains(&tile, 0x03E00000u));
    assert(xbox_d3d8_tile_contains(&tile, 0x83FFFFFFu));
    assert(!xbox_d3d8_tile_contains(&tile, 0x04000000u));
    assert(!xbox_d3d8_tile_contains(NULL, 0x03E00000u));
    tile.size = 0;
    assert(!xbox_d3d8_tile_contains(&tile, 0x03E00000u));
    puts("d3d_hle_tile_semantic_test: OK");
    return 0;
}
