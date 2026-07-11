#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../xemu-calltrace-argsets.h"

static void set(uint32_t a[6], uint32_t v) {
    for (int i = 0; i < 6; i++) a[i] = v + i;
}

static CTEdgeArgs make(uint16_t cap) {
    CTEdgeArgs ea = { 0, cap, NULL };
    ea.sets = malloc(sizeof(uint32_t) * CT_ARGSET_DWORDS * cap);
    return ea;
}

int main(void)
{
    /* dedup + overflow at a small cap */
    CTEdgeArgs ea = make(4);
    uint32_t a[6], b[6], c[6];
    set(a, 100);
    set(b, 200);

    assert(ct_argset_intern(&ea, a) == 0);
    assert(ct_argset_intern(&ea, a) == 0);      /* identical -> dedup */
    assert(ea.nsets == 1);
    assert(ct_argset_intern(&ea, b) == 1);      /* distinct -> new */
    assert(ea.nsets == 2);
    set(c, 300); assert(ct_argset_intern(&ea, c) == 2);
    set(c, 400); assert(ct_argset_intern(&ea, c) == 3);
    assert(ea.nsets == 4);                       /* cap reached */
    set(c, 500);                                 /* 5th distinct overflows */
    assert(ct_argset_intern(&ea, c) == CT_ARGSET_OVERFLOW);
    assert(ea.nsets == 4);
    assert(ct_argset_intern(&ea, a) == 0);      /* known set still dedups */
    free(ea.sets);

    /* large cap (>255): set indices exceed a byte, must be 16-bit clean */
    CTEdgeArgs ex = make(CT_ARGSET_CAP_EXTREME);
    for (uint32_t i = 0; i < 300; i++) {
        uint32_t w[6];
        set(w, 1000 + i * 7);
        assert(ct_argset_intern(&ex, w) == (uint16_t)i);
    }
    assert(ex.nsets == 300);
    uint32_t w299[6];
    set(w299, 1000 + 299 * 7);
    assert(ct_argset_intern(&ex, w299) == 299);  /* a >255 index dedups */
    free(ex.sets);

    printf("PASS\n");
    return 0;
}
