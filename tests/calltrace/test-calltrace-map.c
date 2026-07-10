#include <assert.h>
#include <stdio.h>
#include "../../xemu-calltrace-map.h"

int main(void)
{
    CTMap m;
    assert(ct_map_init(&m));
    assert(m.num_entries == 0);

    /* new edge inserts with count 1 */
    assert(ct_map_add(&m, 0x11000, 0x22000));
    assert(m.num_entries == 1);

    /* same edge increments count, not entries */
    assert(ct_map_add(&m, 0x11000, 0x22000));
    assert(m.num_entries == 1);

    /* different callee from the same site is a distinct edge */
    assert(ct_map_add(&m, 0x11000, 0x33000));
    assert(m.num_entries == 2);

    /* the incremented edge holds count 2 */
    uint64_t key = ct_map_key(0x11000, 0x22000);
    uint64_t found = 0;
    for (uint32_t i = 0; i < CT_MAP_CAPACITY; i++) {
        if (m.slots[i].key == key) {
            found = m.slots[i].count;
        }
    }
    assert(found == 2);

    /* zero key (both args 0) is ignored but reports success */
    assert(ct_map_add(&m, 0, 0));
    assert(m.num_entries == 2);

    /* fill to the cap: new keys refused, existing keys still count */
    for (uint32_t i = 0; m.num_entries < CT_MAP_MAX_ENTRIES; i++) {
        ct_map_add(&m, 0x100000 + i, 0x200000);
    }
    assert(m.num_entries == CT_MAP_MAX_ENTRIES);
    assert(!ct_map_add(&m, 0xF0000000, 0xE0000000)); /* new -> refused */
    assert(ct_map_add(&m, 0x11000, 0x22000));        /* existing -> ok  */

    ct_map_free(&m);
    assert(m.slots == NULL);

    /* ct_map_add_indexed assigns stable, dense indices */
    CTMap m2;
    assert(ct_map_init(&m2));
    CTEdge *e0 = ct_map_add_indexed(&m2, 0xAAAA, 0xBBBB);
    CTEdge *e1 = ct_map_add_indexed(&m2, 0xCCCC, 0xDDDD);
    assert(e0 && e0->index == 0 && e0->count == 1);
    assert(e1 && e1->index == 1 && e1->count == 1);
    CTEdge *e0b = ct_map_add_indexed(&m2, 0xAAAA, 0xBBBB);
    assert(e0b == e0 && e0b->index == 0 && e0b->count == 2);
    assert(ct_map_add_indexed(&m2, 0, 0) == NULL); /* sentinel */
    assert(m2.num_entries == 2);
    ct_map_free(&m2);

    printf("PASS\n");
    return 0;
}
