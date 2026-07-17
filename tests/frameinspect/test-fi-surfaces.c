#include <stdio.h>
#include "../../xemu-frameinspect-surfaces.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

static FISurfaceKey mk(uint32_t addr, uint32_t fmt, uint32_t pitch,
                       uint32_t w, uint32_t h)
{
    FISurfaceKey k = {0};
    k.addr = addr; k.format = fmt; k.pitch = pitch;
    k.width = w; k.height = h;
    k.size = (uint64_t)pitch * h;
    k.swizzle = 0; k.color = 1; k.aa = 0; k.scale = 1;
    return k;
}

int main(void)
{
    FISurfaceStore s;
    CHECK(fi_surfaces_init(&s));

    /* first intern creates generation 0 */
    FISurfaceKey a = mk(0x1000, 4, 2560, 640, 480);
    uint32_t g0 = fi_surfaces_intern(&s, &a);
    CHECK(g0 != FI_SURFGEN_INVALID);
    CHECK(s.gens[g0].generation == 0);
    CHECK(s.gens[g0].prev_at_addr == FI_SURFGEN_INVALID);
    CHECK(s.gens[g0].extent == 0x1000u + 2560u * 480u);

    /* identical key at same addr -> same generation, no growth */
    uint32_t g0b = fi_surfaces_intern(&s, &a);
    CHECK(g0b == g0 && s.num_gens == 1);

    /* changed format at same addr -> new generation, gen counter bumps,
     * prev link points back */
    FISurfaceKey a2 = mk(0x1000, 5, 2560, 640, 480);
    uint32_t g1 = fi_surfaces_intern(&s, &a2);
    CHECK(g1 != g0 && s.gens[g1].generation == 1);
    CHECK(s.gens[g1].prev_at_addr == g0);
    CHECK(s.num_gens == 2);

    /* a different addr whose byte range overlaps g1 -> flagged alias
     * (both flagged) */
    FISurfaceKey b = mk(0x1000 + 2560 * 100, 4, 2560, 640, 380);
    uint32_t gb = fi_surfaces_intern(&s, &b);
    CHECK(s.gens[gb].alias && s.gens[g1].alias);

    /* a disjoint addr -> not aliased */
    FISurfaceKey c = mk(0x800000, 4, 2560, 640, 480);
    uint32_t gc = fi_surfaces_intern(&s, &c);
    CHECK(!s.gens[gc].alias);

    /* Authoritative byte size, not pitch*height, drives alias detection. */
    FISurfaceKey wide = mk(0xA00000, 4, 64, 64, 4);
    wide.size = 64u * 4u * 4u;
    uint32_t gw = fi_surfaces_intern(&s, &wide);
    FISurfaceKey tail = mk(0xA00000 + 512, 4, 64, 16, 4);
    uint32_t gt = fi_surfaces_intern(&s, &tail);
    CHECK(s.gens[gw].alias && s.gens[gt].alias);

    FISurfaceKey invalid = mk(0xB00000, 4, 64, 16, 4);
    invalid.size = 0;
    CHECK(fi_surfaces_intern(&s, &invalid) == FI_SURFGEN_INVALID);
    invalid.addr = UINT64_MAX - 1;
    invalid.size = 4;
    CHECK(fi_surfaces_intern(&s, &invalid) == FI_SURFGEN_INVALID);

    /* cap: refuse with INVALID + truncated flag */
    while (s.num_gens < FI_SURF_MAX_GENS) {
        FISurfaceKey k = mk(0x2000000u + s.num_gens * 0x10000u, 4, 256, 64, 64);
        CHECK(fi_surfaces_intern(&s, &k) != FI_SURFGEN_INVALID);
    }
    FISurfaceKey over = mk(0xF0000000u, 4, 256, 64, 64);
    CHECK(fi_surfaces_intern(&s, &over) == FI_SURFGEN_INVALID);
    CHECK(s.truncated);

    fi_surfaces_free(&s);
    CHECK(s.gens == NULL);
    printf("PASS\n");
    return 0;
}
