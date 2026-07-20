#include <stdio.h>
#include "../../xemu-frameinspect-commandlog.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

int main(void)
{
    uint64_t initial_bytes =
        (uint64_t)FI_COMMANDLOG_INITIAL_CAP * sizeof(FICommandRec);
    FIBudget budget = { .limit = initial_bytes * 2, .used = 0 };
    FICommandLog log;
    CHECK(fi_commandlog_init(&log, &budget));
    CHECK(budget.used == initial_bytes);

    FICommandRec header = {
        .phys_addr = 0x03A4F200,
        .dma_instance = 0x00001230,
        .dma_get = 0x200,
        .raw = 0x000C1810,
        .writer_node = 41,
        .confidence = FI_ORIG_ATTRIBUTED,
        .kind = FI_CMD_METHOD_HEADER,
        .data.header = {
            .method = 0x1810,
            .count = 3,
            .subchannel = 0,
            .method_type = FI_CMD_METHOD_INCREASING,
        },
    };
    uint32_t packet = fi_commandlog_append(&log, &budget, &header);
    CHECK(packet == 0);
    CHECK(log.recs[packet].seq == 0);
    CHECK(log.recs[packet].data.header.method == 0x1810);
    CHECK(log.recs[packet].data.header.count == 3);

    FICommandRec parameter = {
        .phys_addr = 0x03A4F204,
        .dma_instance = 0x00001230,
        .dma_get = 0x204,
        .raw = 0xAABBCCDD,
        .writer_node = 52,
        .confidence = FI_ORIG_PARTIAL,
        .kind = FI_CMD_METHOD_PARAM,
        .data.parameter = {
            .packet = packet,
            .method = 0x1814,
            .parameter_index = 1,
            .subchannel = 0,
            .method_type = FI_CMD_METHOD_INCREASING,
        },
    };
    uint32_t param_id = fi_commandlog_append(&log, &budget, &parameter);
    CHECK(param_id == 1);
    CHECK(log.recs[param_id].data.parameter.packet == packet);
    CHECK(log.recs[param_id].data.parameter.method == 0x1814);
    CHECK(log.recs[param_id].raw == 0xAABBCCDD);
    parameter.raw = 0;
    CHECK(log.recs[param_id].raw == 0xAABBCCDD);

    FICommandRec call = {
        .phys_addr = 0x03A4F208,
        .dma_instance = 0x00001230,
        .dma_get = 0x208,
        .raw = 0x00000802,
        .confidence = FI_ORIG_UNATTRIBUTED,
        .kind = FI_CMD_CALL,
        .data.control = { .target = 0x800 },
    };
    uint32_t call_id = fi_commandlog_append(&log, &budget, &call);
    CHECK(call_id == 2);
    CHECK(log.recs[call_id].data.control.target == 0x800);

    /* Exhaust the initial allocation. The next growth exceeds the budget. */
    budget.limit = initial_bytes;
    FICommandRec reserved = { .kind = FI_CMD_RESERVED };
    while (log.num_recs < FI_COMMANDLOG_INITIAL_CAP) {
        CHECK(fi_commandlog_append(&log, &budget, &reserved) !=
              FI_COMMAND_INVALID);
    }
    CHECK(fi_commandlog_append(&log, &budget, &reserved) ==
          FI_COMMAND_INVALID);
    CHECK(log.truncated);

    fi_commandlog_free(&log, &budget);
    CHECK(log.recs == NULL);
    CHECK(budget.used == 0);
    printf("PASS\n");
    return 0;
}
