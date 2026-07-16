#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-methodlog.h"

int main(void)
{
    FIMethodLog m;
    assert(fi_methodlog_init(&m));

    uint32_t r0 = fi_methodlog_append(&m, 0x1810, 0, 0x2A, 0x03A4F20C,
                                      41, FI_ORIG_ATTRIBUTED);
    uint32_t r1 = fi_methodlog_append(&m, 0x1814, 0, 0x00, 0x03A4F210,
                                      0, FI_ORIG_UNATTRIBUTED);
    assert(r0 == 0 && r1 == 1 && m.num_recs == 2);
    assert(m.recs[r0].method == 0x1810 && m.recs[r0].param == 0x2A);
    assert(m.recs[r0].phys_addr == 0x03A4F20C && m.recs[r0].writer_node == 41);
    assert(m.recs[r0].confidence == FI_ORIG_ATTRIBUTED);
    assert(m.recs[r1].confidence == FI_ORIG_UNATTRIBUTED);

    /* a batch owns a contiguous record range */
    fi_methodlog_mark_batch(&m, 7, 0, 2);
    assert(m.num_batches == 1);
    assert(m.batches[0].batch_event == 7);
    assert(m.batches[0].first_rec == 0 && m.batches[0].rec_count == 2);

    /* record cap */
    while (m.num_recs < FI_METHODLOG_CAP) {
        fi_methodlog_append(&m, 0, 0, 0, 0, 0, FI_ORIG_UNATTRIBUTED);
    }
    assert(fi_methodlog_append(&m, 1, 0, 0, 0, 0, FI_ORIG_ATTRIBUTED)
           == FI_METHOD_INVALID);
    assert(m.truncated);

    fi_methodlog_free(&m);
    assert(m.recs == NULL);
    printf("PASS\n");
    return 0;
}
