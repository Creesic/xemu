#include <stdio.h>
#include "../../xemu-frameinspect-eventlog.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

int main(void)
{
    FIEventLog l;
    FIBudget b = { .limit = 64u << 20, .used = 0 };
    CHECK(fi_eventlog_init(&l, &b));

    uint32_t e0 = fi_eventlog_append(&l, &b, FI_EV_BATCH, 3, 100, 0, 0, 0);
    uint32_t e1 = fi_eventlog_append(&l, &b, FI_EV_CLEAR, 3, 0, 0, 0, 0);
    CHECK(e0 == 0 && e1 == 1 && l.count == 2);
    CHECK(l.events[e0].kind == FI_EV_BATCH && l.events[e0].surface_gen == 3);
    CHECK(l.events[e0].seq == 0 && l.events[e1].seq == 1);
    CHECK(l.events[e0].a0 == 100);

    /* budget: take within limit succeeds, over-limit refused without charge */
    FIBudget small = { .limit = 1000, .used = 0 };
    CHECK(fi_budget_try(&small, 600) && small.used == 600);
    CHECK(!fi_budget_try(&small, 500) && small.used == 600);
    CHECK(fi_budget_try(&small, 400) && small.used == 1000);
    FIBudget wrap = { .limit = UINT64_MAX, .used = UINT64_MAX - 1 };
    CHECK(!fi_budget_try(&wrap, 2) && wrap.used == UINT64_MAX - 1);

    /* cap: fill to FI_EVENTLOG_CAP, next append refused + truncated flag */
    for (uint32_t i = l.count; i < FI_EVENTLOG_CAP; i++) {
        CHECK(fi_eventlog_append(&l, &b, FI_EV_METHOD, 0, 0, 0, 0, 0)
              != FI_EVENT_INVALID);
    }
    CHECK(fi_eventlog_append(&l, &b, FI_EV_METHOD, 0, 0, 0, 0, 0)
          == FI_EVENT_INVALID);
    CHECK(l.truncated);

    fi_eventlog_free(&l, &b);
    CHECK(l.events == NULL);

    /* A denied growth leaves both the log and budget unchanged. */
    FIEventLog limited;
    FIBudget limited_budget = {
        .limit = 4096u * sizeof(FIEvent),
        .used = 0,
    };
    CHECK(fi_eventlog_init(&limited, &limited_budget));
    for (uint32_t i = 0; i < limited.cap; i++) {
        CHECK(fi_eventlog_append(&limited, &limited_budget, FI_EV_METHOD,
                                 0, 0, 0, 0, 0) != FI_EVENT_INVALID);
    }
    uint64_t used_before = limited_budget.used;
    CHECK(fi_eventlog_append(&limited, &limited_budget, FI_EV_METHOD,
                             0, 0, 0, 0, 0) == FI_EVENT_INVALID);
    CHECK(limited.count == 4096 && limited_budget.used == used_before);
    fi_eventlog_free(&limited, &limited_budget);
    CHECK(limited_budget.used == 0);
    printf("PASS\n");
    return 0;
}
