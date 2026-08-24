#include "d3d8_palette.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint32_t palette[4] = {
        0xFF000000u, 0xFF112233u, 0xFF445566u, 0xFF778899u,
    };
    assert(xbox_d3d8_palette_entry_count(0u << 30) == 256u);
    assert(xbox_d3d8_palette_entry_count(1u << 30) == 128u);
    assert(xbox_d3d8_palette_entry_count(2u << 30) == 64u);
    assert(xbox_d3d8_palette_entry_count(3u << 30) == 32u);
    assert(xbox_d3d8_palette_lookup(palette, 4u, 2u) == 0xFF445566u);
    assert(xbox_d3d8_palette_lookup(palette, 4u, 6u) == 0xFF445566u);
    puts("d3d_hle_palette_semantic_test: OK");
    return 0;
}
