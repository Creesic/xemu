#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-calltree.h"

int main(void)
{
    FICallTree t;
    assert(fi_calltree_init(&t));

    /* init pre-creates the unknown root as node 0 */
    assert(t.num_nodes == 1);
    assert(t.nodes[FI_NODE_ROOT].parent == FI_NODE_INVALID);

    /* interning the same triple twice returns the same node */
    uint32_t a = fi_calltree_intern(&t, FI_NODE_ROOT, 0x11000, 0x22000);
    uint32_t b = fi_calltree_intern(&t, FI_NODE_ROOT, 0x11000, 0x22000);
    assert(a != FI_NODE_INVALID && a == b);
    assert(t.num_nodes == 2);

    /* different parent => different path node, same (site, callee) */
    uint32_t c = fi_calltree_intern(&t, a, 0x11000, 0x22000);
    assert(c != a && c != FI_NODE_INVALID);
    assert(t.nodes[c].parent == a);
    assert(t.nodes[c].call_site == 0x11000 && t.nodes[c].callee == 0x22000);

    /* argsets: dedup within and across nodes; per-node id-slot cap */
    uint32_t s1[6] = {1, 2, 3, 4, 5, 6};
    uint32_t s2[6] = {9, 2, 3, 4, 5, 6};
    fi_calltree_add_args(&t, a, s1);
    fi_calltree_add_args(&t, a, s1);          /* dup: no new set/slot */
    fi_calltree_add_args(&t, a, s2);
    assert(t.nodes[a].n_argsets == 2);
    assert(t.num_argsets == 2);
    fi_calltree_add_args(&t, c, s1);          /* cross-node dedup: pool stays 2 */
    assert(t.num_argsets == 2 && t.nodes[c].n_argsets == 1);
    assert(memcmp(t.argsets[t.nodes[a].argset_ids[0]], s1, 24) == 0);
    for (int i = 0; i < FI_NODE_ARGSETS + 3; i++) {
        uint32_t sx[6] = {100u + (uint32_t)i, 0, 0, 0, 0, 0};
        fi_calltree_add_args(&t, c, sx);
    }
    assert(t.nodes[c].n_argsets == FI_NODE_ARGSETS); /* slots capped, no overflow */

    /* node cap: refuse with FI_NODE_INVALID and set the truncated flag */
    while (t.num_nodes < FI_CT_MAX_NODES) {
        fi_calltree_intern(&t, FI_NODE_ROOT, 0x100000u + t.num_nodes, 0x5);
    }
    assert(fi_calltree_intern(&t, FI_NODE_ROOT, 0xF000F000u, 0xE000E000u)
           == FI_NODE_INVALID);
    assert(t.nodes_truncated);
    assert(fi_calltree_intern(&t, FI_NODE_ROOT, 0x11000, 0x22000) == a);

    fi_calltree_free(&t);
    assert(t.nodes == NULL);
    printf("PASS\n");
    return 0;
}
