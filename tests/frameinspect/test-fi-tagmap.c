#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-tagmap.h"

int main(void)
{
    FITagMap m;
    assert(fi_tagmap_init(&m, 1024));

    /* untagged reads 0 */
    assert(fi_tagmap_lookup(&m, 0x10) == 0);

    /* aligned full-dword store: clean tag, node recoverable */
    fi_tagmap_tag(&m, 0x10, 4, 7);
    uint32_t tag = fi_tagmap_lookup(&m, 0x10);
    assert(tag != 0 && !(tag & FI_TAG_PARTIAL) && FI_TAG_NODE(tag) == 7);

    /* node 0 (unknown root) is distinguishable from unattributed */
    fi_tagmap_tag(&m, 0x20, 4, 0);
    tag = fi_tagmap_lookup(&m, 0x20);
    assert(tag != 0 && FI_TAG_NODE(tag) == 0);

    /* byte store: partial flag on its dword */
    fi_tagmap_tag(&m, 0x31, 1, 9);
    tag = fi_tagmap_lookup(&m, 0x30);
    assert((tag & FI_TAG_PARTIAL) && FI_TAG_NODE(tag) == 9);

    /* unaligned 4-byte store crossing a dword boundary: both partial */
    fi_tagmap_tag(&m, 0x42, 4, 3);
    assert(fi_tagmap_lookup(&m, 0x40) & FI_TAG_PARTIAL);
    assert(fi_tagmap_lookup(&m, 0x44) & FI_TAG_PARTIAL);
    assert(FI_TAG_NODE(fi_tagmap_lookup(&m, 0x44)) == 3);

    /* aligned 8-byte store: both dwords clean */
    fi_tagmap_tag(&m, 0x50, 8, 4);
    assert(!(fi_tagmap_lookup(&m, 0x50) & FI_TAG_PARTIAL));
    assert(!(fi_tagmap_lookup(&m, 0x54) & FI_TAG_PARTIAL));

    /* out-of-range bytes ignored; in-range prefix still tagged */
    fi_tagmap_tag(&m, 1020, 8, 5);
    assert(FI_TAG_NODE(fi_tagmap_lookup(&m, 1020)) == 5);
    assert(fi_tagmap_lookup(&m, 1024) == 0);   /* also out-of-range read */

    fi_tagmap_free(&m);
    assert(m.tags == NULL);
    printf("PASS\n");
    return 0;
}
