#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-eventlog.h"

int main(void)
{
    FIEventLog l;
    assert(fi_eventlog_init(&l));

    uint32_t e0 = fi_eventlog_append(&l, FI_EV_BATCH, 3, 100, 0, 0, 0);
    uint32_t e1 = fi_eventlog_append(&l, FI_EV_CLEAR, 3, 0, 0, 0, 0);
    assert(e0 == 0 && e1 == 1 && l.count == 2);
    assert(l.events[e0].kind == FI_EV_BATCH && l.events[e0].surface_gen == 3);
    assert(l.events[e0].seq == 0 && l.events[e1].seq == 1);
    assert(l.events[e0].a0 == 100);

    /* budget: take within limit succeeds, over-limit refused without charge */
    FIBudget b = { .limit = 1000, .used = 0 };
    assert(fi_budget_try(&b, 600) && b.used == 600);
    assert(!fi_budget_try(&b, 500) && b.used == 600);
    assert(fi_budget_try(&b, 400) && b.used == 1000);

    /* cap: fill to FI_EVENTLOG_CAP, next append refused + truncated flag */
    while (l.count < FI_EVENTLOG_CAP) {
        fi_eventlog_append(&l, FI_EV_METHOD, 0, 0, 0, 0, 0);
    }
    assert(fi_eventlog_append(&l, FI_EV_METHOD, 0, 0, 0, 0, 0) == FI_EVENT_INVALID);
    assert(l.truncated);

    fi_eventlog_free(&l);
    assert(l.events == NULL);
    printf("PASS\n");
    return 0;
}
