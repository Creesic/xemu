#include <assert.h>
#include <stdio.h>
#define CT_EVENT_CAP 2500u        /* small cap for the test */
#define CT_EVENT_CHUNK 1024u      /* small chunk for the test */
#include "../../xemu-calltrace-events.h"

int main(void)
{
    CTEvents ev;
    ct_events_init(&ev);
    assert(ev.count == 0);

    for (uint32_t i = 0; i < 3000; i++) {
        bool ok = ct_events_append(&ev, i);
        assert(ok == (i < CT_EVENT_CAP));
    }
    assert(ev.count == CT_EVENT_CAP);

    /* walk the chunks back into a flat check */
    uint64_t seen = 0;
    for (CTEventChunk *c = ev.head; c; c = c->next) {
        for (uint32_t j = 0; j < c->fill; j++) {
            assert(c->data[j] == (uint32_t)seen);
            seen++;
        }
    }
    assert(seen == CT_EVENT_CAP);

    ct_events_free(&ev);
    assert(ev.head == NULL && ev.count == 0);
    printf("PASS\n");
    return 0;
}
