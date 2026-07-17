#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-resources.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

int main(void)
{
    FIResourcePool p;
    FIBudget budget = { .limit = 300u << 20, .used = 0 };
    CHECK(fi_resources_init(&p, &budget));

    uint8_t a[64], b[64];
    memset(a, 0xAB, sizeof(a));
    memset(b, 0xCD, sizeof(b));

    uint32_t r0 = fi_resources_intern(&p, &budget, 1, a, sizeof(a), 0);
    CHECK(r0 != FI_RES_INVALID);
    CHECK(p.res[r0].len == 64 && p.res[r0].kind == 1);

    /* identical bytes+kind+meta -> same id, blob doesn't grow */
    uint64_t used = p.blob_used;
    uint32_t r0b = fi_resources_intern(&p, &budget, 1, a, sizeof(a), 0);
    CHECK(r0b == r0 && p.blob_used == used);

    /* different bytes -> new id */
    uint32_t r1 = fi_resources_intern(&p, &budget, 1, b, sizeof(b), 0);
    CHECK(r1 != r0);

    /* same bytes, different kind -> distinct */
    uint32_t r2 = fi_resources_intern(&p, &budget, 2, a, sizeof(a), 0);
    CHECK(r2 != r0);

    /* same bytes+kind, different meta -> distinct (e.g. format/dims) */
    uint32_t r3 = fi_resources_intern(&p, &budget, 1, a, sizeof(a), 0x1234);
    CHECK(r3 != r0);

    /* stored bytes are retrievable and correct */
    CHECK(memcmp(p.blob + p.res[r0].off, a, 64) == 0);
    CHECK(fi_resources_intern(&p, &budget, 1, NULL, 1, 0)
          == FI_RES_INVALID);

    /* Reallocation preserves an input pointer that aliases the blob. */
    const uint32_t large_len = 900u << 10;
    uint8_t *large = (uint8_t *)malloc(large_len);
    CHECK(large != NULL);
    memset(large, 0x5A, large_len);
    uint32_t large_id = fi_resources_intern(&p, &budget, 9, large,
                                            large_len, 0);
    CHECK(large_id != FI_RES_INVALID);
    free(large);
    const uint8_t *aliased = p.blob + p.res[large_id].off;
    uint32_t copy_id = fi_resources_intern(&p, &budget, 10, aliased,
                                           large_len, 0);
    CHECK(copy_id != FI_RES_INVALID);
    CHECK(memcmp(p.blob + p.res[copy_id].off,
                 p.blob + p.res[large_id].off, large_len) == 0);

    fi_resources_free(&p, &budget);
    CHECK(p.blob == NULL);
    CHECK(budget.used == 0);
    printf("PASS\n");
    return 0;
}
