#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-calltrace-argsets.h"

static void set(uint32_t a[6], uint32_t v) {
    for (int i = 0; i < 6; i++) a[i] = v + i;
}

int main(void)
{
    CTEdgeArgs ea;
    memset(&ea, 0, sizeof(ea));
    assert(ea.nsets == 0);

    uint32_t a[6], b[6];
    set(a, 100);
    set(b, 200);

    /* first insert -> index 0 */
    assert(ct_argset_intern(&ea, a) == 0);
    /* identical snapshot dedups to same index, no new set */
    assert(ct_argset_intern(&ea, a) == 0);
    assert(ea.nsets == 1);
    /* distinct snapshot -> index 1 */
    assert(ct_argset_intern(&ea, b) == 1);
    assert(ea.nsets == 2);
    /* old one still dedups */
    assert(ct_argset_intern(&ea, a) == 0);
    assert(ea.nsets == 2);

    /* fill to the cap with distinct sets: already have 2, add 14 more = 16 */
    for (uint32_t i = 2; i < CT_ARGSET_CAP; i++) {
        uint32_t c[6];
        set(c, 1000 + i * 10);
        assert(ct_argset_intern(&ea, c) == (uint8_t)i);
    }
    assert(ea.nsets == CT_ARGSET_CAP);

    /* a new, 17th distinct set overflows and is NOT stored */
    uint32_t ov[6];
    set(ov, 99999);
    assert(ct_argset_intern(&ea, ov) == CT_ARGSET_OVERFLOW);
    assert(ea.nsets == CT_ARGSET_CAP);
    /* an already-known set still dedups after overflow */
    assert(ct_argset_intern(&ea, a) == 0);

    printf("PASS\n");
    return 0;
}
