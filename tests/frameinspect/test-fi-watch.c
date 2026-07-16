#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-watch.h"

int main(void)
{
    FIWatchEngine w;
    assert(fi_watch_init(&w));

    /* watch set add/find/remove */
    assert(fi_watch_add(&w, 0x8C440));
    assert(fi_watch_find(&w, 0x8C440) == 0);
    assert(fi_watch_find(&w, 0x11111) == -1);
    fi_watch_remove(&w, 0x8C440);
    assert(fi_watch_find(&w, 0x8C440) == -1);
    assert(fi_watch_add(&w, 0x8C440));

    /* enter captures regs/esp/stack and orders invocations */
    uint32_t regs[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t st16[16] = {0};
    st16[0] = 0xAABB;
    uint32_t i0 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    uint32_t i1 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    assert(i0 != FI_INVOC_INVALID && i1 == i0 + 1);
    assert(w.invocs[i0].order == 0 && w.invocs[i1].order == 1);
    assert(w.invocs[i0].node == 77 && w.invocs[i0].stack16[0] == 0xAABB);
    assert(w.invocs[i0].state == FI_INVOC_OPEN);

    /* clean exit records EAX; unclean marks incomplete without a value */
    fi_watch_exit(&w, i0, true, 0x1234);
    assert(w.invocs[i0].state == FI_INVOC_COMPLETE);
    assert(w.invocs[i0].eax_ret == 0x1234);
    fi_watch_exit(&w, i1, false, 0xFFFF);
    assert(w.invocs[i1].state == FI_INVOC_INCOMPLETE);

    /* write log stores old+new bytes */
    uint32_t i2 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    uint8_t oldb[4] = {0, 0, 0, 0}, newb[4] = {0xEF, 0xBE, 0xAD, 0xDE};
    fi_watch_log_write(&w, i2, 0x1000, oldb, newb, 4);
    assert(w.num_recs == 1);
    assert(w.recs[0].addr == 0x1000 && w.recs[0].len == 4);
    assert(memcmp(&w.pool[w.recs[0].new_off], newb, 4) == 0);

    /* contiguous same-invocation writes coalesce (REP-style), and the
     * coalesced runs hold the full byte sequences in order */
    fi_watch_log_write(&w, i2, 0x1004, oldb, newb, 4);
    fi_watch_log_write(&w, i2, 0x1008, oldb, newb, 4);
    assert(w.num_recs == 1 && w.recs[0].len == 12);
    uint8_t exp_new[12], exp_old[12] = {0};
    for (int k = 0; k < 3; k++) {
        memcpy(&exp_new[4 * k], newb, 4);
    }
    assert(memcmp(&w.pool[w.recs[0].old_off], exp_old, 12) == 0);
    assert(memcmp(&w.pool[w.recs[0].new_off], exp_new, 12) == 0);

    /* non-contiguous or different invocation starts a new record */
    fi_watch_log_write(&w, i2, 0x2000, oldb, newb, 4);
    assert(w.num_recs == 2);
    uint32_t i3 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    fi_watch_log_write(&w, i3, 0x2004, oldb, newb, 4);
    assert(w.num_recs == 3);

    /* MMIO writes are counted, not logged */
    fi_watch_count_mmio(&w, i3);
    assert(w.invocs[i3].mmio_writes == 1 && w.num_recs == 3);

    /* per-watch invocation cap */
    uint32_t got = 0;
    for (uint32_t k = 0; k < FI_WATCH_MAX_INVOC + 8; k++) {
        if (fi_watch_enter(&w, 0, 77, regs, 0x7000, st16)
            != FI_INVOC_INVALID) {
            got++;
        }
    }
    assert(got == FI_WATCH_MAX_INVOC - 4);   /* 4 used above */
    assert(w.invocs_truncated);

    fi_watch_deinit(&w);
    assert(w.recs == NULL);
    printf("PASS\n");
    return 0;
}
