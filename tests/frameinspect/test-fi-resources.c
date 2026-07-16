#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-resources.h"

int main(void)
{
    FIResourcePool p;
    assert(fi_resources_init(&p));

    uint8_t a[64], b[64];
    memset(a, 0xAB, sizeof(a));
    memset(b, 0xCD, sizeof(b));

    uint32_t r0 = fi_resources_intern(&p, 1, a, sizeof(a), 0);
    assert(r0 != FI_RES_INVALID);
    assert(p.res[r0].len == 64 && p.res[r0].kind == 1);

    /* identical bytes+kind+meta -> same id, blob doesn't grow */
    uint64_t used = p.blob_used;
    uint32_t r0b = fi_resources_intern(&p, 1, a, sizeof(a), 0);
    assert(r0b == r0 && p.blob_used == used);

    /* different bytes -> new id */
    uint32_t r1 = fi_resources_intern(&p, 1, b, sizeof(b), 0);
    assert(r1 != r0);

    /* same bytes, different kind -> distinct */
    uint32_t r2 = fi_resources_intern(&p, 2, a, sizeof(a), 0);
    assert(r2 != r0);

    /* same bytes+kind, different meta -> distinct (e.g. format/dims) */
    uint32_t r3 = fi_resources_intern(&p, 1, a, sizeof(a), 0x1234);
    assert(r3 != r0);

    /* stored bytes are retrievable and correct */
    assert(memcmp(p.blob + p.res[r0].off, a, 64) == 0);

    fi_resources_free(&p);
    assert(p.blob == NULL);
    printf("PASS\n");
    return 0;
}
