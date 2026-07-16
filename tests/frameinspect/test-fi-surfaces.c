#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-surfaces.h"

static FISurfaceKey mk(uint32_t addr, uint32_t fmt, uint32_t pitch,
                       uint32_t w, uint32_t h)
{
    FISurfaceKey k = {0};
    k.addr = addr; k.format = fmt; k.pitch = pitch;
    k.width = w; k.height = h;
    k.swizzle = 0; k.color = 1; k.aa = 0; k.scale = 1;
    return k;
}

int main(void)
{
    FISurfaceStore s;
    assert(fi_surfaces_init(&s));

    /* first intern creates generation 0 */
    FISurfaceKey a = mk(0x1000, 4, 2560, 640, 480);
    uint32_t g0 = fi_surfaces_intern(&s, &a);
    assert(g0 != FI_SURFGEN_INVALID);
    assert(s.gens[g0].generation == 0);
    assert(s.gens[g0].prev_at_addr == FI_SURFGEN_INVALID);
    assert(s.gens[g0].extent == 0x1000u + 2560u * 480u);

    /* identical key at same addr -> same generation, no growth */
    uint32_t g0b = fi_surfaces_intern(&s, &a);
    assert(g0b == g0 && s.num_gens == 1);

    /* changed format at same addr -> new generation, gen counter bumps,
     * prev link points back */
    FISurfaceKey a2 = mk(0x1000, 5, 2560, 640, 480);
    uint32_t g1 = fi_surfaces_intern(&s, &a2);
    assert(g1 != g0 && s.gens[g1].generation == 1);
    assert(s.gens[g1].prev_at_addr == g0);
    assert(s.num_gens == 2);

    /* a different addr whose byte range overlaps g1 -> flagged alias
     * (both flagged) */
    FISurfaceKey b = mk(0x1000 + 2560 * 100, 4, 2560, 640, 380);
    uint32_t gb = fi_surfaces_intern(&s, &b);
    assert(s.gens[gb].alias && s.gens[g1].alias);

    /* a disjoint addr -> not aliased */
    FISurfaceKey c = mk(0x800000, 4, 2560, 640, 480);
    uint32_t gc = fi_surfaces_intern(&s, &c);
    assert(!s.gens[gc].alias);

    /* cap: refuse with INVALID + truncated flag */
    while (s.num_gens < FI_SURF_MAX_GENS) {
        FISurfaceKey k = mk(0x2000000u + s.num_gens * 0x10000u, 4, 256, 64, 64);
        fi_surfaces_intern(&s, &k);
    }
    FISurfaceKey over = mk(0xF0000000u, 4, 256, 64, 64);
    assert(fi_surfaces_intern(&s, &over) == FI_SURFGEN_INVALID);
    assert(s.truncated);

    fi_surfaces_free(&s);
    assert(s.gens == NULL);
    printf("PASS\n");
    return 0;
}
