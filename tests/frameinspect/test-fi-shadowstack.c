#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-calltree.h"
#include "../../xemu-frameinspect-shadowstack.h"

int main(void)
{
    FICallTree t;
    FIShadow s;
    FIPopped pops[FI_SS_MAX_POPS];
    uint32_t np;
    assert(fi_calltree_init(&t));
    fi_shadow_init(&s, &t);

    /* empty stack: current is the unknown root */
    assert(fi_shadow_current(&s, 0xD0000100) == FI_NODE_ROOT);

    /* call chain builds path nodes: A -> B */
    uint32_t a = fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    uint32_t b = fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    assert(a != FI_NODE_ROOT && t.nodes[b].parent == a);
    assert(fi_shadow_current(&s, 0xD0000100) == b);

    /* matched RET pops exactly one frame */
    assert(fi_shadow_ret(&s, 0xD0000100, 0x2015, pops, &np) == a);
    assert(np == 0);   /* nothing watched */

    /* RET skipping frames (e.g. after longjmp-like unwind) pops through */
    uint32_t b2 = fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    fi_shadow_call(&s, 0xD0000100, 0x3010, 0x4000, 0x3015, 0x6FE0);
    (void)b2;
    assert(fi_shadow_ret(&s, 0xD0000100, 0x1005, pops, &np) == FI_NODE_ROOT);

    /* unmatched RET = desync: reset to unknown root, no fabricated parent */
    fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    assert(fi_shadow_ret(&s, 0xD0000100, 0xDEAD, pops, &np) == FI_NODE_ROOT);
    assert(fi_shadow_current(&s, 0xD0000100) == FI_NODE_ROOT);

    /* separate thread keys get separate stacks */
    uint32_t t1 = fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    uint32_t t2 = fi_shadow_call(&s, 0xD0000200, 0x5000, 0x6000, 0x5005, 0x9000);
    assert(fi_shadow_current(&s, 0xD0000100) == t1);
    assert(fi_shadow_current(&s, 0xD0000200) == t2);
    assert(t.nodes[t2].parent == FI_NODE_ROOT);

    /* ESP discontinuity: a CALL above dead frames pops them first */
    fi_shadow_ret(&s, 0xD0000100, 0x1005, pops, &np);       /* empty stack */
    fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    uint32_t d = fi_shadow_call(&s, 0xD0000100, 0x1100, 0x8000, 0x1105, 0x7100);
    assert(t.nodes[d].parent == FI_NODE_ROOT); /* not under stale frames */

    /* watch marking + pop reporting: clean on matched RET */
    fi_shadow_ret(&s, 0xD0000100, 0x1105, pops, &np);
    uint32_t w = fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    (void)w;
    fi_shadow_set_watch(&s, 0xD0000100, 42);
    assert(fi_shadow_in_watch(&s, 0xD0000100));
    fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    assert(fi_shadow_in_watch(&s, 0xD0000100));  /* subtree inherits */
    fi_shadow_ret(&s, 0xD0000100, 0x2015, pops, &np);
    assert(np == 0);                              /* watched frame still open */
    fi_shadow_ret(&s, 0xD0000100, 0x1005, pops, &np);
    assert(np == 1 && pops[0].watch_invoc == 42 && pops[0].clean);
    assert(!fi_shadow_in_watch(&s, 0xD0000100));

    /* watched frame killed by desync reports unclean */
    fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    fi_shadow_set_watch(&s, 0xD0000100, 43);
    fi_shadow_ret(&s, 0xD0000100, 0xDEAD, pops, &np);
    assert(np == 1 && pops[0].watch_invoc == 43 && !pops[0].clean);

    /* depth overflow: calls beyond FI_SS_MAX_DEPTH keep returning a node */
    for (int i = 0; i < FI_SS_MAX_DEPTH + 8; i++) {
        uint32_t n = fi_shadow_call(&s, 0xD0000300, 0x1000u + (uint32_t)i,
                                    0x2000u + (uint32_t)i, 0x1005u + (uint32_t)i,
                                    0x9000u - 16u * (uint32_t)i);
        assert(n != FI_NODE_INVALID);
    }

    fi_calltree_free(&t);
    printf("PASS\n");
    return 0;
}
